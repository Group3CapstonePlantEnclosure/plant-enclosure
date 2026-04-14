#!/usr/bin/env python3
"""
Plant Monitor Camera Server
Raspberry Pi Zero 2W + OV5647 5MP

Two operating modes:
  STILL MODE  — 2592x1944 5MP, used for /capture (plant ID)
  VIDEO MODE  — 1080p/720p/480p at native framerates, used for /stream

The camera switches modes on demand. While streaming in video mode,
/capture will temporarily switch to still mode, shoot, then switch back.

OV5647 native video modes:
  1920x1080 @ 30fps
  1280x720  @ 60fps  (also 30fps available)
  640x480   @ 60fps  (also 30/90fps available)
"""

import io
import time
import threading
import requests
import socket
from flask import Flask, Response, request, jsonify
from picamera2 import Picamera2
from libcamera import Transform, controls as libcam_controls

# ─── Optional GPIO flash LED on pin 18 ───────────────────────────────────────
try:
    import RPi.GPIO as GPIO
    FLASH_PIN = 18
    GPIO.setmode(GPIO.BCM)
    GPIO.setup(FLASH_PIN, GPIO.OUT)
    flash_pwm = GPIO.PWM(FLASH_PIN, 1000)
    flash_pwm.start(0)
    HAS_GPIO = True
    print("[FLASH] GPIO ready on pin 18")
except Exception:
    HAS_GPIO = False
    print("[WARN] GPIO not available — flash control disabled")

# ─── Firebase ─────────────────────────────────────────────────────────────────
FIREBASE_DB_URL = "https://plant-enclosure-default-rtdb.firebaseio.com"

# ─── OV5647 supported video modes ─────────────────────────────────────────────
# Each entry: resolution_id -> { size, supported framerates, config_type }
# config_type "video" uses create_video_configuration (native pipeline, no delay)
# config_type "still" uses create_still_configuration (max quality, slower)
VIDEO_MODES = {
    # id : (width, height, [supported_fps], config_type)
    10: ((2592, 1944), [5, 2, 1],        "still"),   # 5MP — still only, low fps
    9:  ((1920, 1080), [30],              "video"),   # 1080p @ 30fps native
    8:  ((1280, 720),  [60, 30],          "video"),   # 720p  @ 60 or 30fps
    6:  ((640,  480),  [90, 60, 30],      "video"),   # 480p  @ 90/60/30fps
}

# ─── Camera state ─────────────────────────────────────────────────────────────
cam_state = {
    "mode_id":    9,          # current resolution id (default 1080p)
    "fps":        30,         # current framerate
    "brightness": 0.0,
    "contrast":   1.0,
    "saturation": 1.0,
    "vflip":      False,
    "hmirror":    False,
    "is_still":   False,      # True when temporarily in still mode for capture
}

picam2   = Picamera2()
cam_lock = threading.Lock()  # Prevents stream + capture colliding

# ─── Configure camera ─────────────────────────────────────────────────────────
def build_config(mode_id, fps):
    """Build a picamera2 config for the given mode and fps."""
    mode      = VIDEO_MODES[mode_id]
    size      = mode[0]
    cfg_type  = mode[2]
    transform = Transform(vflip=cam_state["vflip"], hflip=cam_state["hmirror"])

    if cfg_type == "still" or mode_id == 10:
        cfg = picam2.create_still_configuration(
            main={"size": size, "format": "RGB888"},
            buffer_count=2,
            transform=transform
        )
    else:
        # Cap framerate using FrameDurationLimits (microseconds)
        frame_duration = int(1_000_000 / fps)
        cfg = picam2.create_video_configuration(
            main={"size": size, "format": "RGB888"},
            controls={"FrameDurationLimits": (frame_duration, frame_duration)},
            buffer_count=4,
            transform=transform
        )
    return cfg

def apply_config(mode_id, fps, settle=1.0):
    """Stop, reconfigure, restart camera. Call inside cam_lock."""
    picam2.stop()
    cfg = build_config(mode_id, fps)
    picam2.configure(cfg)
    picam2.start()
    time.sleep(settle)
    # Reapply user controls after reconfigure
    _apply_controls()
    cam_state["mode_id"] = mode_id
    cam_state["fps"]     = fps
    mode = VIDEO_MODES[mode_id]
    print(f"[CAM] Mode: {mode[0][0]}x{mode[0][1]} @ {fps}fps ({mode[2]})")

def _apply_controls():
    """Push current brightness/contrast/saturation/gain to sensor."""
    picam2.set_controls({
        "Brightness": cam_state["brightness"],
        "Contrast":   cam_state["contrast"],
        "Saturation": cam_state["saturation"],
        "AeEnable":   True,
    })

# Boot into 1080p @ 30fps
print("[CAMERA] Booting at 1080p 30fps...")
cfg = build_config(9, 30)
picam2.configure(cfg)
picam2.start()
time.sleep(2)
print("[CAMERA] Ready")

# ─── Flask App ────────────────────────────────────────────────────────────────
app = Flask(__name__)

@app.after_request
def add_cors(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    return response

# ─── MJPEG stream ─────────────────────────────────────────────────────────────
def generate_mjpeg():
    """
    In video mode: grab frames as fast as the sensor delivers them.
    In still mode (5MP): grab at ~1fps (still pipeline is slow by nature).
    """
    boundary = b"--frame\r\n"
    while True:
        with cam_lock:
            buf = io.BytesIO()
            picam2.capture_file(buf, format='jpeg')
            frame = buf.getvalue()

        yield (boundary +
               b"Content-Type: image/jpeg\r\n"
               b"Content-Length: " + str(len(frame)).encode() + b"\r\n\r\n" +
               frame + b"\r\n")

        # In still mode add a small delay so we don't hammer the CPU
        if cam_state["mode_id"] == 10:
            time.sleep(0.8)

@app.route("/stream")
def stream():
    return Response(
        generate_mjpeg(),
        mimetype="multipart/x-mixed-replace; boundary=frame"
    )

# ─── Full-res 5MP capture (for plant ID) ─────────────────────────────────────
@app.route("/capture")
def capture():
    """
    Always shoots at 5MP regardless of current stream mode.
    Switches to still mode, captures, switches back.
    """
    with cam_lock:
        current_mode = cam_state["mode_id"]
        current_fps  = cam_state["fps"]
        switched     = current_mode != 10

        if switched:
            print("[CAPTURE] Switching to 5MP still mode...")
            picam2.stop()
            cfg = build_config(10, 1)
            picam2.configure(cfg)
            picam2.start()
            time.sleep(1.5)  # Let AE/AWB settle at new res

        # Shoot
        buf = io.BytesIO()
        picam2.capture_file(buf, format='jpeg')
        jpeg_bytes = buf.getvalue()
        print(f"[CAPTURE] {len(jpeg_bytes)//1024}KB at 2592x1944")

        if switched:
            print(f"[CAPTURE] Switching back to mode {current_mode} @ {current_fps}fps...")
            picam2.stop()
            cfg = build_config(current_mode, current_fps)
            picam2.configure(cfg)
            picam2.start()
            time.sleep(0.8)
            _apply_controls()

    return Response(jpeg_bytes, mimetype="image/jpeg")

# ─── Flash ────────────────────────────────────────────────────────────────────
@app.route("/flash")
def flash():
    val = request.args.get("v", "0")
    try:
        pwm_val = max(0, min(255, int(val)))
        if HAS_GPIO:
            flash_pwm.ChangeDutyCycle((pwm_val / 255) * 100)
    except ValueError:
        pass
    return "OK", 200

# ─── Camera action endpoint ───────────────────────────────────────────────────
@app.route("/action")
def action():
    var = request.args.get("var", "")
    val = request.args.get("val", "0")

    try:
        val_f = float(val)
        val_i = int(float(val))
    except ValueError:
        return "bad value", 400

    with cam_lock:

        # ── Resolution change ──────────────────────────────────────────────────
        if var == "framesize":
            if val_i not in VIDEO_MODES:
                return "unknown mode", 400
            mode     = VIDEO_MODES[val_i]
            # Pick the highest fps for this mode by default
            best_fps = mode[1][0]
            apply_config(val_i, best_fps, settle=1.0)

        # ── Framerate change ───────────────────────────────────────────────────
        elif var == "framerate":
            mode = VIDEO_MODES.get(cam_state["mode_id"])
            if mode and val_i in mode[1]:
                apply_config(cam_state["mode_id"], val_i, settle=0.5)
            else:
                return f"fps {val_i} not supported for this mode", 400

        # ── Image controls ─────────────────────────────────────────────────────
        elif var == "brightness":
            cam_state["brightness"] = val_f / 2.0   # -2..2 → -1.0..1.0
            picam2.set_controls({"Brightness": cam_state["brightness"]})
            time.sleep(0.3)

        elif var == "contrast":
            cam_state["contrast"] = (val_f + 2) / 4.0 * 4.0
            picam2.set_controls({"Contrast": cam_state["contrast"]})
            time.sleep(0.3)

        elif var == "saturation":
            cam_state["saturation"] = (val_f + 2) / 4.0 * 4.0
            picam2.set_controls({"Saturation": cam_state["saturation"]})
            time.sleep(0.3)

        # ── Flip / mirror ──────────────────────────────────────────────────────
        elif var == "vflip":
            cam_state["vflip"] = bool(val_i)
            apply_config(cam_state["mode_id"], cam_state["fps"], settle=0.8)

        elif var == "hmirror":
            cam_state["hmirror"] = bool(val_i)
            apply_config(cam_state["mode_id"], cam_state["fps"], settle=0.8)

        # ── Special effects / night mode ───────────────────────────────────────
        elif var == "special_effect":
            if val_i == 2:    # Grayscale
                picam2.set_controls({"Saturation": 0.0})
            elif val_i == 4:  # Night mode
                picam2.set_controls({
                    "Saturation":   1.5,
                    "Contrast":     1.5,
                    "Brightness":   0.2,
                    "AeEnable":     True,
                    "AnalogueGain": 8.0,
                })
            else:             # Normal — reset everything including gain
                picam2.set_controls({
                    "Saturation":   cam_state["saturation"],
                    "Contrast":     cam_state["contrast"],
                    "Brightness":   cam_state["brightness"],
                    "AeEnable":     True,
                    "AnalogueGain": 1.0,
                })
            # AE/AWB needs time to recalibrate — prevents black frames on toggle
            time.sleep(1.5)

        else:
            return "unknown var", 400

    print(f"[ACTION] {var}={val}")
    return "", 200

# ─── Status ───────────────────────────────────────────────────────────────────
@app.route("/status")
def status():
    mode = VIDEO_MODES[cam_state["mode_id"]]
    return jsonify({
        "status":     "ok",
        "resolution": f"{mode[0][0]}x{mode[0][1]}",
        "fps":        cam_state["fps"],
        "cam":        "OV5647",
        "state":      cam_state
    })

# ─── Push IP to Firebase ──────────────────────────────────────────────────────
def push_ip_to_firebase():
    time.sleep(3)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
        url     = f"{FIREBASE_DB_URL}/settings.json"
        payload = {"camera_ip": f"http://{local_ip}:5000"}
        r = requests.patch(url, json=payload, timeout=10)
        if r.status_code == 200:
            print(f"[Firebase] IP pushed: http://{local_ip}:5000")
        else:
            print(f"[Firebase] Push failed: {r.status_code}")
    except Exception as e:
        print(f"[Firebase] Error: {e}")

threading.Thread(target=push_ip_to_firebase, daemon=True).start()

# ─── Run ──────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print("[SERVER] Plant Monitor Camera Server on port 5000")
    app.run(host="0.0.0.0", port=5000, threaded=True)

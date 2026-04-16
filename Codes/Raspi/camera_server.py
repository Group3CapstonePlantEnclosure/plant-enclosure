#!/usr/bin/env python3
"""
Plant Monitor Camera Server
Raspberry Pi Zero 2W + OV5647 5MP
"""

import io
import time
import threading
import requests
import socket
from flask import Flask, Response, request, jsonify, render_template
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
VIDEO_MODES = {
    10: ((2592, 1944), [5, 2, 1],        "still"),   # 5MP — still only
    9:  ((1920, 1080), [30],              "video"),   # 1080p @ 30fps
    8:  ((1280, 720),  [60, 30],          "video"),   # 720p  @ 60 or 30fps
    6:  ((640,  480),  [90, 60, 30],      "video"),   # 480p  @ 90/60/30fps
}

# ─── Camera state ─────────────────────────────────────────────────────────────
cam_state = {
    "mode_id":    9,
    "fps":        30,
    "brightness": 0.0,
    "contrast":   1.0,
    "saturation": 1.0,
    "vflip":      False,
    "hmirror":    False,
    "is_still":   False,
}

picam2   = Picamera2()
cam_lock = threading.Lock()

# ─── Configure camera ─────────────────────────────────────────────────────────
def build_config(mode_id, fps):
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
        frame_duration = int(1_000_000 / fps)
        cfg = picam2.create_video_configuration(
            main={"size": size, "format": "RGB888"},
            controls={"FrameDurationLimits": (frame_duration, frame_duration)},
            buffer_count=4,
            transform=transform
        )
    return cfg

def apply_config(mode_id, fps, settle=1.0):
    picam2.stop()
    cfg = build_config(mode_id, fps)
    picam2.configure(cfg)
    picam2.start()
    time.sleep(settle)
    _apply_controls()
    cam_state["mode_id"] = mode_id
    cam_state["fps"]     = fps

def _apply_controls():
    picam2.set_controls({
        "Brightness": cam_state["brightness"],
        "Contrast":   cam_state["contrast"],
        "Saturation": cam_state["saturation"],
        "AeEnable":   True,
    })

# Boot into 1080p @ 30fps
cfg = build_config(9, 30)
picam2.configure(cfg)
picam2.start()
time.sleep(2)

# ─── Flask App ────────────────────────────────────────────────────────────────
app = Flask(__name__)

@app.after_request
def add_cors(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    return response

@app.route('/')
def home():
    return render_template('monitor.html')

# ─── MJPEG stream ─────────────────────────────────────────────────────────────
def generate_mjpeg():
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
    with cam_lock:
        current_mode = cam_state["mode_id"]
        current_fps  = cam_state["fps"]
        switched     = current_mode != 10

        if switched:
            picam2.stop()
            cfg = build_config(10, 1)
            picam2.configure(cfg)
            picam2.start()
            time.sleep(1.5)

        buf = io.BytesIO()
        picam2.capture_file(buf, format='jpeg')
        jpeg_bytes = buf.getvalue()

        if switched:
            picam2.stop()
            cfg = build_config(current_mode, current_fps)
            picam2.configure(cfg)
            picam2.start()
            time.sleep(0.8)
            _apply_controls()

    return Response(jpeg_bytes, mimetype="image/jpeg")

# ─── PlantNet Identification (5MP) ────────────────────────────────────────────
@app.route("/identify")
def identify_plant():
    API_KEY = "2b10r2kgy7B3WbTrm7jRs5Jqx"
    
    with cam_lock:
        current_mode = cam_state["mode_id"]
        current_fps  = cam_state["fps"]
        switched     = current_mode != 10

        if switched:
            picam2.stop()
            cfg = build_config(10, 1)
            picam2.configure(cfg)
            picam2.start()
            time.sleep(1.5) 

        buf = io.BytesIO()
        picam2.capture_file(buf, format='jpeg')
        jpeg_bytes = buf.getvalue()

        if switched:
            picam2.stop()
            cfg = build_config(current_mode, current_fps)
            picam2.configure(cfg)
            picam2.start()
            time.sleep(0.8)
            _apply_controls()

    url = f"https://my-api.plantnet.org/v2/identify/all?api-key={API_KEY}&include-related-images=false"
    files = {'images': ('plant.jpg', jpeg_bytes, 'image/jpeg')}
    data = {'organs': 'leaf'}

    try:
        response = requests.post(url, files=files, data=data)
        return Response(response.content, mimetype="application/json", status=response.status_code)
    except Exception as e:
        return jsonify({"error": str(e)}), 500

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
        if var == "framesize":
            if val_i in VIDEO_MODES:
                apply_config(val_i, VIDEO_MODES[val_i][1][0], settle=1.0)
        elif var == "brightness":
            cam_state["brightness"] = val_f / 2.0
            picam2.set_controls({"Brightness": cam_state["brightness"]})
        elif var == "vflip":
            cam_state["vflip"] = bool(val_i)
            apply_config(cam_state["mode_id"], cam_state["fps"], settle=0.8)
        # Add other actions as needed

    return "", 200

# Paste your exact Tailscale URL here
TAILSCALE_FUNNEL_URL = "https://picam.tail43c692.ts.net" 

def push_ip_to_firebase():
    time.sleep(5) 
    try:
        url = f"{FIREBASE_DB_URL}/settings.json"
        # We push the new Funnel URL! 
        payload = {"camera_ip": TAILSCALE_FUNNEL_URL} 

        r = requests.patch(url, json=payload, timeout=10)
        if r.status_code == 200:
            print(f"[Firebase] Public Funnel Pushed: {TAILSCALE_FUNNEL_URL}")
        else:
            print(f"[Firebase] Push failed: {r.status_code}")
    except Exception as e:
        print(f"[ERROR] Firebase sync failed: {e}")
# ─── Main Entry Point ─────────────────────────────────────────────────────────
if __name__ == "__main__":
    # Start the Firebase IP push in the background
    threading.Thread(target=push_ip_to_firebase, daemon=True).start()
    
    print(f"\n[SYSTEM] Server starting on Port 5000...")
    print(f"[SYSTEM] Public Link: {TAILSCALE_FUNNEL_URL}")
    
    # CRITICAL: We use port 5000 because Tailscale is proxying that port!
    app.run(host='0.0.0.0', port=5000, threaded=True, debug=False)

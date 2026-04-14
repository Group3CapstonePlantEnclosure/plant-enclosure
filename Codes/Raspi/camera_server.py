#!/usr/bin/env python3
"""
Plant Monitor Camera Server - HIGH QUALITY MODE
Raspberry Pi Zero 2W + OV5647 5MP

Uses still_configuration at all times for maximum image quality.
Stream is a series of high-res JPEGs (slower but much better quality).
Capture uses full 5MP with a settle delay for plant ID.
"""

import io
import os
import time
import threading
import requests
import socket
from flask import Flask, Response, request, jsonify
from picamera2 import Picamera2

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

# ─── Camera Setup (still config for max quality) ──────────────────────────────
picam2 = Picamera2()

config = picam2.create_still_configuration(
    main={"size": (2592, 1944), "format": "RGB888"},
    buffer_count=2
)
picam2.configure(config)
picam2.start()

print("[CAMERA] Warming up (2s)...")
time.sleep(2)
print("[CAMERA] Ready at 2592x1944 (5MP)")

# Camera state tracking
cam_settings = {
    "brightness": 0.0,
    "contrast":   1.0,
    "saturation": 1.0,
    "vflip":      False,
    "hmirror":    False,
}

# Lock so capture and stream don't collide
cam_lock = threading.Lock()

# ─── Flask App ────────────────────────────────────────────────────────────────
app = Flask(__name__)

@app.after_request
def add_cors(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    return response

# ─── MJPEG Stream (high quality, ~2fps is fine for plant monitoring) ──────────
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

        time.sleep(0.5)  # 2fps — lower number = faster but more CPU

@app.route("/stream")
def stream():
    return Response(
        generate_mjpeg(),
        mimetype="multipart/x-mixed-replace; boundary=frame"
    )

# ─── Full-res capture with settle time (for plant ID) ─────────────────────────
@app.route("/capture")
def capture():
    with cam_lock:
        # Let AE/AWB settle on the scene before grabbing
        time.sleep(1.5)
        buf = io.BytesIO()
        picam2.capture_file(buf, format='jpeg')
        jpeg_bytes = buf.getvalue()

    print(f"[CAPTURE] Shot taken: {len(jpeg_bytes)//1024}KB")
    return Response(jpeg_bytes, mimetype="image/jpeg")

# ─── Flash control ────────────────────────────────────────────────────────────
@app.route("/flash")
def flash():
    val = request.args.get("v", "0")
    try:
        pwm_val = max(0, min(255, int(val)))
        duty = (pwm_val / 255) * 100
        if HAS_GPIO:
            flash_pwm.ChangeDutyCycle(duty)
            print(f"[FLASH] Set to {duty:.1f}%")
    except ValueError:
        pass
    return "OK", 200

# ─── Camera controls ──────────────────────────────────────────────────────────
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
        if var == "brightness":
            mapped = val_f / 2.0          # -2..2 → -1.0..1.0
            cam_settings["brightness"] = mapped
            picam2.set_controls({"Brightness": mapped})
            time.sleep(0.3)  # Let sensor settle after control change

        elif var == "contrast":
            mapped = (val_f + 2) / 4.0 * 4.0   # -2..2 → 0..4
            cam_settings["contrast"] = mapped
            picam2.set_controls({"Contrast": mapped})
            time.sleep(0.3)

        elif var == "saturation":
            mapped = (val_f + 2) / 4.0 * 4.0
            cam_settings["saturation"] = mapped
            picam2.set_controls({"Saturation": mapped})
            time.sleep(0.3)

        elif var == "vflip":
            cam_settings["vflip"] = bool(val_i)
            _apply_transform()

        elif var == "hmirror":
            cam_settings["hmirror"] = bool(val_i)
            _apply_transform()

        elif var == "special_effect":
            if val_i == 2:    # Grayscale
                picam2.set_controls({"Saturation": 0.0})
            elif val_i == 4:  # Night mode — boost exposure, disable auto exposure limit
                picam2.set_controls({
                    "Saturation": 1.5,
                    "Contrast":   1.5,
                    "Brightness": 0.2,
                    "AeEnable":   True,
                    "AnalogueGain": 8.0,   # Push ISO high for low light
                })
            else:             # Normal — fully reset sensor to auto defaults
                picam2.set_controls({
                    "Saturation":   cam_settings["saturation"],
                    "Contrast":     cam_settings["contrast"],
                    "Brightness":   cam_settings["brightness"],
                    "AeEnable":     True,
                    "AnalogueGain": 1.0,   # Reset gain back to normal
                })
            # Critical: give AE/AWB time to recalibrate — prevents black frames
            time.sleep(1.5)

        elif var == "framesize":
            # Map to real OV5647 supported modes
            sizes = {
                10: (2592, 1944),  # 5MP full res
                9:  (1920, 1080),  # 1080p
                8:  (1280, 720),   # 720p
                7:  (800,  600),   # SVGA
                6:  (640,  480),   # VGA 
                5:  (400,  296),   # CIF
                4:  (320,  240),   # QVGA
            }
            size = sizes.get(val_i, (1920, 1080))
            picam2.stop()
            from libcamera import Transform
            cfg = picam2.create_still_configuration(
                main={"size": size, "format": "RGB888"},
                buffer_count=2,
                transform=Transform(
                    vflip=cam_settings["vflip"],
                    hflip=cam_settings["hmirror"]
                )
            )
            picam2.configure(cfg)
            picam2.start()
            time.sleep(1.5)  # Settle after resolution switch
            print(f"[ACTION] Resolution changed to {size}")

        else:
            return "unknown var", 400

    print(f"[ACTION] {var} = {val}")
    return "", 200

def _apply_transform():
    """Apply vflip/hmirror — must be called inside cam_lock."""
    from libcamera import Transform
    picam2.stop()
    cfg = picam2.create_still_configuration(
        main={"size": (2592, 1944), "format": "RGB888"},
        buffer_count=2,
        transform=Transform(vflip=cam_settings["vflip"], hflip=cam_settings["hmirror"])
    )
    picam2.configure(cfg)
    picam2.start()
    time.sleep(1)  # Settle after reconfigure

# ─── Status endpoint ──────────────────────────────────────────────────────────
@app.route("/status")
def status():
    return jsonify({
        "status":     "ok",
        "resolution": "2592x1944 (5MP)",
        "mode":       "still_configuration",
        "cam":        "OV5647",
        "settings":   cam_settings
    })

# ─── Push IP to Firebase on boot ──────────────────────────────────────────────
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
    print("[SERVER] Plant Monitor Camera Server starting on port 5000...")
    print("[SERVER] Mode: Full 5MP still configuration (quality priority)")
    app.run(host="0.0.0.0", port=5000, threaded=True)

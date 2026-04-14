#!/usr/bin/env python3
"""
Plant Monitor Camera Server for Raspberry Pi Zero 2W
OV5647 5MP Camera Module via Picamera2

Endpoints:
  GET /stream   - MJPEG live stream
  GET /capture  - Full-resolution JPEG snapshot
  GET /flash    - ?v=0-255 (PWM on GPIO 18 if LED wired up)
  GET /action   - ?var=brightness|contrast|saturation|vflip|hmirror&val=N
  GET /status   - JSON health check
"""

import io
import os
import time
import json
import threading
import requests
from flask import Flask, Response, request, jsonify

# ─── Picamera2 ────────────────────────────────────────────────────────────────
from picamera2 import Picamera2
from picamera2.encoders import MJPEGEncoder
from picamera2.outputs import FileOutput
from libcamera import controls, Transform

# ─── Optional: GPIO flash LED on pin 18 ──────────────────────────────────────
try:
    import RPi.GPIO as GPIO
    FLASH_PIN = 18
    GPIO.setmode(GPIO.BCM)
    GPIO.setup(FLASH_PIN, GPIO.OUT)
    flash_pwm = GPIO.PWM(FLASH_PIN, 1000)
    flash_pwm.start(0)
    HAS_GPIO = True
except Exception:
    HAS_GPIO = False
    print("[WARN] GPIO not available — flash control disabled")

# ─── Firebase ─────────────────────────────────────────────────────────────────
FIREBASE_DB_URL = "https://plant-enclosure-default-rtdb.firebaseio.com"

# ─── Camera Setup ─────────────────────────────────────────────────────────────
picam2 = Picamera2()

# Stream config: lower res for smooth MJPEG
stream_config = picam2.create_video_configuration(
    main={"size": (1280, 720), "format": "RGB888"},
    lores={"size": (640, 480),  "format": "YUV420"},
)

# Full-res still config: 2592x1944 (5MP max for OV5647)
still_config = picam2.create_still_configuration(
    main={"size": (2592, 1944), "format": "RGB888"},
)

# Start with stream config
picam2.configure(stream_config)

# Camera state
cam_settings = {
    "brightness":     0.0,   # -1.0 to 1.0
    "contrast":       1.0,   # 0.0 to 32.0
    "saturation":     1.0,   # 0.0 to 32.0
    "vflip":          False,
    "hmirror":        False,
}

picam2.start()
time.sleep(1)  # Warm-up

# ─── MJPEG Stream ─────────────────────────────────────────────────────────────
stream_lock  = threading.Lock()
latest_frame = None

def capture_frames():
    """Background thread: continuously grab frames for the MJPEG stream."""
    global latest_frame
    while True:
        with stream_lock:
            buf = io.BytesIO()
            picam2.capture_file(buf, format='jpeg')
            latest_frame = buf.getvalue()
        time.sleep(0.05)  # ~20 fps cap

frame_thread = threading.Thread(target=capture_frames, daemon=True)
frame_thread.start()

def generate_mjpeg():
    boundary = b"--frame\r\n"
    while True:
        with stream_lock:
            frame = latest_frame
        if frame:
            yield (boundary +
                   b"Content-Type: image/jpeg\r\n"
                   b"Content-Length: " + str(len(frame)).encode() + b"\r\n\r\n" +
                   frame + b"\r\n")
        time.sleep(0.05)

# ─── Flask App ────────────────────────────────────────────────────────────────
app = Flask(__name__)

@app.after_request
def add_cors(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    return response

@app.route("/stream")
def stream():
    return Response(
        generate_mjpeg(),
        mimetype="multipart/x-mixed-replace; boundary=frame"
    )

@app.route("/capture")
def capture():
    """
    Switch to full 5MP resolution, grab one frame, switch back.
    This gives you the highest quality image for plant ID.
    """
    global latest_frame

    picam2.stop()
    picam2.configure(still_config)
    picam2.start()
    time.sleep(0.3)  # Let AE/AWB settle

    buf = io.BytesIO()
    picam2.capture_file(buf, format='jpeg')
    jpeg_bytes = buf.getvalue()

    # Switch back to stream config
    picam2.stop()
    picam2.configure(stream_config)
    picam2.start()
    time.sleep(0.2)
    # Restart frame grabber thread isn't needed — it re-grabs automatically

    return Response(jpeg_bytes, mimetype="image/jpeg")

@app.route("/flash")
def flash():
    val = request.args.get("v", "0")
    try:
        pwm_val = max(0, min(255, int(val)))
        duty = (pwm_val / 255) * 100
        if HAS_GPIO:
            flash_pwm.ChangeDutyCycle(duty)
    except ValueError:
        pass
    return "OK", 200

@app.route("/action")
def action():
    var = request.args.get("var", "")
    val = request.args.get("val", "0")

    try:
        val_f = float(val)
        val_i = int(float(val))
    except ValueError:
        return "bad value", 400

    if var == "brightness":
        # ESP32 sends -2..2; map to Picamera2 -1.0..1.0
        mapped = val_f / 2.0
        cam_settings["brightness"] = mapped
        picam2.set_controls({"Brightness": mapped})

    elif var == "contrast":
        # ESP32 sends -2..2; map to 0.0..4.0
        mapped = (val_f + 2) / 4.0 * 4.0
        cam_settings["contrast"] = mapped
        picam2.set_controls({"Contrast": mapped})

    elif var == "saturation":
        mapped = (val_f + 2) / 4.0 * 4.0
        cam_settings["saturation"] = mapped
        picam2.set_controls({"Saturation": mapped})

    elif var == "vflip":
        cam_settings["vflip"] = bool(val_i)
        _apply_transform()

    elif var == "hmirror":
        cam_settings["hmirror"] = bool(val_i)
        _apply_transform()

    elif var == "special_effect":
        # 0=normal, 2=grayscale — remap to saturation shortcut
        if val_i == 2:   # Grayscale
            picam2.set_controls({"Saturation": 0.0})
        elif val_i == 4: # Green tint (night mode workaround)
            picam2.set_controls({"Saturation": 2.0, "Contrast": 2.0})
        else:            # Normal
            picam2.set_controls({"Saturation": cam_settings["saturation"]})

    elif var == "framesize":
        # Map ESP32 framesize IDs to Pi resolutions
        sizes = {
            4:  (320,  240),   # QVGA
            5:  (400,  296),   # CIF
            6:  (640,  480),   # VGA
            7:  (800,  600),   # SVGA
            8:  (1024, 768),   # XGA
            9:  (1280, 720),   # 720p (replaces SXGA)
            10: (1280, 960),   # Near-UXGA
        }
        size = sizes.get(val_i, (640, 480))
        picam2.stop()
        cfg = picam2.create_video_configuration(main={"size": size, "format": "RGB888"})
        picam2.configure(cfg)
        picam2.start()
        time.sleep(0.3)
    else:
        return "unknown var", 400

    return "", 200

def _apply_transform():
    t = Transform(vflip=cam_settings["vflip"], hflip=cam_settings["hmirror"])
    picam2.stop()
    # Re-apply current stream config with new transform
    cfg = picam2.create_video_configuration(
        main={"size": (1280, 720), "format": "RGB888"},
        transform=t
    )
    picam2.configure(cfg)
    picam2.start()

@app.route("/status")
def status():
    import socket
    hostname = socket.gethostname()
    ip = socket.gethostbyname(hostname)
    return jsonify({"status": "ok", "ip": ip, "cam": "OV5647"})

# ─── Push IP to Firebase ──────────────────────────────────────────────────────
def push_ip_to_firebase():
    import socket
    time.sleep(2)  # Wait for network
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()

        url = f"{FIREBASE_DB_URL}/settings.json"
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
    app.run(host="0.0.0.0", port=5000, threaded=True)

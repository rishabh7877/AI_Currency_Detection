import os
import io
import time
import threading
import logging
from collections import deque

import numpy as np
import cv2
import torch
from flask import Flask, request
from ultralytics import YOLO

# ================= CONFIG =================
MODEL_PATH   = "best.pt"
IMGSZ        = 320
CONF         = 0.8
HOST         = "0.0.0.0"
PORT         = 5000
# Use all physical cores for inference; tweak if you share the machine
NUM_THREADS  = min(os.cpu_count() or 4, 8)

# ================= LOGGING =================
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("yolo-server")

# ================= TORCH =================
torch.set_num_threads(NUM_THREADS)
torch.set_grad_enabled(False)   # no gradients needed at inference — saves memory & time
log.info(f"PyTorch threads: {NUM_THREADS}")

# ================= MODEL =================
log.info(f"Loading model: {MODEL_PATH}")
model = YOLO(MODEL_PATH)
model.fuse()   # fuse Conv+BN layers → faster inference

# Force model to eval mode (YOLO does this internally but be explicit)
if hasattr(model, "model"):
    model.model.eval()

# --- Warm-up: run a few dummy frames so JIT/kernels are compiled before first real request ---
log.info("Warming up model...")
_dummy = np.zeros((320, 320, 3), dtype=np.uint8)
for _ in range(3):
    model(_dummy, imgsz=IMGSZ, verbose=False)
log.info("Warm-up done ✓")

# ================= FPS TRACKER =================
_fps_lock   = threading.Lock()
_fps_times  = deque(maxlen=30)   # rolling window of last 30 frame timestamps

def record_frame():
    with _fps_lock:
        _fps_times.append(time.monotonic())

def get_fps() -> float:
    with _fps_lock:
        if len(_fps_times) < 2:
            return 0.0
        return (len(_fps_times) - 1) / (_fps_times[-1] - _fps_times[0])

# ================= INFERENCE LOCK =================
# Single lock ensures frames queue rather than thrash CPU with parallel inference.
# For multi-cam setups, replace with a ThreadPoolExecutor(max_workers=N).
_infer_lock = threading.Lock()

# ================= FLASK APP =================
app = Flask(__name__)

# Disable Flask's default request logging noise (we log ourselves)
logging.getLogger("werkzeug").setLevel(logging.WARNING)


@app.route("/detect", methods=["POST"])
def detect():
    t0 = time.monotonic()

    raw = request.data
    if not raw:
        return "No data", 400

    # --- Decode JPEG bytes to numpy array ---
    img_np = np.frombuffer(raw, np.uint8)
    img    = cv2.imdecode(img_np, cv2.IMREAD_COLOR)

    if img is None:
        log.warning("Failed to decode image")
        return "Invalid image", 400

    # --- Run inference (serialized to avoid CPU thrashing) ---
    with _infer_lock:
        results = model(img, imgsz=IMGSZ, conf=CONF, verbose=False)

    r = results[0]

    if r.boxes is None or len(r.boxes) == 0:
        label = "none"
    else:
        # Pick box with highest confidence
        best   = r.boxes[r.boxes.conf.argmax()]
        cls_id = int(best.cls[0])
        conf   = float(best.conf[0])
        label  = model.names[cls_id]
        log.info(f"Detected: {label} ({conf:.2f}) | "
                 f"img={img.shape[1]}x{img.shape[0]} "
                 f"payload={len(raw)/1024:.1f}KB "
                 f"latency={( time.monotonic()-t0)*1000:.0f}ms "
                 f"FPS={get_fps():.1f}")

    record_frame()
    return label, 200


@app.route("/stats", methods=["GET"])
def stats():
    """Quick health-check endpoint — hit from browser to confirm server is running."""
    return {
        "fps":     round(get_fps(), 2),
        "model":   MODEL_PATH,
        "threads": NUM_THREADS,
        "imgsz":   IMGSZ,
        "conf":    CONF,
    }, 200


# ================= MAIN =================
if __name__ == "__main__":
    log.info(f"Server starting on {HOST}:{PORT}")
    app.run(host=HOST, port=PORT, threaded=True, use_reloader=False)

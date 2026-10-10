"""OpenCV image-quality gate. Runs BEFORE the AI model so blurry/dark photos are rejected."""
import cv2
import numpy as np


def decode(data: bytes):
    img = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        raise ValueError("Valid image file illa (JPG/PNG upload pannunga)")
    return img


def quality(img) -> dict:
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    h, w = g.shape
    sharp = float(cv2.Laplacian(g, cv2.CV_64F).var())
    bright = float(g.mean())
    notes, ok = [], True
    if min(h, w) < 200:
        notes.append("Resolution romba kammi (min 200px)"); ok = False
    if sharp < 25:
        notes.append("Photo blur ah irukku, steady ah thirumba edunga"); ok = False
    if bright < 30:
        notes.append("Photo romba dark, light la edunga"); ok = False
    elif bright > 245:
        notes.append("Photo overexposed"); ok = False
    return {"ok": ok, "notes": notes, "width": w, "height": h,
            "sharpness": round(sharp, 1), "brightness": round(bright, 1)}

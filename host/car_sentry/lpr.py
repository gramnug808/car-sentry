"""LPR from a USB webcam — capture + on-device OCR.

Capture: OpenCV (cv2) if installed, else a v4l2/ffmpeg attempt,
else a stub that reports "no camera backend" — never pretends
to have a frame. OCR: Tesseract (`tesseract` binary) if present,
else a stub interface so the plate-repair pipeline is testable
without any hardware. OCR output always goes through plates.py
repair, so the same ABC123 / 123ABC / custom rules apply.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass

from .plates import PlateResult, repair_plate


@dataclass
class Frame:
    path: str            # saved frame path (or "" for stub)
    backend: str         # "opencv" | "ffmpeg-v4l2" | "stub"
    ok: bool = True
    note: str = ""


class OcrEngine:
    """Interface: image path -> raw OCR text (may be empty)."""
    def ocr_text(self, image_path: str) -> str:
        raise NotImplementedError


class TesseractOcr(OcrEngine):
    def __init__(self, psm: str = "7"):
        self.psm = psm

    def ocr_text(self, image_path: str) -> str:
        if not shutil.which("tesseract"):
            return ""
        try:
            r = subprocess.run(["tesseract", image_path, "stdout",
                                "--psm", self.psm, "-c",
                                "tessedit_char_whitelist=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"],
                               capture_output=True, text=True, timeout=30)
            return r.stdout or ""
        except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
            return ""


class StubOcr(OcrEngine):
    """Test / demo OCR: returns a preset string regardless of image."""
    def __init__(self, preset: str = ""):
        self.preset = preset

    def ocr_text(self, image_path: str) -> str:
        return self.preset


def default_ocr() -> OcrEngine:
    return TesseractOcr() if shutil.which("tesseract") else StubOcr("")


def camera_available() -> tuple[bool, str]:
    if os.path.exists("/dev/video0"):
        return True, "/dev/video0 present"
    try:
        import cv2  # noqa: F401
        return False, "cv2 installed but no /dev/video0 found"
    except ImportError:
        return False, "no /dev/video0 and no OpenCV (cv2) installed"


def capture_frame(out_path: str, camera_index: int = 0) -> Frame:
    """Grab one frame from the USB webcam. Returns Frame(ok=False) if none."""
    try:
        import cv2
        cap = cv2.VideoCapture(camera_index)
        ok, frame = cap.read()
        cap.release()
        if not ok or frame is None:
            return Frame(out_path, "opencv", False, "cv2 opened camera but read no frame")
        cv2.imwrite(out_path, frame)
        return Frame(out_path, "opencv", True, "captured via OpenCV")
    except ImportError:
        pass
    if os.path.exists("/dev/video0") and shutil.which("ffmpeg"):
        try:
            r = subprocess.run(["ffmpeg", "-y", "-f", "v4l2", "-i", "/dev/video0",
                                "-frames:v", "1", out_path],
                               capture_output=True, text=True, timeout=15)
            if r.returncode == 0 and os.path.exists(out_path):
                return Frame(out_path, "ffmpeg-v4l2", True, "captured via ffmpeg/v4l2")
        except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
            pass
    return Frame("", "stub", False, "no camera backend available (no cv2, no /dev/video0+ffmpeg)")


def read_plate_from_image(image_path: str, ocr: OcrEngine | None = None) -> PlateResult | None:
    """OCR an image file, then repair the raw text into a plate."""
    engine = ocr or default_ocr()
    raw = engine.ocr_text(image_path)
    if not raw.strip():
        return None
    return repair_plate(raw)

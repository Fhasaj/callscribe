"""Wraps faster-whisper with device/model autodetect and a CUDA -> CPU
fallback, since this app runs both on a CUDA-capable desktop and a
CPU-only laptop. Also hosts the QThread worker that drains the audio
capture queue and emits live transcript text."""
from __future__ import annotations

import logging
from typing import Optional

import numpy as np
from faster_whisper import WhisperModel
from PySide6.QtCore import QThread, Signal

logger = logging.getLogger(__name__)


def cuda_available() -> bool:
    try:
        import ctranslate2

        return ctranslate2.get_cuda_device_count() > 0
    except Exception:
        return False


def suggest_model_size() -> str:
    return "medium" if cuda_available() else "small"


class WhisperEngine:
    """Loads a faster-whisper model, preferring CUDA and falling back
    to CPU if CUDA is unavailable or the load fails (e.g. missing
    system cuDNN)."""

    def __init__(self, model_size: str = "", device: str = "auto"):
        self.model_size = model_size or suggest_model_size()
        self.device, self.compute_type = self._resolve_device(device)
        self.model = self._load_model()
        self._detected_language: Optional[str] = None

    def _resolve_device(self, requested: str) -> tuple[str, str]:
        if requested == "cpu":
            return "cpu", "int8"
        if requested == "cuda":
            return "cuda", "float16"
        return ("cuda", "float16") if cuda_available() else ("cpu", "int8")

    def _load_model(self) -> WhisperModel:
        try:
            return WhisperModel(self.model_size, device=self.device, compute_type=self.compute_type)
        except Exception:
            if self.device != "cpu":
                logger.warning("Falling back to CPU for Whisper (%s load failed)", self.device, exc_info=True)
                self.device, self.compute_type = "cpu", "int8"
                return WhisperModel(self.model_size, device="cpu", compute_type="int8")
            raise

    def transcribe(self, audio: np.ndarray) -> str:
        if audio.size == 0:
            return ""
        language = self._detected_language
        segments, info = self.model.transcribe(audio, language=language, vad_filter=True)
        text = " ".join(seg.text.strip() for seg in segments).strip()
        if self._detected_language is None and info.language:
            self._detected_language = info.language
        return text


class TranscriptionWorker(QThread):
    """Consumes audio chunks from a CallRecorder's chunk_queue and emits
    transcribed text incrementally. `chunk_transcribed` fires per chunk
    (for a live-updating transcript view); `finished_all` fires once the
    queue's sentinel (None) is drained."""

    chunk_transcribed = Signal(str)
    error = Signal(str)
    finished_all = Signal()

    def __init__(self, engine: WhisperEngine, chunk_queue):
        super().__init__()
        self._engine = engine
        self._queue = chunk_queue

    def run(self) -> None:
        try:
            while True:
                chunk = self._queue.get()
                if chunk is None:
                    break
                text = self._engine.transcribe(chunk)
                if text:
                    self.chunk_transcribed.emit(text)
        except Exception as exc:  # surface to UI instead of dying silently
            logger.exception("Transcription worker failed")
            self.error.emit(str(exc))
        finally:
            self.finished_all.emit()

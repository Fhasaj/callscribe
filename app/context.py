"""Shared services (settings, DB, Whisper engine) handed to every tab."""
from __future__ import annotations

from typing import Optional

from PySide6.QtCore import QObject, QThread, Signal

from app import config
from app.db import database
from app.db.repository import Repository
from app.transcription.whisper_engine import WhisperEngine


class _WhisperLoader(QThread):
    loaded = Signal()
    failed = Signal(str)

    def __init__(self, model_size: str, device: str):
        super().__init__()
        self.model_size = model_size
        self.device = device
        self.engine: Optional[WhisperEngine] = None

    def run(self) -> None:
        try:
            self.engine = WhisperEngine(self.model_size, self.device)
            self.loaded.emit()
        except Exception as exc:
            self.failed.emit(str(exc))


class AppContext(QObject):
    whisper_ready = Signal()
    whisper_failed = Signal(str)

    def __init__(self):
        super().__init__()
        config.ensure_data_dirs()
        self.settings = config.load_settings()
        self.conn = database.connect(self.settings.db_path)
        self.repo = Repository(self.conn)
        self.whisper_engine: Optional[WhisperEngine] = None
        self._loader: Optional[_WhisperLoader] = None

    def preload_whisper(self) -> None:
        if self.whisper_engine is not None or self._loader is not None:
            return
        self._loader = _WhisperLoader(self.settings.whisper_model_size, self.settings.whisper_device)
        self._loader.loaded.connect(self._on_loaded)
        self._loader.failed.connect(self._on_failed)
        self._loader.start()

    def reload_whisper(self) -> None:
        self.whisper_engine = None
        self.preload_whisper()

    def _on_loaded(self) -> None:
        self.whisper_engine = self._loader.engine
        self._loader = None
        self.whisper_ready.emit()

    def _on_failed(self, message: str) -> None:
        self._loader = None
        self.whisper_failed.emit(message)

    def save_settings(self) -> None:
        config.save_settings(self.settings)

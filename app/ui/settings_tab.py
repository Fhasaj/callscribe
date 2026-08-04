"""Settings tab: Ollama model picker, Whisper size/device, audio
sources, DB path, and daily reminder config -- all per-machine."""
from __future__ import annotations

from PySide6.QtCore import QThread, Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QWidget,
)

from app.audio import devices
from app.config import RECOMMENDED_OLLAMA_MODEL
from app.llm import ollama_client
from app.transcription.whisper_engine import cuda_available, suggest_model_size


class _PullWorker(QThread):
    progress = Signal(str)
    done = Signal()
    failed = Signal(str)

    def __init__(self, host: str, model: str):
        super().__init__()
        self._host = host
        self._model = model

    def run(self) -> None:
        try:
            ollama_client.pull_model(self._host, self._model, on_progress=self.progress.emit)
            self.done.emit()
        except ollama_client.OllamaError as exc:
            self.failed.emit(str(exc))


class SettingsTab(QWidget):
    settings_saved = Signal()

    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self._pull_worker: _PullWorker | None = None

        layout = QFormLayout(self)

        model_row = QHBoxLayout()
        self.model_combo = QComboBox()
        self.model_combo.setEditable(True)
        model_row.addWidget(self.model_combo, 1)
        refresh_models_btn = QPushButton("Refresh")
        refresh_models_btn.clicked.connect(self._refresh_models)
        model_row.addWidget(refresh_models_btn)
        self.pull_btn = QPushButton(f"Pull {RECOMMENDED_OLLAMA_MODEL}")
        self.pull_btn.clicked.connect(self._pull_recommended)
        model_row.addWidget(self.pull_btn)
        layout.addRow("Ollama model:", model_row)
        self.ollama_status_label = QLabel("")
        self.ollama_status_label.setWordWrap(True)
        layout.addRow("", self.ollama_status_label)

        self.whisper_size_combo = QComboBox()
        self.whisper_size_combo.addItems(["small", "base", "medium", "large-v3"])
        layout.addRow("Whisper model size:", self.whisper_size_combo)

        self.whisper_device_combo = QComboBox()
        self.whisper_device_combo.addItems(["auto", "cuda", "cpu"])
        layout.addRow("Whisper device:", self.whisper_device_combo)

        detected = "CUDA detected" if cuda_available() else "No CUDA detected on this machine (will use CPU)"
        self.whisper_detected_label = QLabel(detected)
        layout.addRow("Detected:", self.whisper_detected_label)

        audio_row = QHBoxLayout()
        self.mic_combo = QComboBox()
        audio_row.addWidget(QLabel("Mic:"))
        audio_row.addWidget(self.mic_combo, 1)
        layout.addRow("Microphone source:", audio_row)

        monitor_row = QHBoxLayout()
        self.monitor_combo = QComboBox()
        monitor_row.addWidget(QLabel("Loopback:"))
        monitor_row.addWidget(self.monitor_combo, 1)
        layout.addRow("Speaker/loopback source:", monitor_row)

        refresh_devices_btn = QPushButton("Refresh audio devices")
        refresh_devices_btn.clicked.connect(self._refresh_audio_devices)
        layout.addRow("", refresh_devices_btn)

        db_row = QHBoxLayout()
        self.db_path_edit = QLineEdit()
        db_row.addWidget(self.db_path_edit)
        browse_btn = QPushButton("Browse...")
        browse_btn.clicked.connect(self._browse_db)
        db_row.addWidget(browse_btn)
        layout.addRow("Database file:", db_row)

        self.reminder_enabled_check = QCheckBox("Enabled")
        layout.addRow("Daily task reminder:", self.reminder_enabled_check)
        self.reminder_time_edit = QLineEdit()
        self.reminder_time_edit.setPlaceholderText("HH:MM")
        layout.addRow("Reminder time:", self.reminder_time_edit)

        self.tray_check = QCheckBox("Minimize to tray instead of quitting on close")
        layout.addRow("", self.tray_check)

        save_btn = QPushButton("Save Settings")
        save_btn.clicked.connect(self._save)
        layout.addRow("", save_btn)

        self._load_from_settings()
        self._refresh_models()
        self._refresh_audio_devices()

    def _load_from_settings(self) -> None:
        s = self.ctx.settings
        self.whisper_size_combo.setCurrentText(s.whisper_model_size or suggest_model_size())
        self.whisper_device_combo.setCurrentText(s.whisper_device)
        self.db_path_edit.setText(s.db_path)
        self.reminder_enabled_check.setChecked(s.reminder_enabled)
        self.reminder_time_edit.setText(s.reminder_time)
        self.tray_check.setChecked(s.minimize_to_tray)

    def _refresh_models(self) -> None:
        self.model_combo.clear()
        try:
            models = ollama_client.list_models(self.ctx.settings.ollama_host)
        except ollama_client.OllamaError as exc:
            self.ollama_status_label.setText(f"Ollama not reachable: {exc}")
            return
        if not models:
            self.ollama_status_label.setText("No models installed yet. Use 'Pull recommended model'.")
        else:
            self.ollama_status_label.setText(f"{len(models)} model(s) installed.")
        self.model_combo.addItems(models)
        if self.ctx.settings.ollama_model in models:
            self.model_combo.setCurrentText(self.ctx.settings.ollama_model)
        elif models:
            self.model_combo.setCurrentText(models[0])

    def _pull_recommended(self) -> None:
        self.pull_btn.setEnabled(False)
        self.ollama_status_label.setText(f"Pulling {RECOMMENDED_OLLAMA_MODEL}...")
        self._pull_worker = _PullWorker(self.ctx.settings.ollama_host, RECOMMENDED_OLLAMA_MODEL)
        self._pull_worker.progress.connect(self.ollama_status_label.setText)
        self._pull_worker.done.connect(self._on_pull_done)
        self._pull_worker.failed.connect(self._on_pull_failed)
        self._pull_worker.start()

    def _on_pull_done(self) -> None:
        self.ollama_status_label.setText("Pull complete.")
        self.pull_btn.setEnabled(True)
        self._refresh_models()

    def _on_pull_failed(self, message: str) -> None:
        self.ollama_status_label.setText(f"Pull failed: {message}")
        self.pull_btn.setEnabled(True)

    def _refresh_audio_devices(self) -> None:
        self.mic_combo.clear()
        self.monitor_combo.clear()
        for source in devices.list_mic_sources():
            self.mic_combo.addItem(source.description, source.name)
        for source in devices.list_monitor_sources():
            self.monitor_combo.addItem(source.description, source.name)
        self._select_combo_by_data(self.mic_combo, self.ctx.settings.mic_source)
        self._select_combo_by_data(self.monitor_combo, self.ctx.settings.monitor_source)

    @staticmethod
    def _select_combo_by_data(combo: QComboBox, value: str) -> None:
        if not value:
            return
        idx = combo.findData(value)
        if idx >= 0:
            combo.setCurrentIndex(idx)

    def _browse_db(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "Choose database file", self.db_path_edit.text(), "SQLite DB (*.db)"
        )
        if path:
            self.db_path_edit.setText(path)

    def _save(self) -> None:
        s = self.ctx.settings
        s.ollama_model = self.model_combo.currentText().strip()
        s.whisper_model_size = self.whisper_size_combo.currentText()
        s.whisper_device = self.whisper_device_combo.currentText()
        s.mic_source = self.mic_combo.currentData() or ""
        s.monitor_source = self.monitor_combo.currentData() or ""
        new_db_path = self.db_path_edit.text().strip()
        db_path_changed = new_db_path != s.db_path
        s.db_path = new_db_path
        s.reminder_enabled = self.reminder_enabled_check.isChecked()
        s.reminder_time = self.reminder_time_edit.text().strip() or "09:00"
        s.minimize_to_tray = self.tray_check.isChecked()
        self.ctx.save_settings()
        self.ctx.reload_whisper()
        note = " Restart CallScribe for the new database path to take effect." if db_path_changed else ""
        QMessageBox.information(self, "Saved", "Settings saved." + note)
        self.settings_saved.emit()

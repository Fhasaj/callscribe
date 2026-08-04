"""Record tab: start/stop a call, live transcript, then summary/email
result panel with copy-to-clipboard buttons."""
from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

from PySide6.QtCore import QTimer, Signal
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from app import config
from app.audio.capture import CallRecorder
from app.llm.summarizer import CallSummary, SummarizeWorker
from app.transcription.whisper_engine import TranscriptionWorker


class RecordTab(QWidget):
    call_finished = Signal()
    recording_state_changed = Signal(bool)

    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self.state = "idle"  # idle | recording | processing
        self.recorder: CallRecorder | None = None
        self.transcription_worker: TranscriptionWorker | None = None
        self.summarize_worker: SummarizeWorker | None = None
        self.call_id: int | None = None
        self.customer_id: int | None = None
        self.elapsed_seconds = 0
        self._pending_duration: int | None = None
        self._pending_wav_path: str | None = None

        self.elapsed_timer = QTimer(self)
        self.elapsed_timer.timeout.connect(self._tick_elapsed)

        self._build_ui()
        self.ctx.whisper_ready.connect(self._on_whisper_ready)
        self.ctx.whisper_failed.connect(self._on_whisper_failed)
        self._refresh_customers()
        self._set_state("idle")

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        form = QHBoxLayout()
        form.addWidget(QLabel("Customer:"))
        self.customer_combo = QComboBox()
        self.customer_combo.setEditable(True)
        form.addWidget(self.customer_combo, 1)
        self.start_btn = QPushButton("Start Recording")
        self.start_btn.clicked.connect(self._toggle_recording)
        form.addWidget(self.start_btn)
        layout.addLayout(form)

        status_row = QHBoxLayout()
        self.status_label = QLabel("Loading Whisper model...")
        self.elapsed_label = QLabel("00:00")
        status_row.addWidget(self.status_label)
        status_row.addStretch()
        status_row.addWidget(self.elapsed_label)
        layout.addLayout(status_row)

        layout.addWidget(QLabel("Live transcript:"))
        self.transcript_view = QTextEdit()
        self.transcript_view.setReadOnly(True)
        layout.addWidget(self.transcript_view, 2)

        result_box = QGroupBox("Call notes (editable before copying)")
        result_layout = QVBoxLayout(result_box)
        result_layout.addWidget(QLabel("Summary:"))
        self.summary_edit = QTextEdit()
        result_layout.addWidget(self.summary_edit)
        copy_summary_btn = QPushButton("Copy Summary")
        copy_summary_btn.clicked.connect(lambda: self._copy(self.summary_edit))
        result_layout.addWidget(copy_summary_btn)

        result_layout.addWidget(QLabel("Customer email draft:"))
        self.email_edit = QTextEdit()
        result_layout.addWidget(self.email_edit)
        copy_email_btn = QPushButton("Copy Email")
        copy_email_btn.clicked.connect(lambda: self._copy(self.email_edit))
        result_layout.addWidget(copy_email_btn)
        layout.addWidget(result_box, 2)

    # -- customer list ----------------------------------------------

    def _refresh_customers(self) -> None:
        current = self.customer_combo.currentText()
        self.customer_combo.blockSignals(True)
        self.customer_combo.clear()
        for row in self.ctx.repo.list_customers():
            self.customer_combo.addItem(row["name"])
        self.customer_combo.setCurrentText(current)
        self.customer_combo.blockSignals(False)

    # -- whisper readiness --------------------------------------------

    def _on_whisper_ready(self) -> None:
        if self.state == "idle":
            self.status_label.setText("Idle")

    def _on_whisper_failed(self, message: str) -> None:
        self.status_label.setText(f"Whisper failed to load: {message}")

    # -- recording lifecycle --------------------------------------------

    def _toggle_recording(self) -> None:
        if self.state == "recording":
            self._stop_recording()
        elif self.state == "idle":
            self._start_recording()

    def _start_recording(self) -> None:
        if self.ctx.whisper_engine is None:
            QMessageBox.information(
                self, "Model loading", "The Whisper model is still loading -- try again in a few seconds."
            )
            self.ctx.preload_whisper()
            return
        customer_name = self.customer_combo.currentText().strip()
        if not customer_name:
            QMessageBox.warning(self, "Customer required", "Enter or select a customer name before starting.")
            return

        self.customer_id = self.ctx.repo.get_or_create_customer(customer_name)
        self.call_id = self.ctx.repo.create_call(self.customer_id)
        self.transcript_view.clear()
        self.summary_edit.clear()
        self.email_edit.clear()

        self.recorder = CallRecorder(self.ctx.settings.mic_source, self.ctx.settings.monitor_source)
        try:
            self.recorder.start()
        except Exception as exc:
            QMessageBox.critical(self, "Recording failed", f"Could not start audio capture:\n{exc}")
            self.recorder = None
            return

        self.transcription_worker = TranscriptionWorker(self.ctx.whisper_engine, self.recorder.chunk_queue)
        self.transcription_worker.chunk_transcribed.connect(self._on_chunk_transcribed)
        self.transcription_worker.error.connect(self._on_transcription_error)
        self.transcription_worker.finished_all.connect(self._on_transcription_finished)
        self.transcription_worker.start()

        self.elapsed_seconds = 0
        self.elapsed_label.setText("00:00")
        self.elapsed_timer.start(1000)
        self._set_state("recording")
        self.recording_state_changed.emit(True)

    def _tick_elapsed(self) -> None:
        self.elapsed_seconds += 1
        m, s = divmod(self.elapsed_seconds, 60)
        self.elapsed_label.setText(f"{m:02d}:{s:02d}")

    def _on_chunk_transcribed(self, text: str) -> None:
        self.ctx.repo.append_transcript(self.call_id, text)
        self.transcript_view.append(text)

    def _on_transcription_error(self, message: str) -> None:
        QMessageBox.warning(self, "Transcription error", message)

    def _stop_recording(self) -> None:
        self.elapsed_timer.stop()
        recordings_dir = Path(config.RECORDINGS_DIR)
        audio_path = recordings_dir / f"call_{self.call_id}.wav"
        duration, wav_path = self.recorder.stop(save_wav_path=audio_path)
        self.recorder = None
        self._pending_duration = duration
        self._pending_wav_path = wav_path
        self._set_state("processing")
        self.recording_state_changed.emit(False)

    def _on_transcription_finished(self) -> None:
        if self._pending_duration is None:
            return  # finished_all can also fire after an error before Stop was pressed
        duration, wav_path = self._pending_duration, self._pending_wav_path
        self._pending_duration = None
        self._pending_wav_path = None
        self._finish_processing(duration, wav_path)

    def _finish_processing(self, duration: int, wav_path: str | None) -> None:
        self.ctx.repo.finish_call(self.call_id, duration, wav_path, status="summarizing")
        call = self.ctx.repo.get_call(self.call_id)
        transcript = call["transcript"] or ""
        model = self.ctx.settings.ollama_model
        if not model:
            QMessageBox.warning(self, "No model selected", "Pick an Ollama model in Settings before generating notes.")
            self.ctx.repo.set_call_status(self.call_id, "error")
            self._set_state("idle")
            self.call_finished.emit()
            return

        self.summarize_worker = SummarizeWorker(
            self.ctx.settings.ollama_host,
            model,
            transcript,
            self.customer_combo.currentText().strip(),
            dt.date.today().isoformat(),
        )
        self.summarize_worker.succeeded.connect(self._on_summary_ready)
        self.summarize_worker.failed.connect(self._on_summary_failed)
        self.summarize_worker.start()

    def _on_summary_ready(self, result: CallSummary) -> None:
        self.ctx.repo.add_note(self.call_id, result.summary_text, result.email_draft, json.dumps(result.key_points))
        for task_desc in result.tasks:
            self.ctx.repo.add_task(task_desc, call_id=self.call_id, customer_id=self.customer_id, source="call")
        self.ctx.repo.set_call_status(self.call_id, "done")
        self.summary_edit.setPlainText(result.summary_text)
        self.email_edit.setPlainText(result.email_draft)
        self._set_state("idle")
        self._refresh_customers()
        self.call_finished.emit()

    def _on_summary_failed(self, message: str) -> None:
        self.ctx.repo.set_call_status(self.call_id, "error")
        QMessageBox.warning(self, "Summarization failed", message)
        self._set_state("idle")
        self.call_finished.emit()

    # -- misc -------------------------------------------------------

    def _set_state(self, state: str) -> None:
        self.state = state
        labels = {"idle": "Idle", "recording": "Recording...", "processing": "Transcribing & summarizing..."}
        self.status_label.setText(labels[state])
        self.start_btn.setText("Stop Recording" if state == "recording" else "Start Recording")
        self.start_btn.setEnabled(state != "processing")
        self.customer_combo.setEnabled(state == "idle")

    @staticmethod
    def _copy(edit: QTextEdit) -> None:
        QApplication.clipboard().setText(edit.toPlainText())

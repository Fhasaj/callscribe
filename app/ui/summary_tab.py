"""Yearly Summary tab: computed stats plus an optional LLM-written
prose recap of the year, grounded in the year's call summaries."""
from __future__ import annotations

import datetime as dt

from PySide6.QtWidgets import (
    QApplication,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from app.llm.summarizer import NarrativeWorker


class SummaryTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self._stats: dict | None = None
        self._worker: NarrativeWorker | None = None

        layout = QVBoxLayout(self)

        row = QHBoxLayout()
        row.addWidget(QLabel("Year:"))
        self.year_spin = QSpinBox()
        self.year_spin.setRange(2000, 2100)
        self.year_spin.setValue(dt.date.today().year)
        row.addWidget(self.year_spin)
        load_btn = QPushButton("Load stats")
        load_btn.clicked.connect(self.refresh)
        row.addWidget(load_btn)
        row.addStretch()
        layout.addLayout(row)

        self.stats_label = QLabel("")
        self.stats_label.setWordWrap(True)
        layout.addWidget(self.stats_label)

        self.narrative_btn = QPushButton("Generate narrative recap (uses local LLM)")
        self.narrative_btn.clicked.connect(self._generate_narrative)
        layout.addWidget(self.narrative_btn)

        self.narrative_view = QTextEdit()
        layout.addWidget(self.narrative_view, 1)
        copy_btn = QPushButton("Copy recap")
        copy_btn.clicked.connect(lambda: QApplication.clipboard().setText(self.narrative_view.toPlainText()))
        layout.addWidget(copy_btn)

        self.refresh()

    def refresh(self) -> None:
        year = self.year_spin.value()
        self._stats = self.ctx.repo.yearly_stats(year)
        hours = round(self._stats["total_seconds"] / 3600, 1)
        top = ", ".join(f"{c['name']} ({c['n']})" for c in self._stats["top_customers"][:5]) or "n/a"
        self.stats_label.setText(
            f"Calls: {self._stats['call_count']}    Total call time: {hours}h    "
            f"Tasks completed: {self._stats['tasks_completed']}\nTop customers: {top}"
        )

    def _generate_narrative(self) -> None:
        if not self._stats:
            return
        model = self.ctx.settings.ollama_model
        if not model:
            QMessageBox.warning(self, "No model selected", "Pick an Ollama model in Settings first.")
            return
        if not self._stats["summaries"]:
            QMessageBox.information(self, "Nothing to summarize", "No call notes recorded for this year yet.")
            return
        year = self.year_spin.value()
        self.narrative_btn.setEnabled(False)
        self.narrative_view.setPlainText("Generating...")
        self._worker = NarrativeWorker(
            self.ctx.settings.ollama_host, model, year, self._stats["summaries"], self._stats
        )
        self._worker.succeeded.connect(self._on_narrative_ready)
        self._worker.failed.connect(self._on_narrative_failed)
        self._worker.start()

    def _on_narrative_ready(self, text: str) -> None:
        self.narrative_view.setPlainText(text)
        self.narrative_btn.setEnabled(True)

    def _on_narrative_failed(self, message: str) -> None:
        QMessageBox.warning(self, "Failed", message)
        self.narrative_btn.setEnabled(True)

"""History tab: browse past calls, view transcript/notes/email, and
re-generate a summary if needed."""
from __future__ import annotations

import json

from PySide6.QtWidgets import (
    QApplication,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QMessageBox,
    QPushButton,
    QTabWidget,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from app.llm.summarizer import SummarizeWorker


class HistoryTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self._calls: list = []
        self._regen_worker: SummarizeWorker | None = None

        layout = QHBoxLayout(self)

        left = QVBoxLayout()
        filter_row = QHBoxLayout()
        self.search_edit = QLineEdit()
        self.search_edit.setPlaceholderText("Search transcript or customer...")
        self.search_edit.returnPressed.connect(self.refresh)
        filter_row.addWidget(self.search_edit)
        refresh_btn = QPushButton("Search")
        refresh_btn.clicked.connect(self.refresh)
        filter_row.addWidget(refresh_btn)
        left.addLayout(filter_row)
        self.call_list = QListWidget()
        self.call_list.currentRowChanged.connect(self._on_select)
        left.addWidget(self.call_list)
        layout.addLayout(left, 1)

        right = QVBoxLayout()
        self.detail_label = QLabel("Select a call")
        right.addWidget(self.detail_label)
        self.detail_tabs = QTabWidget()
        self.transcript_view = QTextEdit()
        self.transcript_view.setReadOnly(True)
        self.summary_view = QTextEdit()
        self.email_view = QTextEdit()
        self.detail_tabs.addTab(self.transcript_view, "Transcript")
        self.detail_tabs.addTab(self.summary_view, "Summary")
        self.detail_tabs.addTab(self.email_view, "Email draft")
        right.addWidget(self.detail_tabs)

        copy_row = QHBoxLayout()
        copy_summary_btn = QPushButton("Copy Summary")
        copy_summary_btn.clicked.connect(lambda: QApplication.clipboard().setText(self.summary_view.toPlainText()))
        copy_email_btn = QPushButton("Copy Email")
        copy_email_btn.clicked.connect(lambda: QApplication.clipboard().setText(self.email_view.toPlainText()))
        copy_row.addWidget(copy_summary_btn)
        copy_row.addWidget(copy_email_btn)
        right.addLayout(copy_row)

        self.regen_btn = QPushButton("Re-generate summary")
        self.regen_btn.clicked.connect(self._regenerate)
        right.addWidget(self.regen_btn)

        layout.addLayout(right, 2)

        self.refresh()

    def refresh(self) -> None:
        search = self.search_edit.text().strip() or None
        self._calls = self.ctx.repo.list_calls(search=search)
        self.call_list.clear()
        for call in self._calls:
            when = (call["started_at"] or "")[:16].replace("T", " ")
            self.call_list.addItem(f"{when} - {call['customer_name'] or 'Unknown'} ({call['status']})")

    def _on_select(self, row: int) -> None:
        if row < 0 or row >= len(self._calls):
            return
        call = self._calls[row]
        self.detail_label.setText(f"{call['customer_name'] or 'Unknown'} -- {call['started_at']}")
        self.transcript_view.setPlainText(call["transcript"] or "(no transcript)")
        note = self.ctx.repo.get_note_for_call(call["id"])
        self.summary_view.setPlainText(note["summary"] if note else "")
        self.email_view.setPlainText(note["email_draft"] if note else "")

    def _regenerate(self) -> None:
        row = self.call_list.currentRow()
        if row < 0:
            return
        call = self._calls[row]
        if not call["transcript"]:
            QMessageBox.information(self, "No transcript", "This call has no transcript to summarize.")
            return
        model = self.ctx.settings.ollama_model
        if not model:
            QMessageBox.warning(self, "No model selected", "Pick an Ollama model in Settings first.")
            return

        call_id = call["id"]
        customer_id = call["customer_id"]
        self.regen_btn.setEnabled(False)
        self._regen_worker = SummarizeWorker(
            self.ctx.settings.ollama_host,
            model,
            call["transcript"],
            call["customer_name"] or "",
            (call["started_at"] or "")[:10],
        )

        def on_ok(result):
            self.ctx.repo.add_note(call_id, result.summary_text, result.email_draft, json.dumps(result.key_points))
            for task_desc in result.tasks:
                self.ctx.repo.add_task(task_desc, call_id=call_id, customer_id=customer_id, source="call")
            self.summary_view.setPlainText(result.summary_text)
            self.email_view.setPlainText(result.email_draft)
            self.regen_btn.setEnabled(True)

        def on_fail(message):
            QMessageBox.warning(self, "Failed", message)
            self.regen_btn.setEnabled(True)

        self._regen_worker.succeeded.connect(on_ok)
        self._regen_worker.failed.connect(on_fail)
        self._regen_worker.start()

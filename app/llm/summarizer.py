"""Orchestrates transcript -> {summary, email, tasks} via Ollama, and
hosts the QThread worker used by the Record tab on Stop."""
from __future__ import annotations

from dataclasses import dataclass, field

from PySide6.QtCore import QThread, Signal

from app.llm import ollama_client, prompts


@dataclass
class CallSummary:
    summary_text: str
    email_draft: str
    tasks: list[str] = field(default_factory=list)
    key_points: list[str] = field(default_factory=list)


def summarize_call(host: str, model: str, transcript: str, customer_name: str, call_date: str) -> CallSummary:
    prompt = prompts.build_summary_prompt(transcript, customer_name, call_date)
    data = ollama_client.generate_json(host, model, prompt)

    bullets = data.get("summary") or []
    if isinstance(bullets, str):
        bullets = [bullets]
    tasks = data.get("tasks") or []
    if isinstance(tasks, str):
        tasks = [tasks]
    email = data.get("customer_email") or ""

    summary_text = "\n".join(f"- {b}" for b in bullets) if bullets else ""
    return CallSummary(
        summary_text=summary_text,
        email_draft=email.strip(),
        tasks=[t.strip() for t in tasks if str(t).strip()],
        key_points=[str(b).strip() for b in bullets],
    )


def generate_yearly_narrative(host: str, model: str, year: int, summaries: list[str], stats: dict) -> str:
    prompt = prompts.build_yearly_narrative_prompt(year, summaries, stats)
    return ollama_client.generate_text(host, model, prompt)


class SummarizeWorker(QThread):
    """Runs summarize_call off the GUI thread; emits the result or an error."""

    succeeded = Signal(object)  # CallSummary
    failed = Signal(str)

    def __init__(self, host: str, model: str, transcript: str, customer_name: str, call_date: str):
        super().__init__()
        self._host = host
        self._model = model
        self._transcript = transcript
        self._customer_name = customer_name
        self._call_date = call_date

    def run(self) -> None:
        try:
            result = summarize_call(
                self._host, self._model, self._transcript, self._customer_name, self._call_date
            )
            self.succeeded.emit(result)
        except Exception as exc:
            self.failed.emit(str(exc))


class NarrativeWorker(QThread):
    """Runs generate_yearly_narrative off the GUI thread."""

    succeeded = Signal(str)
    failed = Signal(str)

    def __init__(self, host: str, model: str, year: int, summaries: list[str], stats: dict):
        super().__init__()
        self._host = host
        self._model = model
        self._year = year
        self._summaries = summaries
        self._stats = stats

    def run(self) -> None:
        try:
            text = generate_yearly_narrative(self._host, self._model, self._year, self._summaries, self._stats)
            self.succeeded.emit(text)
        except Exception as exc:
            self.failed.emit(str(exc))

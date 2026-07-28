"""Daily desktop reminder for open tasks. A QTimer ticks once a minute
and fires a notify-send once per day at the configured time (plus an
in-app signal so the Tasks tab / tray badge can refresh)."""
from __future__ import annotations

import datetime as dt
import subprocess

from PySide6.QtCore import QObject, QTimer, Signal

from app.db.repository import Repository


def send_notification(title: str, body: str) -> None:
    try:
        subprocess.run(["notify-send", "--app-name=CallScribe", "--", title, body], timeout=5)
    except (subprocess.SubprocessError, FileNotFoundError, OSError):
        pass  # no notification daemon available -- in-app badge still covers it


class ReminderScheduler(QObject):
    reminder_fired = Signal(list)  # list[sqlite3.Row] of due open tasks

    def __init__(self, repo: Repository, reminder_time: str = "09:00", enabled: bool = True):
        super().__init__()
        self._repo = repo
        self.reminder_time = reminder_time
        self.enabled = enabled
        self._last_fired_date: str | None = None
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(60_000)

    def _tick(self) -> None:
        if not self.enabled:
            return
        now = dt.datetime.now()
        today = now.date().isoformat()
        if today == self._last_fired_date:
            return
        if now.strftime("%H:%M") < self.reminder_time:
            return
        self._last_fired_date = today
        self.fire_now()

    def fire_now(self) -> list:
        due = self._repo.count_open_tasks_due(dt.date.today().isoformat())
        if due:
            lines = "\n".join(f"- {row['description']}" for row in due[:10])
            more = f"\n(+{len(due) - 10} more)" if len(due) > 10 else ""
            send_notification(f"{len(due)} task(s) due", lines + more)
        self.reminder_fired.emit(due)
        return due

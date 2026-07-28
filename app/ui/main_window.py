"""Main window: tab container + tray wiring + daily reminder scheduler."""
from __future__ import annotations

from PySide6.QtWidgets import QApplication, QMainWindow, QTabWidget

from app.tasks.reminder import ReminderScheduler
from app.ui.history_tab import HistoryTab
from app.ui.record_tab import RecordTab
from app.ui.settings_tab import SettingsTab
from app.ui.summary_tab import SummaryTab
from app.ui.tasks_tab import TasksTab
from app.ui.tray import TrayIcon


class MainWindow(QMainWindow):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self._force_quit = False

        self.setWindowTitle("CallScribe")
        self.resize(1050, 750)

        self.tabs = QTabWidget()
        self.record_tab = RecordTab(ctx)
        self.history_tab = HistoryTab(ctx)
        self.tasks_tab = TasksTab(ctx)
        self.summary_tab = SummaryTab(ctx)
        self.settings_tab = SettingsTab(ctx)
        self.tabs.addTab(self.record_tab, "Record")
        self.tabs.addTab(self.history_tab, "History")
        self.tabs.addTab(self.tasks_tab, "Tasks")
        self.tabs.addTab(self.summary_tab, "Yearly Summary")
        self.tabs.addTab(self.settings_tab, "Settings")
        self.setCentralWidget(self.tabs)

        self.record_tab.call_finished.connect(self._on_call_finished)
        self.record_tab.recording_state_changed.connect(self._on_recording_state_changed)
        self.settings_tab.settings_saved.connect(self._on_settings_saved)

        self.reminder = ReminderScheduler(
            ctx.repo, ctx.settings.reminder_time, ctx.settings.reminder_enabled
        )
        self.reminder.reminder_fired.connect(lambda _tasks: self.tasks_tab.refresh())

        self.tray = TrayIcon(self)
        if QSystemTraySupported():
            self.tray.show()

    def _on_call_finished(self) -> None:
        self.history_tab.refresh()
        self.tasks_tab.refresh()

    def _on_recording_state_changed(self, recording: bool) -> None:
        self.tray.set_recording_state(recording)

    def _on_settings_saved(self) -> None:
        self.reminder.reminder_time = self.ctx.settings.reminder_time
        self.reminder.enabled = self.ctx.settings.reminder_enabled

    def toggle_recording_from_tray(self) -> None:
        self.record_tab._toggle_recording()

    def closeEvent(self, event) -> None:
        if self.ctx.settings.minimize_to_tray and self.tray.isVisible() and not self._force_quit:
            event.ignore()
            self.hide()
        else:
            event.accept()
            QApplication.quit()

    def quit_app(self) -> None:
        self._force_quit = True
        self.close()


def QSystemTraySupported() -> bool:
    from PySide6.QtWidgets import QSystemTrayIcon

    return QSystemTrayIcon.isSystemTrayAvailable()

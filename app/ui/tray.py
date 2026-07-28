"""System tray icon with quick start/stop recording and quit."""
from __future__ import annotations

from PySide6.QtWidgets import QMenu, QStyle, QSystemTrayIcon


class TrayIcon(QSystemTrayIcon):
    def __init__(self, main_window):
        icon = main_window.style().standardIcon(QStyle.StandardPixmap.SP_MediaVolume)
        super().__init__(icon, main_window)
        self._idle_icon = icon
        self._recording_icon = main_window.style().standardIcon(QStyle.StandardPixmap.SP_MediaPlay)
        self.main_window = main_window

        menu = QMenu()
        self.toggle_action = menu.addAction("Start Recording")
        self.toggle_action.triggered.connect(main_window.toggle_recording_from_tray)
        menu.addAction("Show Window", self._show_window)
        menu.addSeparator()
        menu.addAction("Quit", main_window.quit_app)
        self.setContextMenu(menu)
        self.activated.connect(self._on_activated)
        self.setToolTip("CallScribe")

    def _on_activated(self, reason: QSystemTrayIcon.ActivationReason) -> None:
        if reason == QSystemTrayIcon.ActivationReason.Trigger:
            self._show_window()

    def _show_window(self) -> None:
        self.main_window.showNormal()
        self.main_window.activateWindow()

    def set_recording_state(self, recording: bool) -> None:
        self.toggle_action.setText("Stop Recording" if recording else "Start Recording")
        self.setIcon(self._recording_icon if recording else self._idle_icon)

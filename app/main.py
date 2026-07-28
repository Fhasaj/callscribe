"""CallScribe entry point."""
from __future__ import annotations

import sys

from PySide6.QtWidgets import QApplication

from app.context import AppContext
from app.ui.main_window import MainWindow
from app.utils.logging_setup import setup_logging


def main() -> int:
    setup_logging()
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)
    app.setApplicationName("CallScribe")

    ctx = AppContext()
    ctx.preload_whisper()

    window = MainWindow(ctx)
    window.show()

    return app.exec()


if __name__ == "__main__":
    sys.exit(main())

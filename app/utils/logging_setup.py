"""Basic logging configuration -- logs to both stderr and a file under
the data dir so background-thread errors (audio/transcription/LLM
workers) are visible even if a dialog gets missed."""
from __future__ import annotations

import logging

from app.config import DATA_DIR


def setup_logging(level: int = logging.INFO) -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    log_file = DATA_DIR / "callscribe.log"
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        handlers=[logging.FileHandler(log_file), logging.StreamHandler()],
    )

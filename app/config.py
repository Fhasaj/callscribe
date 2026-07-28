"""Per-machine settings, stored outside the repo so two machines never
clobber each other's config. Lives at ~/.config/callscribe/config.json."""
from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path

CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "callscribe"
CONFIG_FILE = CONFIG_DIR / "config.json"

DATA_DIR = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share")) / "callscribe"
RECORDINGS_DIR = DATA_DIR / "recordings"
DEFAULT_DB_PATH = DATA_DIR / "callscribe.db"

OLLAMA_HOST_DEFAULT = "http://localhost:11434"
RECOMMENDED_OLLAMA_MODEL = "qwen2.5:7b-instruct"


@dataclass
class Settings:
    db_path: str = str(DEFAULT_DB_PATH)
    ollama_host: str = OLLAMA_HOST_DEFAULT
    ollama_model: str = ""  # empty = not chosen yet, pick in Settings tab
    whisper_model_size: str = ""  # empty = auto-suggest based on detected device
    whisper_device: str = "auto"  # auto | cuda | cpu
    mic_source: str = ""  # empty = use system default source
    monitor_source: str = ""  # empty = use "<default-sink>.monitor"
    reminder_time: str = "09:00"
    reminder_enabled: bool = True
    minimize_to_tray: bool = True

    def to_dict(self) -> dict:
        return asdict(self)


def load_settings() -> Settings:
    if CONFIG_FILE.exists():
        try:
            raw = json.loads(CONFIG_FILE.read_text())
        except (json.JSONDecodeError, OSError):
            raw = {}
        defaults = asdict(Settings())
        defaults.update({k: v for k, v in raw.items() if k in defaults})
        return Settings(**defaults)
    return Settings()


def save_settings(settings: Settings) -> None:
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text(json.dumps(settings.to_dict(), indent=2))


def ensure_data_dirs() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    RECORDINGS_DIR.mkdir(parents=True, exist_ok=True)

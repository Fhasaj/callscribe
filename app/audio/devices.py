"""Enumerate PipeWire/PulseAudio sources so Settings can offer a mic
source and a monitor (loopback) source to pick from -- names differ
per machine so nothing here is hardcoded."""
from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass


@dataclass
class AudioSource:
    name: str
    description: str
    is_monitor: bool


def list_sources() -> list[AudioSource]:
    try:
        out = subprocess.run(
            ["pactl", "-f", "json", "list", "sources"],
            capture_output=True,
            text=True,
            timeout=5,
            check=True,
        ).stdout
    except (subprocess.SubprocessError, FileNotFoundError, OSError):
        return []
    try:
        raw = json.loads(out)
    except json.JSONDecodeError:
        return []
    sources = []
    for entry in raw:
        name = entry.get("name", "")
        sources.append(
            AudioSource(
                name=name,
                description=entry.get("description", name),
                is_monitor=name.endswith(".monitor"),
            )
        )
    return sources


def list_mic_sources() -> list[AudioSource]:
    return [s for s in list_sources() if not s.is_monitor]


def list_monitor_sources() -> list[AudioSource]:
    return [s for s in list_sources() if s.is_monitor]


def default_mic_source() -> str:
    try:
        return subprocess.run(
            ["pactl", "get-default-source"], capture_output=True, text=True, timeout=5, check=True
        ).stdout.strip()
    except (subprocess.SubprocessError, FileNotFoundError, OSError):
        return ""


def default_monitor_source() -> str:
    try:
        sink = subprocess.run(
            ["pactl", "get-default-sink"], capture_output=True, text=True, timeout=5, check=True
        ).stdout.strip()
    except (subprocess.SubprocessError, FileNotFoundError, OSError):
        return ""
    return f"{sink}.monitor" if sink else ""


def audio_backend_available() -> bool:
    try:
        subprocess.run(["pactl", "info"], capture_output=True, timeout=5, check=True)
        return True
    except (subprocess.SubprocessError, FileNotFoundError, OSError):
        return False

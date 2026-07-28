"""Captures mic + monitor (loopback) audio via `parec`, mixes them to
mono float32 @ 16kHz, hands fixed-size chunks to the transcription
worker via a queue, and archives the full call as a WAV file."""
from __future__ import annotations

import queue
import subprocess
import threading
import time
from pathlib import Path
from typing import Optional

import numpy as np
import soundfile as sf

from app.audio import devices

SAMPLE_RATE = 16000
BLOCK_SECONDS = 1.0
BLOCK_SAMPLES = int(SAMPLE_RATE * BLOCK_SECONDS)
BLOCK_BYTES = BLOCK_SAMPLES * 2  # s16le mono
CHUNK_SECONDS = 20  # how much audio is handed to whisper at a time


def _parec_cmd(source: str) -> list[str]:
    cmd = ["parec", "--format=s16le", f"--rate={SAMPLE_RATE}", "--channels=1", "--raw"]
    if source:
        cmd.append(f"--device={source}")
    return cmd


class _PipeReader(threading.Thread):
    """Reads fixed-size PCM blocks from a parec subprocess into a queue."""

    def __init__(self, source: str):
        super().__init__(daemon=True)
        self.source = source
        self.blocks: "queue.Queue[Optional[bytes]]" = queue.Queue(maxsize=50)
        self.start_error: Optional[Exception] = None
        self._proc: Optional[subprocess.Popen] = None
        self._stop = threading.Event()

    def run(self) -> None:
        try:
            self._proc = subprocess.Popen(_parec_cmd(self.source), stdout=subprocess.PIPE)
        except OSError as exc:
            self.start_error = exc
            self.blocks.put(None)
            return
        stdout = self._proc.stdout
        while not self._stop.is_set():
            data = stdout.read(BLOCK_BYTES)
            if not data:
                break
            if len(data) < BLOCK_BYTES:
                data = data + b"\x00" * (BLOCK_BYTES - len(data))
            self.blocks.put(data)
        self.blocks.put(None)

    def stop(self) -> None:
        self._stop.set()
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self._proc.kill()


class CallRecorder:
    """Owns the two parec readers, mixes their output, chunks it for
    transcription, and keeps the full-call samples for archival."""

    def __init__(self, mic_source: str = "", monitor_source: str = ""):
        mic_source = mic_source or devices.default_mic_source()
        monitor_source = monitor_source or devices.default_monitor_source()
        self.mic_reader = _PipeReader(mic_source)
        self.monitor_reader = _PipeReader(monitor_source)
        self.chunk_queue: "queue.Queue[Optional[np.ndarray]]" = queue.Queue()
        self._mixer_thread: Optional[threading.Thread] = None
        self._full_audio: list[np.ndarray] = []
        self._pending: list[np.ndarray] = []
        self._pending_samples = 0
        self._started_at: Optional[float] = None

    def start(self) -> None:
        self._started_at = time.monotonic()
        self.mic_reader.start()
        self.monitor_reader.start()
        self._mixer_thread = threading.Thread(target=self._mix_loop, daemon=True)
        self._mixer_thread.start()

    def _mix_loop(self) -> None:
        chunk_samples_target = SAMPLE_RATE * CHUNK_SECONDS
        mic_done = mon_done = False
        while not (mic_done and mon_done):
            mic_bytes = None
            if not mic_done:
                mic_bytes = self.mic_reader.blocks.get()
                if mic_bytes is None:
                    mic_done = True
            mon_bytes = None
            if not mon_done:
                mon_bytes = self.monitor_reader.blocks.get()
                if mon_bytes is None:
                    mon_done = True
            if mic_done and mon_done:
                break
            mic_arr = self._to_float(mic_bytes)
            mon_arr = self._to_float(mon_bytes)
            mixed = np.clip(mic_arr + mon_arr, -1.0, 1.0)
            self._full_audio.append(mixed)
            self._pending.append(mixed)
            self._pending_samples += len(mixed)
            if self._pending_samples >= chunk_samples_target:
                self._flush_pending()
        self._flush_pending()
        self.chunk_queue.put(None)

    @staticmethod
    def _to_float(data: Optional[bytes]) -> np.ndarray:
        if not data:
            return np.zeros(BLOCK_SAMPLES, dtype=np.float32)
        return np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768.0

    def _flush_pending(self) -> None:
        if not self._pending:
            return
        chunk = np.concatenate(self._pending)
        self._pending = []
        self._pending_samples = 0
        if chunk.size:
            self.chunk_queue.put(chunk)

    def stop(self, save_wav_path: Optional[Path] = None) -> tuple[int, Optional[str]]:
        self.mic_reader.stop()
        self.monitor_reader.stop()
        if self._mixer_thread:
            self._mixer_thread.join(timeout=10)
        duration = int(time.monotonic() - self._started_at) if self._started_at else 0
        wav_path_str = None
        if save_wav_path and self._full_audio:
            full = np.concatenate(self._full_audio)
            save_wav_path.parent.mkdir(parents=True, exist_ok=True)
            sf.write(str(save_wav_path), full, SAMPLE_RATE)
            wav_path_str = str(save_wav_path)
        return duration, wav_path_str

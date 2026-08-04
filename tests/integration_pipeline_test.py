"""Manual integration test (not part of unittest suite): exercises the
real non-GUI pipeline end-to-end -- captures audio via parec while a
synthesized test call plays through the speaker, transcribes it, runs
it through the real local Ollama model, and writes to a temp SQLite DB.
Run directly: .venv/bin/python tests/integration_pipeline_test.py
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.audio.capture import CallRecorder
from app.db.database import connect
from app.db.repository import Repository
from app.llm.summarizer import summarize_call
from app.transcription.whisper_engine import WhisperEngine

TEST_WAV = "/tmp/pipeline_test.wav"


def main() -> None:
    print("Loading Whisper engine...")
    engine = WhisperEngine(model_size="small")
    print(f"  device={engine.device} compute_type={engine.compute_type}")

    print("Starting audio capture (mic + monitor)...")
    recorder = CallRecorder()
    recorder.start()

    def play_test_audio():
        time.sleep(0.5)
        subprocess.run(["paplay", TEST_WAV])

    player = threading.Thread(target=play_test_audio)
    player.start()
    player.join()
    time.sleep(1.0)  # let the tail of the audio get captured

    tmp = tempfile.TemporaryDirectory()
    wav_path = Path(tmp.name) / "call.wav"
    duration, saved_wav = recorder.stop(save_wav_path=wav_path)
    print(f"Captured {duration}s, saved to {saved_wav}")

    print("Transcribing captured chunks...")
    transcript_parts = []
    while True:
        chunk = recorder.chunk_queue.get()
        if chunk is None:
            break
        text = engine.transcribe(chunk)
        if text:
            transcript_parts.append(text)
    transcript = " ".join(transcript_parts).strip()
    print("TRANSCRIPT:", repr(transcript))
    assert transcript, "expected non-empty transcript from captured loopback audio"

    print("Storing in temp SQLite DB...")
    db_path = Path(tmp.name) / "pipeline.db"
    conn = connect(db_path)
    repo = Repository(conn)
    customer_id = repo.get_or_create_customer("Pipeline Test Customer")
    call_id = repo.create_call(customer_id)
    repo.append_transcript(call_id, transcript)
    repo.finish_call(call_id, duration, str(saved_wav))

    print("Summarizing via local Ollama...")
    result = summarize_call(
        "http://localhost:11434", "qwen2.5:7b-instruct", transcript, "Pipeline Test Customer", "2026-07-28"
    )
    repo.add_note(call_id, result.summary_text, result.email_draft, "[]")
    for task_desc in result.tasks:
        repo.add_task(task_desc, call_id=call_id, customer_id=customer_id, source="call")

    print("\nSUMMARY:\n", result.summary_text)
    print("\nEMAIL:\n", result.email_draft)
    print("\nTASKS:", result.tasks)

    stored_call = repo.get_call(call_id)
    stored_note = repo.get_note_for_call(call_id)
    stored_tasks = repo.list_tasks(status="open")
    assert stored_call["transcript"] == transcript
    assert stored_note["summary"] == result.summary_text
    assert len(stored_tasks) == len(result.tasks)

    print("\nPIPELINE INTEGRATION TEST PASSED")
    conn.close()


if __name__ == "__main__":
    main()

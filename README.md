# CallScribe

A local-first desktop app for taking Teams call notes: records the
call's audio (your mic + the other side, via speaker loopback),
transcribes it with a local Whisper model, and uses a local Ollama LLM
to write call notes and a copy-pasteable customer follow-up email --
plus tasks extracted from the call, a daily task reminder, and a
yearly recap. Nothing leaves your machine.

Runs on Linux with PipeWire (tested on Fedora and Arch). Designed to
run on more than one machine (e.g. a laptop and a desktop) -- each
machine keeps its own local settings (`~/.config/callscribe/`) and,
by default, its own SQLite database (`~/.local/share/callscribe/`).

## Requirements

- Python 3.11+ (tested on 3.14)
- [Ollama](https://ollama.com) installed and running locally
- PipeWire with pulse compatibility (`pactl`/`parec`) -- standard on
  current Fedora/Arch/most distros
- `notify-send` (libnotify) for desktop reminder popups (optional --
  the in-app Tasks tab works without it)
- A CUDA-capable GPU is optional. If present (with working
  CUDA/cuDNN), Whisper transcription uses it automatically; otherwise
  it falls back to CPU without crashing.

## Setup

```bash
./scripts/setup_env.sh
```

This creates a `.venv`, installs Python dependencies, and checks for
`pactl`/`parec`/`notify-send`/a running Ollama daemon, printing
warnings for anything missing.

## Running

```bash
.venv/bin/python -m app.main
```

### First run

Open the **Settings** tab and:

1. **Ollama model** -- pick an installed model, or click "Pull
   qwen2.5:7b-instruct" to download a good general-purpose default
   (coding-focused models like `qwen2.5-coder` also work but write
   stiffer prose).
2. **Whisper model size/device** -- auto-suggested based on whether a
   CUDA GPU was detected on this machine (`medium` on GPU, `small` on
   CPU). Larger sizes are more accurate but slower.
3. **Microphone / loopback source** -- pick your mic and the speaker
   monitor source. Defaults are auto-detected from PipeWire's default
   sink/source, but pick explicitly if you have multiple audio
   devices.
4. Optionally adjust the database file location, daily reminder time,
   and tray behavior.

### Using it

- **Record tab**: type/select a customer, click "Start Recording",
  talk -- the transcript fills in live. Click "Stop Recording" when
  the call ends; CallScribe transcribes any remaining audio and asks
  the local LLM to produce a summary and a customer email, both
  editable with one-click copy buttons.
- **History tab**: browse/search past calls, view the transcript and
  notes, or re-generate the summary.
- **Tasks tab**: manual tasks plus tasks auto-extracted from calls;
  check them off as you complete them. A daily notification fires at
  the configured time listing what's open (also mirrored in-app).
- **Yearly Summary tab**: stats (call count, hours, top customers,
  tasks completed) for a chosen year, plus an optional LLM-written
  prose recap grounded in that year's call summaries.
- The tray icon lets you start/stop recording and quit without
  keeping the main window open; closing the window minimizes to tray
  by default (configurable in Settings).

## A note on call recording

Recording calls may be subject to consent requirements depending on
your jurisdiction and company policy. This is a local personal tool --
whether and how to inform call participants that you're recording is
your responsibility.

## Architecture

```
app/
  main.py            entry point
  context.py          shared services (settings, DB, Whisper engine)
  config.py            per-machine settings (~/.config/callscribe/config.json)
  db/                    SQLite schema + repository (customers, calls, notes, tasks)
  audio/                  parec-based mic+loopback capture, device enumeration
  transcription/           faster-whisper wrapper with CUDA/CPU autodetect
  llm/                      Ollama REST client, prompts, summarization orchestration
  tasks/                     daily reminder scheduler
  ui/                         PySide6 GUI: 5 tabs + tray icon
```

Audio capture uses `parec` (PipeWire's Pulse-compat recording tool)
against named sources rather than PortAudio device indices, since
Pulse/PipeWire's monitor (loopback) sources aren't reliably exposed
that way. Mic and loopback are captured independently, summed to mono
float32 @ 16kHz, and fed directly to faster-whisper (no ffmpeg
dependency needed since the capture format already matches what
Whisper expects).

## Tests

```bash
.venv/bin/python -m unittest discover -s tests -p "test_*.py"
```

`tests/integration_pipeline_test.py` is a manual (not auto-discovered)
end-to-end check that exercises real audio capture + real Whisper
transcription + a real local Ollama call together -- run it directly
with `.venv/bin/python tests/integration_pipeline_test.py` (requires a
working audio setup and Ollama running).

## Known limitations (v1)

- No Teams API integration -- audio is captured via system loopback,
  so it works with the Teams desktop app, the Teams PWA, or any other
  call in a browser tab.
- No speaker diarization -- the transcript is a single merged stream,
  not labeled "you" vs "customer".
- No automatic cross-machine sync of the database -- point the DB path
  (Settings) at a synced folder (e.g. Syncthing/Nextcloud) yourself if
  you want notes shared between machines.
- Linux/PipeWire only.

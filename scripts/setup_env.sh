#!/usr/bin/env bash
# Sets up CallScribe's Python venv and checks system dependencies.
# Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== CallScribe setup =="

if [ ! -d .venv ]; then
    echo "Creating virtualenv (.venv)..."
    python3 -m venv .venv
fi

echo "Installing Python dependencies..."
.venv/bin/pip install --upgrade pip -q
.venv/bin/pip install -r requirements.txt

echo
echo "== System dependency checks =="

if command -v pactl >/dev/null 2>&1; then
    echo "[ok] pactl found (PipeWire/PulseAudio)"
else
    echo "[MISSING] pactl not found -- install pipewire-pulse (or pulseaudio-utils)."
fi

if command -v parec >/dev/null 2>&1; then
    echo "[ok] parec found (used for call audio capture)"
else
    echo "[MISSING] parec not found -- install pipewire-pulse (or pulseaudio-utils)."
fi

if command -v notify-send >/dev/null 2>&1; then
    echo "[ok] notify-send found (used for daily task reminders)"
else
    echo "[warn] notify-send not found -- desktop reminder popups won't show (in-app Tasks tab still works). Install libnotify."
fi

if curl -sf --max-time 3 http://localhost:11434/api/tags >/dev/null 2>&1; then
    echo "[ok] Ollama is running on localhost:11434"
else
    echo "[warn] Ollama does not seem to be running. Start it (e.g. 'ollama serve' or your system service) before using CallScribe."
fi

echo
echo "Setup complete. Run the app with:"
echo "  .venv/bin/python -m app.main"

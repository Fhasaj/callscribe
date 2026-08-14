#!/usr/bin/env bash
# Sets up CallScribe (C++/FTXUI) -- configures + builds via CMake and
# downloads the default Whisper GGML model. Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== CallScribe (C++) setup =="
echo

echo "== Toolchain checks =="
missing=0
for tool in cmake ninja g++ curl pactl parec; do
    if command -v "$tool" >/dev/null 2>&1; then
        echo "[ok] $tool found"
    else
        echo "[MISSING] $tool not found"
        missing=1
    fi
done

if command -v notify-send >/dev/null 2>&1; then
    echo "[ok] notify-send found (used for daily task reminders)"
else
    echo "[warn] notify-send not found -- reminder popups won't show. Install libnotify."
fi

if pkg-config --exists libcurl 2>/dev/null; then
    echo "[ok] libcurl dev headers found"
else
    echo "[MISSING] libcurl dev headers not found."
    echo "          Fedora: sudo dnf install libcurl-devel"
    echo "          Arch:   sudo pacman -S curl"
    missing=1
fi

if curl -sf --max-time 3 http://localhost:11434/api/tags >/dev/null 2>&1; then
    echo "[ok] Ollama is running on localhost:11434"
else
    echo "[warn] Ollama does not seem to be running -- start it before using CallScribe."
fi

if [ "$missing" -ne 0 ]; then
    echo
    echo "Install the missing tools above, then re-run this script."
    exit 1
fi

echo
echo "== Configuring (CMake will fetch FTXUI, nlohmann/json, whisper.cpp, and"
echo "   a vendored SQLite -- first run downloads/builds these, can take a while) =="
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

echo
echo "== Building =="
cmake --build build -j"$(nproc)"

echo
echo "== Whisper model =="
model_size="small"
if command -v nvidia-smi >/dev/null 2>&1; then
    model_size="medium"
    echo "NVIDIA GPU detected -- using model size '$model_size' (change in Settings if desired)."
fi
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}/callscribe-cpp"
model_dir="$data_dir/models"
model_path="$model_dir/ggml-${model_size}.bin"
mkdir -p "$model_dir"
if [ -f "$model_path" ]; then
    echo "[ok] $model_path already present"
else
    echo "Downloading $model_path ..."
    curl -L --fail -o "$model_path.part" \
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-${model_size}.bin"
    mv "$model_path.part" "$model_path"
    echo "[ok] downloaded"
fi

echo
echo "Setup complete. Run the app with:"
echo "  ./build/callscribe"

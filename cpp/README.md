# CallScribe (C++ / FTXUI terminal version)

A terminal-UI rebuild of CallScribe -- local-first Teams call notes: mic +
speaker-loopback capture, local Whisper (whisper.cpp) transcription with
**speaker labeling** (your mic side is tagged with your name, the
loopback/other side tagged with the participants you enter), a local
Ollama LLM for call summaries/follow-up emails/task extraction, task
tracking with daily reminders, a yearly recap, and a chat-style email
proofreader -- all in your terminal via [FTXUI](https://github.com/ArthurSonzogni/FTXUI).

This is a from-scratch C++ implementation living alongside the Python/
PySide6 GUI version (`../`) in this repo's `cpp` git branch -- `main`
has the Python app only; `cpp` has both. They're independent: separate
config (`~/.config/callscribe-cpp/`), separate database
(`~/.local/share/callscribe-cpp/`) by default.

## Requirements

- CMake 3.21+, Ninja, a C++20 compiler (g++/clang)
- `libcurl` dev headers (`libcurl-devel` on Fedora, `curl` on Arch --
  both ship the headers)
- [Ollama](https://ollama.com) installed and running locally
- PipeWire with pulse compatibility (`pactl`/`parec`)
- `notify-send` (libnotify) for desktop reminder popups (optional)
- A CUDA-capable GPU is optional -- detected automatically at CMake
  configure time (`find_package(CUDAToolkit)`); whisper.cpp is built
  with GPU support if found, CPU-only otherwise, no runtime switch
  needed.

SQLite is vendored (fetched + compiled from the official amalgamation)
so no system `sqlite-devel` package is required. FTXUI, nlohmann/json,
and whisper.cpp are fetched via CMake `FetchContent` on first configure.

## Setup

```bash
./scripts/setup_env.sh
```

Checks the toolchain, configures + builds (first run fetches/builds
FTXUI, nlohmann/json, whisper.cpp, and vendored SQLite -- this takes a
few minutes), and downloads the default Whisper GGML model (`small` on
CPU-only machines, `medium` if `nvidia-smi` is detected) into
`~/.local/share/callscribe-cpp/models/`.

## Running

```bash
./build/callscribe
```

### First run

Go to the **Settings** screen and:

1. Pick/refresh an **Ollama model**, or "Pull qwen2.5:7b-instruct" for a
   good general-purpose default.
2. Pick a **Whisper model size** and "Download model" if it's not
   already present (sizes: tiny/base/small/medium/large-v3 -- bigger is
   slower but more accurate; GPU machines can comfortably run larger
   sizes).
3. Pick your **mic** and **loopback/monitor** audio sources (auto-
   detected defaults are used if you don't).
4. Set **your name** -- this labels your side of the transcript.
5. Optionally set the database path and daily reminder time.

### Using it

- **Record** -- enter a meeting name / customer / participants, Start
  Recording, talk -- the transcript fills in live with each line
  labeled by speaker (your name for the mic, the participants you
  entered for the loopback side). Stop Recording transcribes any
  remaining audio and asks the local LLM for a summary + customer
  email, both with one-key copy-to-clipboard.
- **History** -- search past calls, view transcript/summary/email,
  re-generate a summary.
- **Tasks** -- manual + call-extracted tasks, checkable, with a daily
  desktop notification for what's open.
- **Calendar** -- your Teams/Outlook calendar, read-only, drawn as a
  real calendar: **Day** (hour-by-hour with a marker on the current
  hour), **Week** (seven columns from today), or **Month** (a
  Monday-first grid with today's cell outlined). An **Agenda** toggle
  switches any of them to a flat chronological list with organizer,
  location and join links. No Microsoft sign-in: see
  [Importing your calendar](#importing-your-calendar) below.
- **Yearly Summary** -- stats for a chosen year plus an optional
  LLM-written prose recap.
- **Proofread** -- a small chat: paste or type an email, press Send,
  get a grammar/clarity-corrected version back as a reply, with copy
  support. Doesn't touch the call database -- just a quick local LLM
  utility.
- **Settings** -- as above.

Tab between screens with the top menu (arrow keys / mouse). Within a
screen, Tab/Shift+Tab moves between fields; Enter/click activates
buttons.

## Importing your calendar

The Calendar tab reads your calendar without signing in to a Microsoft
account -- there's no OAuth, no Graph API, no token. Two ways in, both
read-only; pick one under **Setup** on the Calendar tab.

**1. Published ICS feed (stays in sync).** In Outlook on the web:
Settings -> Calendar -> Shared calendars -> *Publish a calendar* ->
choose your calendar, permission **"Can view all details"** -> Publish.
Copy the `.ics` link (not the HTML one) and paste it into Setup. The
app re-downloads it on a timer (default every 30 minutes).

Two caveats worth knowing:

- Many corporate tenants **disable calendar publishing** by policy. If
  the Publish option is missing or greyed out, use option 2.
- Microsoft regenerates the published feed lazily -- changes can take
  a few hours to appear. It is not a live view.

The feed URL is a **bearer secret**: anyone who has it can read your
calendar without authenticating. It's stored in
`~/.config/callscribe-cpp/config.json`, which CallScribe now writes
`0600` (owner-only). Revoke it from the same Outlook page if it leaks.

**2. Local `.ics` file (always works).** Outlook desktop -> File ->
Save Calendar -> iCalendar format, or save any single meeting invite
(`.ics` attachment). Point Setup at the file path. Re-export whenever
you want fresh data -- the app re-reads the file on the same timer.

Either way, the last successfully read feed is cached at
`~/.local/share/callscribe-cpp/calendar-cache.ics`, so the agenda still
renders when you're offline or the link is temporarily failing; the
status line says when it's showing a stale copy and why.

In the grids, a meeting in progress shows green, past ones grey, and
anything marked "show as free" is dimmed. Month cells show up to three
meetings plus a `+n more` count; switch to Week or Agenda for the rest.

Recurring meetings are expanded locally (daily/weekly/monthly/yearly
rules with `INTERVAL`, `COUNT`, `UNTIL`, `BYDAY`, `BYMONTHDAY`,
`BYMONTH`), including `EXDATE` skips and single occurrences the
organizer moved or cancelled. Times honor the invite's own time zone
via the system tz database, so a 09:00 weekly meeting stays at 09:00
across daylight-saving changes.

## A note on call recording

Recording calls may be subject to consent requirements depending on
your jurisdiction and company policy. This is a local personal tool --
whether and how to inform call participants that you're recording is
your responsibility.

## Architecture

```
src/
  main.cpp            entry point
  context.hpp/.cpp      shared services: settings, DB, reminder scheduler,
                          two Whisper engines (mic + monitor, loaded async)
  config.hpp/.cpp        per-machine settings (~/.config/callscribe-cpp/config.json)
  db/                      SQLite schema + repository (customers, calls,
                             transcript_segments, notes, tasks)
  audio/                    two independent parec-fed capture streams --
                             deliberately NOT mixed, so each can be
                             transcribed and speaker-labeled separately
  transcription/             whisper.cpp wrapper; one context per stream
                              since whisper.cpp isn't safe for concurrent
                              use across threads on one context
  llm/                        libcurl-based Ollama client, prompts, and
                               summarization/proofreading orchestration
  calendar/                    iCalendar reader: RFC 5545 parsing +
                                recurrence expansion, and the ICS
                                 source (published URL / local file)
                                  with an offline cache
  tasks/                       daily reminder scheduler (background
                                thread + notify-send)
  ui/                           FTXUI screens: Record, History, Tasks,
                                 Calendar, Yearly Summary, Proofread,
                                  Settings
  util/                          small helpers: blocking queue, clipboard
                                  (wl-copy/xclip/xsel), WAV writer,
                                  subprocess capture, time formatting
```

The audio + speaker-labeling design is the main behavioral difference
from the Python GUI version: mic and loopback are captured and
transcribed as two independent streams (two `parec` processes, two
Whisper contexts) rather than mixed into one before transcription --
that's what makes labeling "you" vs. "the other participant(s)"
possible without real diarization. It roughly doubles transcription
compute versus the mixed-stream approach, which is an acceptable
tradeoff given whisper.cpp's efficiency.

## Tests

```bash
cmake --build build --target callscribe_tests && ./build/callscribe_tests
```

Unit tests (doctest) cover the DB repository, prompt builders, date
formatting, text wrapping, and the iCalendar parser (folding, escaping,
Windows time zone mapping, recurrence expansion, EXDATE/RECURRENCE-ID,
all-day events, and DST-safe repeats).

```bash
cmake --build build --target integration_pipeline_test && ./build/integration_pipeline_test
```

A manual end-to-end check (not part of the default build/ctest): real
`parec` capture of `espeak-ng`-synthesized audio on the monitor source,
through the real downloaded Whisper model, a real local Ollama call,
and a temp SQLite DB -- requires a working audio setup and Ollama
running.

## Known limitations (v1)

- Speaker labeling is binary (you vs. "the other participant(s)"), not
  true per-person diarization when multiple people are on the other
  end -- there's only one loopback audio channel to work with.
- No Teams API integration -- audio is captured via system loopback, so
  it works with the Teams desktop app, the Teams PWA, or any other call
  in a browser tab. The Calendar tab is likewise read-only ICS import,
  not a Microsoft Graph client: you can view your agenda, not create or
  respond to invites.
- No automatic cross-machine sync of the database -- point the DB path
  (Settings) at a synced folder yourself if you want notes shared
  between machines.
- Linux/PipeWire only.

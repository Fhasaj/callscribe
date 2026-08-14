# CallScribe

A local-first terminal app for Teams call notes: mic + speaker-loopback
capture, local Whisper (whisper.cpp) transcription with **speaker
labeling** (your mic side is tagged with your name, the loopback/other
side tagged with the participants you enter), a local Ollama LLM for
call summaries/follow-up emails/task extraction, task tracking with
daily reminders, a read-only calendar, a yearly recap, and a chat /
email-proofreading assistant -- all in your terminal via
[FTXUI](https://github.com/ArthurSonzogni/FTXUI).

Nothing leaves your machine. Designed to run on more than one machine
(e.g. a laptop and a desktop): each keeps its own settings
(`~/.config/callscribe-cpp/`) and, by default, its own SQLite database
(`~/.local/share/callscribe-cpp/`).

## Requirements

- CMake 3.21+, Ninja, a C++20 compiler (g++/clang)
- `libcurl` dev headers (`libcurl-devel` on Fedora, `curl` on Arch --
  both ship the headers)
- [Ollama](https://ollama.com) installed and running locally
- PipeWire with pulse compatibility (`pactl`/`parec`)
- `notify-send` (libnotify) for desktop reminder popups (optional)
- `wl-clipboard`, `xclip` or `xsel` for copy/paste (optional)
- Write access to `/dev/uinput` for the Mouse Mover (optional -- the
  app prints the udev-rule fix if permission is denied)
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
5. Leave **echo cancellation** on unless it causes trouble -- it keeps
   speaker bleed out of the mic stream (see Architecture).
6. Optionally set the database path, the daily reminder time, the
   Mouse Mover interval/step, and the **prompt presets** -- the
   wording used to ask for the summary, the follow-up email, and the
   task list is editable.

### Using it

- **Record** -- enter a meeting name / customer / participants, Start
  Recording, talk -- the transcript fills in live with each line
  labeled by speaker (your name for the mic, the participants you
  entered for the loopback side), with live level meters, a pause
  button, and a field for your own free-form notes. Stop Recording
  transcribes any remaining audio and asks the local LLM for a summary,
  a customer email and the call's action items, all with one-key
  copy-to-clipboard. The full audio is archived as a WAV under
  `~/.local/share/callscribe-cpp/recordings/`.
- **History** -- search past calls, view transcript/summary/email,
  re-generate a summary.
- **Tasks** -- manual + call-extracted tasks, checkable, with due dates
  and priorities and a daily desktop notification for what's open. The
  menu entry carries a count badge for anything due today or overdue,
  so it's visible from every screen.
- **Calendar** -- your Teams/Outlook calendar, read-only, drawn as a
  real calendar: **Day** (hour-by-hour with a marker on the current
  hour), **Week** (seven columns from today), or **Month** (a
  Monday-first grid with today's cell outlined). An **Agenda** toggle
  switches any of them to a flat chronological list with organizer,
  location and join links. Meetings you've already recorded are matched
  against the call database and marked, so what's left is what still
  needs writing up. No Microsoft sign-in: see
  [Importing your calendar](#importing-your-calendar) below.
- **Yearly Summary** -- stats for a chosen year plus an optional
  LLM-written prose recap.
- **Chat** -- a small local-LLM chat with two modes. **Chat** is a
  free-form assistant that remembers the thread; **Proofread** takes a
  pasted email and returns a grammar/clarity-corrected version, each
  message independent of the last. Each mode keeps its own transcript,
  and a reasoning model's chain of thought is folded away behind a
  per-message toggle. Doesn't touch the call database.
- **Mouse Mover** -- nudges the pointer on a timer so the machine (and
  any presence-tracking app) stays awake. The nudge is a small square
  that ends where it started, so the cursor never drifts. While it's
  running the menu entry is marked from every screen.
- **Settings** -- as above.

Tab between screens with the top menu (arrow keys / mouse). Within a
screen, Tab/Shift+Tab moves between fields; Enter/click activates
buttons.

Terminal apps swallow the mouse, so the terminal's own copy/paste is
unavailable while CallScribe runs. Instead: drag to select, then
right-click (or `Ctrl+G`) for a copy / paste / select-all menu;
`Ctrl+V` pastes straight into the focused field. Copying unwraps the
soft line breaks the panes added, so pasted text keeps its original
paragraphs.

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
                             transcribed and speaker-labeled separately;
                             plus PipeWire WebRTC echo cancellation
  transcription/             whisper.cpp wrapper; one context per stream
                              since whisper.cpp isn't safe for concurrent
                              use across threads on one context
  llm/                        libcurl-based Ollama client, prompts, and
                               summarization/proofreading orchestration
  calendar/                    iCalendar reader: RFC 5545 parsing +
                                recurrence expansion, the ICS source
                                 (published URL / local file) with an
                                  offline cache, and call<->event matching
  tasks/                       daily reminder scheduler (background
                                thread + notify-send)
  ui/                           FTXUI screens: Record, History, Tasks,
                                 Calendar, Yearly Summary, Chat,
                                  Mouse Mover, Settings
  util/                          small helpers: blocking queue, clipboard
                                  (wl-copy/xclip/xsel), WAV writer,
                                  subprocess capture, time formatting,
                                  uinput virtual pointer
```

The audio + speaker-labeling design is the core of the app: mic and
loopback are captured and transcribed as two independent streams (two
`parec` processes, two Whisper contexts) rather than mixed into one
before transcription -- that's what makes labeling "you" vs. "the other
participant(s)" possible without real diarization. It roughly doubles
transcription compute versus a mixed-stream approach, which is an
acceptable tradeoff given whisper.cpp's efficiency. Capture goes
through `parec` against *named* sources rather than PortAudio device
indices, since Pulse/PipeWire monitor (loopback) sources aren't
reliably exposed that way; both streams arrive as mono float32 @ 16kHz,
which is exactly what whisper.cpp wants, so there's no ffmpeg
dependency.

Echo cancellation loads PipeWire's WebRTC `echo-cancel` module for the
duration of a recording and temporarily switches the default sink to
the virtual one it creates. That routing is the point: the far end's
audio has to pass through the virtual sink for the canceller to have a
reference signal to subtract from the mic. A side benefit is that the
virtual sink's monitor carries only the far end, a cleaner "them"
stream than the system-wide output monitor. The previous default sink
is restored on stop.

The Mouse Mover writes to a synthetic input device via `/dev/uinput`,
because Wayland deliberately gives ordinary clients no way to warp the
pointer. Events written there travel back up through libinput and reach
the compositor indistinguishably from a real mouse's, which is what
resets the idle timer that presence-tracking apps watch.

## Tests

```bash
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Unit tests (doctest) cover the DB repository, prompt builders, date
formatting, text wrapping, task extraction from messy model replies,
call<->calendar-event matching, and the iCalendar parser (folding,
escaping, Windows time zone mapping, recurrence expansion,
EXDATE/RECURRENCE-ID, all-day events, and DST-safe repeats). Run the
binary directly (`./build/callscribe_tests`) to filter or list cases.

```bash
cmake --build build --target integration_pipeline_test && ./build/integration_pipeline_test
```

A manual end-to-end check (not part of the default build/ctest): real
`parec` capture of `espeak-ng`-synthesized audio on the monitor source,
through the real downloaded Whisper model, a real local Ollama call,
and a temp SQLite DB -- requires a working audio setup and Ollama
running.

## Known limitations

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

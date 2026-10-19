# CallScribe (C++ / FTXUI terminal version) - Personal Branch 

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
- [Ollama](https://ollama.com) installed and running locally -- or
  llama.cpp's `llama-server` instead (Settings > AI > Server); its
  `--api-key`, if any, goes in the desktop keyring
- PipeWire with pulse compatibility (`pactl`/`parec`)
- `libsecret` and OpenSSL dev headers (`libsecret-devel openssl-devel`
  on Fedora; `libsecret openssl` on Arch) -- the Spotify login token is
  kept in the desktop keyring (KWallet on Plasma, GNOME Keyring on
  GNOME) via the Secret Service API
- `notify-send` (libnotify) for desktop reminder and focus-timer popups
  (optional)
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
  desktop notification for what's open. `[steps]` on a task asks the
  model for its first few concrete steps, which you tick to add.
- **Calendar** -- your Teams/Outlook calendar, read-only, drawn as a
  real calendar: **Day** (hour-by-hour with a marker on the current
  hour), **Week** (seven columns from today), or **Month** (a
  Monday-first grid with today's cell outlined). An **Agenda** toggle
  switches any of them to a flat chronological list with organizer,
  location and join links. No Microsoft sign-in: see
  [Importing your calendar](#importing-your-calendar) below.
- **Review** -- the year at a glance, and answers for your performance
  review: paste the questions from the review form and the local model
  writes first-person answers from your call notes, completed tasks,
  wins, focus notes and daily notes -- the most relevant that fit its
  context, citing real dates and customers, never invented numbers.
- **Proofread** -- a small chat: paste or type an email, press Send,
  get a grammar/clarity-corrected version back as a reply, with copy
  support. Doesn't touch the call database -- just a quick local LLM
  utility.
- **Focus** -- a Pomodoro timer: focus (25 min), short break (5), and
  a long break (15) after every fourth round; lengths are in Settings.
  Starting a round asks for the project (Indica / Freelance / SkyPin /
  Other) and the next step; finishing one asks where you got to, and
  saves the lot -- tag, time actually focused, timestamps -- to the
  log below the timer. Each change of phase pops a desktop
  notification and rings the terminal bell, and the time left shows
  on the tab from anywhere in the app. Keys: `s` start/pause, `k`
  skip, `r` reset. Breaks start on their own; focus rounds wait for
  you. "Just 5 min" starts a short round that asks whether to keep
  going; thoughts parked with `^P` wait here for your break; after 90
  minutes of focus or recording with no real break you get a check-in
  (Settings); and "This week" totals time per project, with a
  copyable timesheet.
- **Today** -- what the day held (calls, tasks done, focus time, wins),
  a line of your own about it, and your recent wins. Save it at the
  end of the day -- a reminder comes at 17:30 -- and the review reads
  it back.
- **Anywhere** -- `^W` logs a win, `^P` parks a stray thought, and the
  title bar shows what the current focus round is for.
- **Settings** -- as above, plus the optional Spotify link (below).

### Spotify (optional)

With a Spotify account connected, a focus round starts your chosen
playlist and a break pauses it. Without one, the timer works exactly
the same. Playback control needs Spotify Premium.

1. Create an app at <https://developer.spotify.com/dashboard> (Web
   API) and add the Redirect URI `http://127.0.0.1:8888/callback`
   (or another port -- it must match the one in Settings exactly).
2. In **Settings > Spotify**, paste the app's Client ID and a playlist
   (`spotify:playlist:...` or an `open.spotify.com/playlist/...` link),
   then **Connect**. Your browser opens on Spotify's consent page; if it
   can't be opened, the link is shown (and copied) to open by hand.
3. Have Spotify open somewhere -- desktop app, phone, speaker. If
   nothing is playing, CallScribe picks the first device it can see.

Once connected, the Focus tab shows a mini player -- track, artist,
device, progress -- with Prev / Play-Pause / Next buttons, or `b` /
`p` / `n` from the keyboard. With nothing loaded, Play starts your
focus playlist.

The login is Authorization Code + PKCE, so there's no client secret.
The refresh token goes into your desktop keyring through libsecret,
never into a file; **Disconnect** removes it.

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
  focus/                       Pomodoro state machine (clock-free, so
                                it's unit-tested with fake time)
  spotify/                     PKCE login with a loopback redirect
                                listener, Web API playback on a worker
                                thread, refresh token in the keyring
  ui/                           FTXUI screens: Record, History, Tasks,
                                 Calendar, Review, Chat, Focus, Today,
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

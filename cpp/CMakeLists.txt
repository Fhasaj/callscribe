cmake_minimum_required(VERSION 3.21)
project(callscribe_cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CALLSCRIBE_BUILD_TESTS "Build CallScribe test binaries" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
# PKCE for the Spotify login (SHA-256 + a CSPRNG). libcurl already pulls
# OpenSSL in on every distro we target, so this adds no new package.
find_package(OpenSSL REQUIRED)
# The Spotify refresh token lives in the desktop keyring (Secret Service),
# not on disk: libsecret-devel on Fedora, libsecret on Arch.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

# -- SQLite3 -- vendored amalgamation (no system sqlite-devel required,
# keeps setup identical across machines without needing sudo) -----------
FetchContent_Declare(sqlite3
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
)
FetchContent_MakeAvailable(sqlite3)
add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_ENABLE_FTS5)

# -- nlohmann/json -----------------------------------------------------
# Declared here but NOT made available until after sherpa-onnx below.
#
# sherpa-onnx fetches nlohmann/json under this same FetchContent name and then
# calls add_subdirectory() on it unconditionally, which collides with ours
# ("binary directory is already used to build a source directory"). Declaring
# first still pins the version -- FetchContent honours the first declaration --
# so sherpa populates the checkout we asked for, adds it once, and our
# MakeAvailable further down becomes a no-op that just picks up the target.
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
)

# -- FTXUI ---------------------------------------------------------------
set(FTXUI_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(FTXUI_BUILD_TESTS OFF CACHE INTERNAL "")
set(FTXUI_BUILD_DOCS OFF CACHE INTERNAL "")
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v7.0.1
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui)

# -- whisper.cpp -- auto-enable CUDA backend if a CUDA toolkit is present
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
  message(STATUS "CallScribe: CUDA toolkit found -- building whisper.cpp with GPU support")
  set(GGML_CUDA ON CACHE BOOL "" FORCE)
else()
  message(STATUS "CallScribe: no CUDA toolkit found -- whisper.cpp will run on CPU")
  set(GGML_CUDA OFF CACHE BOOL "" FORCE)
endif()
set(WHISPER_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(WHISPER_BUILD_TESTS OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_Declare(whispercpp
  GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
  GIT_TAG v1.9.1
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(whispercpp)

# -- sherpa-onnx -- speaker diarization (pyannote segmentation + speaker
# embeddings + clustering), run offline on the far-end audio after a call.
#
# Only the diarization path is wanted, so everything else the project can
# build is switched off: TTS, the websocket servers, portaudio, the JNI
# bindings, the example binaries. Without this it pulls in espeak-ng and a
# websocket stack for code we never call. C_API stays on -- diarizer.cpp talks
# to the stable C interface rather than the internal C++ headers.
set(SHERPA_ONNX_ENABLE_TTS OFF CACHE INTERNAL "")
set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE INTERNAL "")
set(SHERPA_ONNX_ENABLE_PORTAUDIO OFF CACHE INTERNAL "")
set(SHERPA_ONNX_ENABLE_JNI OFF CACHE INTERNAL "")
set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE INTERNAL "")
set(SHERPA_ONNX_BUILD_C_API_EXAMPLES OFF CACHE INTERNAL "")
set(SHERPA_ONNX_ENABLE_TESTS OFF CACHE INTERNAL "")
set(SHERPA_ONNX_ENABLE_PYTHON OFF CACHE INTERNAL "")
set(SHERPA_ONNX_ENABLE_C_API ON CACHE INTERNAL "")
set(SHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION ON CACHE INTERNAL "")
# Off deliberately: sherpa defaults this ON and appends -static-libstdc++
# -static-libgcc to the *global* CXX flags, which would then also apply to our
# code and to the CUDA host compile.
set(SHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY OFF CACHE INTERNAL "")
FetchContent_Declare(sherpaonnx
  GIT_REPOSITORY https://github.com/k2-fsa/sherpa-onnx.git
  GIT_TAG v1.13.6
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(sherpaonnx)

# Safe now: sherpa-onnx has already populated and added the json checkout we
# declared above, so this only resolves the nlohmann_json target for us.
FetchContent_MakeAvailable(json)

# -- core library (everything except the FTXUI screens + main) -----------
add_library(callscribe_core STATIC
  src/config.cpp
  src/context.cpp
  src/util/subprocess.cpp
  src/util/clipboard.cpp
  src/util/virtual_pointer.cpp
  src/util/wav_writer.cpp
  src/util/download.cpp
  src/util/date_format.cpp
  src/util/keyring.cpp
  src/db/database.cpp
  src/db/repository.cpp
  src/audio/devices.cpp
  src/audio/echo_cancel.cpp
  src/audio/capture.cpp
  src/transcription/whisper_engine.cpp
  src/transcription/diarizer.cpp
  src/calendar/ics_parser.cpp
  src/calendar/calendar_source.cpp
  src/calendar/call_match.cpp
  src/llm/ollama_client.cpp
  src/llm/prompts.cpp
  src/llm/evidence.cpp
  src/llm/summarizer.cpp
  src/tasks/reminder.cpp
  src/tasks/day_digest.cpp
  src/tasks/meeting_reminder.cpp
  src/focus/pomodoro.cpp
  src/spotify/pkce.cpp
  src/spotify/keyring.cpp
  src/spotify/callback_listener.cpp
  src/spotify/spotify_client.cpp
)
target_include_directories(callscribe_core PUBLIC src)
target_link_libraries(callscribe_core PUBLIC
  nlohmann_json::nlohmann_json
  sqlite3
  CURL::libcurl
  OpenSSL::Crypto
  PkgConfig::LIBSECRET
  whisper
  sherpa-onnx-c-api
  Threads::Threads
)

# -- main executable (TUI) ------------------------------------------------
add_executable(callscribe
  src/main.cpp
  src/ui/app.cpp
  src/ui/scroll_pane.cpp
  src/ui/text_wrap.cpp
  src/ui/wrapped_input.cpp
  src/ui/record_screen.cpp
  src/ui/history_screen.cpp
  src/ui/calendar_screen.cpp
  src/ui/proposed_tasks.cpp
  src/ui/tasks_screen.cpp
  src/ui/summary_screen.cpp
  src/ui/chat_screen.cpp
  src/ui/mouse_mover_screen.cpp
  src/ui/focus_screen.cpp
  src/ui/quick_capture.cpp
  src/ui/today_screen.cpp
  src/ui/settings_screen.cpp
)
target_link_libraries(callscribe PRIVATE
  callscribe_core
  ftxui::screen
  ftxui::dom
  ftxui::component
)

if(CALLSCRIBE_BUILD_TESTS)
  set(DOCTEST_NO_INSTALL ON CACHE INTERNAL "")
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(doctest)

  add_executable(callscribe_tests
    tests/test_repository.cpp
    tests/test_prompts.cpp
    tests/test_date_format.cpp
    tests/test_ics_parser.cpp
    tests/test_call_match.cpp
    tests/test_task_extract.cpp
    tests/test_text_wrap.cpp
    tests/test_diarizer.cpp
    tests/test_pomodoro.cpp
    tests/test_spotify_pkce.cpp
    tests/test_review.cpp
    tests/test_meeting_reminder.cpp
    src/ui/text_wrap.cpp
  )
  target_link_libraries(callscribe_tests PRIVATE callscribe_core doctest::doctest ftxui::dom ftxui::screen)

  add_executable(integration_pipeline_test EXCLUDE_FROM_ALL
    tests/integration_pipeline_test.cpp
  )
  target_link_libraries(integration_pipeline_test PRIVATE callscribe_core)

  enable_testing()
  add_test(NAME callscribe_tests COMMAND callscribe_tests)
endif()

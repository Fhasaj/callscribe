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
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)

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

# -- core library (everything except the FTXUI screens + main) -----------
add_library(callscribe_core STATIC
  src/config.cpp
  src/context.cpp
  src/util/subprocess.cpp
  src/util/clipboard.cpp
  src/util/virtual_pointer.cpp
  src/util/wav_writer.cpp
  src/util/date_format.cpp
  src/util/pdf_writer.cpp
  src/db/database.cpp
  src/db/repository.cpp
  src/audio/devices.cpp
  src/audio/echo_cancel.cpp
  src/audio/capture.cpp
  src/transcription/whisper_engine.cpp
  src/calendar/ics_parser.cpp
  src/calendar/calendar_source.cpp
  src/calendar/call_match.cpp
  src/llm/ollama_client.cpp
  src/llm/prompts.cpp
  src/llm/summarizer.cpp
  src/tasks/reminder.cpp
)
target_include_directories(callscribe_core PUBLIC src)
target_link_libraries(callscribe_core PUBLIC
  nlohmann_json::nlohmann_json
  sqlite3
  CURL::libcurl
  whisper
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
  src/ui/tasks_screen.cpp
  src/ui/summary_screen.cpp
  src/ui/chat_screen.cpp
  src/ui/mouse_mover_screen.cpp
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
    tests/test_pdf_writer.cpp
    tests/test_date_format.cpp
    tests/test_ics_parser.cpp
    tests/test_call_match.cpp
    tests/test_task_extract.cpp
    tests/test_text_wrap.cpp
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

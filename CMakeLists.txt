# ╔════════════════════════════════════════════════════════════════════════════╗
# ║  Kairos — Unified Orchestration Daemon                                     ║
# ║  Top-level CMake build file                                                ║
# ╚════════════════════════════════════════════════════════════════════════════╝

cmake_minimum_required(VERSION 3.22)

project(kairos
    VERSION 0.1.0
    DESCRIPTION "Unified orchestration daemon — scheduling, workflows, and filesystem monitoring"
    HOMEPAGE_URL "https://github.com/araray/kairos"
    LANGUAGES CXX
)

# ── C++20 Standard ─────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Export compile_commands.json for IDE/clangd support.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ────────────────────────────────────────────────────────────────
include(cmake/KairosOptions.cmake)

# ── Sanitizer support ─────────────────────────────────────────────────────
# Apply -fsanitize= flags globally when KAIROS_SANITIZERS is set.
# SQLite's bundled amalgamation triggers known false-positive UBSan
# findings in its B-tree MemPage pointer casts — we suppress UBSan
# for that specific translation unit after FetchContent populates it.
if(KAIROS_SANITIZERS AND NOT MSVC)
    string(REPLACE "," ";" _san_list "${KAIROS_SANITIZERS}")
    string(REPLACE ";" "," _san_flags "${_san_list}")
    set(_san_compile "-fsanitize=${_san_flags} -fno-omit-frame-pointer")
    set(_san_link    "-fsanitize=${_san_flags}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_san_compile}")
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${_san_compile}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_san_link}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_san_link}")
    message(STATUS "Sanitizers enabled: ${_san_flags}")
elseif(KAIROS_SANITIZERS AND MSVC)
    message(STATUS "Sanitizers skipped (MSVC uses /fsanitize= via build.ps1 -Asan)")
endif()

# ── Dependencies ───────────────────────────────────────────────────────────
include(cmake/KairosDependencies.cmake)

# ── SQLite UBSan suppression ──────────────────────────────────────────────
# SQLite's amalgamation (compiled by SQLiteCpp) triggers known UBSan false
# positives in btree.c MemPage pointer casts (line ~78114 in sqlite3.c).
# Suppress undefined-behavior sanitizer for the amalgamation only.
# The sqlite3 target is created by SQLiteCpp's CMakeLists.txt.
if(KAIROS_SANITIZERS AND NOT MSVC AND TARGET sqlite3)
    string(FIND "${KAIROS_SANITIZERS}" "undefined" _has_ubsan)
    if(NOT _has_ubsan EQUAL -1)
        target_compile_options(sqlite3 PRIVATE -fno-sanitize=undefined)
        message(STATUS "UBSan suppressed for sqlite3 amalgamation (known false positives)")
    endif()
endif()

# ── Core library (always built) ────────────────────────────────────────────
add_library(kairos_core STATIC
    src/config/defaults.cpp
    src/config/config_store.cpp
    src/config/validation.cpp
    src/core/id_generator.cpp
    src/kel/value.cpp
    src/kel/lexer.cpp
    src/kel/parser.cpp
    src/kel/evaluator.cpp
    src/exec/env_builder.cpp
    src/exec/runner_pool.cpp
    src/exec/output_sink.cpp
    src/persist/migration.cpp
    src/persist/database.cpp
    src/persist/db_writer.cpp
    src/persist/query_reader.cpp
    src/observability/logging.cpp
    src/observability/json_formatter.cpp
    src/observability/metrics.cpp
    src/observability/null_tracer.cpp
    src/engine/trigger_types.cpp
    src/engine/dag.cpp
    src/engine/execution_plan.cpp
    src/engine/event_types.cpp
    src/engine/scheduler.cpp
    src/engine/pipeline.cpp
    src/engine/cancel_registry.cpp
    src/watch/sample.cpp
    src/watch/watch_engine.cpp
    src/watch/real_scanner.cpp
    src/watch/debounce_buffer.cpp
    src/watch/hash_util.cpp
    src/config/yaml_loader.cpp
    src/mcp/transport.cpp
    src/mcp/handler.cpp
    src/migration/migration_tool.cpp
    src/tui/tui_helpers.cpp
    src/platform/timezone.cpp
)

# ── Native watch backends (platform-specific) ─────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(kairos_core PRIVATE src/watch/inotify_watcher.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(kairos_core PRIVATE src/watch/fsevents_watcher.cpp)
    target_link_libraries(kairos_core PUBLIC "-framework CoreServices")
elseif(WIN32)
    target_sources(kairos_core PRIVATE src/watch/rdcw_watcher.cpp)
else()
    target_sources(kairos_core PRIVATE src/watch/file_watcher_stub.cpp)
endif()

target_include_directories(kairos_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(kairos_core PUBLIC
    confy
    SQLiteCpp
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    yaml-cpp::yaml-cpp
    croncpp::croncpp
)

# Embed version info.
target_compile_definitions(kairos_core PUBLIC
    KAIROS_VERSION="${PROJECT_VERSION}"
    KAIROS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    KAIROS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    KAIROS_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# ── Platform abstraction layer ─────────────────────────────────────────────
add_library(kairos_platform STATIC)

if(WIN32)
    target_sources(kairos_platform PRIVATE
        src/platform/paths_win32.cpp
        src/platform/terminal_win32.cpp
        src/platform/signals_win32.cpp
        src/platform/threading_win32.cpp
        src/platform/instance_lock_win32.cpp
        src/platform/environment_win32.cpp
        src/exec/process_handle_win32.cpp
    )
    target_link_libraries(kairos_platform PRIVATE Shell32 Ole32)
else()
    target_sources(kairos_platform PRIVATE
        src/platform/paths_posix.cpp
        src/platform/terminal_posix.cpp
        src/platform/signals_posix.cpp
        src/platform/threading_posix.cpp
        src/platform/instance_lock_posix.cpp
        src/platform/environment_posix.cpp
        src/exec/process_handle_posix.cpp
    )
    target_link_libraries(kairos_platform PRIVATE pthread)
endif()

target_include_directories(kairos_platform PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(kairos_platform PUBLIC kairos_core)

# ── Main executable ────────────────────────────────────────────────────────
add_executable(kairos
    src/main.cpp
    src/cli/cli_app.cpp
    src/cli/cli_migrate.cpp
    src/daemon/daemon.cpp
    src/daemon/command_reader.cpp
)

target_link_libraries(kairos PRIVATE
    kairos_core
    kairos_platform
    CLI11::CLI11
)

# ── Optional: HTTP server (KAIROS_HTTP=ON) ─────────────────────────────────
if(KAIROS_HTTP)
    target_sources(kairos PRIVATE src/http/http_server.cpp)
    target_compile_definitions(kairos PRIVATE KAIROS_HTTP_ENABLED)
    target_link_libraries(kairos PRIVATE httplib::httplib)
    # inja is header-only; include path comes from FetchContent.
    target_include_directories(kairos PRIVATE
        ${inja_SOURCE_DIR}/include
    )
endif()

# ── Optional: OpenTelemetry tracing (KAIROS_OTEL=ON) ──────────────────────
if(KAIROS_OTEL)
    target_sources(kairos_core PRIVATE src/observability/otel_tracer.cpp)
    target_compile_definitions(kairos_core PUBLIC KAIROS_OTEL)
    target_link_libraries(kairos_core PUBLIC
        opentelemetry-cpp::trace
        opentelemetry-cpp::ostream_span_exporter
    )
    # OTLP HTTP exporter is only available when the system SDK is installed
    # (FetchContent builds omit it to avoid protobuf/abseil dependency).
    if(KAIROS_OTEL_OTLP)
        target_link_libraries(kairos_core PUBLIC
            opentelemetry-cpp::otlp_http_exporter
        )
        target_compile_definitions(kairos_core PUBLIC KAIROS_OTEL_OTLP)
        message(STATUS "OTel: OTLP HTTP exporter enabled")
    else()
        message(STATUS "OTel: ostream exporter only (no OTLP)")
    endif()
endif()

# ── Optional: Ansible Vault support (KAIROS_VAULT=ON) ─────────────────────
if(KAIROS_VAULT)
    target_sources(kairos_core PRIVATE src/security/secret_store.cpp)
    target_compile_definitions(kairos_core PUBLIC KAIROS_VAULT)
    target_link_libraries(kairos_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
    target_sources(kairos_core PRIVATE src/security/secret_store_stub.cpp)
endif()

# ── Optional: Docker runner support (KAIROS_DOCKER=ON) ───────────────────
if(KAIROS_DOCKER)
    target_sources(kairos_core PRIVATE src/exec/docker_process_handle.cpp)
    target_compile_definitions(kairos_core PUBLIC KAIROS_DOCKER_ENABLED)
else()
    target_sources(kairos_core PRIVATE src/exec/docker_process_handle_stub.cpp)
endif()

# ── Optional: Ansible runner support (KAIROS_ANSIBLE=ON) ────────────────
if(KAIROS_ANSIBLE)
    target_sources(kairos_core PRIVATE src/exec/ansible_process_handle.cpp)
    target_compile_definitions(kairos_core PUBLIC KAIROS_ANSIBLE_ENABLED)
else()
    target_sources(kairos_core PRIVATE src/exec/ansible_process_handle_stub.cpp)
endif()

# ── Optional: systemd integration (Linux only) ───────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SYSTEMD QUIET libsystemd)
        if(SYSTEMD_FOUND)
            target_compile_definitions(kairos PRIVATE KAIROS_SYSTEMD)
            target_link_libraries(kairos PRIVATE ${SYSTEMD_LIBRARIES})
            target_include_directories(kairos PRIVATE ${SYSTEMD_INCLUDE_DIRS})
            message(STATUS "systemd: sd_notify integration enabled")
        else()
            message(STATUS "systemd: libsystemd not found — sd_notify stubs used")
        endif()
    endif()
endif()

# ── Windows Service integration ───────────────────────────────────────────
if(WIN32)
    target_sources(kairos PRIVATE src/daemon/win32_service.cpp)
    target_link_libraries(kairos PRIVATE advapi32)
endif()

# ── Optional: TUI dashboard (KAIROS_TUI=ON) ─────────────────────────────
if(KAIROS_TUI)
    FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG        v5.0.0
        GIT_SHALLOW    TRUE
    )
    set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(ftxui)

    target_sources(kairos PRIVATE src/tui/tui_dashboard.cpp)
    target_compile_definitions(kairos PRIVATE KAIROS_TUI_ENABLED)
    target_link_libraries(kairos PRIVATE
        ftxui::screen ftxui::dom ftxui::component)
    message(STATUS "TUI: FTXUI dashboard enabled")
else()
    target_sources(kairos PRIVATE src/tui/tui_dashboard_stub.cpp)
    message(STATUS "TUI: dashboard disabled (stub)")
endif()

# ── Tests ──────────────────────────────────────────────────────────────────
if(KAIROS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install + Packaging ────────────────────────────────────────────────────
include(cmake/KairosInstall.cmake)
include(cmake/KairosCPack.cmake)

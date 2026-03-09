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

# ── Dependencies ───────────────────────────────────────────────────────────
include(cmake/KairosDependencies.cmake)

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
    )
    target_link_libraries(kairos_platform PRIVATE Shell32 Ole32)
else()
    target_sources(kairos_platform PRIVATE
        src/platform/paths_posix.cpp
        src/platform/terminal_posix.cpp
        src/platform/signals_posix.cpp
        src/platform/threading_posix.cpp
        src/platform/instance_lock_posix.cpp
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
        opentelemetry-cpp::otlp_http_exporter
    )
endif()

# ── Tests ──────────────────────────────────────────────────────────────────
if(KAIROS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install ────────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS kairos
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/kairos
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

install(FILES deploy/kairos.toml.example
    DESTINATION ${CMAKE_INSTALL_DATADIR}/kairos
)

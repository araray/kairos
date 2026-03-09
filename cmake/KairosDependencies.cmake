# ╔════════════════════════════════════════════════════════════════════════════╗
# ║  Kairos Dependency Management (FetchContent)                              ║
# ╚════════════════════════════════════════════════════════════════════════════╝

include(FetchContent)

# ── Required dependencies ──────────────────────────────────────────────────

# confy-cpp (configuration library — project requirement).
FetchContent_Declare(confy-cpp
    GIT_REPOSITORY https://github.com/araray/confy-cpp.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

# SQLiteCpp (RAII wrapper for SQLite3).
FetchContent_Declare(SQLiteCpp
    GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
    GIT_TAG        3.3.1
    GIT_SHALLOW    TRUE
)
# SQLiteCpp options: disable its own tests/examples.
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)

# spdlog (structured logging).
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.13.0
    GIT_SHALLOW    TRUE
)

# nlohmann/json (JSON handling).
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# yaml-cpp (YAML parsing for workflow/watch definitions).
FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
    GIT_SHALLOW    TRUE
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)

# CLI11 (CLI argument parsing).
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.1
    GIT_SHALLOW    TRUE
)

# croncpp (cron expression parsing — header-only).
FetchContent_Declare(croncpp
    GIT_REPOSITORY https://github.com/mariusbancila/croncpp.git
    GIT_TAG        v2023.03.30
    GIT_SHALLOW    TRUE
)

# ── Make all required deps available ───────────────────────────────────────
FetchContent_MakeAvailable(
    confy-cpp
    SQLiteCpp
    spdlog
    nlohmann_json
    yaml-cpp
    CLI11
    croncpp
)

# ── Test dependencies ──────────────────────────────────────────────────────
if(KAIROS_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ── Optional: HTTP server dependencies ─────────────────────────────────────
if(KAIROS_HTTP)
    FetchContent_Declare(httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG        v0.15.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(httplib)
endif()

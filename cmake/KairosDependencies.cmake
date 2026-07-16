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
    # Kairos uses HTTP (plain) only — TLS is handled by reverse proxy.
    # Disabling OpenSSL prevents transitive curl linkage on some systems.
    # §14.1: Audited — httplib does NOT link curl directly, but OpenSSL
    # discovery can indirectly pull in curl on linuxbrew/conda systems.
    set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_REQUIRE_BROTLI OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)

    # inja — Jinja2-compatible template engine (header-only, MIT).
    # Depends on nlohmann/json (already present — FetchContent deduplicates).
    FetchContent_Declare(inja
        GIT_REPOSITORY https://github.com/pantor/inja.git
        GIT_TAG        v3.4.0
        GIT_SHALLOW    TRUE
    )
    set(INJA_USE_EMBEDDED_JSON OFF CACHE BOOL "" FORCE)
    set(INJA_INSTALL OFF CACHE BOOL "" FORCE)
    set(INJA_EXPORT OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(httplib inja)
endif()

# ── Optional: Ansible Vault support (OpenSSL) ─────────────────────────
if(KAIROS_VAULT)
    find_package(OpenSSL REQUIRED)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
endif()

# ── Optional: OpenTelemetry tracing ────────────────────────────────────
if(KAIROS_OTEL)
    # Strategy (v0.2.0+):
    #   Default: FetchContent — static libs, ostream exporter, zero
    #   transitive deps (no protobuf, no abseil, no curl).  This avoids
    #   the fragile scenario where an OS upgrade breaks the system SDK's
    #   shared-library dependency chain (e.g. libprotobuf.so.NN).
    #
    #   Opt-in:  -DKAIROS_OTEL_SYSTEM=ON  →  try find_package first.
    #   If found AND runtime deps validate, use system SDK with OTLP.
    #   Otherwise fall back to FetchContent automatically.
    #
    # The KAIROS_OTEL_OTLP variable tracks whether OTLP is available.
    # otel_tracer.cpp checks #ifdef KAIROS_OTEL_OTLP at compile time.

    set(_use_fetchcontent TRUE)

    if(KAIROS_OTEL_SYSTEM)
        find_package(opentelemetry-cpp QUIET)
        if(opentelemetry-cpp_FOUND)
            # Validate transitive deps before committing to system SDK.
            include(${CMAKE_CURRENT_LIST_DIR}/CheckOtelSystemDeps.cmake)
            kairos_check_otel_system_deps()

            if(KAIROS_OTEL_SYSTEM_DEPS_OK)
                message(STATUS "opentelemetry-cpp found (system) — OTLP enabled")
                set(KAIROS_OTEL_OTLP ON CACHE BOOL "OTLP HTTP exporter available" FORCE)
                set(_use_fetchcontent FALSE)

                # System SDK exports namespaced targets.
                set(KAIROS_OTEL_TRACE_LIB   opentelemetry-cpp::trace              CACHE STRING "" FORCE)
                set(KAIROS_OTEL_OSTREAM_LIB opentelemetry-cpp::ostream_span_exporter CACHE STRING "" FORCE)
                set(KAIROS_OTEL_OTLP_LIB    opentelemetry-cpp::otlp_http_exporter CACHE STRING "" FORCE)
            else()
                message(WARNING
                    "System OTel SDK found but has broken runtime dependencies. "
                    "Falling back to FetchContent (ostream exporter, no OTLP).")
            endif()
        else()
            message(STATUS
                "KAIROS_OTEL_SYSTEM=ON but opentelemetry-cpp not found. "
                "Falling back to FetchContent.")
        endif()
    endif()

    if(_use_fetchcontent)
        message(STATUS
            "Building opentelemetry-cpp from source via FetchContent. "
            "OTLP export disabled (requires protobuf + abseil). "
            "Traces will use ostream exporter (stderr). "
            "Pass -DKAIROS_OTEL_SYSTEM=ON to use system SDK for OTLP support.")

        FetchContent_Declare(opentelemetry-cpp
            GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
            GIT_TAG        v1.14.2
            GIT_SHALLOW    TRUE
        )
        # No OTLP — avoids protobuf/abseil dependency entirely.
        set(WITH_OTLP_HTTP OFF CACHE BOOL "" FORCE)
        set(WITH_OTLP_GRPC OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(WITH_BENCHMARK OFF CACHE BOOL "" FORCE)
        set(WITH_ABSEIL OFF CACHE BOOL "" FORCE)
        set(OPENTELEMETRY_INSTALL OFF CACHE BOOL "" FORCE)

        # OTel v1.14.2 unconditionally does find_package(Protobuf) at
        # top level. If it finds protobuf >= 3.22 on the system, it
        # demands abseil-cpp — even with WITH_OTLP=OFF. Block it.
        # Also block CURL — the OTLP HTTP exporter needs it, but we
        # disabled OTLP. Without this, OTel's cmake may find a system
        # curl and link it transitively (e.g. linuxbrew's libcurl.so.4
        # which lacks version symbols → runtime warning).
        set(CMAKE_DISABLE_FIND_PACKAGE_Protobuf TRUE)
        set(CMAKE_DISABLE_FIND_PACKAGE_protobuf TRUE)
        set(CMAKE_DISABLE_FIND_PACKAGE_CURL TRUE)

        FetchContent_MakeAvailable(opentelemetry-cpp)

        # Restore package discoverability for the rest of the project.
        unset(CMAKE_DISABLE_FIND_PACKAGE_Protobuf)
        unset(CMAKE_DISABLE_FIND_PACKAGE_protobuf)
        unset(CMAKE_DISABLE_FIND_PACKAGE_CURL)
        set(KAIROS_OTEL_OTLP OFF CACHE BOOL "OTLP HTTP exporter not available" FORCE)

        # FetchContent creates non-namespaced targets.
        set(KAIROS_OTEL_TRACE_LIB       opentelemetry_trace              CACHE STRING "" FORCE)
        set(KAIROS_OTEL_OSTREAM_LIB     opentelemetry_exporter_ostream_span CACHE STRING "" FORCE)
        set(KAIROS_OTEL_OTLP_LIB        ""                               CACHE STRING "" FORCE)
    endif()

    unset(_use_fetchcontent)
endif()

# ╔════════════════════════════════════════════════════════════════════════════╗
# ║  Kairos Build Options                                                     ║
# ╚════════════════════════════════════════════════════════════════════════════╝

# Feature flags.
option(KAIROS_HTTP        "Build HTTP server and Web UI"       OFF)
option(KAIROS_OTEL        "Build with OpenTelemetry tracing"   OFF)
option(KAIROS_OTEL_SYSTEM "Prefer system-installed OTel SDK (enables OTLP). OFF = FetchContent (ostream only, no protobuf deps)" OFF)
option(KAIROS_VAULT       "Build with Ansible Vault support"   OFF)
option(KAIROS_DOCKER      "Build with Docker runner support"   OFF)
option(KAIROS_ANSIBLE     "Build with Ansible runner support"  OFF)
option(KAIROS_TUI         "Build TUI dashboard (FTXUI)"        OFF)
option(KAIROS_BUILD_TESTS "Build test suite"                   ON)

# Sanitizer support.
# Values: "" (none), "address", "undefined", "address,undefined", "thread"
set(KAIROS_SANITIZERS "" CACHE STRING
    "Comma-separated sanitizers to enable (address, undefined, thread)")

# Dependency management strategy.
option(KAIROS_USE_SYSTEM_DEPS
    "Prefer system-installed libraries over FetchContent"       OFF)

# Packaging.
option(KAIROS_EMBED_ASSETS
    "Embed web assets into binary (vs filesystem)"             ON)

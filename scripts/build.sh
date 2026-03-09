#!/usr/bin/env bash
# =============================================================================
#  ╦╔═┌─┐┬┬─┐┌─┐┌─┐
#  ╠╩╗├─┤│├┬┘│ │└─┐
#  ╩ ╩┴ ┴┴┴└─└─┘└─┘
#  Build Script — configures and compiles the Kairos orchestration daemon
# =============================================================================
#
#  Usage:
#    ./scripts/build.sh                      # Default: debug build
#    ./scripts/build.sh --release            # Release build
#    ./scripts/build.sh --profile san        # Sanitizer profile
#    ./scripts/build.sh --http               # Enable HTTP server + Web UI
#    ./scripts/build.sh --clean              # Wipe build dir first
#    ./scripts/build.sh --help               # Full option reference
#
# =============================================================================

set -euo pipefail

# ── Locate project root (parent of scripts/) ─────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Color palette ─────────────────────────────────────────────────────────────
if [[ -t 1 ]] && command -v tput &>/dev/null && [[ $(tput colors 2>/dev/null || echo 0) -ge 8 ]]; then
    C_RESET="\033[0m"
    C_BOLD="\033[1m"
    C_DIM="\033[2m"
    C_RED="\033[31m"
    C_GREEN="\033[32m"
    C_YELLOW="\033[33m"
    C_BLUE="\033[34m"
    C_MAGENTA="\033[35m"
    C_CYAN="\033[36m"
    C_WHITE="\033[37m"
    C_BG_GREEN="\033[42m"
    C_BG_RED="\033[41m"
else
    C_RESET="" C_BOLD="" C_DIM="" C_RED="" C_GREEN="" C_YELLOW=""
    C_BLUE="" C_MAGENTA="" C_CYAN="" C_WHITE="" C_BG_GREEN="" C_BG_RED=""
fi

# ── Logging helpers ───────────────────────────────────────────────────────────
_log()   { echo -e "${C_CYAN}${C_BOLD}▸${C_RESET} $*"; }
_ok()    { echo -e "${C_GREEN}${C_BOLD}✔${C_RESET} $*"; }
_warn()  { echo -e "${C_YELLOW}${C_BOLD}⚠${C_RESET} $*"; }
_err()   { echo -e "${C_RED}${C_BOLD}✘${C_RESET} $*" >&2; }
_die()   { _err "$@"; exit 1; }
_hdr()   { echo -e "\n${C_BOLD}${C_MAGENTA}━━━ $* ━━━${C_RESET}"; }

# ── Defaults ──────────────────────────────────────────────────────────────────
BUILD_TYPE="Debug"
BUILD_DIR=""                # Auto-computed if empty
ENABLE_TESTS="ON"
ENABLE_HTTP="OFF"
ENABLE_OTEL="OFF"
ENABLE_VAULT="OFF"
ENABLE_TUI="OFF"
ENABLE_ASAN="OFF"
ENABLE_UBSAN="OFF"
ENABLE_TSAN="OFF"
CLEAN=false
VERBOSE=false
DRY_RUN=false
JOBS=""                     # Empty = auto (nproc)
PROFILE=""
INSTALL_PREFIX=""
EXTRA_CMAKE_ARGS=()
COMPILER=""                 # Empty = system default

# ── Help ──────────────────────────────────────────────────────────────────────
show_help() {
    cat <<'HEADER'

  ╦╔═┌─┐┬┬─┐┌─┐┌─┐
  ╠╩╗├─┤│├┬┘│ │└─┐
  ╩ ╩┴ ┴┴┴└─└─┘└─┘
  Build Script
HEADER
    echo ""
    echo -e "  ${C_BOLD}USAGE${C_RESET}"
    echo "    ./scripts/build.sh [OPTIONS...]"
    echo ""
    echo -e "  ${C_BOLD}BUILD TYPE${C_RESET}"
    echo "    --debug                Debug build (default)"
    echo "    --release              Release build (-O2, NDEBUG)"
    echo "    --relwithdebinfo       Release with debug info (-O2 -g)"
    echo "    --minsizerel           Minimum-size release (-Os)"
    echo ""
    echo -e "  ${C_BOLD}FEATURES${C_RESET}"
    echo "    --tests / --no-tests   Build test suite (default: on)"
    echo "    --http                 Build HTTP server + Web UI"
    echo "    --otel                 Build with OpenTelemetry tracing"
    echo "    --vault                Build with Ansible Vault support (OpenSSL)"
    echo "    --tui                  Build TUI dashboard (FTXUI)"
    echo ""
    echo -e "  ${C_BOLD}SANITIZERS${C_RESET}"
    echo "    --asan                 Enable AddressSanitizer"
    echo "    --ubsan                Enable UndefinedBehaviorSanitizer"
    echo "    --tsan                 Enable ThreadSanitizer"
    echo "    --sanitizers           Enable ASan + UBSan together"
    echo ""
    echo -e "  ${C_BOLD}PROFILES${C_RESET}  ${C_DIM}(predefined option bundles)${C_RESET}"
    echo "    --profile dev          Debug + tests (default)"
    echo "    --profile san          Debug + ASan + UBSan + tests"
    echo "    --profile release      Release + no tests"
    echo "    --profile ci           Debug + tests + ASan + UBSan"
    echo "    --profile full         Release + HTTP + tests"
    echo ""
    echo -e "  ${C_BOLD}TOOLCHAIN${C_RESET}"
    echo "    --compiler gcc         Use GCC (sets CC/CXX)"
    echo "    --compiler clang       Use Clang (sets CC/CXX)"
    echo "    -j, --jobs <N>         Parallel build jobs (default: nproc)"
    echo ""
    echo -e "  ${C_BOLD}DIRECTORIES${C_RESET}"
    echo "    --build-dir <path>     Custom build directory"
    echo "    --prefix <path>        Install prefix (for --install)"
    echo ""
    echo -e "  ${C_BOLD}ACTIONS${C_RESET}"
    echo "    --clean                Remove build dir before configuring"
    echo "    --configure-only       Configure but don't build"
    echo "    --install              Build then install"
    echo "    --dry-run              Print CMake command without executing"
    echo "    --verbose              Verbose build output"
    echo "    -- <args...>           Pass extra arguments to CMake"
    echo ""
    echo -e "  ${C_BOLD}EXAMPLES${C_RESET}"
    echo "    ${C_DIM}# Quick debug build${C_RESET}"
    echo "    ./scripts/build.sh"
    echo ""
    echo "    ${C_DIM}# Release build, install to /usr/local${C_RESET}"
    echo "    ./scripts/build.sh --release --prefix /usr/local --install"
    echo ""
    echo "    ${C_DIM}# Sanitizer build${C_RESET}"
    echo "    ./scripts/build.sh --profile san"
    echo ""
    echo "    ${C_DIM}# Full-featured release${C_RESET}"
    echo "    ./scripts/build.sh --profile full"
    echo ""
    echo "    ${C_DIM}# Vault + HTTP (requires OpenSSL)${C_RESET}"
    echo "    ./scripts/build.sh --http --vault"
    echo ""
    echo "    ${C_DIM}# Clang debug, pass extra flags${C_RESET}"
    echo "    ./scripts/build.sh --compiler clang -- -DCMAKE_VERBOSE_MAKEFILE=ON"
    echo ""
}

# ── Parse arguments ───────────────────────────────────────────────────────────
CONFIGURE_ONLY=false
DO_INSTALL=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        # Build type
        --debug)            BUILD_TYPE="Debug"; shift ;;
        --release)          BUILD_TYPE="Release"; shift ;;
        --relwithdebinfo)   BUILD_TYPE="RelWithDebInfo"; shift ;;
        --minsizerel)       BUILD_TYPE="MinSizeRel"; shift ;;

        # Features
        --tests)            ENABLE_TESTS="ON"; shift ;;
        --no-tests)         ENABLE_TESTS="OFF"; shift ;;
        --http)             ENABLE_HTTP="ON"; shift ;;
        --no-http)          ENABLE_HTTP="OFF"; shift ;;
        --otel)             ENABLE_OTEL="ON"; shift ;;
        --vault)            ENABLE_VAULT="ON"; shift ;;
        --no-vault)         ENABLE_VAULT="OFF"; shift ;;
        --tui)              ENABLE_TUI="ON"; shift ;;

        # Sanitizers
        --asan)             ENABLE_ASAN="ON"; shift ;;
        --ubsan)            ENABLE_UBSAN="ON"; shift ;;
        --tsan)             ENABLE_TSAN="ON"; shift ;;
        --sanitizers)       ENABLE_ASAN="ON"; ENABLE_UBSAN="ON"; shift ;;

        # Profiles
        --profile)
            [[ $# -ge 2 ]] || _die "--profile requires a name"
            PROFILE="$2"; shift 2 ;;

        # Toolchain
        --compiler)
            [[ $# -ge 2 ]] || _die "--compiler requires gcc or clang"
            COMPILER="$2"; shift 2 ;;
        -j|--jobs)
            [[ $# -ge 2 ]] || _die "-j requires a number"
            JOBS="$2"; shift 2 ;;

        # Directories
        --build-dir)
            [[ $# -ge 2 ]] || _die "--build-dir requires a path"
            BUILD_DIR="$2"; shift 2 ;;
        --prefix)
            [[ $# -ge 2 ]] || _die "--prefix requires a path"
            INSTALL_PREFIX="$2"; shift 2 ;;

        # Actions
        --clean)            CLEAN=true; shift ;;
        --configure-only)   CONFIGURE_ONLY=true; shift ;;
        --install)          DO_INSTALL=true; shift ;;
        --dry-run)          DRY_RUN=true; shift ;;
        --verbose)          VERBOSE=true; shift ;;

        # Help
        -h|--help)          show_help; exit 0 ;;

        # Passthrough to CMake
        --)                 shift; EXTRA_CMAKE_ARGS+=("$@"); break ;;

        *)
            _die "Unknown option: $1 (try --help)" ;;
    esac
done

# ── Apply profiles ────────────────────────────────────────────────────────────
if [[ -n "$PROFILE" ]]; then
    case "$PROFILE" in
        dev)
            BUILD_TYPE="Debug"
            ENABLE_TESTS="ON"
            ;;
        san|sanitizer|sanitizers)
            BUILD_TYPE="Debug"
            ENABLE_ASAN="ON"; ENABLE_UBSAN="ON"
            ENABLE_TESTS="ON"
            ;;
        release)
            BUILD_TYPE="Release"
            ENABLE_TESTS="OFF"
            ;;
        ci)
            BUILD_TYPE="Debug"
            ENABLE_ASAN="ON"; ENABLE_UBSAN="ON"
            ENABLE_TESTS="ON"
            ;;
        full)
            BUILD_TYPE="Release"
            ENABLE_HTTP="ON"
            ENABLE_OTEL="ON"
            ENABLE_VAULT="ON"
            ENABLE_TESTS="ON"
            ;;
        full-debug)
            BUILD_TYPE="Debug"
            ENABLE_HTTP="ON"
            ENABLE_OTEL="ON"
            ENABLE_VAULT="ON"
            ENABLE_TESTS="ON"
            ;;
        full-san-debug)
            BUILD_TYPE="Debug"
            ENABLE_HTTP="ON"
            ENABLE_OTEL="ON"
            ENABLE_VAULT="ON"
            ENABLE_TESTS="ON"
            ENABLE_ASAN="ON"
            ENABLE_UBSAN="ON"
            ;;
        *)
            _die "Unknown profile: $PROFILE (choose: dev, san, release, ci, full)"
            ;;
    esac
fi

# ── Compiler setup ────────────────────────────────────────────────────────────
if [[ -n "$COMPILER" ]]; then
    case "$COMPILER" in
        gcc)    export CC=gcc CXX=g++ ;;
        clang)  export CC=clang CXX=clang++ ;;
        *)      _die "Unknown compiler: $COMPILER (choose: gcc, clang)" ;;
    esac
fi

# ── Compute build directory name ──────────────────────────────────────────────
if [[ -z "$BUILD_DIR" ]]; then
    dir_slug="$(echo "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')"
    if [[ "$ENABLE_ASAN" == "ON" || "$ENABLE_UBSAN" == "ON" || "$ENABLE_TSAN" == "ON" ]]; then
        dir_slug+="-san"
    fi
    if [[ -n "$COMPILER" ]]; then
        dir_slug+="-${COMPILER}"
    fi
    BUILD_DIR="${PROJECT_ROOT}/build/${dir_slug}"
fi

# ── Compute job count ─────────────────────────────────────────────────────────
if [[ -z "$JOBS" ]]; then
    JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi

# ── Construct CMake arguments ─────────────────────────────────────────────────
CMAKE_ARGS=(
    -S "${PROJECT_ROOT}"
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DKAIROS_BUILD_TESTS="${ENABLE_TESTS}"
    -DKAIROS_HTTP="${ENABLE_HTTP}"
    -DKAIROS_OTEL="${ENABLE_OTEL}"
    -DKAIROS_VAULT="${ENABLE_VAULT}"
    -DKAIROS_TUI="${ENABLE_TUI}"
)

# Sanitizer flags
if [[ "$ENABLE_ASAN" == "ON" ]]; then
    CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer")
    CMAKE_ARGS+=(-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer")
    CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address")
fi
if [[ "$ENABLE_UBSAN" == "ON" ]]; then
    CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS:-} -fsanitize=undefined")
    CMAKE_ARGS+=(-DCMAKE_C_FLAGS="${CMAKE_C_FLAGS:-} -fsanitize=undefined")
    CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS="${CMAKE_EXE_LINKER_FLAGS:-} -fsanitize=undefined")
fi
if [[ "$ENABLE_TSAN" == "ON" ]]; then
    CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="-fsanitize=thread")
    CMAKE_ARGS+=(-DCMAKE_C_FLAGS="-fsanitize=thread")
    CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread")
fi

if [[ -n "$INSTALL_PREFIX" ]]; then
    CMAKE_ARGS+=(-DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}")
fi

CMAKE_ARGS+=("${EXTRA_CMAKE_ARGS[@]}")

BUILD_ARGS=(--build "${BUILD_DIR}" -j "${JOBS}")
if [[ "$VERBOSE" == true ]]; then
    BUILD_ARGS+=(--verbose)
fi

# ── Banner ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${C_BOLD}${C_CYAN}  ╦╔═┌─┐┬┬─┐┌─┐┌─┐${C_RESET}"
echo -e "${C_BOLD}${C_CYAN}  ╠╩╗├─┤│├┬┘│ │└─┐${C_RESET}"
echo -e "${C_BOLD}${C_CYAN}  ╩ ╩┴ ┴┴┴└─└─┘└─┘${C_RESET}"
echo -e "  ${C_DIM}Build System${C_RESET}"
echo ""

# ── Configuration summary ────────────────────────────────────────────────────
_hdr "Configuration"

_cfg() {
    local label="$1" value="$2" color="${3:-$C_WHITE}"
    printf "  ${C_DIM}%-20s${C_RESET} ${color}%s${C_RESET}\n" "$label" "$value"
}

_cfg "Build type" "$BUILD_TYPE" "$C_BOLD"
_cfg "Build directory" "$BUILD_DIR"
_cfg "Parallel jobs" "$JOBS"

if [[ -n "$COMPILER" ]]; then
    _cfg "Compiler" "$COMPILER" "$C_YELLOW"
fi

# Features
features=""
[[ "$ENABLE_TESTS" == "ON" ]] && features+="tests "
[[ "$ENABLE_HTTP"  == "ON" ]] && features+="http "
[[ "$ENABLE_OTEL"  == "ON" ]] && features+="otel "
[[ "$ENABLE_VAULT" == "ON" ]] && features+="vault "
[[ "$ENABLE_TUI"   == "ON" ]] && features+="tui "
_cfg "Features" "${features:-none}"

# Sanitizers
san_list=""
[[ "$ENABLE_ASAN"  == "ON" ]] && san_list+="ASan "
[[ "$ENABLE_UBSAN" == "ON" ]] && san_list+="UBSan "
[[ "$ENABLE_TSAN"  == "ON" ]] && san_list+="TSan "
if [[ -n "$san_list" ]]; then
    _cfg "Sanitizers" "$san_list" "$C_RED"
else
    _cfg "Sanitizers" "none" "$C_DIM"
fi

if [[ -n "$PROFILE" ]]; then
    _cfg "Profile" "$PROFILE" "$C_MAGENTA"
fi

echo ""

# ── Dry-run mode ──────────────────────────────────────────────────────────────
if [[ "$DRY_RUN" == true ]]; then
    _hdr "Dry Run — commands that would execute"
    echo ""
    if [[ "$CLEAN" == true ]]; then
        echo -e "  ${C_DIM}rm -rf${C_RESET} ${BUILD_DIR}"
    fi
    echo -e "  ${C_DIM}cmake${C_RESET} ${CMAKE_ARGS[*]}"
    if [[ "$CONFIGURE_ONLY" != true ]]; then
        echo -e "  ${C_DIM}cmake${C_RESET} ${BUILD_ARGS[*]}"
    fi
    if [[ "$DO_INSTALL" == true ]]; then
        echo -e "  ${C_DIM}cmake${C_RESET} --install ${BUILD_DIR}"
    fi
    echo ""
    exit 0
fi

# ── Clean ─────────────────────────────────────────────────────────────────────
if [[ "$CLEAN" == true ]]; then
    if [[ -d "$BUILD_DIR" ]]; then
        _log "Cleaning ${BUILD_DIR}..."
        rm -rf "$BUILD_DIR"
        _ok "Clean complete"
    else
        _warn "Build directory doesn't exist, nothing to clean"
    fi
fi

# ── Configure ─────────────────────────────────────────────────────────────────
_hdr "Configure"

SECONDS=0
if ! cmake "${CMAKE_ARGS[@]}" 2>&1 | while IFS= read -r line; do
    echo -e "  ${C_DIM}${line}${C_RESET}"
done; then
    _die "CMake configuration failed"
fi
config_time=$SECONDS
_ok "Configured in ${config_time}s"

if [[ "$CONFIGURE_ONLY" == true ]]; then
    echo ""
    _ok "Configure-only mode — skipping build"
    echo -e "  ${C_DIM}Build dir: ${BUILD_DIR}${C_RESET}"
    echo ""
    exit 0
fi

# ── Build ─────────────────────────────────────────────────────────────────────
_hdr "Build"

SECONDS=0
if cmake "${BUILD_ARGS[@]}" 2>&1 | while IFS= read -r line; do
    if [[ "$line" == *"error:"* || "$line" == *"Error"* ]]; then
        echo -e "  ${C_RED}${line}${C_RESET}"
    elif [[ "$line" == *"warning:"* ]]; then
        echo -e "  ${C_YELLOW}${line}${C_RESET}"
    elif [[ "$line" == *"Building"* || "$line" == *"Linking"* || "$line" == *"Built target"* ]]; then
        echo -e "  ${C_GREEN}${line}${C_RESET}"
    else
        echo -e "  ${C_DIM}${line}${C_RESET}"
    fi
done; then
    build_time=$SECONDS
else
    _die "Build failed after ${SECONDS}s"
fi
_ok "Built in ${build_time}s"

# ── Install (optional) ───────────────────────────────────────────────────────
if [[ "$DO_INSTALL" == true ]]; then
    _hdr "Install"
    local_prefix="${INSTALL_PREFIX:-/usr/local}"
    _log "Installing to ${local_prefix}..."
    cmake --install "${BUILD_DIR}" 2>&1 | while IFS= read -r line; do
        echo -e "  ${C_DIM}${line}${C_RESET}"
    done
    _ok "Installed"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo -e "${C_BG_GREEN}${C_BOLD}${C_WHITE} BUILD SUCCEEDED ${C_RESET}"
echo ""
echo -e "  ${C_DIM}Build type   ${C_RESET} ${BUILD_TYPE}"
echo -e "  ${C_DIM}Build dir    ${C_RESET} ${BUILD_DIR}"
echo -e "  ${C_DIM}Config time  ${C_RESET} ${config_time}s"
echo -e "  ${C_DIM}Build time   ${C_RESET} ${build_time}s"

# Show binary locations
if [[ -f "${BUILD_DIR}/kairos" ]]; then
    echo -e "  ${C_DIM}Binary       ${C_RESET} ${BUILD_DIR}/kairos"
fi

if [[ "$ENABLE_TESTS" == "ON" ]]; then
    test_count=$(find "${BUILD_DIR}" -maxdepth 3 -name 'test_*' -executable -type f 2>/dev/null | wc -l)
    echo -e "  ${C_DIM}Test binaries${C_RESET} ${test_count} executables"
fi

echo ""
echo -e "  ${C_DIM}Next steps:${C_RESET}"
echo -e "    ${C_CYAN}./scripts/test.sh${C_RESET}                 # Run all tests"
echo -e "    ${C_CYAN}${BUILD_DIR}/kairos version${C_RESET}"
echo -e "    ${C_CYAN}${BUILD_DIR}/kairos start --config deploy/kairos.toml.example${C_RESET}"
echo ""

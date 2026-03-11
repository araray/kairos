#!/usr/bin/env bash
# =============================================================================
#  ╦╔═┌─┐┬┬─┐┌─┐┌─┐
#  ╠╩╗├─┤│├┬┘│ │└─┐
#  ╩ ╩┴ ┴┴┴└─└─┘└─┘
#  Test Runner — run, filter, and inspect the Kairos test suite
# =============================================================================
#
#  Usage:
#    ./scripts/test.sh                          # Run all tests
#    ./scripts/test.sh --filter "Sha256"        # Tests matching regex
#    ./scripts/test.sh --list                   # List all tests without running
#    ./scripts/test.sh --san                    # Run against sanitizer build
#    ./scripts/test.sh --help                   # Full option reference
#
# =============================================================================

set -euo pipefail

# ── Locate project root ──────────────────────────────────────────────────────
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
BUILD_DIR=""                # Auto-detect
FILTER=""                   # CTest -R regex
EXCLUDE=""                  # CTest -E regex
VERBOSE=false
LIST_ONLY=false
OUTPUT_ON_FAILURE=true
REPEAT=1
USE_SAN_BUILD=false
USE_RELEASE_BUILD=false
PARALLEL=""                 # CTest --parallel
STOP_ON_FAIL=false
RERUN_FAILED=false
DRY_RUN=false

# ── Help ──────────────────────────────────────────────────────────────────────
show_help() {
    cat <<'HEADER'

  ╦╔═┌─┐┬┬─┐┌─┐┌─┐
  ╠╩╗├─┤│├┬┘│ │└─┐
  ╩ ╩┴ ┴┴┴└─└─┘└─┘
  Test Runner
HEADER
    echo ""
    echo -e "  ${C_BOLD}USAGE${C_RESET}"
    echo "    ./scripts/test.sh [OPTIONS...]"
    echo ""
    echo -e "  ${C_BOLD}NAME FILTERS${C_RESET}"
    echo "    -R, --filter <regex>   Only tests whose name matches regex"
    echo "    -E, --exclude <regex>  Exclude tests whose name matches regex"
    echo ""
    echo -e "  ${C_BOLD}SUITE SHORTCUTS${C_RESET}  ${C_DIM}(convenience filters)${C_RESET}"
    echo "    --tui                  Run TUI dashboard tests only"
    echo "    --kel                  Run KEL (expression language) tests only"
    echo "    --watch                Run watch engine tests only"
    echo "    --mcp                  Run MCP server tests only"
    echo "    --engine               Run engine (pipeline/DAG/scheduler) tests only"
    echo "    --migration            Run migration tool tests only"
    echo "    --e2e                  Run end-to-end tests only"
    echo ""
    echo -e "  ${C_BOLD}BUILD SELECTION${C_RESET}"
    echo "    --build-dir <path>     Explicit build directory"
    echo "    --san                  Use sanitizer build (build/debug-san)"
    echo "    --release              Use release build (build/release)"
    echo "    --clear-cache          Clear cached build directory selection"
    echo ""
    echo -e "  ${C_BOLD}EXECUTION${C_RESET}"
    echo "    --repeat <N>           Repeat each test N times"
    echo "    --parallel <N>         Run N tests in parallel"
    echo "    --stop-on-fail         Stop on first failure (CTest --stop-on-failure)"
    echo "    --rerun-failed         Only rerun previously-failed tests"
    echo ""
    echo -e "  ${C_BOLD}OUTPUT${C_RESET}"
    echo "    -v, --verbose          Show all test output, not just failures"
    echo "    -q, --quiet            Suppress test output on failure too"
    echo "    --list                 List matching tests without running"
    echo "    --dry-run              Print CTest command without executing"
    echo ""
    echo -e "  ${C_BOLD}EXAMPLES${C_RESET}"
    echo "    ${C_DIM}# Run everything${C_RESET}"
    echo "    ./scripts/test.sh"
    echo ""
    echo "    ${C_DIM}# Only SHA-256 and ID tests${C_RESET}"
    echo "    ./scripts/test.sh --filter 'Sha256|ContentId'"
    echo ""
    echo "    ${C_DIM}# Sanitizer build, stop on first failure${C_RESET}"
    echo "    ./scripts/test.sh --san --stop-on-fail"
    echo ""
    echo "    ${C_DIM}# List all tests${C_RESET}"
    echo "    ./scripts/test.sh --list"
    echo ""
    echo "    ${C_DIM}# Stress-test: repeat all tests 10 times${C_RESET}"
    echo "    ./scripts/test.sh --repeat 10"
    echo ""
    echo "    ${C_DIM}# Run a single test by name${C_RESET}"
    echo "    ./scripts/test.sh --filter 'SchemaTest.AllTablesExist'"
    echo ""
    echo "    ${C_DIM}# Run TUI dashboard tests${C_RESET}"
    echo "    ./scripts/test.sh --tui"
    echo ""
}

# ── Parse arguments ───────────────────────────────────────────────────────────
BUILD_CACHE="${PROJECT_ROOT}/.builddir_cache"

while [[ $# -gt 0 ]]; do
    case "$1" in
        # Name filters
        -R|--filter)
            [[ $# -ge 2 ]] || _die "--filter requires a regex"
            FILTER="$2"; shift 2 ;;
        -E|--exclude)
            [[ $# -ge 2 ]] || _die "--exclude requires a regex"
            EXCLUDE="$2"; shift 2 ;;

        # Suite shortcuts (convenience)
        --tui)              FILTER="Tui"; shift ;;
        --kel)              FILTER="Kel|Lexer|Parser|Evaluator"; shift ;;
        --watch)            FILTER="Watch|Scan|Inotify|Debounce|Hash"; shift ;;
        --mcp)              FILTER="Mcp"; shift ;;
        --engine)           FILTER="Pipeline|Dag|Scheduler|Trigger|Execution"; shift ;;
        --migration)        FILTER="Migration"; shift ;;
        --e2e)              FILTER="E2E|DaemonLifecycle"; shift ;;

        # Build selection
        --build-dir)
            [[ $# -ge 2 ]] || _die "--build-dir requires a path"
            BUILD_DIR="$2"; shift 2 ;;
        --san|--sanitizer)
            USE_SAN_BUILD=true; shift ;;
        --release)
            USE_RELEASE_BUILD=true; shift ;;
        --clear-cache)
            if [[ -f "$BUILD_CACHE" ]]; then
                rm -f "$BUILD_CACHE"
                _ok "Build directory cache cleared: ${BUILD_CACHE}"
            else
                _log "No build directory cache to clear."
            fi
            exit 0 ;;

        # Execution
        --repeat)
            [[ $# -ge 2 ]] || _die "--repeat requires a number"
            REPEAT="$2"; shift 2 ;;
        --parallel)
            [[ $# -ge 2 ]] || _die "--parallel requires a number"
            PARALLEL="$2"; shift 2 ;;
        --stop-on-fail)     STOP_ON_FAIL=true; shift ;;
        --rerun-failed)     RERUN_FAILED=true; shift ;;

        # Output
        -v|--verbose)       VERBOSE=true; shift ;;
        -q|--quiet)         OUTPUT_ON_FAILURE=false; shift ;;
        --list)             LIST_ONLY=true; shift ;;
        --dry-run)          DRY_RUN=true; shift ;;

        # Help
        -h|--help)          show_help; exit 0 ;;

        *)
            _die "Unknown option: $1 (try --help)" ;;
    esac
done

# ── Discover build directories ───────────────────────────────────────────────
discover_build_dirs() {
    local dirs=()
    if [[ -d "${PROJECT_ROOT}/build" ]]; then
        while IFS= read -r -d '' cache_file; do
            dir="$(dirname "$cache_file")"
            dirs+=("$dir")
        done < <(find "${PROJECT_ROOT}/build" -maxdepth 2 -name CMakeCache.txt -print0 2>/dev/null)
    fi
    printf '%s\n' "${dirs[@]}"
}

select_build_dir() {
    local dirs=("$@")
    local count=${#dirs[@]}

    if [[ $count -eq 0 ]]; then
        return 1
    fi

    # ── IMPORTANT ──────────────────────────────────────────────────
    # This function is called via command substitution:
    #   BUILD_DIR=$(select_build_dir ...)
    # Therefore ALL interactive output (menu, prompts, errors) MUST
    # go to stderr (>&2). Only the final selected path goes to stdout.
    # ───────────────────────────────────────────────────────────────

    echo "" >&2
    echo -e "${C_BOLD}Multiple build directories found:${C_RESET}" >&2
    echo "" >&2
    for i in "${!dirs[@]}"; do
        local dir="${dirs[$i]}"
        local rel_dir="${dir#${PROJECT_ROOT}/}"
        local build_type=""

        if [[ -f "${dir}/CMakeCache.txt" ]]; then
            build_type=$(grep "^CMAKE_BUILD_TYPE:" "${dir}/CMakeCache.txt" 2>/dev/null | cut -d'=' -f2 || echo "")
        fi

        local marker=""
        if [[ -f "$BUILD_CACHE" ]] && [[ "$(cat "$BUILD_CACHE")" == "$dir" ]]; then
            marker=" ${C_GREEN}(last used)${C_RESET}"
        fi

        if [[ -n "$build_type" ]]; then
            echo -e "  ${C_CYAN}$((i + 1))${C_RESET}) ${C_BOLD}${rel_dir}${C_RESET}  ${C_DIM}[${build_type}]${C_RESET}${marker}" >&2
        else
            echo -e "  ${C_CYAN}$((i + 1))${C_RESET}) ${C_BOLD}${rel_dir}${C_RESET}${marker}" >&2
        fi
    done
    echo "" >&2

    local choice
    while true; do
        echo -ne "${C_YELLOW}Select build directory${C_RESET} (1-${count}, or ${C_DIM}Enter${C_RESET} for last used): " >&2
        read -r choice

        # Enter with a valid cache → use cached directory.
        if [[ -z "$choice" ]] && [[ -f "$BUILD_CACHE" ]]; then
            local cached_dir
            cached_dir="$(cat "$BUILD_CACHE")"
            for dir in "${dirs[@]}"; do
                if [[ "$dir" == "$cached_dir" ]]; then
                    echo "$cached_dir"  # → stdout (captured by caller)
                    return 0
                fi
            done
            echo -e "${C_YELLOW}Cached directory no longer valid, please select manually.${C_RESET}" >&2
            continue
        fi

        # Enter with no cache → pick the first directory.
        if [[ -z "$choice" ]] && [[ ! -f "$BUILD_CACHE" ]]; then
            local selected="${dirs[0]}"
            echo "$selected" > "$BUILD_CACHE"
            echo "$selected"  # → stdout
            return 0
        fi

        # Numeric selection.
        if [[ "$choice" =~ ^[0-9]+$ ]] && [[ "$choice" -ge 1 ]] && [[ "$choice" -le "$count" ]]; then
            local selected="${dirs[$((choice - 1))]}"
            echo "$selected" > "$BUILD_CACHE"
            echo "$selected"  # → stdout
            return 0
        fi

        echo -e "${C_RED}Invalid selection. Enter a number between 1 and ${count}.${C_RESET}" >&2
    done
}

# ── Find build directory ─────────────────────────────────────────────────────
if [[ -n "$BUILD_DIR" ]]; then
    : # Already set via --build-dir
elif [[ "$USE_SAN_BUILD" == true ]]; then
    san_candidates=(
        "${PROJECT_ROOT}/build/debug-san"
        "${PROJECT_ROOT}/build/debug-san-gcc"
        "${PROJECT_ROOT}/build/debug-san-clang"
    )
    BUILD_DIR=""
    for d in "${san_candidates[@]}"; do
        if [[ -f "${d}/CTestTestfile.cmake" ]]; then
            BUILD_DIR="$d"
            break
        fi
    done
    if [[ -z "$BUILD_DIR" ]]; then
        _err "No sanitizer build directory found."
        echo -e "  ${C_YELLOW}Hint:${C_RESET} Run ${C_BOLD}./scripts/build.sh --profile san${C_RESET} first."
        exit 1
    fi
elif [[ "$USE_RELEASE_BUILD" == true ]]; then
    BUILD_DIR="${PROJECT_ROOT}/build/release"
else
    mapfile -t discovered_dirs < <(discover_build_dirs)

    if [[ ${#discovered_dirs[@]} -eq 0 ]]; then
        _err "No build directory found. Run ./scripts/build.sh first."
        echo -e "  ${C_YELLOW}Hint:${C_RESET} No directories with CMakeCache.txt found under ${C_BOLD}${PROJECT_ROOT}/build${C_RESET}"
        exit 1
    elif [[ ${#discovered_dirs[@]} -eq 1 ]]; then
        BUILD_DIR="${discovered_dirs[0]}"
        echo "$BUILD_DIR" > "$BUILD_CACHE"
    else
        BUILD_DIR=$(select_build_dir "${discovered_dirs[@]}")
        if [[ -z "$BUILD_DIR" ]]; then
            _die "No build directory selected."
        fi
    fi
fi

# Validate
if [[ ! -f "${BUILD_DIR}/CTestTestfile.cmake" ]]; then
    _err "Build directory is invalid: ${BUILD_DIR}"
    echo -e "  ${C_DIM}Missing CTestTestfile.cmake${C_RESET}"
    echo ""
    if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        echo -e "  ${C_YELLOW}Hint:${C_RESET} Directory exists but tests were not configured."
        echo -e "        Run ${C_BOLD}./scripts/build.sh --tests${C_RESET} first."
    else
        echo -e "  ${C_YELLOW}Hint:${C_RESET} Run ${C_BOLD}./scripts/build.sh${C_RESET} first."
    fi
    exit 1
fi

# ── Construct CTest arguments ────────────────────────────────────────────────
CTEST_ARGS=(--test-dir "${BUILD_DIR}")

if [[ -n "$FILTER" ]]; then
    CTEST_ARGS+=(-R "$FILTER")
fi
if [[ -n "$EXCLUDE" ]]; then
    CTEST_ARGS+=(-E "$EXCLUDE")
fi

if [[ "$VERBOSE" == true ]]; then
    CTEST_ARGS+=(-V)
elif [[ "$OUTPUT_ON_FAILURE" == true ]]; then
    CTEST_ARGS+=(--output-on-failure)
fi

if [[ "$REPEAT" -gt 1 ]]; then
    CTEST_ARGS+=(--repeat until-fail:"${REPEAT}")
fi
if [[ -n "$PARALLEL" ]]; then
    CTEST_ARGS+=(--parallel "$PARALLEL")
fi
if [[ "$STOP_ON_FAIL" == true ]]; then
    CTEST_ARGS+=(--stop-on-failure)
fi
if [[ "$RERUN_FAILED" == true ]]; then
    CTEST_ARGS+=(--rerun-failed)
fi

# ── Banner ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${C_BOLD}${C_CYAN}  ╦╔═┌─┐┬┬─┐┌─┐┌─┐${C_RESET}"
echo -e "${C_BOLD}${C_CYAN}  ╠╩╗├─┤│├┬┘│ │└─┐${C_RESET}"
echo -e "${C_BOLD}${C_CYAN}  ╩ ╩┴ ┴┴┴└─└─┘└─┘${C_RESET}"
echo -e "  ${C_DIM}Test Runner${C_RESET}"
echo ""

# ── Configuration summary ────────────────────────────────────────────────────
_cfg() {
    local label="$1" value="$2" color="${3:-$C_WHITE}"
    printf "  ${C_DIM}%-20s${C_RESET} ${color}%s${C_RESET}\n" "$label" "$value"
}

BUILD_DIR_DISPLAY="$BUILD_DIR"
if [[ -f "$BUILD_CACHE" ]] && [[ "$(cat "$BUILD_CACHE" 2>/dev/null)" == "$BUILD_DIR" ]]; then
    BUILD_DIR_DISPLAY="${BUILD_DIR} ${C_DIM}(cached)${C_RESET}"
fi
_cfg "Build directory" "$BUILD_DIR_DISPLAY"

[[ -n "$FILTER" ]]  && _cfg "Name filter" "$FILTER" "$C_YELLOW"
[[ -n "$EXCLUDE" ]] && _cfg "Exclude" "$EXCLUDE" "$C_YELLOW"

if [[ "$USE_SAN_BUILD" == true ]]; then
    _cfg "Sanitizers" "enabled (ASan/UBSan)" "$C_RED"
fi

if [[ "$REPEAT" -gt 1 ]]; then
    _cfg "Repeat" "${REPEAT}x" "$C_MAGENTA"
fi

# ── List mode ─────────────────────────────────────────────────────────────────
if [[ "$LIST_ONLY" == true ]]; then
    _hdr "Available Tests"
    echo ""

    test_count=0
    while IFS= read -r line; do
        if [[ "$line" =~ ^[[:space:]]*Test[[:space:]]+#([0-9]+):[[:space:]]+(.+) ]]; then
            num="${BASH_REMATCH[1]}"
            name="${BASH_REMATCH[2]}"
            test_count=$((test_count + 1))

            # Color by test domain (infer from name)
            if [[ "$name" == *Sha256* || "$name" == *Hex* || "$name" == *ContentId* || "$name" == *RunId* || "$name" == *CorrelationId* ]]; then
                color="$C_BLUE"; badge="id  "
            elif [[ "$name" == *Config* || "$name" == *Validation* ]]; then
                color="$C_GREEN"; badge="cfg "
            elif [[ "$name" == *Path* || "$name" == *Platform* ]]; then
                color="$C_MAGENTA"; badge="plat"
            elif [[ "$name" == *Schema* || "$name" == *Migration* || "$name" == *Database* ]]; then
                color="$C_YELLOW"; badge="db  "
            elif [[ "$name" == *Json* || "$name" == *Log* ]]; then
                color="$C_CYAN"; badge="log "
            elif [[ "$name" == *Exit* ]]; then
                color="$C_RED"; badge="exit"
            elif [[ "$name" == *Tui* || "$name" == *Dashboard* ]]; then
                color="$C_MAGENTA"; badge="tui "
            elif [[ "$name" == *Mcp* ]]; then
                color="$C_CYAN"; badge="mcp "
            elif [[ "$name" == *Watch* || "$name" == *Scan* || "$name" == *Inotify* ]]; then
                color="$C_GREEN"; badge="wtch"
            elif [[ "$name" == *Kel* || "$name" == *Lexer* || "$name" == *Parser* || "$name" == *Evaluator* ]]; then
                color="$C_BLUE"; badge="kel "
            elif [[ "$name" == *Pipeline* || "$name" == *Dag* || "$name" == *Scheduler* || "$name" == *Trigger* ]]; then
                color="$C_YELLOW"; badge="eng "
            elif [[ "$name" == *Runner* || "$name" == *Process* || "$name" == *Docker* ]]; then
                color="$C_RED"; badge="exec"
            else
                color="$C_WHITE"; badge="    "
            fi

            printf "  ${C_DIM}%3s${C_RESET}  ${color}%-4s${C_RESET}  %s\n" "$num" "$badge" "$name"
        fi
    done < <(ctest --test-dir "${BUILD_DIR}" -N \
        ${FILTER:+-R "$FILTER"} \
        ${EXCLUDE:+-E "$EXCLUDE"} \
        2>&1 || true)

    echo ""
    echo -e "  ${C_BOLD}${test_count}${C_RESET} test(s) matched"
    echo ""
    exit 0
fi

# ── Dry-run mode ──────────────────────────────────────────────────────────────
if [[ "$DRY_RUN" == true ]]; then
    _hdr "Dry Run — command that would execute"
    echo ""
    echo -e "  ${C_DIM}ctest${C_RESET} ${CTEST_ARGS[*]}"
    echo ""
    exit 0
fi

# ── Colorized CTest output ───────────────────────────────────────────────────
colorize_ctest() {
    while IFS= read -r line; do
        if [[ "$line" =~ Passed ]]; then
            echo -e "  ${C_GREEN}${line}${C_RESET}"
        elif [[ "$line" =~ Failed|FAILED|Error ]]; then
            echo -e "  ${C_RED}${C_BOLD}${line}${C_RESET}"
        elif [[ "$line" =~ "***" ]]; then
            echo -e "  ${C_RED}${line}${C_RESET}"
        elif [[ "$line" =~ ^[[:space:]]*[0-9]+/[0-9]+ ]]; then
            if [[ "$line" == *"Passed"* ]]; then
                echo -e "  ${C_GREEN}${line}${C_RESET}"
            elif [[ "$line" == *"Failed"* || "$line" == *"FAILED"* ]]; then
                echo -e "  ${C_RED}${C_BOLD}${line}${C_RESET}"
            else
                echo -e "  ${C_DIM}${line}${C_RESET}"
            fi
        elif [[ "$line" =~ "100% tests passed" ]]; then
            echo -e "  ${C_GREEN}${C_BOLD}${line}${C_RESET}"
        elif [[ "$line" =~ "tests passed" ]]; then
            echo -e "  ${C_YELLOW}${C_BOLD}${line}${C_RESET}"
        elif [[ "$line" =~ "Total Test time" ]]; then
            echo -e "  ${C_CYAN}${line}${C_RESET}"
        else
            echo -e "  ${C_DIM}${line}${C_RESET}"
        fi
    done
}

# ── Run tests ─────────────────────────────────────────────────────────────────
_hdr "Running Tests"
echo ""

SECONDS=0
overall_exit=0
if ctest "${CTEST_ARGS[@]}" 2>&1 | colorize_ctest; then
    :
else
    overall_exit=1
fi
elapsed=$SECONDS

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
if [[ $overall_exit -eq 0 ]]; then
    echo -e "${C_BG_GREEN}${C_BOLD}${C_WHITE} ALL TESTS PASSED ${C_RESET}  ${C_DIM}(${elapsed}s)${C_RESET}"
else
    echo -e "${C_BG_RED}${C_BOLD}${C_WHITE} SOME TESTS FAILED ${C_RESET}  ${C_DIM}(${elapsed}s)${C_RESET}"
fi

echo ""
echo -e "  ${C_DIM}Build dir    ${C_RESET} ${BUILD_DIR}"
echo -e "  ${C_DIM}Elapsed      ${C_RESET} ${elapsed}s"
[[ -n "$FILTER" ]]  && echo -e "  ${C_DIM}Filter       ${C_RESET} ${FILTER}"
[[ -n "$EXCLUDE" ]] && echo -e "  ${C_DIM}Exclude      ${C_RESET} ${EXCLUDE}"
echo ""

if [[ $overall_exit -ne 0 ]]; then
    echo -e "  ${C_DIM}Rerun failures:${C_RESET}  ./scripts/test.sh --rerun-failed"
    echo -e "  ${C_DIM}Verbose rerun:${C_RESET}   ./scripts/test.sh --rerun-failed -v"
    echo ""
fi

exit $overall_exit

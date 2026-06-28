#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPM_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# SPM debug runner.
#
# Usage:
#   tools/run_debug.sh
#   tools/run_debug.sh --trace
#   tools/run_debug.sh --trace-exec
#   tools/run_debug.sh --xinsn
#   tools/run_debug.sh --binary ./test/other.c
#   tools/run_debug.sh --max-tick 5000000000

cd "$SPM_DIR"

SRC="./test/spm_step_test.c"
XINSN=0
TRACE=0
TRACE_EXEC=0
MAX_TICK=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --xinsn)        XINSN=1; shift ;;
        --trace)        TRACE=1; shift ;;
        --trace-exec)   TRACE=1; TRACE_EXEC=1; shift ;;
        --max-tick)     MAX_TICK="$2"; shift 2 ;;
        --binary)       SRC="$2"; shift 2 ;;
        *)              echo "Unknown option: $1"; exit 1 ;;
    esac
done

BIN="${SRC%.c}"

CFLAGS=(-O2 -static)
if [[ "$XINSN" -eq 1 ]]; then
    CFLAGS+=(-DUSE_XSPM_INSN)
fi

echo "=== Compiling: $SRC ==="
riscv64-unknown-linux-gnu-gcc "${CFLAGS[@]}" -o "$BIN" "$SRC"

GEM5_ARGS=()
if [[ "$TRACE" -eq 1 ]]; then
    DEBUG_FLAGS="SpmDma,ScratchpadMem"
    if [[ "$TRACE_EXEC" -eq 1 ]]; then
        DEBUG_FLAGS="$DEBUG_FLAGS,Exec"
    fi
    GEM5_ARGS+=(--debug-flags="$DEBUG_FLAGS" --debug-file=debug_trace.txt)
    echo "=== Debug trace -> m5out/debug_trace.txt ==="
fi

RUN_ARGS=(run_spm.py --binary "$BIN")
if [[ -n "$MAX_TICK" ]]; then
    RUN_ARGS+=(--max-tick "$MAX_TICK")
fi

echo "=== Running gem5 ==="
gem5.opt "${GEM5_ARGS[@]}" "${RUN_ARGS[@]}"

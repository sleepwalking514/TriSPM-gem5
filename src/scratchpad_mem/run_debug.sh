#!/bin/bash
set -e

# ------------------------------------------------------------------
# SPM debug runner
#
# Usage:
#   ./run_debug.sh                          # run step test, no DMA tracing
#   ./run_debug.sh --trace                  # enable SpmDma + ScratchpadMem trace
#   ./run_debug.sh --trace-exec             # also dump every instruction (BIG output)
#   ./run_debug.sh --xinsn                  # compile with USE_XSPM_INSN
#   ./run_debug.sh --binary ./test/other.c  # compile and run a different test
#   ./run_debug.sh --max-tick 5000000000    # stop after N ticks
# ------------------------------------------------------------------

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

CFLAGS="-O2 -static"
if [ "$XINSN" -eq 1 ]; then
    CFLAGS="$CFLAGS -DUSE_XSPM_INSN"
fi

echo "=== Compiling: $SRC ==="
riscv64-unknown-linux-gnu-gcc $CFLAGS -o "$BIN" "$SRC"

GEM5_FLAGS=""
if [ "$TRACE" -eq 1 ]; then
    DEBUG_FLAGS="SpmDma,ScratchpadMem"
    if [ "$TRACE_EXEC" -eq 1 ]; then
        DEBUG_FLAGS="$DEBUG_FLAGS,Exec"
    fi
    GEM5_FLAGS="--debug-flags=$DEBUG_FLAGS --debug-file=debug_trace.txt"
    echo "=== Debug trace -> m5out/debug_trace.txt ==="
fi

PYFLAGS="--binary $BIN --spm_size 64KiB"

echo "=== Running gem5 ==="
if [ -n "$MAX_TICK" ]; then
    echo "=== Max tick: $MAX_TICK ==="
    eval gem5.opt $GEM5_FLAGS run_spm.py $PYFLAGS --max-tick "$MAX_TICK"
else
    eval gem5.opt $GEM5_FLAGS run_spm.py $PYFLAGS
fi

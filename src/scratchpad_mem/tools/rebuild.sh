#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPM_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GEM5_ROOT="$(cd "$SPM_DIR/../.." && pwd)"

cd "$GEM5_ROOT"
scons build/RISCV/gem5.opt -j"$(nproc)"

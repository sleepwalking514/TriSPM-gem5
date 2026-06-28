#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPM_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$SPM_DIR"

MAT_N=${1:-32}
MAT_BS=${2:-16}
SPM_SIZE=${SPM_SIZE:-128KiB}
TAG="${MAT_N}_${MAT_BS}"

echo "===== N=${MAT_N} BS=${MAT_BS} SPM_SIZE=${SPM_SIZE} ====="

mkdir -p m5out

cd test
riscv64-unknown-linux-gnu-gcc -O3 -static \
    -DN="${MAT_N}" -DBS="${MAT_BS}" \
    -o cache_gemm ./cache_gemm.c

riscv64-unknown-linux-gnu-gcc -O3 -static \
    -DN="${MAT_N}" -DBS="${MAT_BS}" \
    -o spm_gemm_2d ./spm_gemm_2d.c
cd ..

echo "--- cache baseline ---"
gem5.opt run_spm.py --binary ./test/cache_gemm --cache_baseline
mv m5out/stats.txt "m5out/cache_gemm_wo_spm_${TAG}.txt"

echo "--- SPM GEMM, 2D DMA ---"
gem5.opt run_spm.py --binary ./test/spm_gemm_2d --spm_size "$SPM_SIZE"
mv m5out/stats.txt "m5out/spm_gemm_2d_${TAG}.txt"

echo "===== done ====="

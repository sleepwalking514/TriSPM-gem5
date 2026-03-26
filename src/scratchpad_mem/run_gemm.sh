#!/bin/bash
set -e

# ============================================================
# 用法:  bash run_gemm.sh [N] [BS]
#   默认 N=32 BS=16
#   示例:  bash run_gemm.sh 256 32
# ============================================================

MAT_N=${1:-32}
MAT_BS=${2:-16}
TAG="${MAT_N}_${MAT_BS}"

echo "===== N=${MAT_N}  BS=${MAT_BS} ====="

# ---------- 编译 ----------
cd test

# Cache baseline (N 和 BS 在 cache_gemm.c 中也可用 -D 传入)
riscv64-unknown-linux-gnu-gcc -O3 -static \
    -DN=${MAT_N} -DBS=${MAT_BS} \
    -o cache_gemm ./cache_gemm.c

# SPM + custom instructions
riscv64-unknown-linux-gnu-gcc -O3 -static \
    -DN=${MAT_N} -DBS=${MAT_BS} \
    -o spm_gemmX ./spm_gemmX.c

cd ..

# ---------- 运行 ----------

# # Cache baseline (关闭 SPM 系统)
# echo "--- cache baseline ---"
# gem5.opt run_spm.py --binary ./test/cache_gemm --cache_baseline
# mv m5out/stats.txt "m5out/cache_gemm_wo_spm_${TAG}.txt"

# # SPM + custom 指令 v1
# echo "--- spm gemmX ---"
# gem5.opt run_spm_v1.py --binary ./test/spm_gemmX
# mv m5out/stats.txt "m5out/spm_v1_gemmX_${TAG}.txt"

# # SPM + custom 指令 v2
# echo "--- spm gemmX ---"
# gem5.opt run_spm_v2.py --binary ./test/spm_gemmX
# mv m5out/stats.txt "m5out/spm_v2_gemmX_${TAG}.txt"

# # SPM + custom 指令 v3
# echo "--- spm gemmX ---"
# gem5.opt run_spm_v3.py --binary ./test/spm_gemmX
# mv m5out/stats.txt "m5out/spm_v3_gemmX_${TAG}.txt"

# SPM + custom 指令 v4
echo "--- spm gemmX ---"
gem5.opt run_spm.py --binary ./test/spm_gemmX --spm_intlv 64
mv m5out/stats.txt "m5out/spm_gemmX_${TAG}.txt"

echo "===== done ====="

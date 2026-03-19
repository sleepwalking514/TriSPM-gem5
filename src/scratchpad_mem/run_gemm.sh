# 编译
cd test
riscv64-unknown-linux-gnu-gcc -O3 -static -o cache_gemm ./cache_gemm.c
# riscv64-unknown-linux-gnu-gcc -O3 -static -o spm_gemm ./spm_gemm.c
# riscv64-unknown-linux-gnu-gcc -O3 -static -o spm_gemmX ./spm_gemmX.c
cd ..

# # 跑 Cache (启动spm系统)
# gem5.opt run_spm.py --binary ./test/cache_gemm
# mv m5out/stats.txt m5out/cache_gemm.txt

# 跑 Cache (关闭spm系统)
gem5.opt run_spm.py --binary ./test/cache_gemm --cache_baseline
mv m5out/stats.txt m5out/cache_gemm_wo_spm.txt

# # 跑 SPM
# gem5.opt run_spm.py --binary ./test/spm_gemm --spm_size 128KiB
# mv m5out/stats.txt m5out/spm_gemm.txt

# # 跑 SPM costom指令集
# gem5.opt run_spm.py --binary ./test/spm_gemmX
# mv m5out/stats.txt m5out/spm_gemmX.txt

#!/bin/bash
set -e

cd test
riscv64-unknown-linux-gnu-gcc -O2 -static -o spm_xinsn_test ./spm_xinsn_test.c
cd ..

gem5.opt run_spm.py --binary ./test/spm_xinsn_test --spm_size 64KiB

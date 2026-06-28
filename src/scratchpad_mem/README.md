# TriSPM Scratchpad Memory Simulator

This directory contains the TriSPM additions to gem5. The supported public
entry point is `run_spm.py`; historical versioned runners have been removed.

## Memory Model

`run_spm.py` models ordinary DRAM through the normal cache hierarchy. There is
no separate DRAM window for DMA buffers.

The explicit address regions are:

- `0x00000000..0x3fffffff`: ordinary 1 GiB DRAM, cacheable
- `0x40000000`: SPM base, routed to `ScratchpadMemory`
- `0xf0000000`: `SpmDmaEngine` MMIO registers

SPM and DMA MMIO are mapped as device-style regions in gem5 SE mode. CPU SPM
loads and stores are routed through the O3 `spm_port`; ordinary DRAM accesses
continue to use the cache hierarchy.

`dma_buf_malloc()` is kept as a source-compatible allocation helper. Under
gem5 it returns normal cacheable DRAM from `malloc()`. When `TRISPM_REAL_HW` is
enabled, the runtime may return DMA-capable host memory.

## Main Files

- `run_spm.py`: gem5 SE-mode runner for cache-only and SPM configurations
- `ScratchpadMemory.py`, `scratchpad_memory.cc/hh`: SPM object and timing model
- `SpmDmaEngine.py`, `spm_dma_engine.cc/hh`: MMIO/XSPM DMA engine
- `spm_dma_iface.hh`: CPU-side custom-instruction hooks
- `libspm.h`: userspace helper API used by workloads and micro-tests
- `libspm_real_hw.c`: optional real-hardware runtime support

## Local Tools

Debug/demo scripts live under `tools/`:

- `tools/rebuild.sh`: rebuild `build/RISCV/gem5.opt`
- `tools/run_debug.sh`: compile and run the step-by-step SPM micro-test
- `tools/run_xinsn_test.sh`: compile and run the XSPM instruction test
- `tools/run_gemm.sh`: compile and run a small cache/SPM GEMM comparison

The `test/` directory contains micro-test sources. Built test binaries and
`m5out/` output are ignored by `.gitignore`.

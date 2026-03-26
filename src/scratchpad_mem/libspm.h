#ifndef LIBSPM_H
#define LIBSPM_H

// Userspace helper for gem5 Scratchpad + SpmDmaEngine (RISC-V).
// Assumes:
//   - SPM is mapped at SPM_BASE (address routing bypasses cache)
//   - SpmDmaEngine MMIO is mapped at DMA_MMIO_BASE
//   - gem5 SE mode: no real OS / IOMMU. Addresses are treated as physical.
//
// Two DMA paths:
//   1) MMIO: spm_dma_copy() using standard ld/sd to program SpmDmaEngine registers
//   2) Custom ISA: xspm_dma() / xspm_dma_wait() using spm.dma / spm.dma.w instructions

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -------------------- Address map (match your run_spm.py) --------------------

#define SPM_BASE         0x40000000ULL

#define DMA_MMIO_BASE    0xF0000000ULL


// -------------------- SpmDmaEngine register offsets --------------------------
// 4-register MMIO interface with descriptor queue (default depth: 4).
// Write SRC, DST, then LEN (writing LEN enqueues a DMA transfer).
// Read STATUS to check completion (0 = all idle, >0 = pending+active count).
#define DMA_REG_SRC          0x00
#define DMA_REG_DST          0x08
#define DMA_REG_LEN          0x10
#define DMA_REG_STATUS       0x18


// -------------------- Low-level MMIO helpers (RV64) --------------------------

static inline uintptr_t _dma_reg_addr(uint32_t off)
{
    return (uintptr_t)(DMA_MMIO_BASE + (uintptr_t)off);
}

static inline void _fence_io(void)
{
    // Order MMIO + memory so the device sees a fully written descriptor.
    asm volatile("fence iorw, iorw" ::: "memory");
}

static inline void dma_write8(uint32_t off, uint8_t val)
{
    uintptr_t addr = _dma_reg_addr(off);
    asm volatile("sb %0, 0(%1)" :: "r"(val), "r"(addr) : "memory");
}

static inline void dma_write16(uint32_t off, uint16_t val)
{
    uintptr_t addr = _dma_reg_addr(off);
    asm volatile("sh %0, 0(%1)" :: "r"(val), "r"(addr) : "memory");
}

static inline void dma_write32(uint32_t off, uint32_t val)
{
    uintptr_t addr = _dma_reg_addr(off);
    asm volatile("sw %0, 0(%1)" :: "r"(val), "r"(addr) : "memory");
}

static inline void dma_write64(uint32_t off, uint64_t val)
{
    uintptr_t addr = _dma_reg_addr(off);
    asm volatile("sd %0, 0(%1)" :: "r"(val), "r"(addr) : "memory");
}

static inline uint64_t dma_read64(uint32_t off)
{
    uintptr_t addr = _dma_reg_addr(off);
    uint64_t val;
    asm volatile("ld %0, 0(%1)" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

static inline uint16_t dma_read16(uint32_t off)
{
    uintptr_t addr = _dma_reg_addr(off);
    uint16_t val;
    asm volatile("lh %0, 0(%1)" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

static inline uint32_t dma_read32(uint32_t off)
{
    uintptr_t addr = _dma_reg_addr(off);
    uint32_t val;
    asm volatile("lw %0, 0(%1)" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

// -------------------- SPM SPI -----------------

// Simple linear allocator
static unsigned long _spm_current_offset = 0x0;

static size_t get_spm_size() {
    // 获取环境变量字符串
    char* env_val = getenv("SPM_SIZE_BYTES");
    
    if (env_val != NULL) {
        // 转成整数
        return (size_t)atol(env_val);
    } else {
        // 返回默认值
        printf("Warning: SPM_SIZE_BYTES not set, defaulting to 1MiB\n");
        return 1024 * 1024; 
    }
}

// SPM version malloc
static inline void* spm_malloc(size_t size) {
    size_t SPM_MAX_SIZE = get_spm_size();
    // 越界检查
    if (_spm_current_offset + size > SPM_MAX_SIZE) {
        printf("[SPM Error] Out of memory! Requested: %lu, Free: %lu\n", 
               size, SPM_MAX_SIZE - _spm_current_offset);
        return NULL;
    }

    // 计算返回的地址 (基址 + 偏移)
    void* ptr = (void*)(SPM_BASE + _spm_current_offset);

    // 更新偏移量 (简单的8字节对齐)
    size_t aligned_size = (size + 7) & ~7;
    _spm_current_offset += aligned_size;

    return ptr;
}

// SPM version memset
static inline void* spm_memset(void* s, int c, size_t n) {
    size_t SPM_MAX_SIZE = get_spm_size();

    // 越界检查
    if ((uintptr_t)s + n > SPM_BASE + SPM_MAX_SIZE) {
        printf("[SPM Error] Out of Memory! Requested: %lu, Free: %lu\n",
                n, SPM_BASE + SPM_MAX_SIZE - (uintptr_t)s);
        return NULL;
    }

    memset(s, c, n);

    return s;
}

// Reset allocator
static inline void spm_free_all() {
    _spm_current_offset = 0x0;
}

// -------------------- DMA API ------------------------------------------------

static unsigned long _dma_buf_current_offset = 0x0;

static inline uintptr_t get_dma_buf_base(void) {
    char *env = getenv("DMA_BUF_BASE");
    if (env) return (uintptr_t)strtoull(env, NULL, 0);
    // fallback：和 run_spm.py 默认一致
    return (uintptr_t)0x30000000ULL;
}

static inline size_t get_dma_buf_size(void) {
    char *env = getenv("DMA_BUF_SIZE");
    if (env) return (size_t)strtoull(env, NULL, 0);
    // fallback：1MiB
    return (size_t)(1024 * 1024);
}

static inline void* dma_buf_malloc(size_t size) {
    uintptr_t base = get_dma_buf_base();
    size_t    cap  = get_dma_buf_size();

    // 8B 对齐
    size_t aligned_size = (size + 7) & ~((size_t)7);

    if (_dma_buf_current_offset + aligned_size > cap) {
        printf("[DMA_BUF Error] Out of memory! Requested: %lu, Free: %lu\n",
               (unsigned long)aligned_size,
               (unsigned long)(cap - _dma_buf_current_offset));
        return NULL;
    }

    void *ptr = (void *)(base + _dma_buf_current_offset);
    _dma_buf_current_offset += aligned_size;
    return ptr;
}

static inline void dma_buf_free_all(void) {
    _dma_buf_current_offset = 0x0;
}

static inline int spm_dma_wait(void)
{
    const uint64_t max_iters = 20000000ULL;
    for (uint64_t it = 0; it < max_iters; ++it) {
        if (dma_read64(DMA_REG_STATUS) == 0) {
            _fence_io();
            return 0;
        }
    }
    return -1;
}

static inline int spm_dma_copy(void *dst, const void *src, size_t nbytes)
{
    if (nbytes == 0) return 0;

    dma_write64(DMA_REG_SRC, (uint64_t)(uintptr_t)src);
    dma_write64(DMA_REG_DST, (uint64_t)(uintptr_t)dst);
    _fence_io();
    dma_write64(DMA_REG_LEN, (uint64_t)nbytes);
    _fence_io();

    return spm_dma_wait();
}

// -------------------- gem5 m5ops pseudo-instructions (RISC-V) ------
// Encoding: .word 0x0000007b | (func << 25)
// See util/m5/src/abi/riscv/m5op.S

#define M5OP_RESET_STATS  0x40
#define M5OP_DUMP_STATS   0x41

static inline void m5_reset_stats(uint64_t ns_delay, uint64_t ns_period)
{
    register uint64_t _a0 asm("a0") = ns_delay;
    register uint64_t _a1 asm("a1") = ns_period;
    asm volatile(".word %[op]"
                 : : [op] "i"(0x0000007b | (M5OP_RESET_STATS << 25)),
                     "r"(_a0), "r"(_a1) : "memory");
}

static inline void m5_dump_stats(uint64_t ns_delay, uint64_t ns_period)
{
    register uint64_t _a0 asm("a0") = ns_delay;
    register uint64_t _a1 asm("a1") = ns_period;
    asm volatile(".word %[op]"
                 : : [op] "i"(0x0000007b | (M5OP_DUMP_STATS << 25)),
                     "r"(_a0), "r"(_a1) : "memory");
}

// -------------------- Xspm custom instructions (alternative to MMIO) ------
// Uses custom-0 opcode (0x0B) with:
//   spm.dma   rd, rs1, rs2   funct3=0  R-type  (rd=dst, rs1=src, rs2=len)
//   spm.dma.w                funct3=1  I-type  (wait for all DMA completion)
// Transfers are bidirectional: src/dst can be any mapped address (SPM or DRAM).
// The DMA engine has a descriptor queue (default 4 entries); spm.dma enqueues
// a transfer, spm.dma.w blocks until all queued transfers complete.
// Requires gem5 built with the Xspm decoder patch.

#ifdef USE_XSPM_INSN

static inline void xspm_dma(uintptr_t spm_dst, uintptr_t dram_src,
                             uint64_t nbytes)
{
    asm volatile(".insn r 0x0B, 0, 0, %0, %1, %2"
                 : : "r"(spm_dst), "r"(dram_src), "r"(nbytes) : "memory");
}

static inline void xspm_dma_wait(void)
{
    asm volatile(".insn i 0x0B, 1, x0, x0, 0" : : : "memory");
    _fence_io();
}

#endif /* USE_XSPM_INSN */

#ifdef __cplusplus
}
#endif

#endif // LIBSPM_H

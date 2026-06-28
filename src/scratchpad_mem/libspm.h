#ifndef LIBSPM_H
#define LIBSPM_H

// Userspace helper for gem5 Scratchpad + SpmDmaEngine (RISC-V).
// Assumes:
//   - SPM is mapped at SPM_BASE (address routing bypasses cache)
//   - SpmDmaEngine MMIO is mapped at DMA_MMIO_BASE
//   - ordinary DRAM is cacheable by default
//   - gem5 SE mode: no real OS / IOMMU. Addresses are treated as physical.
//
// DMA APIs:
//   - spm_dma_copy() uses MMIO register writes.
//   - xspm_dma() / xspm_dma_wait() use Xspm custom instructions.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TRISPM_REAL_HW
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#endif

#ifdef __cplusplus
extern "C" {
#endif

// -------------------- Address map (match your run_spm.py) --------------------

#ifndef SPM_BASE
#define SPM_BASE         0x40000000ULL
#endif

#ifndef DMA_MMIO_BASE
#define DMA_MMIO_BASE    0xF0000000ULL
#endif

// -------------------- SpmDmaEngine register offsets
// -------------------------- 7-register MMIO interface with descriptor queue
// (default depth: 32).
//
// 1D transfer: Write SRC, DST, then LEN (writing LEN enqueues).
// 2D transfer: Write SRC, DST, SRC_STRIDE, DST_STRIDE, HEIGHT, then LEN.
//   The engine copies HEIGHT rows of LEN bytes each, advancing source by
//   SRC_STRIDE and destination by DST_STRIDE per row.
//   When HEIGHT <= 1, the transfer is 1D (stride is ignored).
//   The stride/height registers are auto-cleared after each enqueue.
//
// Read STATUS to check completion (0 = all idle, >0 = pending+active count).
#define DMA_REG_SRC          0x00
#define DMA_REG_DST          0x08
#define DMA_REG_LEN          0x10
#define DMA_REG_STATUS       0x18
#define DMA_REG_SRC_STRIDE   0x20
#define DMA_REG_DST_STRIDE   0x28
#define DMA_REG_HEIGHT       0x30


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
        printf("Warning: SPM_SIZE_BYTES not set, defaulting to 32KiB\n");
        return 32 * 1024;
    }
}

#ifdef TRISPM_REAL_HW

#ifndef TRISPM_REAL_HW_DEFAULT_SPM_SIZE
#define TRISPM_REAL_HW_DEFAULT_SPM_SIZE (128 * 1024)
#endif

#ifndef TRISPM_REAL_HW_TCM_GRANULE
#define TRISPM_REAL_HW_TCM_GRANULE (128 * 1024)
#endif

#ifndef TRISPM_REAL_HW_TCM_TOTAL_SIZE
#define TRISPM_REAL_HW_TCM_TOTAL_SIZE (512 * 1024)
#endif

#ifndef TRISPM_REAL_HW_TCM_DEVICE
#define TRISPM_REAL_HW_TCM_DEVICE "/dev/tcm"
#endif

#ifndef TRISPM_REAL_HW_UDMA_DEVICE
#define TRISPM_REAL_HW_UDMA_DEVICE "/dev/udma"
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define TRISPM_UDMA_MEMCPY_CMD _IOR('c', 0, int)

typedef struct
{
    void *src;
    void *dst;
    size_t size;
} trispm_udma_memcpy_msg_t;

static int _trispm_real_hw_tcm_fd = -1;
static int _trispm_real_hw_spm_mapped = 0;
static size_t _trispm_real_hw_spm_mapped_size = 0;

void *trispm_real_hw_runtime_dma_buf_malloc(size_t size);
void trispm_real_hw_runtime_dma_buf_free_all(void);
int trispm_real_hw_runtime_copy(void *dst, const void *src, size_t nbytes);

static inline const char *
trispm_real_hw_dma_backend(void)
{
    const char *backend = getenv("TRISPM_REAL_HW_DMA");
    if (!backend || !backend[0]) {
        return "cpu";
    }
    return backend;
}

static inline size_t
trispm_real_hw_requested_spm_size(void)
{
    size_t size = get_spm_size();
    if (size == 32 * 1024) {
        char *env = getenv("SPM_SIZE_BYTES");
        if (!env) {
            size = TRISPM_REAL_HW_DEFAULT_SPM_SIZE;
        }
    }
    return size;
}

static inline int
trispm_real_hw_validate_spm_size(size_t size)
{
    if (size == 0 || size % TRISPM_REAL_HW_TCM_GRANULE != 0 ||
        size > TRISPM_REAL_HW_TCM_TOTAL_SIZE) {
        fprintf(
            stderr,
            "[TRISPM_REAL_HW] refusing TCM mmap size %lu; "
            "size must be a nonzero multiple of %u bytes and <= %u bytes\n",
            (unsigned long)size, (unsigned)TRISPM_REAL_HW_TCM_GRANULE,
            (unsigned)TRISPM_REAL_HW_TCM_TOTAL_SIZE);
        return -1;
    }
    return 0;
}

static inline int
trispm_real_hw_init_spm(void)
{
    if (_trispm_real_hw_spm_mapped) {
        return 0;
    }

    size_t size = trispm_real_hw_requested_spm_size();
    if (trispm_real_hw_validate_spm_size(size) != 0) {
        return -1;
    }

    const char *dev = getenv("TRISPM_REAL_HW_TCM_DEVICE");
    if (!dev || !dev[0]) {
        dev = TRISPM_REAL_HW_TCM_DEVICE;
    }

    int fd = open(dev, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "[TRISPM_REAL_HW] open %s failed: %s\n", dev,
                strerror(errno));
        return -1;
    }

    void *want = (void *)(uintptr_t)SPM_BASE;
    void *mapped = mmap(want, size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    if (mapped == MAP_FAILED) {
        fprintf(stderr,
                "[TRISPM_REAL_HW] mmap TCM at %p size %lu failed: %s\n", want,
                (unsigned long)size, strerror(errno));
        close(fd);
        return -1;
    }
    if (mapped != want) {
        fprintf(stderr,
                "[TRISPM_REAL_HW] mmap returned %p, expected fixed %p\n",
                mapped, want);
        munmap(mapped, size);
        close(fd);
        return -1;
    }

    _trispm_real_hw_tcm_fd = fd;
    _trispm_real_hw_spm_mapped = 1;
    _trispm_real_hw_spm_mapped_size = size;
    return 0;
}

static inline void
trispm_real_hw_deinit(void)
{
    trispm_real_hw_runtime_dma_buf_free_all();

    if (_trispm_real_hw_spm_mapped) {
        munmap((void *)(uintptr_t)SPM_BASE, _trispm_real_hw_spm_mapped_size);
        _trispm_real_hw_spm_mapped = 0;
        _trispm_real_hw_spm_mapped_size = 0;
    }
    if (_trispm_real_hw_tcm_fd >= 0) {
        close(_trispm_real_hw_tcm_fd);
        _trispm_real_hw_tcm_fd = -1;
    }
}

static inline size_t
trispm_real_hw_effective_spm_size(void)
{
    if (_trispm_real_hw_spm_mapped_size != 0) {
        return _trispm_real_hw_spm_mapped_size;
    }

    return trispm_real_hw_requested_spm_size();
}

static inline int
trispm_real_hw_is_spm_range(const void *ptr, size_t nbytes)
{
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)SPM_BASE;
    size_t size = trispm_real_hw_effective_spm_size();
    if (trispm_real_hw_validate_spm_size(size) != 0) {
        return 0;
    }
    if (start < base || start > base + size) {
        return 0;
    }
    return nbytes <= (size_t)((base + size) - start);
}

static inline int
trispm_real_hw_copy(void *dst, const void *src, size_t nbytes)
{
    return trispm_real_hw_runtime_copy(dst, src, nbytes);
}

static inline void
trispm_real_hw_dma_enqueue_2d(void *dst, const void *src, size_t width,
                              size_t height, size_t src_stride,
                              size_t dst_stride)
{
    if (width == 0 || height == 0) {
        return;
    }
    if (trispm_real_hw_init_spm() != 0) {
        return;
    }

    uint8_t *dst_row = (uint8_t *)dst;
    const uint8_t *src_row = (const uint8_t *)src;
    for (size_t row = 0; row < height; ++row) {
        (void)trispm_real_hw_copy(dst_row, src_row, width);
        dst_row += dst_stride;
        src_row += src_stride;
    }
}

static inline int
trispm_real_hw_dma_wait_count(uint64_t max_pending)
{
    (void)max_pending;
    asm volatile("fence rw, rw" ::: "memory");
    return 0;
}

static inline int
trispm_real_hw_dma_wait(void)
{
    return trispm_real_hw_dma_wait_count(0);
}

static inline int
trispm_real_hw_dma_copy(void *dst, const void *src, size_t nbytes)
{
    return trispm_real_hw_copy(dst, src, nbytes);
}

#endif /* TRISPM_REAL_HW */

// SPM version malloc
static inline void* spm_malloc(size_t size) {
#ifdef TRISPM_REAL_HW
    if (trispm_real_hw_init_spm() != 0) {
        return NULL;
    }
#endif
    size_t SPM_MAX_SIZE = get_spm_size();
#ifdef TRISPM_REAL_HW
    if (SPM_MAX_SIZE == 32 * 1024 && _trispm_real_hw_spm_mapped_size != 0) {
        SPM_MAX_SIZE = _trispm_real_hw_spm_mapped_size;
    }
#endif
    // 越界检查
    if (_spm_current_offset + size > SPM_MAX_SIZE) {
        printf("[SPM Error] Out of memory! Requested: %lu, Free: %lu\n", size,
               SPM_MAX_SIZE - _spm_current_offset);
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
#ifdef TRISPM_REAL_HW
    if (trispm_real_hw_init_spm() != 0) {
        return NULL;
    }
#endif
    size_t SPM_MAX_SIZE = get_spm_size();
#ifdef TRISPM_REAL_HW
    if (SPM_MAX_SIZE == 32 * 1024 && _trispm_real_hw_spm_mapped_size != 0) {
        SPM_MAX_SIZE = _trispm_real_hw_spm_mapped_size;
    }
#endif

    // 越界检查
    if ((uintptr_t)s + n > SPM_BASE + SPM_MAX_SIZE) {
        printf("[SPM Error] Out of Memory! Requested: %lu, Free: %lu\n",
                (unsigned long)n,
                (unsigned long)(SPM_BASE + SPM_MAX_SIZE - (uintptr_t)s));
        return NULL;
    }

    memset(s, c, n);

    return s;
}

// Reset allocator
static inline void spm_free_all() {
    _spm_current_offset = 0x0;
#ifdef TRISPM_REAL_HW
    trispm_real_hw_deinit();
#endif
}

// -------------------- DMA API ------------------------------------------------

#ifndef TRISPM_DMA_BUF_MAX_ALLOCS
#define TRISPM_DMA_BUF_MAX_ALLOCS 256
#endif

#ifndef TRISPM_REAL_HW
static void *_dma_buf_allocs[TRISPM_DMA_BUF_MAX_ALLOCS];
static int _dma_buf_alloc_count = 0;
#endif

static inline void* dma_buf_malloc(size_t size) {
#ifdef TRISPM_REAL_HW
    return trispm_real_hw_runtime_dma_buf_malloc(size);
#else
    void *ptr = malloc(size);
    if (ptr && _dma_buf_alloc_count < TRISPM_DMA_BUF_MAX_ALLOCS) {
        _dma_buf_allocs[_dma_buf_alloc_count++] = ptr;
    }
    return ptr;
#endif
}

static inline void dma_buf_free_all(void) {
#ifdef TRISPM_REAL_HW
    trispm_real_hw_runtime_dma_buf_free_all();
#else
    for (int i = 0; i < _dma_buf_alloc_count; ++i) {
        free(_dma_buf_allocs[i]);
    }
    _dma_buf_alloc_count = 0;
#endif
}

static inline int
spm_dma_wait_count(uint64_t max_pending)
{
#ifdef TRISPM_REAL_HW
    return trispm_real_hw_dma_wait_count(max_pending);
#else
    const uint64_t max_iters = 20000000ULL;
    for (uint64_t it = 0; it < max_iters; ++it) {
        if (dma_read64(DMA_REG_STATUS) <= max_pending) {
            _fence_io();
            return 0;
        }
    }
    return -1;
#endif
}

static inline int
spm_dma_wait(void)
{
    return spm_dma_wait_count(0);
}

static inline int spm_dma_copy(void *dst, const void *src, size_t nbytes)
{
#ifdef TRISPM_REAL_HW
    return trispm_real_hw_dma_copy(dst, src, nbytes);
#else
    if (nbytes == 0) return 0;

    dma_write64(DMA_REG_SRC, (uint64_t)(uintptr_t)src);
    dma_write64(DMA_REG_DST, (uint64_t)(uintptr_t)dst);
    _fence_io();
    dma_write64(DMA_REG_LEN, (uint64_t)nbytes);
    _fence_io();

    return spm_dma_wait();
#endif
}

// 2D strided DMA: copy a rectangular tile.
//   dst        - destination base address (SPM or DRAM)
//   src        - source base address (DRAM or SPM)
//   width      - bytes per row to transfer
//   height     - number of rows
//   src_stride - byte distance between consecutive source rows
//   dst_stride - byte distance between consecutive destination rows
static inline int spm_dma_copy_2d(void *dst, const void *src,
                                  size_t width, size_t height,
                                  size_t src_stride, size_t dst_stride)
{
#ifdef TRISPM_REAL_HW
    trispm_real_hw_dma_enqueue_2d(dst, src, width, height, src_stride,
                                  dst_stride);
    return trispm_real_hw_dma_wait();
#else
    if (width == 0 || height == 0) return 0;

    dma_write64(DMA_REG_SRC, (uint64_t)(uintptr_t)src);
    dma_write64(DMA_REG_DST, (uint64_t)(uintptr_t)dst);
    dma_write64(DMA_REG_SRC_STRIDE, (uint64_t)src_stride);
    dma_write64(DMA_REG_DST_STRIDE, (uint64_t)dst_stride);
    dma_write64(DMA_REG_HEIGHT, (uint64_t)height);
    _fence_io();
    dma_write64(DMA_REG_LEN, (uint64_t)width);  // writing LEN enqueues
    _fence_io();

    return spm_dma_wait();
#endif
}

// 2D async enqueue (non-blocking, caller must call spm_dma_wait() later)
static inline void spm_dma_enqueue_2d(void *dst, const void *src,
                                      size_t width, size_t height,
                                      size_t src_stride, size_t dst_stride)
{
#ifdef TRISPM_REAL_HW
    trispm_real_hw_dma_enqueue_2d(dst, src, width, height, src_stride,
                                  dst_stride);
#else
    dma_write64(DMA_REG_SRC, (uint64_t)(uintptr_t)src);
    dma_write64(DMA_REG_DST, (uint64_t)(uintptr_t)dst);
    dma_write64(DMA_REG_SRC_STRIDE, (uint64_t)src_stride);
    dma_write64(DMA_REG_DST_STRIDE, (uint64_t)dst_stride);
    dma_write64(DMA_REG_HEIGHT, (uint64_t)height);
    _fence_io();
    dma_write64(DMA_REG_LEN, (uint64_t)width);
    _fence_io();
#endif
}

// -------------------- Harness helpers --------------------
//
// publish_input(): copy from a cacheable scratch to a destination buffer
// allocated by the launcher.  gem5 SPM runs use normal cacheable DRAM for all
// ordinary buffers, so this is a memcpy.  The cost is paid outside the
// measured ROI when called before m5_reset_stats().
//
// flush_caches(): walk a buffer larger than the L2 to evict the working
// set out of both L1 and L2 (the scrub itself fills the caches with
// uninteresting lines).  Used to give SPM and cache baselines the same
// DRAM-cold starting state when measuring the kernel ROI.  Tune
// SCRUB_BUFFER_BYTES if the cache hierarchy changes.

#ifndef SCRUB_BUFFER_BYTES
#define SCRUB_BUFFER_BYTES (1U << 20)   /* 1 MiB > L2 (512 KiB) */
#endif
#ifndef CACHE_LINE_BYTES
#define CACHE_LINE_BYTES   64
#endif

static inline void publish_input(void *dst, const void *src, size_t nbytes)
{
#ifdef TRISPM_REAL_HW
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < nbytes; ++i) {
        d[i] = s[i];
    }
    asm volatile("fence rw, rw" ::: "memory");
    return;
#else
    memcpy(dst, src, nbytes);
#endif
}

static inline void flush_caches(void)
{
    static volatile uint8_t scrub[SCRUB_BUFFER_BYTES]
        __attribute__((aligned(CACHE_LINE_BYTES)));
    /* Touch one byte per cache line.  volatile prevents the compiler from
     * coalescing or eliding the load-modify-store pair, so each iteration
     * really hits the cache line. */
    for (size_t i = 0; i < SCRUB_BUFFER_BYTES; i += CACHE_LINE_BYTES) {
        scrub[i] = (uint8_t)(scrub[i] + 1);
    }
    /* Drain the store buffer so the eviction writebacks land in DRAM
     * before the caller proceeds (e.g. before m5_reset_stats). */
    asm volatile("fence rw, rw" ::: "memory");
}

// -------------------- gem5 m5ops pseudo-instructions (RISC-V) ------
// Encoding: .word 0x0000007b | (func << 25)
// See util/m5/src/abi/riscv/m5op.S

#define M5OP_RESET_STATS  0x40
#define M5OP_DUMP_STATS   0x41
#define M5OP_DUMP_RESET_STATS 0x42

#ifdef TRISPM_REAL_HW
static uint64_t _trispm_real_hw_roi_start_ns = 0;
static int _trispm_real_hw_perf_inited = 0;
static int _trispm_real_hw_perf_available = 0;
static int _trispm_real_hw_perf_warned = 0;
static int _trispm_real_hw_cycles_fd = -1;
static int _trispm_real_hw_instr_fd = -1;
static int _trispm_real_hw_perf_measure_instructions = 0;

static inline uint64_t
trispm_real_hw_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline int
trispm_real_hw_perf_event_open(struct perf_event_attr *attr, pid_t pid,
                               int cpu, int group_fd, unsigned long flags)
{
#ifdef SYS_perf_event_open
    return (int)syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
#else
    (void)attr;
    (void)pid;
    (void)cpu;
    (void)group_fd;
    (void)flags;
    return -1;
#endif
}

typedef struct
{
    uint64_t value;
    uint64_t time_enabled;
    uint64_t time_running;
} trispm_real_hw_perf_read_t;

static inline int
trispm_real_hw_perf_open_counter(uint64_t config, int group_fd)
{
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format =
        PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    return trispm_real_hw_perf_event_open(&attr, 0, -1, group_fd, 0);
}

static inline void
trispm_real_hw_perf_init(void)
{
    if (_trispm_real_hw_perf_inited) {
        return;
    }
    _trispm_real_hw_perf_inited = 1;

    const char *env = getenv("TRISPM_REAL_HW_ROI_PERF");
    if (env && strcmp(env, "0") == 0) {
        return;
    }

    const char *instr_env = getenv("TRISPM_REAL_HW_ROI_INSTRUCTIONS");
    _trispm_real_hw_perf_measure_instructions =
        instr_env && strcmp(instr_env, "0") != 0;

    _trispm_real_hw_cycles_fd =
        trispm_real_hw_perf_open_counter(PERF_COUNT_HW_CPU_CYCLES, -1);
    if (_trispm_real_hw_cycles_fd < 0) {
        return;
    }

    if (_trispm_real_hw_perf_measure_instructions) {
        _trispm_real_hw_instr_fd =
            trispm_real_hw_perf_open_counter(PERF_COUNT_HW_INSTRUCTIONS, -1);
        if (_trispm_real_hw_instr_fd < 0) {
            close(_trispm_real_hw_cycles_fd);
            _trispm_real_hw_cycles_fd = -1;
            return;
        }
    }

    _trispm_real_hw_perf_available = 1;
}

static inline uint64_t
trispm_real_hw_perf_scaled(const trispm_real_hw_perf_read_t *r)
{
    if (r->time_running == 0 || r->time_enabled == 0) {
        return r->value;
    }
    if (r->time_running == r->time_enabled) {
        return r->value;
    }
    return (uint64_t)(((long double)r->value * (long double)r->time_enabled) /
                      (long double)r->time_running);
}

static inline void
trispm_real_hw_perf_start(void)
{
    trispm_real_hw_perf_init();
    if (!_trispm_real_hw_perf_available) {
        if (!_trispm_real_hw_perf_warned) {
            fprintf(stderr, "[TRISPM_REAL_HW_ROI] perf_event_open "
                            "unavailable; timing only\n");
            _trispm_real_hw_perf_warned = 1;
        }
        return;
    }

    ioctl(_trispm_real_hw_cycles_fd, PERF_EVENT_IOC_RESET, 0);
    if (_trispm_real_hw_instr_fd >= 0) {
        ioctl(_trispm_real_hw_instr_fd, PERF_EVENT_IOC_RESET, 0);
    }
    ioctl(_trispm_real_hw_cycles_fd, PERF_EVENT_IOC_ENABLE, 0);
    if (_trispm_real_hw_instr_fd >= 0) {
        ioctl(_trispm_real_hw_instr_fd, PERF_EVENT_IOC_ENABLE, 0);
    }
}

static inline void
trispm_real_hw_perf_dump(void)
{
    if (!_trispm_real_hw_perf_available) {
        return;
    }

    if (_trispm_real_hw_instr_fd >= 0) {
        ioctl(_trispm_real_hw_instr_fd, PERF_EVENT_IOC_DISABLE, 0);
    }
    ioctl(_trispm_real_hw_cycles_fd, PERF_EVENT_IOC_DISABLE, 0);

    trispm_real_hw_perf_read_t cycles_readout = {0, 0, 0};
    trispm_real_hw_perf_read_t instr_readout = {0, 0, 0};
    ssize_t cycles_read = read(_trispm_real_hw_cycles_fd, &cycles_readout,
                               sizeof(cycles_readout));
    if (cycles_read == (ssize_t)sizeof(cycles_readout)) {
        uint64_t cycles = trispm_real_hw_perf_scaled(&cycles_readout);
        double cycles_running_pct =
            cycles_readout.time_enabled
                ? 100.0 * (double)cycles_readout.time_running /
                      (double)cycles_readout.time_enabled
                : 0.0;
        if (_trispm_real_hw_instr_fd >= 0 &&
            read(_trispm_real_hw_instr_fd, &instr_readout,
                 sizeof(instr_readout)) == (ssize_t)sizeof(instr_readout)) {
            uint64_t instructions = trispm_real_hw_perf_scaled(&instr_readout);
            double ipc = cycles ? (double)instructions / (double)cycles : 0.0;
            double instructions_running_pct =
                instr_readout.time_enabled
                    ? 100.0 * (double)instr_readout.time_running /
                          (double)instr_readout.time_enabled
                    : 0.0;
            printf("[TRISPM_REAL_HW_ROI_PERF] cycles=%llu instructions=%llu "
                   "ipc=%.6f cycles_running_pct=%.2f "
                   "instructions_running_pct=%.2f\n",
                   (unsigned long long)cycles,
                   (unsigned long long)instructions, ipc, cycles_running_pct,
                   instructions_running_pct);
        } else {
            printf("[TRISPM_REAL_HW_ROI_PERF] cycles=%llu "
                   "cycles_running_pct=%.2f\n",
                   (unsigned long long)cycles, cycles_running_pct);
        }
    }
}

static inline void
trispm_real_hw_roi_start(void)
{
    asm volatile("fence rw, rw" ::: "memory");
    trispm_real_hw_perf_start();
    _trispm_real_hw_roi_start_ns = trispm_real_hw_now_ns();
}

static inline void
trispm_real_hw_roi_dump(int reset_after)
{
    asm volatile("fence rw, rw" ::: "memory");
    uint64_t end_ns = trispm_real_hw_now_ns();
    trispm_real_hw_perf_dump();
    if (_trispm_real_hw_roi_start_ns != 0) {
        uint64_t elapsed_ns = end_ns - _trispm_real_hw_roi_start_ns;
        printf("[TRISPM_REAL_HW_ROI] elapsed_ns=%llu elapsed_ms=%.6f\n",
               (unsigned long long)elapsed_ns, (double)elapsed_ns / 1000000.0);
    } else {
        printf("[TRISPM_REAL_HW_ROI] elapsed_ns=0 elapsed_ms=0.000000 "
               "start_missing=1\n");
    }
    if (reset_after) {
        _trispm_real_hw_roi_start_ns = trispm_real_hw_now_ns();
    }
}
#endif

static inline void m5_reset_stats(uint64_t ns_delay, uint64_t ns_period)
{
#ifdef TRISPM_REAL_HW
    (void)ns_delay;
    (void)ns_period;
    trispm_real_hw_roi_start();
#else
    register uint64_t _a0 asm("a0") = ns_delay;
    register uint64_t _a1 asm("a1") = ns_period;
    asm volatile(".word %[op]"
                 : : [op] "i"(0x0000007b | (M5OP_RESET_STATS << 25)),
                     "r"(_a0), "r"(_a1) : "memory");
#endif
}

static inline void m5_dump_stats(uint64_t ns_delay, uint64_t ns_period)
{
#ifdef TRISPM_REAL_HW
    (void)ns_delay;
    (void)ns_period;
    trispm_real_hw_roi_dump(0);
#else
    register uint64_t _a0 asm("a0") = ns_delay;
    register uint64_t _a1 asm("a1") = ns_period;
    asm volatile(".word %[op]"
                 : : [op] "i"(0x0000007b | (M5OP_DUMP_STATS << 25)),
                     "r"(_a0), "r"(_a1) : "memory");
#endif
}

static inline void m5_dump_reset_stats(uint64_t ns_delay, uint64_t ns_period)
{
#ifdef TRISPM_REAL_HW
    (void)ns_delay;
    (void)ns_period;
    trispm_real_hw_roi_dump(1);
#else
    register uint64_t _a0 asm("a0") = ns_delay;
    register uint64_t _a1 asm("a1") = ns_period;
    asm volatile(".word %[op]"
                 : : [op] "i"(0x0000007b | (M5OP_DUMP_RESET_STATS << 25)),
                     "r"(_a0), "r"(_a1) : "memory");
#endif
}

// -------------------- Xspm custom instructions (alternative to MMIO) ------
// Uses custom-0 opcode (0x0B) with:
//   spm.dma        rd, rs1, rs2   funct3=0  R-type  (rd=dst, rs1=src, rs2=len)
//   spm.dma.w      rd             funct3=1  I-type  (read pending DMA count)
//   spm.dma.stride rs1, rs2       funct3=2  R-type
//   (rs1=src_stride, rs2=dst_stride) spm.dma.2d     rd, rs1, rs2   funct3=3
//   R-type  (rd=dst, rs1=src, rs2=width|height)
//
// 2D usage sequence:
//   spm.dma.stride  x_src_stride, x_dst_stride   // stage strides
//   spm.dma.2d      x_dst, x_src, x_wh           // enqueue
//   spm.dma.w       x_status                     // read pending count
//   (x_wh packs width=low32, height=high32)
//
// Transfers are bidirectional: src/dst can be any mapped address (SPM or
// DRAM). The DMA engine has a descriptor queue (default 32 entries); spm.dma
// enqueues a transfer, and spm.dma.w returns queued+active descriptor count.
// Requires gem5 built with the Xspm decoder patch.

#ifdef USE_XSPM_INSN

static inline void xspm_dma(uintptr_t spm_dst, uintptr_t dram_src,
                             uint64_t nbytes)
{
    asm volatile(".insn r 0x0B, 0, 0, %0, %1, %2"
                 : : "r"(spm_dst), "r"(dram_src), "r"(nbytes) : "memory");
}

static inline void
xspm_dma_wait_count(uint64_t max_pending)
{
    uint64_t pending;
    do {
        asm volatile(".insn i 0x0B, 1, %0, x0, 0"
                     : "=r"(pending) : : "memory");
    } while (pending > max_pending);
    _fence_io();
}

static inline void
xspm_dma_wait(void)
{
    xspm_dma_wait_count(0);
}

// Stage source and destination strides for the next spm.dma.2d.
static inline void xspm_dma_stride(uint64_t src_stride, uint64_t dst_stride)
{
    asm volatile(".insn r 0x0B, 2, 0, x0, %0, %1"
                 : : "r"(src_stride), "r"(dst_stride) : "memory");
}

// Enqueue a 2D strided DMA transfer.
//   dst        - destination base address
//   src        - source base address
//   width      - bytes per row (must fit in 32 bits)
//   height     - number of rows (must fit in 32 bits)
// Strides must have been set by a preceding xspm_dma_stride() call.
static inline void xspm_dma_2d(uintptr_t dst, uintptr_t src,
                                uint32_t width, uint32_t height)
{
    uint64_t wh = (uint64_t)width | ((uint64_t)height << 32);
    asm volatile(".insn r 0x0B, 3, 0, %0, %1, %2"
                 : : "r"(dst), "r"(src), "r"(wh) : "memory");
}

// Convenience: 2D DMA + wait (blocking).
static inline void xspm_dma_copy_2d(uintptr_t dst, uintptr_t src,
                                     uint32_t width, uint32_t height,
                                     uint64_t src_stride, uint64_t dst_stride)
{
    xspm_dma_stride(src_stride, dst_stride);
    xspm_dma_2d(dst, src, width, height);
    xspm_dma_wait();
}

#endif /* USE_XSPM_INSN */

#ifdef __cplusplus
}
#endif

#endif // LIBSPM_H

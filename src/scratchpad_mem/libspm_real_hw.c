#ifndef TRISPM_REAL_HW
#define TRISPM_REAL_HW
#endif
#include "libspm.h"

#ifndef TRISPM_REAL_HW_MAX_DMA_BUFS
#define TRISPM_REAL_HW_MAX_DMA_BUFS 256
#endif

typedef struct
{
    void *ptr;
    size_t size;
    int is_mmap;
} trispm_real_hw_dma_buf_t;

static int trispm_real_hw_udma_fd = -1;
static trispm_real_hw_dma_buf_t
    trispm_real_hw_dma_bufs[TRISPM_REAL_HW_MAX_DMA_BUFS];
static int trispm_real_hw_dma_buf_count = 0;
static int trispm_real_hw_udma_fallback_warned = 0;

#ifndef TRISPM_REAL_HW_UDMA_MIN_COPY_BYTES
#define TRISPM_REAL_HW_UDMA_MIN_COPY_BYTES 1024
#endif

static int
trispm_real_hw_runtime_init_udma(void)
{
    if (trispm_real_hw_udma_fd >= 0) {
        return 0;
    }

    const char *dev = getenv("TRISPM_REAL_HW_UDMA_DEVICE");
    if (!dev || !dev[0]) {
        dev = TRISPM_REAL_HW_UDMA_DEVICE;
    }

    int fd = open(dev, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "[TRISPM_REAL_HW] open %s failed: %s\n", dev,
                strerror(errno));
        return -1;
    }

    trispm_real_hw_udma_fd = fd;
    return 0;
}

static void
trispm_real_hw_runtime_record_dma_buf(void *ptr, size_t size, int is_mmap)
{
    if (!ptr || trispm_real_hw_dma_buf_count >= TRISPM_REAL_HW_MAX_DMA_BUFS) {
        return;
    }
    trispm_real_hw_dma_bufs[trispm_real_hw_dma_buf_count].ptr = ptr;
    trispm_real_hw_dma_bufs[trispm_real_hw_dma_buf_count].size = size;
    trispm_real_hw_dma_bufs[trispm_real_hw_dma_buf_count].is_mmap = is_mmap;
    trispm_real_hw_dma_buf_count++;
}

static int
trispm_real_hw_runtime_is_dma_mmap_range(const void *ptr, size_t nbytes)
{
    uintptr_t start = (uintptr_t)ptr;
    for (int i = 0; i < trispm_real_hw_dma_buf_count; ++i) {
        if (!trispm_real_hw_dma_bufs[i].is_mmap) {
            continue;
        }
        uintptr_t base = (uintptr_t)trispm_real_hw_dma_bufs[i].ptr;
        size_t size = trispm_real_hw_dma_bufs[i].size;
        if (start >= base && start <= base + size &&
            nbytes <= (size_t)((base + size) - start)) {
            return 1;
        }
    }
    return 0;
}

static int
trispm_real_hw_runtime_udma_copy(void *dst, const void *src, size_t nbytes)
{
    if (nbytes == 0) {
        return 0;
    }
    if (!trispm_real_hw_runtime_is_dma_mmap_range(src, nbytes)) {
        return -1;
    }
    if (trispm_real_hw_runtime_init_udma() != 0) {
        return -1;
    }

    trispm_udma_memcpy_msg_t msg;
    msg.src = (void *)src;
    msg.dst = dst;
    msg.size = nbytes;
    if (ioctl(trispm_real_hw_udma_fd, TRISPM_UDMA_MEMCPY_CMD, &msg) < 0) {
        return -1;
    }
    return 0;
}

void *
trispm_real_hw_runtime_dma_buf_malloc(size_t size)
{
    if (strcmp(trispm_real_hw_dma_backend(), "udma") == 0 &&
        trispm_real_hw_runtime_init_udma() == 0) {
        long page_size = sysconf(_SC_PAGESIZE);
        size_t alignment = page_size > 0 ? (size_t)page_size : 4096;
        size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
        void *ptr = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, trispm_real_hw_udma_fd, 0);
        if (ptr != MAP_FAILED) {
            trispm_real_hw_runtime_record_dma_buf(ptr, aligned_size, 1);
            return ptr;
        }
        fprintf(stderr,
                "[TRISPM_REAL_HW] mmap /dev/udma size %lu failed: %s\n",
                (unsigned long)aligned_size, strerror(errno));
    }

    void *ptr = malloc(size);
    trispm_real_hw_runtime_record_dma_buf(ptr, size, 0);
    return ptr;
}

void
trispm_real_hw_runtime_dma_buf_free_all(void)
{
    for (int i = 0; i < trispm_real_hw_dma_buf_count; ++i) {
        void *ptr = trispm_real_hw_dma_bufs[i].ptr;
        size_t size = trispm_real_hw_dma_bufs[i].size;
        if (!ptr) {
            continue;
        }
        if (trispm_real_hw_dma_bufs[i].is_mmap) {
            munmap(ptr, size);
        } else {
            free(ptr);
        }
        trispm_real_hw_dma_bufs[i].ptr = NULL;
        trispm_real_hw_dma_bufs[i].size = 0;
        trispm_real_hw_dma_bufs[i].is_mmap = 0;
    }
    trispm_real_hw_dma_buf_count = 0;

    if (trispm_real_hw_udma_fd >= 0) {
        close(trispm_real_hw_udma_fd);
        trispm_real_hw_udma_fd = -1;
    }
}

int
trispm_real_hw_runtime_copy(void *dst, const void *src, size_t nbytes)
{
    if (nbytes == 0) {
        return 0;
    }

    if (strcmp(trispm_real_hw_dma_backend(), "udma") == 0 &&
        trispm_real_hw_is_spm_range(dst, nbytes) &&
        nbytes >= TRISPM_REAL_HW_UDMA_MIN_COPY_BYTES &&
        nbytes % TRISPM_REAL_HW_UDMA_MIN_COPY_BYTES == 0) {
        if (trispm_real_hw_runtime_udma_copy(dst, src, nbytes) == 0) {
            return 0;
        }
        if (!trispm_real_hw_udma_fallback_warned) {
            fprintf(stderr, "[TRISPM_REAL_HW] udma memcpy failed; falling "
                            "back to CPU copy\n");
            trispm_real_hw_udma_fallback_warned = 1;
        }
    }

    memcpy(dst, src, nbytes);
    return 0;
}

void
trispm_real_hw_runtime_dma_enqueue_2d(uint64_t dst, uint64_t src, size_t width,
                                      size_t height, size_t src_stride,
                                      size_t dst_stride)
{
    if (width == 0 || height == 0) {
        return;
    }

    uint8_t *dst_row = (uint8_t *)(uintptr_t)dst;
    const uint8_t *src_row = (const uint8_t *)(uintptr_t)src;
    for (size_t row = 0; row < height; ++row) {
        (void)trispm_real_hw_runtime_copy(dst_row, src_row, width);
        dst_row += dst_stride;
        src_row += src_stride;
    }
}

int
trispm_real_hw_runtime_dma_wait_count(uint64_t max_pending)
{
    return trispm_real_hw_dma_wait_count(max_pending);
}

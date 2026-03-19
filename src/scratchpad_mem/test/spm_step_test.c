#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../libspm.h"

#define N 8

static void step_marker(const char *msg)
{
    printf("[STEP] %s\n", msg);
    fflush(stdout);
}

static int step0_printf_only(void)
{
    step_marker("step0: printf works");
    return 0;
}

static int step1_env_vars(void)
{
    step_marker("step1: reading env vars ...");

    size_t spm_sz = get_spm_size();
    printf("  SPM_SIZE_BYTES = %lu\n", (unsigned long)spm_sz);
    fflush(stdout);

    uintptr_t dma_base = get_dma_buf_base();
    printf("  DMA_BUF_BASE   = 0x%lx\n", (unsigned long)dma_base);
    fflush(stdout);

    size_t dma_sz = get_dma_buf_size();
    printf("  DMA_BUF_SIZE   = %lu\n", (unsigned long)dma_sz);
    fflush(stdout);

    return (spm_sz == 0 || dma_base == 0 || dma_sz == 0) ? 1 : 0;
}

static int step2_spm_alloc(void)
{
    step_marker("step2: spm_malloc + dma_buf_malloc ...");

    int *spm_ptr = (int *)spm_malloc(N * sizeof(int));
    printf("  spm_malloc  -> %p\n", (void *)spm_ptr);
    fflush(stdout);

    int *dma_ptr = (int *)dma_buf_malloc(N * sizeof(int));
    printf("  dma_buf_malloc -> %p\n", (void *)dma_ptr);
    fflush(stdout);

    return (spm_ptr == NULL || dma_ptr == NULL) ? 1 : 0;
}

static int step3_spm_write_read(void)
{
    step_marker("step3: direct SPM write/read ...");

    int *spm = (int *)spm_malloc(N * sizeof(int));
    if (!spm) { printf("  spm_malloc failed\n"); fflush(stdout); return 1; }

    step_marker("step3a: writing to SPM");
    for (int i = 0; i < N; i++)
        spm[i] = 42 + i;

    step_marker("step3b: reading from SPM");
    int fail = 0;
    for (int i = 0; i < N; i++) {
        if (spm[i] != 42 + i) {
            printf("  FAIL spm[%d]: expected %d, got %d\n", i, 42 + i, spm[i]);
            fflush(stdout);
            fail++;
        }
    }
    return fail;
}

static int step4_dma_buf_write_read(void)
{
    step_marker("step4: DMA buffer write/read ...");

    int *buf = (int *)dma_buf_malloc(N * sizeof(int));
    if (!buf) { printf("  dma_buf_malloc failed\n"); fflush(stdout); return 1; }

    step_marker("step4a: writing to DMA buffer");
    for (int i = 0; i < N; i++)
        buf[i] = 100 + i;

    step_marker("step4b: reading from DMA buffer");
    int fail = 0;
    for (int i = 0; i < N; i++) {
        if (buf[i] != 100 + i) {
            printf("  FAIL buf[%d]: expected %d, got %d\n", i, 100 + i, buf[i]);
            fflush(stdout);
            fail++;
        }
    }
    return fail;
}

static int step5_mmio_dma_dram_to_spm(void)
{
    step_marker("step5: MMIO DMA (DRAM -> SPM) ...");

    int *src = (int *)dma_buf_malloc(N * sizeof(int));
    int *dst = (int *)spm_malloc(N * sizeof(int));
    if (!src || !dst) {
        printf("  alloc failed: src=%p dst=%p\n", (void *)src, (void *)dst);
        fflush(stdout);
        return 1;
    }

    step_marker("step5a: filling DRAM source");
    for (int i = 0; i < N; i++)
        src[i] = 200 + i;

    step_marker("step5b: triggering MMIO DMA (spm_dma_copy)");
    int rc = spm_dma_copy(dst, src, N * sizeof(int));
    printf("  spm_dma_copy returned %d\n", rc);
    fflush(stdout);

    if (rc != 0) return 1;

    step_marker("step5c: verifying SPM contents");
    int fail = 0;
    for (int i = 0; i < N; i++) {
        if (dst[i] != 200 + i) {
            printf("  FAIL [%d]: expected %d, got %d\n", i, 200 + i, dst[i]);
            fflush(stdout);
            fail++;
        }
    }
    return fail;
}

#ifdef USE_XSPM_INSN
static int step6_xinsn_dma_dram_to_spm(void)
{
    step_marker("step6: xspm_dma (DRAM -> SPM) ...");

    int *src = (int *)dma_buf_malloc(N * sizeof(int));
    int *dst = (int *)spm_malloc(N * sizeof(int));
    if (!src || !dst) {
        printf("  alloc failed: src=%p dst=%p\n", (void *)src, (void *)dst);
        fflush(stdout);
        return 1;
    }

    step_marker("step6a: filling DRAM source");
    for (int i = 0; i < N; i++)
        src[i] = 300 + i;

    step_marker("step6b: xspm_dma(dst_spm, src_dram, len)");
    xspm_dma((uintptr_t)dst, (uintptr_t)src, N * sizeof(int));

    step_marker("step6c: xspm_dma_wait()");
    xspm_dma_wait();

    step_marker("step6d: verifying SPM contents");
    int fail = 0;
    for (int i = 0; i < N; i++) {
        if (dst[i] != 300 + i) {
            printf("  FAIL [%d]: expected %d, got %d\n", i, 300 + i, dst[i]);
            fflush(stdout);
            fail++;
        }
    }
    return fail;
}

static int step7_xinsn_dma_spm_to_dram(void)
{
    step_marker("step7: xspm_dma (SPM -> DRAM) ...");

    int *spm_src = (int *)spm_malloc(N * sizeof(int));
    int *dst = (int *)dma_buf_malloc(N * sizeof(int));
    if (!spm_src || !dst) {
        printf("  alloc failed\n"); fflush(stdout);
        return 1;
    }

    step_marker("step7a: filling SPM source");
    for (int i = 0; i < N; i++) {
        spm_src[i] = 400 + i;
        dst[i] = 0;
    }

    step_marker("step7b: xspm_dma(dst_dram, src_spm, len)");
    xspm_dma((uintptr_t)dst, (uintptr_t)spm_src, N * sizeof(int));

    step_marker("step7c: xspm_dma_wait()");
    xspm_dma_wait();

    step_marker("step7d: verifying DRAM contents");
    int fail = 0;
    for (int i = 0; i < N; i++) {
        if (dst[i] != 400 + i) {
            printf("  FAIL [%d]: expected %d, got %d\n", i, 400 + i, dst[i]);
            fflush(stdout);
            fail++;
        }
    }
    return fail;
}
#endif /* USE_XSPM_INSN */

int main(void)
{
    int total = 0, rc;

    printf("=== SPM step-by-step debug test ===\n");
    fflush(stdout);

    rc = step0_printf_only();
    printf("  => %s\n\n", rc ? "FAIL" : "PASS"); fflush(stdout);
    total += rc;

    rc = step1_env_vars();
    printf("  => %s\n\n", rc ? "FAIL" : "PASS"); fflush(stdout);
    total += rc;

    rc = step2_spm_alloc();
    printf("  => %s\n\n", rc ? "FAIL" : "PASS"); fflush(stdout);
    total += rc;

    rc = step3_spm_write_read();
    printf("  => %s\n\n", rc ? "FAIL" : "PASS"); fflush(stdout);
    total += rc;

    rc = step4_dma_buf_write_read();
    printf("  => %s\n\n", rc ? "FAIL" : "PASS"); fflush(stdout);
    total += rc;

    rc = step5_mmio_dma_dram_to_spm();
    printf("  => %s\n\n", rc ? "FAIL" : "PASS"); fflush(stdout);
    total += rc;

#ifdef USE_XSPM_INSN
    rc = step6_xinsn_dma_dram_to_spm();
    printf("  => %s\n\n", rc ? "FAIL" : "PASS"); fflush(stdout);
    total += rc;

    rc = step7_xinsn_dma_spm_to_dram();
    printf("  => %s\n\n", rc ? "FAIL" : "PASS"); fflush(stdout);
    total += rc;
#endif

    printf("=== Result: %s (total failures: %d) ===\n",
           total ? "FAIL" : "ALL PASS", total);
    fflush(stdout);

    return total ? 1 : 0;
}

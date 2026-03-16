#define USE_XSPM_INSN
#include <stdio.h>
#include <stdlib.h>
#include "../libspm.h"

#define N 32

static int test_dram_to_spm(void)
{
    int *src = (int *)dma_buf_malloc(N * sizeof(int));
    int *spm_dst = (int *)spm_malloc(N * sizeof(int));

    for (int i = 0; i < N; i++)
        src[i] = 100 + i;

    spm_dma_copy(spm_dst, src, N * sizeof(int));

    int fail = 0;
    for (int i = 0; i < N; i++) {
        if (spm_dst[i] != 100 + i) {
            printf("  FAIL dram->spm [%d]: expected %d, got %d\n",
                   i, 100 + i, spm_dst[i]);
            fail++;
        }
    }
    return fail;
}

static int test_spm_to_dram(void)
{
    int *spm_src = (int *)spm_malloc(N * sizeof(int));
    int *dst = (int *)dma_buf_malloc(N * sizeof(int));

    for (int i = 0; i < N; i++) {
        spm_src[i] = 200 + i;
        dst[i] = 0;
    }

    xspm_dma((uintptr_t)dst, (uintptr_t)spm_src, N * sizeof(int));
    xspm_dma_wait();

    int fail = 0;
    for (int i = 0; i < N; i++) {
        if (dst[i] != 200 + i) {
            printf("  FAIL spm->dram [%d]: expected %d, got %d\n",
                   i, 200 + i, dst[i]);
            fail++;
        }
    }
    return fail;
}

static int test_gemm_xinsn(void)
{
    int n = 8;
    int *a   = (int *)dma_buf_malloc(n * n * sizeof(int));
    int *b   = (int *)dma_buf_malloc(n * n * sizeof(int));
    int *c   = (int *)dma_buf_malloc(n * n * sizeof(int));
    int *ref = (int *)dma_buf_malloc(n * n * sizeof(int));

    for (int i = 0; i < n * n; i++) {
        a[i] = i + 1;
        b[i] = i + 1;
        c[i] = 0;
        ref[i] = 0;
    }

    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++)
            for (int j = 0; j < n; j++)
                ref[i * n + j] += a[i * n + k] * b[k * n + j];

    int bs = 4;
    int *sa = (int *)spm_malloc(bs * bs * sizeof(int));
    int *sb = (int *)spm_malloc(bs * bs * sizeof(int));
    int *sc = (int *)spm_malloc(bs * bs * sizeof(int));

    for (int i = 0; i < n; i += bs) {
        for (int j = 0; j < n; j += bs) {
            spm_memset(sc, 0, bs * bs * sizeof(int));
            for (int k = 0; k < n; k += bs) {
                for (int m = 0; m < bs; m++) {
                    xspm_dma((uintptr_t)(sa + m * bs),
                             (uintptr_t)(a + (m + i) * n + k),
                             bs * sizeof(int));
                    xspm_dma_wait();

                    xspm_dma((uintptr_t)(sb + m * bs),
                             (uintptr_t)(b + (m + k) * n + j),
                             bs * sizeof(int));
                    xspm_dma_wait();
                }
                for (int ii = 0; ii < bs; ii++)
                    for (int kk = 0; kk < bs; kk++)
                        for (int jj = 0; jj < bs; jj++)
                            sc[ii * bs + jj] += sa[ii * bs + kk] * sb[kk * bs + jj];
            }
            for (int m = 0; m < bs; m++) {
                xspm_dma((uintptr_t)(c + (m + i) * n + j),
                         (uintptr_t)(sc + m * bs),
                         bs * sizeof(int));
                xspm_dma_wait();
            }
        }
    }

    int fail = 0;
    for (int i = 0; i < n * n; i++) {
        if (c[i] != ref[i]) {
            printf("  FAIL gemm [%d]: expected %d, got %d\n", i, ref[i], c[i]);
            fail++;
        }
    }
    return fail;
}

int main(void)
{
    int total = 0;

    printf("=== Xspm custom instruction tests ===\n");

    printf("[1] DRAM -> SPM copy ... ");
    int f1 = test_dram_to_spm();
    printf("%s (%d failures)\n", f1 ? "FAIL" : "PASS", f1);
    total += f1;

    printf("[2] SPM -> DRAM copy ... ");
    int f2 = test_spm_to_dram();
    printf("%s (%d failures)\n", f2 ? "FAIL" : "PASS", f2);
    total += f2;

    printf("[3] Blocked GEMM via xspm ... ");
    int f3 = test_gemm_xinsn();
    printf("%s (%d failures)\n", f3 ? "FAIL" : "PASS", f3);
    total += f3;

    printf("=== Result: %s (total failures: %d) ===\n",
           total ? "FAIL" : "ALL PASS", total);

    return total ? 1 : 0;
}

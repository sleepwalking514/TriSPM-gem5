#define USE_XSPM_INSN
#include <stdio.h>
#include <stdlib.h>
#include "../libspm.h"

#define N 32
#define BS 4

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

static void dma_load_block(int *spm, const int *dram, int ld)
{
    for (int r = 0; r < BS; r++) {
        xspm_dma((uintptr_t)(spm  + r * BS),
                 (uintptr_t)(dram + r * ld),
                 BS * sizeof(int));
        xspm_dma_wait();
    }
}

static void dma_store_block(int *dram, const int *spm, int ld)
{
    for (int r = 0; r < BS; r++) {
        xspm_dma((uintptr_t)(dram + r * ld),
                 (uintptr_t)(spm  + r * BS),
                 BS * sizeof(int));
        xspm_dma_wait();
    }
}

/*
 * BSxBS block multiply in SPM: C += A * B.
 * ikj order with scalar promotion of a[i][k].
 */
static void spm_block_matmul(const int *a, const int *b, int *c)
{
    for (int i = 0; i < BS; i++) {
        for (int k = 0; k < BS; k++) {
            int aik = a[i * BS + k];
            for (int j = 0; j < BS; j++)
                c[i * BS + j] += aik * b[k * BS + j];
        }
    }
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

    int nb = n / BS;

    int *spm_a = (int *)spm_malloc(BS * BS * sizeof(int));
    int *spm_b = (int *)spm_malloc(BS * BS * sizeof(int));
    int *spm_b_buf = (int *)spm_malloc(BS * BS * sizeof(int));
    int *spm_c = (int *)spm_malloc(nb * BS * BS * sizeof(int));

    for (int i = 0; i < n; i += BS) {
        spm_memset(spm_c, 0, nb * BS * BS * sizeof(int));

        for (int k = 0; k < n; k += BS) {
            dma_load_block(spm_a, a + i * n + k, n);
            // for (int j = 0; j < n; j += BS) {
            //     dma_load_block(spm_b, b + k * n + j, n);
            //     spm_block_matmul(spm_a, spm_b,
            //                     spm_c + (j / BS) * BS * BS);
            // }

            dma_load_block(spm_b, b + k * n, n);
            for (int j = 0; j < n - BS; j += BS) {
                for (int ii = 0; ii < BS; ii++) {
                    xspm_dma((uintptr_t)(spm_b_buf + ii * BS),
                             (uintptr_t)(b + k * n + j + BS + ii * n),
                             BS * sizeof(int));

                    for (int kk = 0; kk < BS; kk++) {
                        int aik = spm_a[ii * BS + kk];
                        for (int jj = 0; jj < BS; jj++) {
                            spm_c[(j / BS) * BS * BS + ii * BS + jj] += aik * spm_b[kk * BS + jj];
                        }
                    }

                    xspm_dma_wait();
                }

                int *tmp = spm_b;
                spm_b = spm_b_buf;
                spm_b_buf = tmp;
            }
            spm_block_matmul(spm_a, spm_b,
                             spm_c + ((n - BS) / BS) * BS * BS);
        }

        for (int j = 0; j < n; j += BS)
            dma_store_block(c + i * n + j,
                            spm_c + (j / BS) * BS * BS, n);
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

    // printf("[1] DRAM -> SPM copy ... ");
    // int f1 = test_dram_to_spm();
    // printf("%s (%d failures)\n", f1 ? "FAIL" : "PASS", f1);
    // total += f1;

    // printf("[2] SPM -> DRAM copy ... ");
    // int f2 = test_spm_to_dram();
    // printf("%s (%d failures)\n", f2 ? "FAIL" : "PASS", f2);
    // total += f2;

    printf("[3] Blocked GEMM via xspm ... ");
    int f3 = test_gemm_xinsn();
    printf("%s (%d failures)\n", f3 ? "FAIL" : "PASS", f3);
    total += f3;

    printf("=== Result: %s (total failures: %d) ===\n",
           total ? "FAIL" : "ALL PASS", total);

    return total ? 1 : 0;
}

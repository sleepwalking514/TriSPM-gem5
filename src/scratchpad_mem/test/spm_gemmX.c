#define USE_XSPM_INSN
#include <stdio.h>
#include <stdlib.h>
#include "../libspm.h"

#define BS 32

/*
 * Row-by-row DMA for a BSxBS sub-block.
 * Single-issue DMA engine: must wait after each row.
 * Matrix in DRAM is row-major with leading dimension `ld`;
 * block in SPM is packed contiguous (stride = BS).
 */
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

/*
 * Tiled GEMM on SPM.
 *
 * Loop order: i -> k -> j
 *   - A(i,k) loaded once per (i,k), reused across all j
 *     => 8x fewer A DMA transfers compared to i->j->k
 *   - C row-panel (all j-blocks for current i) stays in SPM
 *     => C is never DMA-loaded, only stored once per i-strip
 *
 * DMA budget (N=256, BS=32, nb=8):
 *   A loads:  nb^2 blocks = 64,  each 32 row DMAs =>  2048  (was 16384)
 *   B loads:  nb^3 blocks = 512, each 32 row DMAs => 16384  (unchanged)
 *   C stores: nb^2 blocks = 64,  each 32 row DMAs =>  2048  (unchanged)
 *   Total: 20480 row DMAs  (was 34816, -41%)
 *
 * SPM footprint: 1*A + 1*B + nb*C = 4 + 4 + 32 = 40 KB
 */
void blocked_gemm(const int *a, const int *b, int *c, int n)
{
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
}

int main(void)
{
    printf("Init...\n");

    int n = 1024;
    int *a = (int *)dma_buf_malloc(n * n * sizeof(int));
    int *b = (int *)dma_buf_malloc(n * n * sizeof(int));
    int *c = (int *)dma_buf_malloc(n * n * sizeof(int));

    m5_reset_stats(0, 0);
    for (int i = 0; i < n * n; i++) {
        a[i] = i + 1;
        b[i] = i + 1;
        c[i] = 0;
    }
    m5_dump_stats(0, 0);
    m5_reset_stats(0, 0);

    blocked_gemm(a, b, c, n);

    m5_dump_stats(0, 0);

    return 0;
}

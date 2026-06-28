#define USE_XSPM_INSN
#include <stdio.h>
#include <stdlib.h>
#include "../libspm.h"

/* Compile-time matrix / tile dimensions.
 * Override at build time:  -DN=256 -DBS=32                          */
#ifndef N
#define N 32
#endif

#ifndef BS
#define BS 16
#endif

#define NB          (N / BS)
#define BLOCK_ELEMS (BS * BS)
#define BLOCK_BYTES (BLOCK_ELEMS * sizeof(int))

#if N < BS || N % BS != 0
#error "N must be >= BS and divisible by BS"
#endif

/*
 * ---------- Block-contiguous DRAM layout ----------
 *
 * An NxN matrix is stored as NB*NB contiguous BS*BS blocks.
 * Block(bi, bj) occupies addresses  base + (bi*NB + bj) * BS*BS
 * Inside each block elements are row-major: block[i*BS + j].
 *
 * This lets us DMA an entire BS*BS tile in ONE transfer (BS*BS*4 bytes)
 * instead of BS separate row-by-row transfers.
 */
static inline const int *blk_a(const int *base, int bi, int bk)
{
    return base + (bi * NB + bk) * BLOCK_ELEMS;
}
static inline const int *blk_b(const int *base, int bk, int bj)
{
    return base + (bk * NB + bj) * BLOCK_ELEMS;
}
static inline int *blk_c(int *base, int bi, int bj)
{
    return base + (bi * NB + bj) * BLOCK_ELEMS;
}

/* ---------- BS*BS micro-kernel: C = A * B (first) or C += A * B --- */

static inline void block_matmul(const int *restrict a, const int *restrict b,
                                 int *restrict c, int is_first)
{
    if (is_first) {
        for (int i = 0; i < BS; i++)
            for (int k = 0; k < BS; k++) {
                int aik = a[i * BS + k];
                for (int j = 0; j < BS; j++)
                    c[i * BS + j] = (k == 0) ? aik * b[k * BS + j]
                                              : c[i * BS + j] + aik * b[k * BS + j];
            }
    } else {
        for (int i = 0; i < BS; i++)
            for (int k = 0; k < BS; k++) {
                int aik = a[i * BS + k];
                for (int j = 0; j < BS; j++)
                    c[i * BS + j] += aik * b[k * BS + j];
            }
    }
}

/*
 * ---------- Tiled GEMM with double-buffered A and B ----------
 *
 * Exploits the 4-entry DMA descriptor queue for three optimizations
 * over the previous single-transfer design:
 *
 *   1) Priming: A[bi,0] and B[0,0] are loaded concurrently
 *      (2 queue entries, 1 wait) instead of two sequential loads.
 *
 *   2) bk transition: at the last bj of each bk, the next A tile
 *      and B[bk+1,0] are prefetched concurrently (2 queue entries)
 *      while the final block_matmul of the current bk executes.
 *      This eliminates the synchronous A load that previously
 *      stalled the pipeline at every bk boundary.
 *
 *   3) C store-back: tiles are flushed in batches of 4 (filling
 *      the full descriptor queue) instead of one-at-a-time.
 *
 * Within the bj loop, B tiles are still double-buffered: DMA for
 * the next B overlaps with the current block_matmul (1 queue entry).
 *
 * Loop order:  bi -> bk -> bj
 *   - A(bi,bk) loaded once per (bi,bk), reused across all bj
 *   - C row-panel stays in SPM, flushed once per bi
 *   - B tiles double-buffered across bj
 *   - A tiles double-buffered across bk
 *
 * SPM footprint:
 *   spm_a0, spm_a1 : 2  blocks               (double-buffer A)
 *   spm_b0, spm_b1 : 2  blocks               (double-buffer B)
 *   spm_c          : NB blocks
 *   Total          : (4 + NB) * BS*BS*4
 *
 * Pipeline stall points per bi strip (vs old design):
 *   Old:   2*NB sync loads + NB sync stores = 3*NB stalls
 *   New:   1 prime wait + ceil(NB/4) store waits ≈ 3
 *
 * With N=256 BS=32 (NB=8):  SPM = 48 KB,  stalls: 3 (was 24)
 */
void blocked_gemm(const int *restrict a, const int *restrict b,
                  int *restrict c)
{
    int *spm_a0 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_a1 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_b0 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_b1 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_c  = (int *)spm_malloc(NB * BLOCK_BYTES);

    for (int bi = 0; bi < NB; bi++) {

        /* ---- Prime: load A[bi,0] and B[0,0] concurrently ---- */
        xspm_dma((uintptr_t)spm_a0,
                 (uintptr_t)blk_a(a, bi, 0), BLOCK_BYTES);
        xspm_dma((uintptr_t)spm_b0,
                 (uintptr_t)blk_b(b, 0, 0),  BLOCK_BYTES);
        xspm_dma_wait();

        int *cur_a = spm_a0, *nxt_a = spm_a1;
        int *next_bk_b = spm_b0;

        for (int bk = 0; bk < NB; bk++) {
            int *cur_b = next_bk_b;
            int *nxt_b = (cur_b == spm_b0) ? spm_b1 : spm_b0;

            for (int bj = 0; bj < NB; bj++) {
                int n_prefetch = 0;

                if (bj < NB - 1) {
                    /* Prefetch next B tile (1 queue entry) */
                    xspm_dma((uintptr_t)nxt_b,
                             (uintptr_t)blk_b(b, bk, bj + 1),
                             BLOCK_BYTES);
                    n_prefetch = 1;
                } else if (bk < NB - 1) {
                    /* Last bj of this bk: prefetch next A and
                     * first B of next bk (2 queue entries).
                     * Both overlap with the final matmul below. */
                    xspm_dma((uintptr_t)nxt_a,
                             (uintptr_t)blk_a(a, bi, bk + 1),
                             BLOCK_BYTES);
                    xspm_dma((uintptr_t)nxt_b,
                             (uintptr_t)blk_b(b, bk + 1, 0),
                             BLOCK_BYTES);
                    n_prefetch = 2;
                    next_bk_b = nxt_b;
                }

                /* Compute: first bk uses = instead of += to avoid memset */
                block_matmul(cur_a, cur_b, spm_c + bj * BLOCK_ELEMS, bk == 0);

                if (n_prefetch > 0)
                    xspm_dma_wait();

                /* Rotate B buffers */
                int *tmp = cur_b; cur_b = nxt_b; nxt_b = tmp;
            }

            /* Rotate A buffers for next bk */
            if (bk < NB - 1) {
                int *tmp = cur_a; cur_a = nxt_a; nxt_a = tmp;
            }
        }

        /* ---- Store C row-panel: batches of 4 (full queue width) ---- */
        for (int bj = 0; bj < NB; bj += 4) {
            int batch = (NB - bj < 4) ? (NB - bj) : 4;
            for (int k = 0; k < batch; k++)
                xspm_dma((uintptr_t)blk_c(c, bi, bj + k),
                         (uintptr_t)(spm_c + (bj + k) * BLOCK_ELEMS),
                         BLOCK_BYTES);
            xspm_dma_wait();
        }
    }
}

/* ---------- helpers ---------- */

/*
 * Compiled at O1 to keep initialization simple and outside the measured ROI.
 */
__attribute__((optimize("O1")))
static void init_block_matrix(int *m)
{
    for (int bi = 0; bi < NB; bi++)
        for (int bj = 0; bj < NB; bj++)
            for (int i = 0; i < BS; i++)
                for (int j = 0; j < BS; j++)
                    m[(bi * NB + bj) * BLOCK_ELEMS + i * BS + j] =
                        (bi * BS + i) * N + (bj * BS + j) + 1;
}

__attribute__((optimize("O1")))
static void zero_matrix(int *m, int count)
{
    for (int i = 0; i < count; i++)
        m[i] = 0;
}

int main(void)
{
    printf("GEMM  N=%d  BS=%d  NB=%d\n", N, BS, NB);

    int *a = (int *)dma_buf_malloc(N * N * sizeof(int));
    int *b = (int *)dma_buf_malloc(N * N * sizeof(int));
    int *c = (int *)dma_buf_malloc(N * N * sizeof(int));

    init_block_matrix(a);
    init_block_matrix(b);
    zero_matrix(c, N * N);

    m5_reset_stats(0, 0);
    m5_dump_stats(0, 0);
    m5_reset_stats(0, 0);

    blocked_gemm(a, b, c);

    m5_dump_stats(0, 0);

    /* Touch result to prevent dead-code elimination of blocked_gemm */
    volatile int sink = 0;
    for (int i = 0; i < N * N; i++)
        sink += ((volatile int *)c)[i];

    return sink & 0;
}

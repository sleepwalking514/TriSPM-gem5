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

/* ---------- DMA helpers (one full block per transfer) ---------- */

static inline void dma_load(int *spm_dst, const int *dram_src)
{
    xspm_dma((uintptr_t)spm_dst, (uintptr_t)dram_src, BLOCK_BYTES);
    xspm_dma_wait();
}

static inline void dma_store(int *dram_dst, const int *spm_src)
{
    xspm_dma((uintptr_t)dram_dst, (uintptr_t)spm_src, BLOCK_BYTES);
    xspm_dma_wait();
}

/* ---------- BS*BS micro-kernel: C += A * B  (ikj, scalar promotion) --- */

static void block_matmul(const int *a, const int *b, int *c)
{
    for (int i = 0; i < BS; i++)
        for (int k = 0; k < BS; k++) {
            int aik = a[i * BS + k];
            for (int j = 0; j < BS; j++)
                c[i * BS + j] += aik * b[k * BS + j];
        }
}

/*
 * ---------- Tiled GEMM with double-buffered B ----------
 *
 * Loop order:  bi -> bk -> bj
 *   - A(bi,bk) loaded once per (bi,bk), reused across all bj
 *   - C row-panel (all bj for current bi) stays in SPM, stored once per bi
 *   - B tiles are double-buffered: DMA for next B overlaps with compute
 *
 * SPM footprint:
 *   spm_a  : 1  block  = BS*BS*4
 *   spm_b0 : 1  block  = BS*BS*4
 *   spm_b1 : 1  block  = BS*BS*4   (double-buffer)
 *   spm_c  : NB blocks = NB*BS*BS*4
 *   Total  : (3 + NB) * BS*BS*4
 *
 * DMA budget (per bi strip):
 *   A loads  :  NB           blocks  (one per bk)
 *   B loads  :  NB * NB      blocks  (NB per bk)
 *   C stores :  NB           blocks
 *   Total    :  NB*(NB+2)    full-block DMAs
 *
 * With N=256 BS=32 (NB=8):  SPM = 44 KB,  DMA = 640 transfers of 4 KB
 */
void blocked_gemm(const int *restrict a, const int *restrict b, int *restrict c)
{
    int *spm_a  = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_b0 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_b1 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_c  = (int *)spm_malloc(NB * BLOCK_BYTES);

    for (int bi = 0; bi < NB; bi++) {

        spm_memset(spm_c, 0, NB * BLOCK_BYTES);

        for (int bk = 0; bk < NB; bk++) {
            dma_load(spm_a, blk_a(a, bi, bk));
            dma_load(spm_b0, blk_b(b, bk, 0));

            int *cur = spm_b0, *nxt = spm_b1;

            for (int bj = 0; bj < NB - 1; bj++) {
                /* async: prefetch next B while computing on current B */
                xspm_dma((uintptr_t)nxt,
                         (uintptr_t)blk_b(b, bk, bj + 1),
                         BLOCK_BYTES);

                block_matmul(spm_a, cur,
                             spm_c + bj * BLOCK_ELEMS);

                xspm_dma_wait();

                int *tmp = cur; cur = nxt; nxt = tmp;
            }

            /* last B block — no further prefetch needed */
            block_matmul(spm_a, cur,
                         spm_c + (NB - 1) * BLOCK_ELEMS);
        }

        for (int bj = 0; bj < NB; bj++)
            dma_store(blk_c(c, bi, bj),
                      spm_c + bj * BLOCK_ELEMS);
    }
}

/* ---------- helpers ---------- */

/*
 * Compiled at O1 to avoid gem5 crash: the O3 auto-vectoriser generates
 * RVV micro-ops (VPinVdMicroInst) that trigger a segfault inside gem5's
 * O3 pipeline when targeting uncacheable DMA-buffer memory.  Since this
 * is only init code (not measured), the lower optimisation level is fine.
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

    return 0;
}

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
#define ROW_BYTES   (N * sizeof(int))

#if N < BS || N % BS != 0
#error "N must be >= BS and divisible by BS"
#endif

/*
 * ---------- Standard row-major DRAM layout ----------
 *
 * Unlike spm_gemmX.c which requires block-contiguous pre-transformation,
 * this version works directly on standard row-major NxN matrices.
 * 2D DMA extracts BS*BS tiles from strided DRAM in a single command,
 * eliminating the need for layout conversion and replacing BS individual
 * 1D row-by-row transfers with one 2D descriptor.
 */
static inline const int *tile_a(const int *base, int bi, int bk)
{
    return base + (bi * BS) * N + (bk * BS);
}
static inline const int *tile_b(const int *base, int bk, int bj)
{
    return base + (bk * BS) * N + (bj * BS);
}
static inline int *tile_c(int *base, int bi, int bj)
{
    return base + (bi * BS) * N + (bj * BS);
}

/* ---------- 2D DMA helpers: load/store a BS*BS tile ---------- */

/* Async enqueue: DRAM (row-major, stride=N*4) -> SPM (contiguous, stride=BS*4) */
static inline void dma_load_tile(int *spm_dst, const int *dram_src)
{
    xspm_dma_stride(ROW_BYTES, BLOCK_BYTES / BS);
    xspm_dma_2d((uintptr_t)spm_dst, (uintptr_t)dram_src,
                BS * sizeof(int), BS);
}

/* Async enqueue: SPM (contiguous) -> DRAM (row-major) */
static inline void dma_store_tile(int *dram_dst, const int *spm_src)
{
    xspm_dma_stride(BLOCK_BYTES / BS, ROW_BYTES);
    xspm_dma_2d((uintptr_t)dram_dst, (uintptr_t)spm_src,
                BS * sizeof(int), BS);
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
 * ---------- Tiled GEMM with 2D DMA + double-buffered A and B ----------
 *
 * Key differences from spm_gemmX.c (1D DMA, block-contiguous layout):
 *
 *   1) Matrices are standard row-major in DRAM — no layout conversion.
 *      2D DMA handles the stride (N*sizeof(int)) <-> contiguous SPM mapping
 *      in hardware, replacing BS per-row 1D transfers with 1 descriptor.
 *
 *   2) Each DMA command is a pair: xspm_dma_stride() + xspm_dma_2d().
 *      The stride register is sticky until the next spm.dma.stride, so
 *      consecutive loads of same-matrix tiles reuse the same stride setting.
 *
 * Pipeline structure (identical to spm_gemmX.c):
 *
 *   - Double-buffer A across bk: cur_a / nxt_a
 *   - Double-buffer B across bj: cur_b / nxt_b
 *   - Prime: load A[bi,0] and B[0,0] concurrently (2 queue entries, 1 wait)
 *   - Inner loop: prefetch next B (or next A+B at bk boundary) overlapping
 *     with block_matmul, then wait
 *   - C row-panel: store in batches of 2 (each store needs stride+2d = 2
 *     instructions, but only 1 queue entry; batch of 2 fits in queue)
 *
 * SPM footprint:
 *   spm_a0, spm_a1 : 2  blocks               (double-buffer A)
 *   spm_b0, spm_b1 : 2  blocks               (double-buffer B)
 *   spm_c          : NB blocks
 *   Total          : (4 + NB) * BS*BS*4
 */
void blocked_gemm_2d(const int *restrict a, const int *restrict b,
                     int *restrict c)
{
    int *spm_a0 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_a1 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_b0 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_b1 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_c  = (int *)spm_malloc(NB * BLOCK_BYTES);

    for (int bi = 0; bi < NB; bi++) {

        /* ---- Prime: load A[bi,0] and B[0,0] concurrently ---- */
        dma_load_tile(spm_a0, tile_a(a, bi, 0));
        dma_load_tile(spm_b0, tile_b(b, 0, 0));
        xspm_dma_wait();

        int *cur_a = spm_a0, *nxt_a = spm_a1;
        int *next_bk_b = spm_b0;

        for (int bk = 0; bk < NB; bk++) {
            int *cur_b = next_bk_b;
            int *nxt_b = (cur_b == spm_b0) ? spm_b1 : spm_b0;

            for (int bj = 0; bj < NB; bj++) {
                int n_prefetch = 0;

                if (bj < NB - 1) {
                    /* Prefetch next B tile */
                    dma_load_tile(nxt_b, tile_b(b, bk, bj + 1));
                    n_prefetch = 1;
                } else if (bk < NB - 1) {
                    /* Last bj: prefetch next A and first B of next bk */
                    dma_load_tile(nxt_a, tile_a(a, bi, bk + 1));
                    dma_load_tile(nxt_b, tile_b(b, bk + 1, 0));
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

        /* ---- Store C row-panel: batches of 2 (stride+2d per tile) ---- */
        for (int bj = 0; bj < NB; bj += 2) {
            int batch = (NB - bj < 2) ? (NB - bj) : 2;
            for (int k = 0; k < batch; k++)
                dma_store_tile(tile_c(c, bi, bj + k),
                               spm_c + (bj + k) * BLOCK_ELEMS);
            xspm_dma_wait();
        }
    }
}

/* ---------- helpers ---------- */

__attribute__((optimize("O1")))
static void init_matrix(int *m)
{
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            m[i * N + j] = i * N + j + 1;
}

__attribute__((optimize("O1")))
static void zero_matrix(int *m, int count)
{
    for (int i = 0; i < count; i++)
        m[i] = 0;
}

/* Simple reference GEMM for correctness check */
__attribute__((optimize("O1")))
static void ref_gemm(const int *a, const int *b, int *c)
{
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++) {
            int aik = a[i * N + k];
            for (int j = 0; j < N; j++)
                c[i * N + j] += aik * b[k * N + j];
        }
}

int main(void)
{
    printf("2D-DMA GEMM  N=%d  BS=%d  NB=%d\n", N, BS, NB);

    int *a = (int *)dma_buf_malloc(N * N * sizeof(int));
    int *b = (int *)dma_buf_malloc(N * N * sizeof(int));
    int *c = (int *)dma_buf_malloc(N * N * sizeof(int));

    init_matrix(a);
    init_matrix(b);
    zero_matrix(c, N * N);

    m5_reset_stats(0, 0);
    m5_dump_stats(0, 0);
    m5_reset_stats(0, 0);

    blocked_gemm_2d(a, b, c);

    m5_dump_stats(0, 0);

    /* Touch result to prevent dead-code elimination */
    volatile int sink = 0;
    for (int i = 0; i < N * N; i++)
        sink += ((volatile int *)c)[i];

    return sink & 0;
}

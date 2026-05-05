/*
 * Overlap vs no-overlap microbenchmark.
 *
 * Same tiled int GEMM, same memory traffic, same compute.  The only
 * difference is whether the DMA prefetch runs concurrently with the
 * block_matmul of the current tile (OVERLAP=1) or the caller waits
 * immediately after each enqueue (OVERLAP=0).
 *
 * Build:
 *   $CLANG $CFLAGS -DOVERLAP=1 -o spm_gemm_overlap_on \
 *       spm_gemm_overlap.c
 *   $CLANG $CFLAGS -DOVERLAP=0 -o spm_gemm_overlap_off \
 *       spm_gemm_overlap.c
 */
#define USE_XSPM_INSN
#include <stdio.h>
#include <stdlib.h>

#include "../libspm.h"

#ifndef N
#define N 256
#endif
#ifndef BS
#define BS 32
#endif
#ifndef OVERLAP
#define OVERLAP 1
#endif

#define NB (N / BS)
#define BLOCK_ELEMS (BS * BS)
#define BLOCK_BYTES (BLOCK_ELEMS * sizeof(int))
#define ROW_BYTES (N * sizeof(int))

static inline const int *
tile_a(const int *base, int bi, int bk)
{
    return base + (bi * BS) * N + (bk * BS);
}
static inline const int *
tile_b(const int *base, int bk, int bj)
{
    return base + (bk * BS) * N + (bj * BS);
}
static inline int *
tile_c(int *base, int bi, int bj)
{
    return base + (bi * BS) * N + (bj * BS);
}

static inline void
dma_load_tile(int *spm_dst, const int *dram_src)
{
    xspm_dma_stride(ROW_BYTES, BLOCK_BYTES / BS);
    xspm_dma_2d((uintptr_t)spm_dst, (uintptr_t)dram_src, BS * sizeof(int), BS);
}
static inline void
dma_store_tile(int *dram_dst, const int *spm_src)
{
    xspm_dma_stride(BLOCK_BYTES / BS, ROW_BYTES);
    xspm_dma_2d((uintptr_t)dram_dst, (uintptr_t)spm_src, BS * sizeof(int), BS);
}

static inline void
block_matmul(const int *restrict a, const int *restrict b, int *restrict c,
             int is_first)
{
    if (is_first) {
        for (int i = 0; i < BS; i++) {
            for (int k = 0; k < BS; k++) {
                int aik = a[i * BS + k];
                for (int j = 0; j < BS; j++) {
                    c[i * BS + j] = (k == 0)
                                        ? aik * b[k * BS + j]
                                        : c[i * BS + j] + aik * b[k * BS + j];
                }
            }
        }
    } else {
        for (int i = 0; i < BS; i++) {
            for (int k = 0; k < BS; k++) {
                int aik = a[i * BS + k];
                for (int j = 0; j < BS; j++) {
                    c[i * BS + j] += aik * b[k * BS + j];
                }
            }
        }
    }
}

/*
 * OVERLAP=1:  classic double-buffer; issue prefetch for NEXT tile,
 *             then compute on CURRENT tile, then wait.
 *
 * OVERLAP=0:  issue load for NEXT tile and wait BEFORE computing,
 *             i.e. every DMA is on the critical path.  The SPM
 *             address generation and double-buffer state are still
 *             there so instruction count is comparable.
 */
void
blocked_gemm(const int *restrict a, const int *restrict b, int *restrict c)
{
    int *spm_a0 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_a1 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_b0 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_b1 = (int *)spm_malloc(BLOCK_BYTES);
    int *spm_c = (int *)spm_malloc(NB * BLOCK_BYTES);

    for (int bi = 0; bi < NB; bi++) {
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
                    dma_load_tile(nxt_b, tile_b(b, bk, bj + 1));
                    n_prefetch = 1;
                } else if (bk < NB - 1) {
                    dma_load_tile(nxt_a, tile_a(a, bi, bk + 1));
                    dma_load_tile(nxt_b, tile_b(b, bk + 1, 0));
                    n_prefetch = 2;
                    next_bk_b = nxt_b;
                }

#if OVERLAP
                /* Prefetch runs concurrently with compute */
                block_matmul(cur_a, cur_b, spm_c + bj * BLOCK_ELEMS, bk == 0);
                if (n_prefetch > 0) {
                    xspm_dma_wait();
                }
#else
                /* Force the prefetch onto the critical path */
                if (n_prefetch > 0) {
                    xspm_dma_wait();
                }
                block_matmul(cur_a, cur_b, spm_c + bj * BLOCK_ELEMS, bk == 0);
#endif

                int *tmp = cur_b;
                cur_b = nxt_b;
                nxt_b = tmp;
            }

            if (bk < NB - 1) {
                int *tmp = cur_a;
                cur_a = nxt_a;
                nxt_a = tmp;
            }
        }

        for (int bj = 0; bj < NB; bj += 2) {
            int batch = (NB - bj < 2) ? (NB - bj) : 2;
            for (int k = 0; k < batch; k++) {
                dma_store_tile(tile_c(c, bi, bj + k),
                               spm_c + (bj + k) * BLOCK_ELEMS);
            }
            xspm_dma_wait();
        }
    }
}

__attribute__((optimize("O1"))) static void
init_matrix(int *m)
{
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            m[i * N + j] = (i * 3 + j) & 0xff;
        }
    }
}
__attribute__((optimize("O1"))) static void
zero_matrix(int *m, int count)
{
    for (int i = 0; i < count; i++) {
        m[i] = 0;
    }
}

int
main(void)
{
    printf("GEMM overlap=%d  N=%d  BS=%d  NB=%d\n", OVERLAP, N, BS, NB);

    int *a = (int *)dma_buf_malloc(N * N * sizeof(int));
    int *b = (int *)dma_buf_malloc(N * N * sizeof(int));
    int *c = (int *)dma_buf_malloc(N * N * sizeof(int));

    init_matrix(a);
    init_matrix(b);
    zero_matrix(c, N * N);

    m5_reset_stats(0, 0);
    blocked_gemm(a, b, c);
    m5_dump_stats(0, 0);

    volatile int sink = 0;
    for (int i = 0; i < N * N; i++) {
        sink += ((volatile int *)c)[i];
    }
    return sink & 0;
}

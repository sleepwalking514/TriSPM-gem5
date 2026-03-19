#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../libspm.h"

#ifndef N
#define N 32
#endif

#ifndef BS
#define BS 16
#endif

/*
 * 6-loop tiled GEMM optimised for hardware cache hierarchy.
 *
 * Outer tiling (ii-kk-jj): working set of 3 BS*BS tiles ≈ 12KB fits in L1D.
 *   - kk in middle: A(ii,kk) tile stays hot in L1 while jj sweeps,
 *     matching the reuse pattern that caches reward automatically.
 *
 * Inner micro-kernel (i-k-j):
 *   - a[i][k] hoisted into a register (scalar promotion) → 1 load per (i,k)
 *   - b[k][j] swept sequentially → perfect cache-line utilisation
 *   - c[i][j] swept sequentially → same cache lines reused across k
 *
 * No explicit data copying — the entire data-movement strategy relies on the
 * cache keeping recently-touched lines warm, which is the whole point of a
 * cache-based memory system.
 */
static void tiled_gemm(const int *restrict a, const int *restrict b,
                        int *restrict c, int n)
{
    for (int ii = 0; ii < n; ii += BS) {
        for (int kk = 0; kk < n; kk += BS) {
            for (int jj = 0; jj < n; jj += BS) {
                for (int i = ii; i < ii + BS; i++) {
                    for (int k = kk; k < kk + BS; k++) {
                        int a_ik = a[i * n + k];
                        for (int j = jj; j < jj + BS; j++)
                            c[i * n + j] += a_ik * b[k * n + j];
                    }
                }
            }
        }
    }
}

int main(void)
{
    printf("Cache GEMM  N=%d  BS=%d\n", N, BS);

    int n = N;
    int *a = (int *)malloc(n * n * sizeof(int));
    int *b = (int *)malloc(n * n * sizeof(int));
    int *c = (int *)malloc(n * n * sizeof(int));

    for (int i = 0; i < n * n; i++) {
        a[i] = i + 1;
        b[i] = i + 1;
        c[i] = 0;
    }

    m5_reset_stats(0, 0);
    m5_dump_stats(0, 0);
    m5_reset_stats(0, 0);

    tiled_gemm(a, b, c, n);

    m5_dump_stats(0, 0);

    free(a);
    free(b);
    free(c);
    return 0;
}

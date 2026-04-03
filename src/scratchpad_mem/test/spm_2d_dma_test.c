#define USE_XSPM_INSN
#include <stdio.h>
#include <stdlib.h>
#include "../libspm.h"

// Matrix dimensions and tile size
#define M   16
#define N   16
#define BS  4

// ---------- Test 1: 2D Load (DRAM -> SPM) ----------
// Load a BSxBS tile from a row-major MxN matrix in DRAM into contiguous SPM.
static int test_2d_load(void)
{
    int *mat = (int *)dma_buf_malloc(M * N * sizeof(int));
    int *spm_tile = (int *)spm_malloc(BS * BS * sizeof(int));

    // Fill matrix with known pattern: mat[i][j] = i*N + j
    for (int i = 0; i < M * N; i++)
        mat[i] = i;

    // Load tile starting at row=2, col=3
    int row0 = 2, col0 = 3;
    int *tile_src = mat + row0 * N + col0;

    spm_dma_copy_2d(
        spm_tile,               // dst: SPM (contiguous)
        tile_src,               // src: DRAM (strided)
        BS * sizeof(int),       // width: bytes per row
        BS,                     // height: number of rows
        N * sizeof(int),        // src_stride: DRAM row pitch
        BS * sizeof(int)        // dst_stride: SPM row pitch (contiguous)
    );

    int fail = 0;
    for (int r = 0; r < BS; r++) {
        for (int c = 0; c < BS; c++) {
            int expected = (row0 + r) * N + (col0 + c);
            int got = spm_tile[r * BS + c];
            if (got != expected) {
                printf("  FAIL 2D load [%d][%d]: expected %d, got %d\n",
                       r, c, expected, got);
                fail++;
            }
        }
    }
    return fail;
}

// ---------- Test 2: 2D Store (SPM -> DRAM) ----------
// Store a BSxBS tile from contiguous SPM back into a row-major matrix in DRAM.
static int test_2d_store(void)
{
    int *mat = (int *)dma_buf_malloc(M * N * sizeof(int));
    int *spm_tile = (int *)spm_malloc(BS * BS * sizeof(int));

    // Clear DRAM matrix
    for (int i = 0; i < M * N; i++)
        mat[i] = 0;

    // Fill SPM tile with known values
    for (int r = 0; r < BS; r++)
        for (int c = 0; c < BS; c++)
            spm_tile[r * BS + c] = 1000 + r * 10 + c;

    // Store tile to row=1, col=2 in DRAM matrix
    int row0 = 1, col0 = 2;
    int *tile_dst = mat + row0 * N + col0;

    spm_dma_copy_2d(
        tile_dst,               // dst: DRAM (strided)
        spm_tile,               // src: SPM (contiguous)
        BS * sizeof(int),       // width
        BS,                     // height
        BS * sizeof(int),       // src_stride: SPM (contiguous)
        N * sizeof(int)         // dst_stride: DRAM row pitch
    );

    int fail = 0;
    for (int r = 0; r < BS; r++) {
        for (int c = 0; c < BS; c++) {
            int expected = 1000 + r * 10 + c;
            int got = mat[(row0 + r) * N + (col0 + c)];
            if (got != expected) {
                printf("  FAIL 2D store [%d][%d]: expected %d, got %d\n",
                       r, c, expected, got);
                fail++;
            }
        }
    }

    // Verify surrounding elements are still zero
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            int in_tile = (i >= row0 && i < row0 + BS &&
                           j >= col0 && j < col0 + BS);
            if (!in_tile && mat[i * N + j] != 0) {
                printf("  FAIL 2D store: mat[%d][%d] = %d (expected 0)\n",
                       i, j, mat[i * N + j]);
                fail++;
            }
        }
    }
    return fail;
}

// ---------- Test 3: 2D vs 1D row-loop equivalence ----------
// Verify that a 2D DMA load produces the same result as BS individual
// 1D DMA copies (one per row).
static int test_2d_vs_1d(void)
{
    int *mat = (int *)dma_buf_malloc(M * N * sizeof(int));
    int *spm_2d = (int *)spm_malloc(BS * BS * sizeof(int));
    int *spm_1d = (int *)spm_malloc(BS * BS * sizeof(int));

    for (int i = 0; i < M * N; i++)
        mat[i] = 500 + i;

    int row0 = 3, col0 = 5;
    int *tile_src = mat + row0 * N + col0;

    // Method A: single 2D DMA
    spm_dma_copy_2d(
        spm_2d, tile_src,
        BS * sizeof(int), BS,
        N * sizeof(int), BS * sizeof(int)
    );

    // Method B: BS individual 1D DMAs (row loop)
    for (int r = 0; r < BS; r++) {
        spm_dma_copy(
            spm_1d + r * BS,
            tile_src + r * N,
            BS * sizeof(int)
        );
    }

    int fail = 0;
    for (int i = 0; i < BS * BS; i++) {
        if (spm_2d[i] != spm_1d[i]) {
            printf("  FAIL 2D-vs-1D [%d]: 2D=%d, 1D=%d\n",
                   i, spm_2d[i], spm_1d[i]);
            fail++;
        }
    }
    return fail;
}

// ---------- Test 4: 1D backward compatibility ----------
// Verify that spm_dma_copy() (which never writes stride/height registers)
// still works correctly after the engine upgrade.
static int test_1d_compat(void)
{
    int count = 64;
    int *src = (int *)dma_buf_malloc(count * sizeof(int));
    int *spm_dst = (int *)spm_malloc(count * sizeof(int));

    for (int i = 0; i < count; i++)
        src[i] = 9000 + i;

    spm_dma_copy(spm_dst, src, count * sizeof(int));

    int fail = 0;
    for (int i = 0; i < count; i++) {
        if (spm_dst[i] != 9000 + i) {
            printf("  FAIL 1D compat [%d]: expected %d, got %d\n",
                   i, 9000 + i, spm_dst[i]);
            fail++;
        }
    }
    return fail;
}

// ---------- Test 5: 2D GEMM tile load/compute/store ----------
// Full round-trip: load tiles from row-major matrices, compute, store back.
static int test_2d_gemm(void)
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

    // Reference GEMM
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++)
            for (int j = 0; j < n; j++)
                ref[i * n + j] += a[i * n + k] * b[k * n + j];

    // Tiled GEMM using 2D DMA
    int *spm_a = (int *)spm_malloc(BS * BS * sizeof(int));
    int *spm_b = (int *)spm_malloc(BS * BS * sizeof(int));
    int *spm_c = (int *)spm_malloc(BS * BS * sizeof(int));

    int nb = n / BS;
    for (int ii = 0; ii < n; ii += BS) {
        for (int jj = 0; jj < n; jj += BS) {
            // Zero the accumulator tile in SPM
            for (int x = 0; x < BS * BS; x++) spm_c[x] = 0;

            for (int kk = 0; kk < n; kk += BS) {
                // 2D load A tile [ii:ii+BS, kk:kk+BS]
                spm_dma_copy_2d(spm_a, a + ii * n + kk,
                                BS * sizeof(int), BS,
                                n * sizeof(int), BS * sizeof(int));

                // 2D load B tile [kk:kk+BS, jj:jj+BS]
                spm_dma_copy_2d(spm_b, b + kk * n + jj,
                                BS * sizeof(int), BS,
                                n * sizeof(int), BS * sizeof(int));

                // Compute C += A * B in SPM
                for (int i = 0; i < BS; i++)
                    for (int k = 0; k < BS; k++) {
                        int aik = spm_a[i * BS + k];
                        for (int j = 0; j < BS; j++)
                            spm_c[i * BS + j] += aik * spm_b[k * BS + j];
                    }
            }

            // 2D store C tile back to DRAM
            spm_dma_copy_2d(c + ii * n + jj, spm_c,
                            BS * sizeof(int), BS,
                            BS * sizeof(int), n * sizeof(int));
        }
    }

    int fail = 0;
    for (int i = 0; i < n * n; i++) {
        if (c[i] != ref[i]) {
            printf("  FAIL 2D gemm [%d]: expected %d, got %d\n",
                   i, ref[i], c[i]);
            fail++;
        }
    }
    return fail;
}

// ---------- Test 6: 2D GEMM tile load/compute/store with XSPM_INSN ----------
// Full round-trip: load tiles from row-major matrices, compute, store back.
static int test_2d_gemmX(void)
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

    // Reference GEMM
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++)
            for (int j = 0; j < n; j++)
                ref[i * n + j] += a[i * n + k] * b[k * n + j];

    // Tiled GEMM using 2D DMA
    int *spm_a = (int *)spm_malloc(BS * BS * sizeof(int));
    int *spm_b = (int *)spm_malloc(BS * BS * sizeof(int));
    int *spm_c = (int *)spm_malloc(BS * BS * sizeof(int));

    int nb = n / BS;
    for (int ii = 0; ii < n; ii += BS) {
        for (int jj = 0; jj < n; jj += BS) {
            // Zero the accumulator tile in SPM
            for (int x = 0; x < BS * BS; x++) spm_c[x] = 0;

            for (int kk = 0; kk < n; kk += BS) {
                // 2D load A tile [ii:ii+BS, kk:kk+BS]
                xspm_dma_copy_2d((uintptr_t)spm_a, (uintptr_t)(a + ii * n + kk),
                                BS * sizeof(int), BS,
                                n * sizeof(int), BS * sizeof(int));

                // 2D load B tile [kk:kk+BS, jj:jj+BS]
                xspm_dma_copy_2d((uintptr_t)spm_b, (uintptr_t)(b + kk * n + jj),
                                BS * sizeof(int), BS,
                                n * sizeof(int), BS * sizeof(int));

                // Compute C += A * B in SPM
                for (int i = 0; i < BS; i++)
                    for (int k = 0; k < BS; k++) {
                        int aik = spm_a[i * BS + k];
                        for (int j = 0; j < BS; j++)
                            spm_c[i * BS + j] += aik * spm_b[k * BS + j];
                    }
            }

            // 2D store C tile back to DRAM
            xspm_dma_copy_2d((uintptr_t)(c + ii * n + jj), (uintptr_t)spm_c,
                            BS * sizeof(int), BS,
                            BS * sizeof(int), n * sizeof(int));
        }
    }

    int fail = 0;
    for (int i = 0; i < n * n; i++) {
        if (c[i] != ref[i]) {
            printf("  FAIL 2D gemm [%d]: expected %d, got %d\n",
                   i, ref[i], c[i]);
            fail++;
        }
    }
    return fail;
}

int main(void)
{
    int total = 0;

    printf("=== 2D DMA tests ===\n");

    printf("[1] 2D Load (DRAM -> SPM) ... ");
    int f1 = test_2d_load();
    printf("%s (%d failures)\n", f1 ? "FAIL" : "PASS", f1);
    total += f1;

    printf("[2] 2D Store (SPM -> DRAM) ... ");
    int f2 = test_2d_store();
    printf("%s (%d failures)\n", f2 ? "FAIL" : "PASS", f2);
    total += f2;

    printf("[3] 2D vs 1D equivalence ... ");
    int f3 = test_2d_vs_1d();
    printf("%s (%d failures)\n", f3 ? "FAIL" : "PASS", f3);
    total += f3;

    printf("[4] 1D backward compat ... ");
    int f4 = test_1d_compat();
    printf("%s (%d failures)\n", f4 ? "FAIL" : "PASS", f4);
    total += f4;

    printf("[5] 2D GEMM tile round-trip ... ");
    int f5 = test_2d_gemm();
    printf("%s (%d failures)\n", f5 ? "FAIL" : "PASS", f5);
    total += f5;

    printf("[6] 2D GEMM tile round-trip with XSPM_INSN ... ");
    int f6 = test_2d_gemmX();
    printf("%s (%d failures)\n", f6 ? "FAIL" : "PASS", f6);
    total += f6;

    printf("=== Result: %s (total failures: %d) ===\n",
           total ? "FAIL" : "ALL PASS", total);

    return total ? 1 : 0;
}

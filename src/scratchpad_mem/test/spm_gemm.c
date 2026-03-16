#include <stdio.h>
#include <stdlib.h>
#include "../libspm.h"

void matmul(const int *a, const int *b, int *c, int n) {
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < n; k++) {
            for (int j = 0; j < n; j++) {
                c[i * n + j] += a[i * n + k] * b[k * n + j];
            }
        }
    }
}

void blocked_gemm(const int *a, const int *b, int *c, int n) {
    int block_size = 1;
    int *spm_a = (int *)spm_malloc(block_size * block_size * sizeof(int));
    int *spm_b = (int *)spm_malloc(block_size * block_size * sizeof(int));
    int *spm_c = (int *)spm_malloc(block_size * block_size * sizeof(int));

    for (int i = 0; i < n; i += block_size) {
        for (int j = 0; j < n; j += block_size) {
            spm_memset(spm_c, 0, block_size * block_size * sizeof(int));
            for (int k = 0; k < n; k += block_size) {
                for (int m = 0; m < block_size; m++) {
                    spm_dma_copy(spm_a + m * block_size, a + (m + i) * n + k, block_size * sizeof(int));
                    spm_dma_copy(spm_b + m * block_size, b + (m + k) * n + j, block_size * sizeof(int));
                }

                matmul(spm_a, spm_b, spm_c, block_size);
            }
            for (int m = 0; m < block_size; m++) {
                spm_dma_copy(c + (m + i) * n + j, spm_c + m * block_size, block_size * sizeof(int));
            }
        }
    }
}

int main(void) {
    printf("Init...\n");

    int n = 2;
    int *a = (int *)dma_buf_malloc(n * n * sizeof(int));
    int *b = (int *)dma_buf_malloc(n * n * sizeof(int));
    int *c = (int *)dma_buf_malloc(n * n * sizeof(int));
    
    for (int i = 0; i < n * n; i++) {
        a[i] = i + 1;
        b[i] = i + 1;
        c[i] = 0;
    }

    int *dram_a = (int *)malloc(n * n * sizeof(int));
    int *dram_b = (int *)malloc(n * n * sizeof(int));
    int *dram_c = (int *)malloc(n * n * sizeof(int));

    for (int i = 0; i < n * n; i++) {
        dram_a[i] = i + 1;
        dram_b[i] = i + 1;
        dram_c[i] = 0;
    }

    matmul(dram_a, dram_b, dram_c, n);

    printf("Start calculation...\n");

    blocked_gemm(a, b, c, n);

    // check the result
    for (int i = 0; i < n * n; i++) {
        if (c[i] != dram_c[i]) {
            printf("FAIL at index %d: expected %d, got %d\n", i, dram_c[i], c[i]);
            return 1;
        }
    }
    printf("PASS\n");
    return 0;
}
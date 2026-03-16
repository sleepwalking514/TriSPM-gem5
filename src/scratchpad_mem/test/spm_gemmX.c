#define USE_XSPM_INSN
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
    int block_size = 32;
    int *spm_a = (int *)spm_malloc(block_size * block_size * sizeof(int));
    int *spm_b = (int *)spm_malloc(block_size * block_size * sizeof(int));
    int *spm_c = (int *)spm_malloc(block_size * block_size * sizeof(int));

    for (int i = 0; i < n; i += block_size) {
        for (int j = 0; j < n; j += block_size) {
            spm_memset(spm_c, 0, block_size * block_size * sizeof(int));
            for (int k = 0; k < n; k += block_size) {
                for (int m = 0; m < block_size; m++) {
                    xspm_dma((uintptr_t)(spm_a + m * block_size),
                             (uintptr_t)(a + (m + i) * n + k),
                             block_size * sizeof(int));
                    xspm_dma_wait();
                    xspm_dma((uintptr_t)(spm_b + m * block_size),
                             (uintptr_t)(b + (m + k) * n + j),
                             block_size * sizeof(int));
                    xspm_dma_wait();
                }

                matmul(spm_a, spm_b, spm_c, block_size);
            }
            for (int m = 0; m < block_size; m++) {
                xspm_dma((uintptr_t)(c + (m + i) * n + j),
                         (uintptr_t)(spm_c + m * block_size),
                         block_size * sizeof(int));
                xspm_dma_wait();
            }
        }
    }
}

int main(void) {
    printf("Init...\n");

    int n = 256;
    int *a = (int *)dma_buf_malloc(n * n * sizeof(int));
    int *b = (int *)dma_buf_malloc(n * n * sizeof(int));
    int *c = (int *)dma_buf_malloc(n * n * sizeof(int));
    
    // 开始测量初始化阶段
    m5_reset_stats(0, 0);
    for (int i = 0; i < n * n; i++) {
        a[i] = i + 1;
        b[i] = i + 1;
        c[i] = 0;
    }
    // 停止统计并清零，为下一个阶段重新计数
    m5_dump_stats(0, 0);
    m5_reset_stats(0, 0);

    // blocked_gemm计算阶段
    blocked_gemm(a, b, c, n);

    // 结束统计
    m5_dump_stats(0, 0);

    return 0;
}
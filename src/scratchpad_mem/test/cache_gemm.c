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
    int *spm_a = (int *)malloc(block_size * block_size * sizeof(int));
    int *spm_b = (int *)malloc(block_size * block_size * sizeof(int));
    int *spm_c = (int *)malloc(block_size * block_size * sizeof(int));
        
    for (int i = 0; i < n; i += block_size) {
        for (int j = 0; j < n; j += block_size) {
            memset(spm_c, 0, block_size * block_size * sizeof(int));
            for (int k = 0; k < n; k += block_size) {
                
                for (int m = 0; m < block_size; m++) {
                    memcpy(spm_a + m * block_size, a + (m + i) * n + k, block_size * sizeof(int));
                    memcpy(spm_b + m * block_size, b + (m + k) * n + j, block_size * sizeof(int));
                }

                matmul(spm_a, spm_b, spm_c, block_size);
            }
            for (int m = 0; m < block_size; m++) {
                memcpy(c + (m + i) * n + j, spm_c + m * block_size, block_size * sizeof(int));
            }
        }
    }
}

int main(void) {
    printf("Init...\n");

    int n = 256;
    int *a = (int *)malloc(n * n * sizeof(int));
    int *b = (int *)malloc(n * n * sizeof(int));
    int *c = (int *)malloc(n * n * sizeof(int));
    
    for (int i = 0; i < n * n; i++) {
        a[i] = i;
        b[i] = i;
        c[i] = 0;
    }

    // 开始测量初始化阶段
    m5_reset_stats(0, 0);
    for (int i = 0; i < n * n; i++) {
        a[i] = i;
        b[i] = i;
        c[i] = 0;
    }
    // 停止统计并清零，为下一个阶段重新计数
    m5_dump_stats(0, 0);
    m5_reset_stats(0, 0);

    blocked_gemm(a, b, c, n);

    // 结束统计
    m5_dump_stats(0, 0);

    return 0;
}
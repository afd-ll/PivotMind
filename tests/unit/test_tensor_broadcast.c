#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tensor.h"
#include "common.h"

int main() {
    // Test 1: same-shape addition (regression)
    printf("=== Test 1: same-shape add ===\n");
    size_t s1[] = {3, 4};
    Tensor* a = tensor_create(DT_FLOAT32, 2, s1);
    Tensor* b = tensor_create(DT_FLOAT32, 2, s1);
    for (size_t i = 0; i < 12; i++) {
        ((float*)a->data)[i] = (float)i;
        ((float*)b->data)[i] = (float)(i * 10);
    }
    Tensor* r = tensor_add(a, b);
    if (!r) { printf("FAIL: tensor_add returned NULL\n"); return 1; }
    printf("  result[0]=%.0f (expect 0)\n", ((float*)r->data)[0]);
    printf("  result[5]=%.0f (expect 55)\n", ((float*)r->data)[5]);
    printf("  result[11]=%.0f (expect 121)\n", ((float*)r->data)[11]);
    int ok1 = ((float*)r->data)[0]==0 && ((float*)r->data)[5]==55 && ((float*)r->data)[11]==121;
    tensor_destroy(r);

    // Test 2: broadcast (3,1) + (1,4) = (3,4)
    printf("=== Test 2: broadcast add (3,1)+(1,4) ===\n");
    size_t s2a[] = {3, 1};
    size_t s2b[] = {1, 4};
    Tensor* a2 = tensor_create(DT_FLOAT32, 2, s2a);
    Tensor* b2 = tensor_create(DT_FLOAT32, 2, s2b);
    ((float*)a2->data)[0] = 1.0f;  // col 0
    ((float*)a2->data)[1] = 2.0f;  // col 1
    ((float*)a2->data)[2] = 3.0f;  // col 2
    ((float*)b2->data)[0] = 10.0f;
    ((float*)b2->data)[1] = 20.0f;
    ((float*)b2->data)[2] = 30.0f;
    ((float*)b2->data)[3] = 40.0f;
    
    Tensor* r2 = tensor_add(a2, b2);
    if (!r2) { printf("FAIL: broadcast add returned NULL\n"); return 1; }
    printf("  shape=(%zu,%zu) out_ndim=%zu\n", r2->shape[0], r2->shape[1], r2->ndim);
    /* 心算（行主序展平，out=(3,4) 的 flat 索引 = i*4+j = [i][j]）：
     *   a2 形状 (3,1) → 列向量，a2[i][0] = i+1        → 数据 [1, 2, 3]
     *   b2 形状 (1,4) → 行向量，b2[0][j] = 10*(j+1)    → 数据 [10, 20, 30, 40]
     *   广播结果 r2[i][j] = a2[i][0] + b2[0][j] = (i+1) + 10*(j+1)
     *   取四个角：
     *     [0][0] = 1 + 10 = 11   （flat 0*4+0 = 0）
     *     [0][3] = 1 + 40 = 41   （flat 0*4+3 = 3）
     *     [2][0] = 3 + 10 = 13   （flat 2*4+0 = 8）
     *     [2][3] = 3 + 40 = 43   （flat 2*4+3 = 11）
     * 修正：原打印把 flat 索引 4/7 标成 [2][0]/[2][3]（标签错位）；
     *       4/7 实为 [1][0]=12 / [1][3]=42，与注明的 [2][0]=13/[2][3]=43 对不上。
     *       期望值本身（13/43）是对的，错的是索引，故改为 8/11 让打印真报出所查位置。 */
    printf("  [0][0]=%.0f (expect 11)\n", ((float*)r2->data)[0]);
    printf("  [0][3]=%.0f (expect 41)\n", ((float*)r2->data)[3]);
    printf("  [2][0]=%.0f (expect 13)\n", ((float*)r2->data)[8]);
    printf("  [2][3]=%.0f (expect 43)\n", ((float*)r2->data)[11]);
    /* 四个角全部断言：原 ok2 只断言了 flat 0/3（[0][0]/[0][3]）两个值，
     * 漏掉 [2][0]=13 与 [2][3]=43——即“只断言一半”。补齐后覆盖 b 跨列广播
     * 与 a2 跨行广播两侧（第 2 行验证 a2[2]=3 是否被正确广播，而非只验第 0 行）。 */
    int ok2 = ((float*)r2->data)[0]==11 && ((float*)r2->data)[3]==41
           && ((float*)r2->data)[8]==13 && ((float*)r2->data)[11]==43;
    tensor_destroy(r2);

    // Test 3: sub with broadcast
    printf("=== Test 3: broadcast sub (3)-(1,) ===\n");
    size_t s3a[] = {3};
    size_t s3b[] = {1};
    Tensor* a3 = tensor_create(DT_FLOAT32, 1, s3a);
    Tensor* b3 = tensor_create(DT_FLOAT32, 1, s3b);
    ((float*)a3->data)[0] = 100;
    ((float*)a3->data)[1] = 200;
    ((float*)a3->data)[2] = 300;
    ((float*)b3->data)[0] = 50;
    
    Tensor* r3 = tensor_sub(a3, b3);
    if (!r3) { printf("FAIL: broadcast sub returned NULL\n"); return 1; }
    printf("  [0]=%.0f (expect 50)\n", ((float*)r3->data)[0]);
    printf("  [1]=%.0f (expect 150)\n", ((float*)r3->data)[1]);
    printf("  [2]=%.0f (expect 250)\n", ((float*)r3->data)[2]);
    /* 心算：a3=[100,200,300] 形状 (3)，b3=[50] 形状 (1) 广播到 (3)，
     *   r3[k] = a3[k] - 50 → [50, 150, 250]。
     * 原 ok3 只断言 [0]/[2]（首尾），漏了 [1]=150（同族“只断言一半”）。 */
    int ok3 = ((float*)r3->data)[0]==50 && ((float*)r3->data)[1]==150 && ((float*)r3->data)[2]==250;
    tensor_destroy(r3);
    
    // Test 4: incompatible shapes
    printf("=== Test 4: incompatible shapes should return NULL ===\n");
    size_t s4a[] = {3, 4};
    size_t s4b[] = {2, 5};
    Tensor* a4 = tensor_create(DT_FLOAT32, 2, s4a);
    Tensor* b4 = tensor_create(DT_FLOAT32, 2, s4b);
    Tensor* r4 = tensor_add(a4, b4);
    printf("  result=%s (expect NULL)\n", r4 ? "NOT NULL" : "NULL");
    int ok4 = (r4 == NULL);
    tensor_destroy(a4); tensor_destroy(b4);
    if (r4) tensor_destroy(r4);

    printf("\n=== SUMMARY ===\n");
    printf("Test1 (same-shape add): %s\n", ok1 ? "PASS" : "FAIL");
    printf("Test2 (broadcast add):  %s\n", ok2 ? "PASS" : "FAIL");
    printf("Test3 (broadcast sub):  %s\n", ok3 ? "PASS" : "FAIL");
    printf("Test4 (incompatible):   %s\n", ok4 ? "PASS" : "FAIL");
    printf("\n(exit code %d = 0 means all PASS)\n", (ok1 && ok2 && ok3 && ok4) ? 0 : 1);
    
    tensor_destroy(a); tensor_destroy(b);
    tensor_destroy(a2); tensor_destroy(b2);
    tensor_destroy(a3); tensor_destroy(b3);
    
    return (ok1 && ok2 && ok3 && ok4) ? 0 : 1;
}

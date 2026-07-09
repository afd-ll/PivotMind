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
    printf("  [0][0]=%.0f (expect 11)\n", ((float*)r2->data)[0]);
    printf("  [0][3]=%.0f (expect 41)\n", ((float*)r2->data)[3]);
    printf("  [2][0]=%.0f (expect 13)\n", ((float*)r2->data)[4]);
    printf("  [2][3]=%.0f (expect 43)\n", ((float*)r2->data)[7]);
    int ok2 = ((float*)r2->data)[0]==11 && ((float*)r2->data)[3]==41;
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
    int ok3 = ((float*)r3->data)[0]==50 && ((float*)r3->data)[2]==250;
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
    
    tensor_destroy(a); tensor_destroy(b);
    tensor_destroy(a2); tensor_destroy(b2);
    tensor_destroy(a3); tensor_destroy(b3);
    
    return (ok1 && ok2 && ok3 && ok4) ? 0 : 1;
}

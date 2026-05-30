#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 创建测试模型二进制文件
int main() {
    FILE* fp = fopen("tests/fixtures/test_model.bin", "wb");
    if (!fp) {
        printf("Error: Cannot create test_model.bin\n");
        return 1;
    }

    // 写入魔数
    uint32_t magic = 0x4D4F444C; // 'MODL'
    fwrite(&magic, sizeof(uint32_t), 1, fp);

    // 写入版本
    int32_t version = 1;
    fwrite(&version, sizeof(int32_t), 1, fp);

    // 写入元数据（简化版）
    char name[64] = "Test Model";
    char desc[256] = "Test model for integration testing";
    fwrite(name, sizeof(char), 64, fp);
    fwrite(desc, sizeof(char), 256, fp);

    // 写入一些虚拟参数
    int32_t num_params = 100;
    fwrite(&num_params, sizeof(int32_t), 1, fp);

    float weights[100];
    for (int i = 0; i < 100; i++) {
        weights[i] = (float)(i + 1) / 100.0f;
    }
    fwrite(weights, sizeof(float), 100, fp);

    fclose(fp);
    printf("Successfully created test_model.bin\n");
    return 0;
}

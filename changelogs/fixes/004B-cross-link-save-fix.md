# 004: 跨连接状态文件读写断裂修复

> 日期: 2026-05-25 | 关联: multi_topology.c save/load

## 问题

`master_save_state` 写节点后直接写跨连接，无分隔标记。`master_load_state` 用 `while(1)` 循环读节点，在文件末尾才 break——但实际会把跨连接字节当节点数据吃掉。

后果：跨拓扑连接数=0，联想引擎限在词汇拓扑内，跳不到语义/情绪/概念拓扑。

## 修复

### master_save_state
节点区写完后插入分隔：
```c
int node_sentinel = -1;          // 节点区结束标记
fwrite(&node_sentinel, sizeof(int), 1, fp);
unsigned int magic = 0xDEADBEEF; // 魔数验证
fwrite(&magic, sizeof(unsigned int), 1, fp);
int link_count = master->cross_link_count;
fwrite(&link_count, sizeof(int), 1, fp);
```

### master_load_state
1. 节点循环中检测 sentinel：
```c
if (fread(&topo_type, sizeof(int), 1, fp) != 1) break;
if (topo_type == -1) break;      // 节点区结束
```

2. 跨连接循环前验证魔数（兼容旧格式）：
```c
if (fread(&cross_magic, sizeof(unsigned int), 1, fp) == 1 && cross_magic == 0xDEADBEEF) {
    fread(&cross_link_count, sizeof(int), 1, fp);     // 新格式
} else {
    fseek(fp, -(long)sizeof(unsigned int), SEEK_CUR); // 旧格式回退
}
```

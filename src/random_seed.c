/**
 * @file random_seed.c
 * @brief 「显式随机种子已设置」标志的唯一实例 —— 契约见 include/common.h。
 *
 * 背景：srand() 的种子是 libc 的**进程级全局状态**，而两个置种入口
 *   （init_random_seed / init_random_from_env）都是 common.h 里的 static inline。
 * 若把「已显式设种」这个标志也放进 common.h，它会按 C 的 static 语义在每个翻译
 * 单元各存一份，跨 TU 互相看不见 ⇒ init_random() 在别的 TU 里仍会
 * srand(time^pid) 把显式种子冲掉，PIVOTMIND_SEED 就还是「装了开关没通电」。
 * 故此处给出**全进程唯一**的定义，供所有包含 common.h 的 TU 共享。
 *
 * 语义：
 *   false（初值）—— 未显式设种 ⇒ init_random() 维持旧的 time/pid 播种行为，
 *                    与 v0.5.36 逐字节一致（零行为改变）。
 *   true          —— 已显式设种 ⇒ init_random() 不得再覆盖。
 */

#include <stdbool.h>

bool pm_random_seed_explicit = false;

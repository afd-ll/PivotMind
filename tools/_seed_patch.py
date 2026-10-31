# -*- coding: utf-8 -*-
"""把 POS_CN_SEEDS 种子表整体替换为「大量常用词」版本。

背景（2026-10-30 pos_probe 基线实测）：
  - 种子命中率本来就是 5/5（不缺节点），但多字种子在词汇拓扑里几乎是孤立词：
    非常(41边) 已经(39边) 什么(56边) 因为(42边) 所以(42边) 如果(42边)
    而单字种子有 1000~3000 条边。
  - centroid = 种子特征纯算术平均 ⇒ 5 个种子里 3 个弱连接，中心被拉向「孤立词特征」
    ⇒ 分类几乎全部落空（POS 覆盖 10.1%）。
  - 因此：种子要多（摊薄单点偏差），但弱连接词必须被排除在 centroid 之外
    （见 emergent_pos.c 中新加的 POS_ANCHOR_MIN_EDGES 过滤）。

用法： python _seed_patch.py <目标c文件>
"""
import io
import re
import sys

# ---- 种子词表（单字为主；虚词类补高频多字，多字只做精确匹配、不参与中心）----
SEEDS = {
    "NOUN": """人 民 官 兵 军 农 工 商 生 师 友 家 亲 父 母 兄 弟 姐 妹 儿 女 子 孙
手 脚 头 眼 耳 鼻 口 舌 牙 心 肝 胃 血 骨 肉 皮 毛 脸 身 背 腿 臂 肩 腰 胸 肚
山 水 河 江 湖 海 天 地 土 石 沙 泥 火 风 雨 雪 云 雷 电 冰 光 影 气 烟 雾 露
树 林 木 草 花 叶 根 果 种 竹 松 柳 桃 李 梅 兰 菊 莲 荷 藤 苗 稻 麦 菜 豆
鸟 鱼 虫 马 牛 羊 猪 狗 猫 鸡 鸭 鹅 虎 狼 熊 兔 鼠 蛇 鹰 燕 蝶 蜂 蚁 象 鹿
书 笔 纸 墨 字 画 图 文 章 句 词 语 声 音 乐 歌 曲 舞 戏 剧 影 像 片 诗 报 刊
门 窗 墙 屋 房 楼 层 室 厅 厨 床 桌 椅 柜 架 灯 钟 镜 伞 包 袋 箱 盒 杯 碗 盘
路 街 巷 桥 车 船 机 站 港 场 店 厂 院 校 园 田 城 村 镇 县 省 国 洲 洋 岛 岸
钱 币 票 卡 证 章 信 网 屏 键 号 码 名 姓 期 时 分 秒 年 月 日 事 情 理 法 规 制
权 责 价 值 意 义 目 的 结 果 原 因 条 件 环 境 东 西 南 北 上 下 左 右 前 后 内 外
边 中 间 角 底 顶 头 尾 手 心 面 背 身 体 脑 眼 神 力 气 声 响 味 色 形 影 迹
球 线 绳 布 衣 裤 鞋 帽 袜 巾 被 枕 板 杆 管 盆 篮 筐 梯 锁 镜 钟 碗 碟 勺 叉
锅 壶 瓶""",
    "VERB": """吃 看 跑 想 说 走 来 去 做 给 拿 打 开 关 要 能 有 是 爱 怕
学 写 读 买 卖 睡 站 坐 听 唱 画 找 玩 笑 哭 叫 喊 问 答 讲
谈 念 背 记 忘 知 懂 认 识 感 觉 思 考 算 试 用 使 让 帮 救 教
推 拉 提 放 收 送 带 抱 抓 摸 碰 踢 跳 飞 游 爬 骑 追 逃 躲
藏 等 停 进 出 回 到 过 起 落 升 降 变 成 赚 花 存 借 还 欠
付 交 换 修 造 建 拆 洗 擦 扫 切 煮 蒸 炒 烤 冻 化 燃 熄 点
亮 灭 响 转 摇 晃 压 挤 塞 捞 拖 挑 搬 抬 扶 抱 握 甩 抖 弹
穿 脱 戴 摘 剪 缝 织 染 晒 晾 烧 倒 灌 洒 泼 兑 搅 拌 削 磨
盯 瞄 望 瞧 瞅 听 闻 尝 咬 嚼 吞 咽 吐 咳 喘 呼 吸 眨 眯 皱
梳 洗 刷 缝 剪 裁 砌 焊 拧 撬 敲 砸 摔 扔 抛 接 递""",
    "ADJ": """大 小 高 低 长 短 多 少 好 坏 美 丑 快 慢 新 旧 老 早 晚 冷
热 红 白 黑 香 甜 苦 难 假 轻 重 亮 暗 深 浅 远 近 宽 窄 厚
薄 粗 细 硬 软 干 湿 净 脏 满 空 全 真 对 错 正 反 强 弱 富
穷 贵 贱 忙 闲 累 饿 渴 困 醒 静 闹 吵 急 稳 险 安 危 乱 齐
直 弯 圆 方 平 斜 尖 钝 滑 糙 脆 韧 稀 稠 淡 浓 鲜 熟 生 死
活 呆 灵 笨 巧 妙 怪 奇 怪 好 糟 差 优 劣 佳 妙 酷 惨 棒 糟
严 松 紧 宽 谨 粗 精 拙 巧 利 钝 晃 胖 瘦 壮 矮 扁 尖 秃 茂 稀 密 鲜 艳 素 雅
俗 憨 傻 蠢 呆 痒 麻 酸 辣 咸 涩 香 臭""",
    "ADV": """很 不 都 也 还 又 就 才 只 太 最 更 常 总 再 已 正 刚 全 真
偏 越 挺 蛮 颇 略 稍 几 尽 顶 极 过 特 尤 分 甚 先 后 却 倒
竟 尚 未 曾 将 快 互 相 一 起 同 时 不 断 陆 续 逐 渐 忽 然 立 刻
马 上 曾 经 从 来 一 直 常 常 偶 尔 或 许 大 概 也 许 简 直 几 乎
特 别 非 常 已 经 一 定 必 须 当 然 确 实 实 际 基 本 大 致 约 略 稍 微
何 必 千 万 丝 毫 万 分 极 其 尤 其 一 向 从 未""",
    "PRON": """我 你 他 她 它 这 那 谁 哪 各 每 此 咱 俺 您 之 其 彼 己 某
自 何 咋 啥 怎 我 们 你 们 他 们 咱 们 大 家 自 己 人 家 别 人 什 么 怎 么 为 什 么""",
    "PREP": """在 从 到 对 用 向 往 被 把 比 跟 同 由 于 为 以 离 按 依 据
随 沿 顺 朝 经 通 过 关 于 除 了 通 过 由 于 关 于 对 于 至 于 按 照 依 照 根 据""",
    "CONJ": """和 但 与 及 或 而 且 若 虽 因 故 并 既 以 及 或 者 但 是 而 且
因 为 所 以 如 果 即 使 虽 然 然 而 因 此 只 要 不 过 除 非 无 论 不 管 要 是""",
    "NUM": """一 二 三 四 五 六 七 八 九 十 百 千 万 零 两 半 个 只 条 张
本 件 位 名 次 遍 趟 回 层 块 片 份 群 批 双 对 副 套 组 排 行
列 页 章 节 段 句 首 篇 部 台 辆 艘 架 支 枝 根 粒 颗 滴 把
堆 串 束 团 包 袋 箱 桶 盘 杯 碗 瓶 罐 斤 两 克 米 尺 寸 度
亿 兆 几 些 多 少 半 整 零 头 尾 第 初 末 俩 仨
第 一 第 二 第 三 一 点 一 些 一 下 一 共 全 部 整 个 一 半 多 数""",
    "PARTICLE": """的 了 着 过 地 得 吗 呢 吧 呀 嘛 啦 啊 哦 么 罢 了 而 已 似 的""",
    "INTERJ": """唉 哎 嘿 喂 噢 咦 哇 哈 哦 嗯 啊 呀 哼 呸 啧 呃 喔 唷 哎 哟
哎 呀 哇 塞 嘿 哟 哎 呦 咦 呀""",
}

ORDER = ["NOUN", "VERB", "ADJ", "ADV", "PRON", "PREP", "CONJ", "NUM", "PARTICLE", "INTERJ"]
TAG = {k: "POS_" + k for k in ORDER}
TAG["NOUN"] = "POS_NOUN"

MAX_PER_LINE = 8


def dedup(words):
    seen = set()
    out = []
    for w in words:
        if w and w not in seen:
            seen.add(w)
            out.append(w)
    return out


def build_block():
    lines = []
    lines.append("static const char* POS_CN_SEEDS[POS_COUNT][POS_ANCHOR_MAX_SEEDS] = {")
    lines.append("    /* POS_UNKNOWN */  {NULL},")
    lines.append("    /* v2.2 阶段0-B：种子从 59 个扩到约 1000 个常用词。动机由 pos_probe 基线实测（2026-10-30）：")
    lines.append("     * 种子命中率本来就是 5/5（不缺节点），但多字种子在词汇拓扑里几乎是孤立词——")
    lines.append("     *   非常(41边) 已经(39边) 什么(56边) 因为(42边) 所以(42边) 如果(42边)")
    lines.append("     * 而单字种子有 1000~3000 条边。centroid = 种子特征**纯算术平均**，")
    lines.append("     * 5 个种子里 3 个弱连接 ⇒ 中心被拉向「孤立词特征」⇒ 分类几乎全部落空（POS 覆盖 10.1%）。")
    lines.append("     * 本次两条并行：①种子摊薄到每类几十~上百个 ②弱连接种子不参与中心")
    lines.append("     *   （POS_ANCHOR_MIN_EDGES 过滤，只管 centroid；seed_tag 精确匹配仍然全用）。")
    lines.append("     * 规则：选该字的主职词类；跨类去重（呀/啊→助词，给→动词，用/在/把→介词，真/全→副词）。 */")
    for k in ORDER:
        ws = dedup(SEEDS[k].split())
        pad = " " * (4 - len(TAG[k]) + 4)
        head = "    /* %-11s */" % (TAG[k] + "   ")
        body = []
        for i in range(0, len(ws), MAX_PER_LINE):
            chunk = ws[i:i + MAX_PER_LINE]
            body.append(", ".join('"%s"' % w for w in chunk))
        lines.append(head.lstrip() + "  {" + (",\n" + " " * 24).join(body) + "},")
    lines.append("};")
    return "\n".join(lines), {k: len(dedup(SEEDS[k].split())) for k in ORDER}


def main():
    path = sys.argv[1]
    src = io.open(path, encoding="utf-8").read()
    block, counts = build_block()
    pat = re.compile(r"static const char\* POS_CN_SEEDS\[POS_COUNT\]\[POS_ANCHOR_MAX_SEEDS\] = \{.*?\n\};",
                     re.S)
    if not pat.search(src):
        raise SystemExit("!! 未找到 POS_CN_SEEDS 定义块")
    new = pat.sub(lambda m: block, src, count=1)
    io.open(path, "w", encoding="utf-8", newline="\n").write(new)
    total = 0
    for k in ORDER:
        if counts[k] > 128:
            print("  !! %s 有 %d 个，超过 POS_ANCHOR_MAX_SEEDS=128 会被截断" % (k, counts[k]))
        total += counts[k]
        print("  %-10s %d" % (k, counts[k]))
    print("  ---- 合计 %d" % total)


if __name__ == "__main__":
    main()

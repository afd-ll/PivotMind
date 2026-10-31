/**
 * @file emergent_pos.c
 * @brief 涌现式词类系统 — 种子锚点 + 特征向量聚类的实现
 */

#include "pivotmind_paths.h"
#include "emergent_pos.h"
#include "word_embed.h"   /* v0.6.12: 邻词分布指纹（替代随机特征的余弦） */
#include "multi_topology.h"
#include "huarong_topology.h"
#include "common.h"
#include "error.h"          /* v0.5.19: LOG_INFO（DistSig诊断用） */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>   /* getpid / fsync */
#include <fcntl.h>    /* open / O_RDONLY / O_DIRECTORY */
#include <errno.h>    /* strerror */

/* 前向声明 (classify 函数中调用, 定义在文件后面) */
int emergent_pos_try_emerge(EmergentPOS* ep);

/* ── v0.6.12 指纹分类的门槛与开关 ──
 * 实测（tools/wemb_probe.c）：同类 0.15–0.45、跨类 0.004–0.26。
 * 取 0.10 作入门槛 —— 宁可多分类（top-3 软输出吸收误判），也不退回随机特征。 */
#define POS_EMBED_MIN_SIM 0.10f

/* 指纹尺度的簇紧密度门槛。
 * 原 EMERGE_COHERENCE_THRESH=0.65 是为 512 维特征设计的，指纹的余弦小一个量级。
 * 实测（tools/emerge_probe.c，动词组 6 + 副词组 6）：
 *   组内均值 **0.126** · 组间均值 **0.039**（3.2x 区分度）· 全局最大 0.462
 * ⇒ 取 0.06：0.126 是 30 对的【均值】，单 pair 大量低于它，取 0.10 会让簇永远
 *   长不到 EMERGE_MIN_CLUSTER_SIZE；0.06 仍高于组间均值 0.039。 */
#define EMERGE_FP_COHERENCE 0.06f
/* 与【簇首】的最低相似度：比平均门槛宽松（组内也有低于 0.10 的 pair），
 * 但必须高于组间均值 0.039 —— 实测 0.039 那档正是副词混进动词簇的位置。 */
#define EMERGE_FP_SEED_SIM  0.05f

/* 涌现出的额外类是否参与分类（默认**不参与**）。
 * 原因：extra_classes 的 centroid 是 512 维 features 的均值，而 features 本身是
 * 重训时的 rand() 噪声 ⇒ 拿它做余弦等于随机分类，会把已有分类结果搅坏。
 * 现阶段额外类只用于「观察聚类是否真的在长」（日志 + /debug 计数）。
 * 等新类 centroid 也建成指纹原型（结构改动）后再打开。 */
static int extra_class_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("PIVOTMIND_POS_EXTRA_CLASS");
        cached = (e && e[0] == '1') ? 1 : 0;
    }
    return cached;
}

static int pos_embed_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("PIVOTMIND_POS_EMBED");
        cached = (e && e[0] == '0') ? 0 : 1;
    }
    return cached;
}

/* ================================================================
 *  内部辅助: POS 标签名
 * ================================================================ */

static const char* pos_label_cn(POSTag tag) {
    static const char* names[] = {
        "未知", "名词", "动词", "形容词", "副词",
        "代词", "介词", "连词", "数词", "助词", "叹词"
    };
    if (tag >= 0 && tag < POS_COUNT) return names[tag];
    return "?";
}

static const char* pos_label_en(POSTag tag) {
    static const char* names[] = {
        "?", "NOUN", "VERB", "ADJ", "ADV",
        "PRON", "PREP", "CONJ", "NUM", "PART", "INTJ"
    };
    if (tag >= 0 && tag < POS_COUNT) return names[tag];
    return "?";
}

/* ================================================================
 *  种子词表 — 每个词类 5 个最典型词（仅本文件使用）
 * ================================================================ */

static const char* POS_CN_SEEDS[POS_COUNT][POS_ANCHOR_MAX_SEEDS] = {
    /* POS_UNKNOWN */  {NULL},
    /* v2.2 阶段0-B：种子从 59 个扩到约 1000 个常用词。动机由 pos_probe 基线实测（2026-10-30）：
     * 种子命中率本来就是 5/5（不缺节点），但多字种子在词汇拓扑里几乎是孤立词——
     *   非常(41边) 已经(39边) 什么(56边) 因为(42边) 所以(42边) 如果(42边)
     * 而单字种子有 1000~3000 条边。centroid = 种子特征**纯算术平均**，
     * 5 个种子里 3 个弱连接 ⇒ 中心被拉向「孤立词特征」⇒ 分类几乎全部落空（POS 覆盖 10.1%）。
     * 本次两条并行：①种子摊薄到每类几十~上百个 ②弱连接种子不参与中心
     *   （POS_ANCHOR_MIN_EDGES 过滤，只管 centroid；seed_tag 精确匹配仍然全用）。
     * 规则：选该字的主职词类；跨类去重（呀/啊→助词，给→动词，用/在/把→介词，真/全→副词）。 */
/* POS_NOUN    */  {"人", "民", "官", "兵", "军", "农", "工", "商",
                        "生", "师", "友", "家", "亲", "父", "母", "兄",
                        "弟", "姐", "妹", "儿", "女", "子", "孙", "手",
                        "脚", "头", "眼", "耳", "鼻", "口", "舌", "牙",
                        "心", "肝", "胃", "血", "骨", "肉", "皮", "毛",
                        "脸", "身", "背", "腿", "臂", "肩", "腰", "胸",
                        "肚", "山", "水", "河", "江", "湖", "海", "天",
                        "地", "土", "石", "沙", "泥", "火", "风", "雨",
                        "雪", "云", "雷", "电", "冰", "光", "影", "气",
                        "烟", "雾", "露", "树", "林", "木", "草", "花",
                        "叶", "根", "果", "种", "竹", "松", "柳", "桃",
                        "李", "梅", "兰", "菊", "莲", "荷", "藤", "苗",
                        "稻", "麦", "菜", "豆", "鸟", "鱼", "虫", "马",
                        "牛", "羊", "猪", "狗", "猫", "鸡", "鸭", "鹅",
                        "虎", "狼", "熊", "兔", "鼠", "蛇", "鹰", "燕",
                        "蝶", "蜂", "蚁", "象", "鹿", "书", "笔", "纸",
                        "墨", "字", "画", "图", "文", "章", "句", "词",
                        "语", "声", "音", "乐", "歌", "曲", "舞", "戏",
                        "剧", "像", "片", "诗", "报", "刊", "门", "窗",
                        "墙", "屋", "房", "楼", "层", "室", "厅", "厨",
                        "床", "桌", "椅", "柜", "架", "灯", "钟", "镜",
                        "伞", "包", "袋", "箱", "盒", "杯", "碗", "盘",
                        "路", "街", "巷", "桥", "车", "船", "机", "站",
                        "港", "场", "店", "厂", "院", "校", "园", "田",
                        "城", "村", "镇", "县", "省", "国", "洲", "洋",
                        "岛", "岸", "钱", "币", "票", "卡", "证", "信",
                        "网", "屏", "键", "号", "码", "名", "姓", "期",
                        "时", "分", "秒", "年", "月", "日", "事", "情",
                        "理", "法", "规", "制", "权", "责", "价", "值",
                        "意", "义", "目", "的", "结", "原", "因", "条",
                        "件", "环", "境", "东", "西", "南", "北", "上",
                        "下", "左", "右", "前", "后", "内", "外", "边",
                        "中", "间", "角", "底", "顶", "尾", "面", "体",
                        "脑", "神", "力", "响", "味", "色", "形", "迹",
                        "球", "线", "绳", "布", "衣", "裤", "鞋", "帽",
                        "袜", "巾", "被", "枕", "板", "杆", "管", "盆",
                        "篮", "筐", "梯", "锁", "碟", "勺", "叉", "锅",
                        "壶", "瓶"},
/* POS_VERB    */  {"吃", "看", "跑", "想", "说", "走", "来", "去",
                        "做", "给", "拿", "打", "开", "关", "要", "能",
                        "有", "是", "爱", "怕", "学", "写", "读", "买",
                        "卖", "睡", "站", "坐", "听", "唱", "画", "找",
                        "玩", "笑", "哭", "叫", "喊", "问", "答", "讲",
                        "谈", "念", "背", "记", "忘", "知", "懂", "认",
                        "识", "感", "觉", "思", "考", "算", "试", "用",
                        "使", "让", "帮", "救", "教", "推", "拉", "提",
                        "放", "收", "送", "带", "抱", "抓", "摸", "碰",
                        "踢", "跳", "飞", "游", "爬", "骑", "追", "逃",
                        "躲", "藏", "等", "停", "进", "出", "回", "到",
                        "过", "起", "落", "升", "降", "变", "成", "赚",
                        "花", "存", "借", "还", "欠", "付", "交", "换",
                        "修", "造", "建", "拆", "洗", "擦", "扫", "切",
                        "煮", "蒸", "炒", "烤", "冻", "化", "燃", "熄",
                        "点", "亮", "灭", "响", "转", "摇", "晃", "压",
                        "挤", "塞", "捞", "拖", "挑", "搬", "抬", "扶",
                        "握", "甩", "抖", "弹", "穿", "脱", "戴", "摘",
                        "剪", "缝", "织", "染", "晒", "晾", "烧", "倒",
                        "灌", "洒", "泼", "兑", "搅", "拌", "削", "磨",
                        "盯", "瞄", "望", "瞧", "瞅", "闻", "尝", "咬",
                        "嚼", "吞", "咽", "吐", "咳", "喘", "呼", "吸",
                        "眨", "眯", "皱", "梳", "刷", "裁", "砌", "焊",
                        "拧", "撬", "敲", "砸", "摔", "扔", "抛", "接",
                        "递"},
/* POS_ADJ     */  {"大", "小", "高", "低", "长", "短", "多", "少",
                        "好", "坏", "美", "丑", "快", "慢", "新", "旧",
                        "老", "早", "晚", "冷", "热", "红", "白", "黑",
                        "香", "甜", "苦", "难", "假", "轻", "重", "亮",
                        "暗", "深", "浅", "远", "近", "宽", "窄", "厚",
                        "薄", "粗", "细", "硬", "软", "干", "湿", "净",
                        "脏", "满", "空", "全", "真", "对", "错", "正",
                        "反", "强", "弱", "富", "穷", "贵", "贱", "忙",
                        "闲", "累", "饿", "渴", "困", "醒", "静", "闹",
                        "吵", "急", "稳", "险", "安", "危", "乱", "齐",
                        "直", "弯", "圆", "方", "平", "斜", "尖", "钝",
                        "滑", "糙", "脆", "韧", "稀", "稠", "淡", "浓",
                        "鲜", "熟", "生", "死", "活", "呆", "灵", "笨",
                        "巧", "妙", "怪", "奇", "糟", "差", "优", "劣",
                        "佳", "酷", "惨", "棒", "严", "松", "紧", "谨",
                        "精", "拙", "利", "晃", "胖", "瘦", "壮", "矮",
                        "扁", "秃", "茂", "密", "艳", "素", "雅", "俗",
                        "憨", "傻", "蠢", "痒", "麻", "酸", "辣", "咸",
                        "涩", "臭"},
/* POS_ADV     */  {"很", "不", "都", "也", "还", "又", "就", "才",
                        "只", "太", "最", "更", "常", "总", "再", "已",
                        "正", "刚", "全", "真", "偏", "越", "挺", "蛮",
                        "颇", "略", "稍", "几", "尽", "顶", "极", "过",
                        "特", "尤", "分", "甚", "先", "后", "却", "倒",
                        "竟", "尚", "未", "曾", "将", "快", "互", "相",
                        "一", "起", "同", "时", "断", "陆", "续", "逐",
                        "渐", "忽", "然", "立", "刻", "马", "上", "经",
                        "从", "来", "直", "偶", "尔", "或", "许", "大",
                        "概", "简", "乎", "别", "非", "定", "必", "须",
                        "当", "确", "实", "际", "基", "本", "致", "约",
                        "微", "何", "千", "万", "丝", "毫", "其", "向"},
/* POS_PRON    */  {"我", "你", "他", "她", "它", "这", "那", "谁",
                        "哪", "各", "每", "此", "咱", "俺", "您", "之",
                        "其", "彼", "己", "某", "自", "何", "咋", "啥",
                        "怎", "们", "大", "家", "人", "别", "什", "么",
                        "为"},
/* POS_PREP    */  {"在", "从", "到", "对", "用", "向", "往", "被",
                        "把", "比", "跟", "同", "由", "于", "为", "以",
                        "离", "按", "依", "据", "随", "沿", "顺", "朝",
                        "经", "通", "过", "关", "除", "了", "至", "照",
                        "根"},
/* POS_CONJ    */  {"和", "但", "与", "及", "或", "而", "且", "若",
                        "虽", "因", "故", "并", "既", "以", "者", "是",
                        "为", "所", "如", "果", "即", "使", "然", "此",
                        "只", "要", "不", "过", "除", "非", "无", "论",
                        "管"},
/* POS_NUM     */  {"一", "二", "三", "四", "五", "六", "七", "八",
                        "九", "十", "百", "千", "万", "零", "两", "半",
                        "个", "只", "条", "张", "本", "件", "位", "名",
                        "次", "遍", "趟", "回", "层", "块", "片", "份",
                        "群", "批", "双", "对", "副", "套", "组", "排",
                        "行", "列", "页", "章", "节", "段", "句", "首",
                        "篇", "部", "台", "辆", "艘", "架", "支", "枝",
                        "根", "粒", "颗", "滴", "把", "堆", "串", "束",
                        "团", "包", "袋", "箱", "桶", "盘", "杯", "碗",
                        "瓶", "罐", "斤", "克", "米", "尺", "寸", "度",
                        "亿", "兆", "几", "些", "多", "少", "整", "头",
                        "尾", "第", "初", "末", "俩", "仨", "点", "下",
                        "共", "全", "数"},
/* POS_PARTICLE    */  {"的", "了", "着", "过", "地", "得", "吗", "呢",
                        "吧", "呀", "嘛", "啦", "啊", "哦", "么", "罢",
                        "而", "已", "似"},
/* POS_INTERJ    */  {"唉", "哎", "嘿", "喂", "噢", "咦", "哇", "哈",
                        "哦", "嗯", "啊", "呀", "哼", "呸", "啧", "呃",
                        "喔", "唷", "哟", "塞", "呦"},
};

static const char* POS_EN_SEEDS[POS_COUNT][POS_ANCHOR_MAX_SEEDS] = {
    /* POS_UNKNOWN */  {NULL, NULL, NULL, NULL, NULL},
    /* POS_NOUN    */  {"apple","time", "table","idea", "person"},
    /* POS_VERB    */  {"eat",  "run",  "think","say",  "see" },
    /* POS_ADJ     */  {"big",  "good", "fast", "new",  "beautiful"},
    /* POS_ADV     */  {"very", "not",  "all",  "often","already"},
    /* POS_PRON    */  {"I",    "you",  "he",   "this", "what"},
    /* POS_PREP    */  {"in",   "from", "to",   "for",  "with"},
    /* POS_CONJ    */  {"and",  "but",  "because","so", "if"  },
    /* POS_NUM     */  {"one",  "ten",  "hundred","first","time"},
    /* POS_PARTICLE*/  {"the",  "a",    "an",   "of",   "to"  },
    /* POS_INTERJ  */  {"oh",   "ah",   "wow",  "hey",  "oops"},
};

/* ================================================================
 *  创建/销毁
 * ================================================================ */

EmergentPOS* emergent_pos_create(const char* lang) {
    EmergentPOS* ep = (EmergentPOS*)calloc(1, sizeof(EmergentPOS));
    if (!ep) return NULL;

    ep->sim_threshold = POS_ANCHOR_DEFAULT_THRESHOLD;
    ep->learn_rate    = POS_ANCHOR_LEARN_RATE;
    ep->classify_count = 0;
    ep->anchor_count   = 0;

    /* 选择语言种子表（v0.6 步 3：同时把语种记进实例，供持久化路径闸门用） */
    const char* (*seed_table)[POS_ANCHOR_MAX_SEEDS];
    int is_en = (lang && lang[0] == 'e');
    seed_table = is_en ? POS_EN_SEEDS : POS_CN_SEEDS;
    ep->lang = is_en ? PM_LANG_EN : PM_LANG_ZH;

    pthread_mutex_init(&ep->lock, NULL);

    /* 遍历 POS 标签（跳过 POS_UNKNOWN=0） */
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        anchor->tag      = (POSTag)tag;
        anchor->label_cn = pos_label_cn((POSTag)tag);
        anchor->label_en = pos_label_en((POSTag)tag);
        anchor->seed_count = 0;

        for (int s = 0; s < POS_ANCHOR_MAX_SEEDS; s++) {
            if (seed_table[tag][s] && seed_table[tag][s][0]) {
                anchor->seeds[anchor->seed_count++] = seed_table[tag][s];
            }
        }

        /* 初始化中心向量为 0 */
        memset(anchor->centroid, 0, sizeof(anchor->centroid));
        anchor->member_count = 0;
        anchor->centroid_stability = 0.0f;
        anchor->is_active = (anchor->seed_count > 0) ? 0 : 0; /* 0=等待中心初始化 */
    }

    ep->anchor_count = POS_COUNT - 1; /* 除去 POS_UNKNOWN */

    fprintf(stderr, "[EmergentPOS] 创建成功: %d 锚点, 语言=%s, 阈值=%.2f\n",
            ep->anchor_count, is_en ? "en" : "zh", ep->sim_threshold);
    return ep;
}

void emergent_pos_destroy(EmergentPOS* ep) {
    if (!ep) return;
    /* 退出前持久化 */
    if (ep->total_classifications > 0) {
        emergent_pos_save(ep, NULL);
    }
    pthread_mutex_destroy(&ep->lock);
    free(ep);
}

/* ================================================================
 *  锚点中心初始化
 * ================================================================ */

int emergent_pos_init_centroids(EmergentPOS* ep, MasterTopology* master) {
    if (!ep || !master) return 0;

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) {
        fprintf(stderr, "[EmergentPOS] 词汇拓扑未就绪，推迟中心初始化\n");
        return 0;
    }

    HuarongTopologyNet* vnet = vocab->net;
    int nc = vnet->node_count;
    if (nc < 10) {
        fprintf(stderr, "[EmergentPOS] 词汇节点不足 (%d)，推迟中心初始化\n", nc);
        return 0;
    }

    int initialized = 0;

    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        if (anchor->is_active) continue; /* 已初始化 */
        if (anchor->seed_count == 0) continue;

        /* 扫描词汇拓扑，找种子词的节点 */
        int found = 0;
        int weak_skipped = 0;   /* v2.2：因边数不足被排除出中心的种子数（可观测） */
        float sum_feat[PM_NODE_FEATURE_DIM];
        memset(sum_feat, 0, sizeof(sum_feat));

        for (int i = 0; i < nc && found < anchor->seed_count; i++) {
            ReasoningNode* node = vnet->nodes[i];
            if (!node || !node->concept || !node->features) continue;

            /* 检查是否是种子词 */
            for (int s = 0; s < anchor->seed_count; s++) {
                if (node->concept && anchor->seeds[s] && strcmp_null(node->concept, anchor->seeds[s]) == 0) {
                    /* v2.2 阶段0-B：弱连接种子不参与中心（seed_tag 精确匹配仍用全部种子）。 */
                    if (node->edge_count >= POS_ANCHOR_MIN_EDGES) {
                        for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
                            sum_feat[d] += node->features[d];
                        }
                        found++;
                    } else {
                        weak_skipped++;
                    }
                    break;
                }
            }
        }

        if (found >= 2) {
            /* 取均值作为锚点中心 */
            float inv_n = 1.0f / (float)found;
            for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
                anchor->centroid[d] = sum_feat[d] * inv_n;
            }
            anchor->member_count = found;
            anchor->is_active = 1;
            anchor->centroid_stability = 0.3f; /* 初始稳定性低 */
            initialized++;
            fprintf(stderr, "[EmergentPOS] 锚点[%s] 中心初始化: %d 个种子词（种子共 %d，弱连接跳过 %d）\n",
                    anchor->label_cn, found, anchor->seed_count, weak_skipped);
        } else if (found == 1) {
            /* 只有一个种子词找到，直接用其特征向量 */
            for (int i = 0; i < nc; i++) {
                ReasoningNode* node = vnet->nodes[i];
                if (!node || !node->concept || !node->features) continue;
                for (int s = 0; s < anchor->seed_count; s++) {
                    if (node->concept && anchor->seeds[s] && strcmp_null(node->concept, anchor->seeds[s]) == 0) {
                        memcpy(anchor->centroid, node->features,
                               PM_NODE_FEATURE_DIM * sizeof(float));
                        anchor->member_count = 1;
                        anchor->is_active = 1;
                        anchor->centroid_stability = 0.15f;
                        initialized++;
                        break;
                    }
                }
                if (anchor->is_active) break;
            }
        } else {
            /* 种子词都不在词汇拓扑中，标记为待初始化 */
            fprintf(stderr, "[EmergentPOS] 锚点[%s] 无种子词节点，待后续初始化\n",
                    anchor->label_cn);
        }
    }

    if (initialized > 0) {
        fprintf(stderr, "[EmergentPOS] 锚点中心初始化完成: %d/%d 激活\n",
                initialized, ep->anchor_count);
    }
    return initialized;
}

/* ================================================================
 *  硬分类
 * ================================================================ */

POSTag emergent_pos_classify(EmergentPOS* ep, const float* features) {
    if (!ep || !features) return POS_UNKNOWN;

    /* v0.5.10: 锁保护——本函数写 unclassified 池/extra_classes/计数器，
     * tag_soft 锁外调用（article_flush 移出 ar->mutex），多 learn worker
     * 并发写会堆损坏（08-08 15:28 double free 实锤） */
    pthread_mutex_lock(&ep->lock);

    float best_sim = ep->sim_threshold;
    POSTag best_tag = POS_UNKNOWN;
    int best_is_extra = 0;

    /* 检查 10 个硬编码锚点 */
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        if (!anchor->is_active) continue;

        float sim = cosine_similarity(features, anchor->centroid, PM_NODE_FEATURE_DIM);
        if (sim > best_sim) {
            best_sim = sim;
            best_tag = (POSTag)tag;
            best_is_extra = 0;
        }
    }

    /* 检查涌现出的额外词类 */
    for (int ei = 0; ei < ep->extra_class_count; ei++) {
        if (!ep->extra_classes[ei].is_active) continue;
        float sim = cosine_similarity(features, ep->extra_classes[ei].centroid, PM_NODE_FEATURE_DIM);
        if (sim > best_sim) {
            best_sim = sim;
            best_tag = (POSTag)(ep->extra_classes[ei].class_id);
            best_is_extra = 1;
        }
    }

    /* 分类成功 → 微调中心 */
    if (best_tag != POS_UNKNOWN) {
        if (best_is_extra && (int)best_tag >= POS_COUNT) {
            /* 微调额外词类中心 */
            int ei = (int)best_tag - POS_COUNT;
            if (ei < 16) {
                float lr = ep->learn_rate;
                for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
                    ep->extra_classes[ei].centroid[d] += lr * (features[d] - ep->extra_classes[ei].centroid[d]);
                }
                ep->extra_classes[ei].member_count++;
            }
        } else {
            emergent_pos_adjust_centroid(ep, best_tag, features);
        }
    } else {
        /* 未分类 → 加入涌现池 */
        if (ep->unclassified_count < 64) {
            memcpy(ep->unclassified_feats[ep->unclassified_count], features,
                   PM_NODE_FEATURE_DIM * sizeof(float));
            ep->unclassified_pool_nodes[ep->unclassified_count] = -1;
            ep->unclassified_count++;
        }
    }

    ep->total_classifications++;
    ep->emerge_check_counter++;

    /* 定期触发涌现检查 */
    if (ep->emerge_check_counter >= EMERGE_CHECK_INTERVAL &&
        ep->unclassified_count >= EMERGE_POOL_TRIGGER) {
        ep->emerge_check_counter = 0;
        emergent_pos_try_emerge(ep);
    }

    /* 每 5000 次分类自动持久化 */
    if (ep->total_classifications % 5000 == 0) {
        emergent_pos_save(ep, NULL);
    }

    pthread_mutex_unlock(&ep->lock);
    return best_tag;
}

/* ================================================================
 *  软分类
 * ================================================================ */

void emergent_pos_classify_soft(EmergentPOS* ep, const float* features,
                                SoftClassResult* result) {
    if (!ep || !features || !result) {
        if (result) { result->count = 0; }
        return;
    }
    result->count = 0;

    /* v0.5.10: 锁保护——本函数写 extra_classes/涌现池/计数器，
     * tag_soft 锁外调用，多 learn worker 并发写会堆损坏
     * （08-08 15:28 double free 实锤）。注意：emergent_pos_tag_soft
     * 调本函数后不再回写节点（v0.5.10 已删），锁只护 ep 内部状态。 */
    pthread_mutex_lock(&ep->lock);

    /* 收集所有超过阈值的锚点 */
    typedef struct { POSTag tag; float sim; } Candidate;
    #define SOFT_CAND_MAX (POS_COUNT + 16)  /* 10 + 最多16个额外词类 */
    Candidate cands[SOFT_CAND_MAX];
    int nc = 0;

    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        if (!anchor->is_active) continue;

        float sim = cosine_similarity(features, anchor->centroid, PM_NODE_FEATURE_DIM);
        if (sim > ep->sim_threshold && nc < SOFT_CAND_MAX) {
            cands[nc].tag = (POSTag)tag;
            cands[nc].sim = sim;
            nc++;
        }
    }

    /* 检查涌现出的额外词类（默认不参与，见 extra_class_enabled 的说明） */
    for (int ei = 0; extra_class_enabled() && ei < ep->extra_class_count && nc < SOFT_CAND_MAX; ei++) {
        if (!ep->extra_classes[ei].is_active) continue;
        float sim = cosine_similarity(features, ep->extra_classes[ei].centroid, PM_NODE_FEATURE_DIM);
        if (sim > ep->sim_threshold) {
            cands[nc].tag = (POSTag)(ep->extra_classes[ei].class_id);
            cands[nc].sim = sim;
            nc++;
        }
    }

    /* 按相似度降序排序 */
    for (int i = 1; i < nc; i++) {
        Candidate tmp = cands[i];
        int j = i - 1;
        while (j >= 0 && cands[j].sim < tmp.sim) {
            cands[j + 1] = cands[j];
            j--;
        }
        cands[j + 1] = tmp;
    }

    /* 输出前 SOFT_CLASS_MAX 个 */
    for (int i = 0; i < nc && i < SOFT_CLASS_MAX; i++) {
        result->tags[i] = cands[i].tag;
        result->confs[i] = cands[i].sim;
        result->count++;
    }

    /* 对最佳候选微调中心 */
    if (result->count > 0) {
        int best_tag = (int)result->tags[0];
        if (best_tag >= POS_COUNT) {
            int ei = best_tag - POS_COUNT;
            if (ei < 16) {
                float lr = ep->learn_rate;
                for (int d = 0; d < PM_NODE_FEATURE_DIM; d++)
                    ep->extra_classes[ei].centroid[d] += lr * (features[d] - ep->extra_classes[ei].centroid[d]);
                ep->extra_classes[ei].member_count++;
            }
        } else {
            emergent_pos_adjust_centroid(ep, result->tags[0], features);
        }
    } else {
        /* 未分类 → 加入涌现池 */
        if (ep->unclassified_count < 64) {
            memcpy(ep->unclassified_feats[ep->unclassified_count], features,
                   PM_NODE_FEATURE_DIM * sizeof(float));
            ep->unclassified_pool_nodes[ep->unclassified_count] = -1;
            ep->unclassified_count++;
        }
    }

    ep->total_classifications++;
    ep->emerge_check_counter++;
    if (result->count > 1) ep->soft_classifications++;

    /* 定期触发涌现检查 */
    if (ep->emerge_check_counter >= EMERGE_CHECK_INTERVAL &&
        ep->unclassified_count >= EMERGE_POOL_TRIGGER) {
        ep->emerge_check_counter = 0;
        emergent_pos_try_emerge(ep);
    }

    pthread_mutex_unlock(&ep->lock);
}

/* ================================================================
 *  按词名标注 — 双层路由
 * ================================================================ */

POSTag emergent_pos_seed_tag(EmergentPOS* ep, const char* word) {
    if (!ep || !word || !word[0]) return POS_UNKNOWN;
    /* 第一层: 种子词检查 — O(n) 线性扫描（人标先验，可信标签唯一来源） */
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        for (int s = 0; s < anchor->seed_count; s++) {
            if (anchor->seeds[s] && strcmp(word, anchor->seeds[s]) == 0) {
                return (POSTag)tag;
            }
        }
    }
    return POS_UNKNOWN;
}

POSTag emergent_pos_tag(EmergentPOS* ep, MasterTopology* master,
                        const char* word) {
    if (!ep || !master || !word || !word[0]) return POS_UNKNOWN;

    /* 懒初始化: 若锚点中心未初始化且词汇拓扑就绪，自动初始化 */
    if (ep->anchor_count > 0 && !ep->anchors[POS_NOUN].is_active) {
        SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
        if (vocab && vocab->net && vocab->net->node_count >= 100) {
            emergent_pos_init_centroids(ep, master);
        }
    }

    /* 第一层: 种子词检查（v2.1 阶段0-A：抽出 emergent_pos_seed_tag，DRY） */
    POSTag seed_tag = emergent_pos_seed_tag(ep, word);
    if (seed_tag != POS_UNKNOWN) return seed_tag;

    /* 第二层: 特征向量相似度匹配 */
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return POS_UNKNOWN;

    int node_id = huarong_net_find_concept(vocab->net, word);
    if (node_id < 0) return POS_UNKNOWN;

    /* v0.5.10 fix: node_id 空洞防御——词巩固/压缩线程可能并发 shrink
     * nodes 数组（node_id 空洞→越界 SIGSEGV，08-08 同族崩溃）。
     * 锁外读的唯一安全边界就是 node_count 本身。 */
    if (node_id >= vocab->net->node_count) return POS_UNKNOWN;

    ReasoningNode* node = vocab->net->nodes[node_id];
    if (!node || !node->features) return POS_UNKNOWN;

    POSTag tag = emergent_pos_classify(ep, node->features);

    /* v0.5.10 fix: 移除回写词类到节点（emergent_class_ids/emergent_class_confs）。
     *
     * 原代码在锁外写共享节点字段 → 对话线程与词巩固线程并发时
     * 悬垂指针 SIGSEGV（08-08 三次 139 崩溃实锤，栈在 diffusion_generate
     * → emergent_pos_tag）。POS 标注是纯查询，回写是缓存优化——但:
     *   1) 本函数调用者（diffusion/multi_topology/cognitive_controller）
     *      全部只用返回值，无人依赖回写结果
     *   2) 回写由喂料路径 emergent_pos_tag_soft（article_flush 持锁调用）
     *      负责，数据持续供给模板生长与 diffusion 的 emergent_class_ids 读取
     *   3) 查询路径零写 = 零悬垂风险，锁外安全
     */
    return tag;
}

void emergent_pos_tag_soft(EmergentPOS* ep, MasterTopology* master,
                           const char* word, SoftClassResult* result) {
    if (result) result->count = 0;
    if (!ep || !master || !word || !word[0] || !result) return;

    /* 懒初始化 */
    if (ep->anchor_count > 0 && !ep->anchors[POS_NOUN].is_active) {
        SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
        if (vocab && vocab->net && vocab->net->node_count >= 100) {
            emergent_pos_init_centroids(ep, master);
        }
    }

    /* 第一层: 种子词检查 */
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        for (int s = 0; s < anchor->seed_count; s++) {
            if (anchor->seeds[s] && strcmp(word, anchor->seeds[s]) == 0) {
                result->tags[0] = (POSTag)tag;
                result->confs[0] = 1.0f;
                result->count = 1;
                return;
            }
        }
    }

    /* ── 第一点五层：邻词分布指纹匹配（v0.6.12）──
     * 为什么插在这里：下面的第二层用 512 维 features 的余弦，而 features 在重训时是
     * `rand()*0.1-0.05` 的**随机噪声** ⇒ 那一层实际等于随机分类。
     * 指纹表示的是「这个词旁边出现哪些高频字」的真实分布（charpairs 1.1 亿次观测），
     * 实测类内/类间相似度比 4.15x（tools/wemb_probe.c：很~太 0.455，吃~的 0.020）。
     * 指纹缺失（多字词 / 表外字 / 未构建）⇒ 原样落到第二层，行为不变。
     * 开关：PIVOTMIND_POS_EMBED=0 关闭。 */
    if (pos_embed_enabled()) {
        int dim = wemb_dim();
        if (dim > 0 && dim <= 1024) {
            float proto[1024];
            float  best_sim[3] = {-2.0f, -2.0f, -2.0f};
            POSTag best_tag[3] = {POS_UNKNOWN, POS_UNKNOWN, POS_UNKNOWN};
            const float* vw = wemb_vector(word);
            int vnnz = vw ? wemb_nnz(word) : 0;
            if (vw && vnnz >= 3) {
            for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
                POSAnchor* anchor = &ep->anchors[tag];
                if (!anchor->is_active || anchor->seed_count <= 0) continue;
                /* 用「该类的种子原型（均值）」比，而不是逐个种子取 max：
                 * 逐 max 会被单个异常种子带偏，均值是类的中心（原型理论的做法）。 */
                if (wemb_centroid(anchor->seeds, anchor->seed_count, proto) <= 0) continue;
                float sim = wemb_cosine(vw, proto);
                if (sim < POS_EMBED_MIN_SIM) continue;
                for (int k = 0; k < 3; k++) {           /* 插入前 3 名 */
                    if (sim > best_sim[k]) {
                        for (int j = 2; j > k; j--) {
                            best_sim[j] = best_sim[j - 1];
                            best_tag[j] = best_tag[j - 1];
                        }
                        best_sim[k] = sim;
                        best_tag[k] = (POSTag)tag;
                        break;
                    }
                }
            }
            }
            if (best_tag[0] != POS_UNKNOWN) {
                int c = 0;
                for (int k = 0; k < 3 && best_tag[k] != POS_UNKNOWN; k++) {
                    result->tags[c]  = best_tag[k];
                    result->confs[c] = (best_sim[k] > 1.0f) ? 1.0f : best_sim[k];
                    c++;
                }
                result->count = c;
                return;
            }
        }
    }

    /* 第二层: 特征向量软匹配 */
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return;

    int node_id = huarong_net_find_concept(vocab->net, word);
    if (node_id < 0) return;

    /* v0.5.10 fix: node_id 空洞防御——词巩固/扩容线程并发 shrink/realloc
     * nodes 数组时锁外读会悬垂（08-08 15:28 double free + SIGSEGV 实锤，
     * 栈: auto_learn_concepts → huarong_net_add_connection）。 */
    if (node_id >= vocab->net->node_count) return;

    /* BG-29-v3：持 net 读锁取指针 → 把 features 拷到栈上 → 解锁 → 再用。
     * 原代码一把锁都不持：扩容方（huarong_topology.c:252-282 /
     * topology_growth.c:483-520）在 net 写锁下 realloc(net->nodes)，
     * 冻结方（node_cache.c:262-289）在 node_locks 下 free(node->features)，
     * 两者都会让本处拿到的指针变悬垂 ⇒ 堆损坏。
     * 锁序：net->mutex(Level2) → node_locks[](Level3)，本处只取 Level2，不会形成环。 */
    float _feat_copy[PM_NODE_FEATURE_DIM];
    pthread_rwlock_rdlock(&vocab->net->mutex);
    ReasoningNode* node = vocab->net->nodes[node_id];
    if (!node || !node->features || node->feature_dim < PM_NODE_FEATURE_DIM) {
        pthread_rwlock_unlock(&vocab->net->mutex);
        return;
    }
    memcpy(_feat_copy, node->features, sizeof(_feat_copy));
    pthread_rwlock_unlock(&vocab->net->mutex);

    emergent_pos_classify_soft(ep, _feat_copy, result);

    /* ── v0.6.12（CX-34）：未分类 ⇒ 入池，等 try_emerge 聚成新词类 ──
     * 原来 `unclassified_pool_nodes` 全仓除本文件外零引用 ⇒ 池永远是空的
     * ⇒ `try_emerge` 永不触发 ⇒ `extra_class_count` 恒 0，聚类系统只活着不生长。
     * 池里同时存【词名】：feats 是 512 维随机噪声，在上面聚类等于随机分组，
     * 而词名能取 word_embed 的分布指纹作判据。
     * 只收单字（3 字节）—— charpairs 指纹是字符级的，多字词没有指纹。 */
    if (result->count == 0 && strlen(word) < 4) {
        pthread_mutex_lock(&ep->lock);
        int dup = 0;
        for (int i = 0; i < ep->unclassified_count; i++)
            if (ep->unclassified_words[i][0] && strcmp(ep->unclassified_words[i], word) == 0) {
                dup = 1;
                break;
            }
        if (!dup && ep->unclassified_count < (int)(sizeof(ep->unclassified_words) / 4)) {
            int slot = ep->unclassified_count;
            snprintf(ep->unclassified_words[slot], 4, "%s", word);
            memcpy(ep->unclassified_feats[slot], _feat_copy, sizeof(_feat_copy));
            ep->unclassified_count++;
        }
        ep->emerge_check_counter++;
        int trig = (ep->emerge_check_counter >= EMERGE_CHECK_INTERVAL &&
                    ep->unclassified_count >= EMERGE_POOL_TRIGGER);
        if (trig) {
            ep->emerge_check_counter = 0;
            emergent_pos_try_emerge(ep);   /* 内部假定调用方持锁（与 classify_soft 一致） */
        }
        pthread_mutex_unlock(&ep->lock);
    }

    /* v0.5.10 fix: 移除锁外回写词类到节点。
     * 原代码在 ar->mutex 锁外（article_flush 特意移出锁外避 O(n²) 聚类）
     * 写共享节点 emergent_class_ids/count——与扩容 realloc 并发时
     * 节点指针悬垂 → 堆损坏（double free）。POS 标注是查询，回写是
     * 缓存优化；模板生长/diffusion 读的数据由喂料主路径持续供给，
     * 此处零写 = 锁外安全。 */
}

/* ================================================================
 *  锚点中心微调
 * ================================================================ */

void emergent_pos_adjust_centroid(EmergentPOS* ep, POSTag tag,
                                  const float* features) {
    if (!ep || !features) return;
    if (tag <= POS_UNKNOWN || tag >= POS_COUNT) return;

    POSAnchor* anchor = &ep->anchors[tag];
    if (!anchor->is_active) return;

    /* 动态学习率: 中心越稳定，步长越小 */
    float lr = ep->learn_rate;
    if (anchor->centroid_stability > POS_ANCHOR_STABILITY_THRESHOLD) {
        lr *= 0.5f; /* 高稳定性时减半 */
    }
    /* 长期使用后进一步衰减 */
    if (ep->classify_count > 10000) lr *= 0.5f;

    /* EMA 更新: centroid += lr * (features - centroid) */
    for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
        anchor->centroid[d] += lr * (features[d] - anchor->centroid[d]);
    }

    anchor->member_count++;

    /* 更新稳定性: 成员越多越稳定 */
    float new_stability = 1.0f - (1.0f / (1.0f + (float)anchor->member_count * 0.1f));
    anchor->centroid_stability = anchor->centroid_stability * 0.95f + new_stability * 0.05f;

    ep->classify_count++;
}

/* ================================================================
 *  新词类涌现 — 从未分类池中发现新的词类
 * ================================================================ */

int emergent_pos_try_emerge(EmergentPOS* ep) {
    if (!ep) return 0;
    if (ep->unclassified_count < EMERGE_POOL_TRIGGER) return 0;
    if (ep->extra_class_count >= 16) return 0; /* 达上限 */

    int n = ep->unclassified_count;
    /* 判据尺度跟着表示走：指纹可用就用指纹尺度，否则沿用 512 维特征的旧门槛。 */
    int use_fp = pos_embed_enabled() && ep->unclassified_words[0][0] != 0;
    float coh_thresh = use_fp ? EMERGE_FP_COHERENCE : EMERGE_COHERENCE_THRESH;
    fprintf(stderr, "[EmergentPOS] 涌现检查: 未分类池 %d 个词（判据=%s 阈值=%.2f）\n",
            n, use_fp ? "分布指纹" : "512维特征", (double)coh_thresh);

    /* 构建 pairwise 余弦相似度矩阵 (上三角) */
    /* 动态分配避免大栈数组 */
    float* sim_matrix = (float*)malloc((size_t)n * (size_t)n * sizeof(float));
    if (!sim_matrix) return 0;

    for (int i = 0; i < n; i++) {
        sim_matrix[i * n + i] = 1.0f;
        for (int j = i + 1; j < n; j++) {
            /* v0.6.12：聚类判据优先用**分布指纹**。
             * feats 是 512 维随机噪声（重训时 rand() 初始化），在其上算余弦
             * 等于随机分组；指纹是「左右邻出现在哪些功能字旁」的真实分布。 */
            float s = -1.0f;
            if (pos_embed_enabled() && ep->unclassified_words[i][0] && ep->unclassified_words[j][0])
                s = wemb_similar(ep->unclassified_words[i], ep->unclassified_words[j]);
            if (s < 0.0f)
                s = cosine_similarity(ep->unclassified_feats[i],
                                    ep->unclassified_feats[j],
                                    PM_NODE_FEATURE_DIM);
            sim_matrix[i * n + j] = s;
            sim_matrix[j * n + i] = s;
        }
    }

    /* 简单贪婪聚类: 找第一个未访问的, 聚其所有相似度 > threshold 的邻居 */
    typedef struct { int* members; int count; int capacity; } Cluster;
    Cluster clusters[16];
    int nclusters = 0;
    int* visited = (int*)calloc((size_t)n, sizeof(int));
    if (!visited) { free(sim_matrix); return 0; }

    for (int i = 0; i < n && nclusters < 16; i++) {
        if (visited[i]) continue;
        visited[i] = 1;

        /* 创建新簇 */
        Cluster* cl = &clusters[nclusters];
        cl->capacity = n;
        cl->members = (int*)malloc((size_t)n * sizeof(int));
        if (!cl->members) break;
        cl->members[0] = i;
        cl->count = 1;

        /* 找所有相似度超阈值的邻居 */
        for (int j = i + 1; j < n; j++) {
            if (visited[j]) continue;
            /* 双判据：① 与簇内成员【平均】相似 ≥ 阈值；② 与【簇首（种子）】相似 ≥ 阈值。
             * 只用平均会跨类漂移 —— 实测「走拿跳」这个簇把副词「更、太」也吸进来了
             * （平均被稀释到刚过线），因为簇只有 1–2 个成员时平均没有约束力。
             * 簇首锚定挡住了这条漂移，同时保留「渐进归并」（第一印象主导）。 */
            float avg = 0.0f;
            int all_connected = 1;
            for (int mi = 0; mi < cl->count; mi++)
                avg += sim_matrix[cl->members[mi] * n + j];
            avg /= (float)cl->count;
            if (avg < coh_thresh) all_connected = 0;
            if (use_fp && sim_matrix[cl->members[0] * n + j] < EMERGE_FP_SEED_SIM) all_connected = 0;
            if (all_connected) {
                visited[j] = 1;
                cl->members[cl->count++] = j;
            }
        }

        if (cl->count >= EMERGE_MIN_CLUSTER_SIZE) {
            nclusters++;
        } else {
            /* 簇太小，释放 */
            free(cl->members);
            cl->members = NULL;
            cl->count = 0;
        }
    }

    free(visited);

    /* 为每个有效簇创建新词类 */
    int created = 0;
    for (int ci = 0; ci < nclusters; ci++) {
        Cluster* cl = &clusters[ci];
        if (cl->count < EMERGE_MIN_CLUSTER_SIZE) {
            free(cl->members);
            continue;
        }

        /* 计算簇内平均 pairwise 相似度（coherence） */
        float total_sim = 0.0f;
        int npairs = 0;
        for (int a = 0; a < cl->count; a++) {
            for (int b = a + 1; b < cl->count; b++) {
                total_sim += sim_matrix[cl->members[a] * n + cl->members[b]];
                npairs++;
            }
        }
        float coherence = (npairs > 0) ? total_sim / (float)npairs : 0.0f;

        if (coherence < coh_thresh) {
            fprintf(stderr, "[EmergentPOS] 簇 %d 紧密度 %.3f < %.2f, 放弃\n",
                    ci, coherence, (double)coh_thresh);
            /* BG-29-v2: 不在中途释放（第二个循环仍要用 members），统一挪到其后 */
            continue;
        }

        /* 创建新词类 */
        /* BG-29: 入口（:580）只在函数开头查一次，而本循环可连开多个类
         * ⇒ ei 可达 16 ⇒ extra_classes[16] 越界写（会踩到 lock）。循环内再查一次。 */
        if (ep->extra_class_count >= 16) {
            fprintf(stderr, "[EmergentPOS] extra_classes 已满(16)，本轮停止新增\n");
            break;
        }
        int ei = ep->extra_class_count;
        ep->extra_classes[ei].class_id = POS_COUNT + ei;
        ep->extra_classes[ei].member_count = cl->count;
        ep->extra_classes[ei].coherence = coherence;
        ep->extra_classes[ei].is_active = 1;

        /* 计算中心：簇内特征向量均值 */
        memset(ep->extra_classes[ei].centroid, 0,
               PM_NODE_FEATURE_DIM * sizeof(float));
        for (int mi = 0; mi < cl->count; mi++) {
            int pool_idx = cl->members[mi];
            for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
                ep->extra_classes[ei].centroid[d] +=
                    ep->unclassified_feats[pool_idx][d];
            }
        }
        float inv_n = 1.0f / (float)cl->count;
        for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
            ep->extra_classes[ei].centroid[d] *= inv_n;
        }

        /* 生成人类可读标签：直接列成员词（前 8 个）。
         * 原来只写 "Emerge%d"，验收时根本看不出这个类像什么。 */
        {
            int off = 0;
            ep->extra_classes[ei].label_hint[0] = 0;
            for (int mi = 0; mi < cl->count && mi < 8 && off + 3 < 32; mi++) {
                const char* mw = ep->unclassified_words[cl->members[mi]];
                if (!mw[0]) continue;
                snprintf(ep->extra_classes[ei].label_hint + off,
                         sizeof(ep->extra_classes[ei].label_hint) - (size_t)off, "%s", mw);
                off += 3;
            }
            if (!ep->extra_classes[ei].label_hint[0])
                snprintf(ep->extra_classes[ei].label_hint,
                         sizeof(ep->extra_classes[ei].label_hint),
                         "Emerge%d", ep->extra_classes[ei].class_id);
        }

        ep->extra_class_count++;
        created++;

        fprintf(stderr,
            "[EmergentPOS] 新词类涌现! class_id=%d size=%d coherence=%.3f label=%s\n",
            ep->extra_classes[ei].class_id, cl->count, coherence,
            ep->extra_classes[ei].label_hint);

        /* BG-29-v2: 不在中途释放，统一挪到第二个循环之后 */
    }

    /* 从池中移除已聚类的词 */
    {
        int* keep = (int*)calloc((size_t)n, sizeof(int));
        if (keep) {
            int kept = 0;
            for (int pi = 0; pi < n; pi++) {
                int in_cluster = 0;
                for (int ci = 0; ci < nclusters; ci++) {
                    if (clusters[ci].count >= EMERGE_MIN_CLUSTER_SIZE) {
                        for (int mi = 0; mi < clusters[ci].count; mi++) {
                            if (clusters[ci].members[mi] == pi) {
                                in_cluster = 1; break;
                            }
                        }
                    }
                    if (in_cluster) break;
                }
                if (!in_cluster) keep[pi] = 1, kept++;
            }

            /* 压缩池: 保留未聚类的 */
            int dst = 0;
            for (int pi = 0; pi < n; pi++) {
                if (keep[pi]) {
                    if (dst != pi) {
                        memcpy(ep->unclassified_feats[dst],
                               ep->unclassified_feats[pi],
                               PM_NODE_FEATURE_DIM * sizeof(float));
                        ep->unclassified_pool_nodes[dst] =
                            ep->unclassified_pool_nodes[pi];
                    }
                    dst++;
                }
            }
            ep->unclassified_count = dst;
            free(keep);
        }
    }

    /* BG-29-v2: 统一释放簇成员数组。
     * 原先在第一个循环里逐个 free，而第二个循环
     * （从池中移除已聚类词）仍会遍历 clusters[ci].members
     * ⇒ ASan 实锤 heap-use-after-free @ :724。挪到这里最安全：
     * 此后 :758 会清池，members 不再被使用。 */
    for (int _ci = 0; _ci < nclusters; _ci++) {
        if (clusters[_ci].members) {
            free(clusters[_ci].members);
            clusters[_ci].members = NULL;
        }
    }

    /* v0.5.8: 聚类检查后清池（无论成功失败）——
     * 散词彼此不相似，永远聚不成簇；保留会导致每 500 次分类调用
     * 反复触发 O(n²) 聚类，锁内长持 → 学习线程堆积
     * （08-07 慢喂料 36 线程排队实锤）。散词为低价值噪声，
     * 清空后重新积累成本低。 */
    ep->unclassified_count = 0;

    free(sim_matrix);
    return created;
}

/* ================================================================
 *  持久化 — 保存/加载锚点中心 + 额外词类
 * ================================================================ */

/* 路径 SSOT：默认落点 = <home>/data/emergent_pos.bin。
 * ⚠ 本宏是【运行期表达式】而非字符串字面量，勿用于需要编译期常量的场合。 */
#define EMERGENT_POS_DEFAULT_FILE pm_file(PM_FILE_EMERGENT_POS)

/* v0.5.30 P0b: 维度闸门（TAIL-DIM-1）——文件头加维度字段，读端校验。
 *
 * 旧病根：v1 头 <magic,version> 里**没有维度字段**，记录宽度由编译期
 * PM_NODE_FEATURE_DIM 决定。512→256 降维后，512 时代旧文件会被按 256 静默读错
 * （质心只读到前半截、后续字段全错位），零日志零感知。
 *
 * 新头（v2）：magic(4) + version(4) + dim(4) + active_count(4) + 记录…
 * 旧头（v1）：magic(4) + version(4) + active_count(4) + 记录…   ← 无 dim
 *
 * 向后兼容铁律（作者亲授）：**无维度字段 = 明确的旧格式**，不拒绝——
 * 按整文件 size 反推维度 + WARN + 记账；**只有反推不出来才拒绝**。
 * 反推方程见 ep_infer_legacy_dim()。 */
#define EMERGENT_POS_MAGIC        0x504D4550u   /* "PMEP" */
#define EMERGENT_POS_VERSION_DIM  2u            /* v2: 头含 dim 字段（本次起写端输出） */
#define EMERGENT_POS_VERSION_LEGACY 1u          /* v1: 无 dim 字段（512 时代旧格式，只读兼容） */
#define EMERGENT_POS_EXTRA_MAX    16            /* extra_classes[16] 上限（与结构体一致） */

/* 记账：v1 旧格式文件按 size 反推维度成功加载的累计次数（进程内只增）。
 * 与 WARN 日志配套——「让失败/降级可见」的计数一侧。 */
static unsigned long g_ep_legacy_dim_inferred = 0;

int emergent_pos_save(EmergentPOS* ep, const char* filepath) {
    if (!ep) return -1;
    /* v0.6 步 3：默认路径**只属于中文实例**。其它语种的实例必须显式给 filepath，
     * 否则多实例会共用同一条默认路径 ⇒ 互相静默覆盖（非中文实例当前无默认落点）。 */
    if (!filepath && ep->lang != PM_LANG_ZH) {
        fprintf(stderr, "[EmergentPOS] 拒绝用默认路径保存非中文实例(lang=%s)"
                        " —— 请显式传 filepath\n", pm_lang_name(ep->lang));
        return -1;
    }
    const char* path = filepath ? filepath : EMERGENT_POS_DEFAULT_FILE;

    /* A-P1-5 fix: tmp+rename 原子写 + fsync，杜绝半写文件；临时名带 pid+序号防互踩 */
    char tmp_path[1024];
    static unsigned long ep_seq = 0;
    unsigned long seq = __sync_add_and_fetch(&ep_seq, 1);
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld.%lu",
                 path, (long)getpid(), seq) >= (int)sizeof(tmp_path)) {
        fprintf(stderr, "[EmergentPOS] 保存失败：路径过长 %s\n", path);
        return -1;
    }
    FILE* f = fopen(tmp_path, "wb");
    if (!f) { return -1; }

    /* A-P1-5 fix: 逐写检查返回值——磁盘满/写失败必须可感知，不再谎报"持久化完成" */
#define EP_WRITE(ptr, sz, n) do { \
        if (fwrite((ptr), (sz), (n), f) != (size_t)(n)) { \
            fprintf(stderr, "[EmergentPOS] 写入失败: %s (%s)\n", tmp_path, strerror(errno)); \
            fclose(f); remove(tmp_path); return -1; \
        } \
    } while (0)

    /* v0.5.30 P0b: 写端输出带维度的新头 —— magic + version(2) + dim + active_count。
     * dim 字段是维度闸门的判据：读端凭它校验，不再靠编译期宏猜。 */
    uint32_t magic   = EMERGENT_POS_MAGIC;              /* "PMEP" */
    uint32_t version = EMERGENT_POS_VERSION_DIM;        /* 2 = 头含 dim（v1 无 dim = 旧格式） */
    uint32_t dim     = (uint32_t)PM_NODE_FEATURE_DIM;   /* 全仓唯一真值源 constants.h:37 */
    EP_WRITE(&magic, sizeof(magic), 1);
    EP_WRITE(&version, sizeof(version), 1);
    EP_WRITE(&dim, sizeof(dim), 1);

    uint32_t active_count = 0;
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++)
        if (ep->anchors[tag].is_active) active_count++;
    EP_WRITE(&active_count, sizeof(active_count), 1);

    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        if (!anchor->is_active) continue;
        EP_WRITE(&tag, sizeof(int), 1);
        EP_WRITE(anchor->centroid, sizeof(float), PM_NODE_FEATURE_DIM);
        EP_WRITE(&anchor->member_count, sizeof(int), 1);
        EP_WRITE(&anchor->centroid_stability, sizeof(float), 1);
    }

    EP_WRITE(&ep->extra_class_count, sizeof(int), 1);
    for (int ei = 0; ei < ep->extra_class_count; ei++) {
        EP_WRITE(&ep->extra_classes[ei].class_id, sizeof(int), 1);
        EP_WRITE(ep->extra_classes[ei].centroid, sizeof(float), PM_NODE_FEATURE_DIM);
        EP_WRITE(&ep->extra_classes[ei].member_count, sizeof(int), 1);
        EP_WRITE(&ep->extra_classes[ei].coherence, sizeof(float), 1);
        EP_WRITE(ep->extra_classes[ei].label_hint, sizeof(char), 32);
    }
#undef EP_WRITE

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fprintf(stderr, "[EmergentPOS] fsync 失败: %s (%s)\n", tmp_path, strerror(errno));
        fclose(f); remove(tmp_path); return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "[EmergentPOS] 关闭失败: %s (%s)\n", tmp_path, strerror(errno));
        remove(tmp_path); return -1;
    }

    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "[EmergentPOS] 原子替换失败: %s → %s (%s)\n",
                tmp_path, path, strerror(errno));
        remove(tmp_path);
        return -1;
    }
    {   /* 目录 fsync */
        char dir_buf[1024];
        snprintf(dir_buf, sizeof(dir_buf), "%s", path);
        char* slash = strrchr(dir_buf, '/');
        if (slash) *slash = '\0'; else strcpy(dir_buf, ".");
        int dfd = open(dir_buf, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) { fsync(dfd); close(dfd); }
    }

    fprintf(stderr, "[EmergentPOS] 持久化完成: %d 硬编码锚点 + %d 额外词类 → %s\n",
            (int)active_count, ep->extra_class_count, path);
    return 0;
}

/* ----------------------------------------------------------------
 * v0.5.30 P0b: 维度闸门辅助（TAIL-DIM-1）
 * ---------------------------------------------------------------- */

/* 旧格式(无维度字段)文件按整文件 size 反推特征维度。
 *
 * 布局方程（A=active_count, E=extra_count, D=特征维度）：
 *   size = 12 + A*(4 + 4D + 4 + 4) + 4 + E*(4 + 4D + 4 + 4 + 32)
 *        = 16 + 12A + 44E + 4D*(A+E)
 *
 * 解法：对 E ∈ [0, EMERGENT_POS_EXTRA_MAX] 逐个求整数 D，再做二次校验
 *      （offset_E = 12 + A*(12+4D) 处的 int 必须恰好 == E）。
 * 判定：
 *   - 0 解  → 返回 0（调用方拒绝：size 与任何候选维度都不自洽）
 *   - 1 解  → 返回 1
 *   - 多解且其中恰一个 == 当前 PM_NODE_FEATURE_DIM → 取它（当前维度读数最稳）
 *   - 仍多解 → 返回 0（歧义不猜，宁可拒绝）
 */
static int ep_infer_legacy_dim(size_t size, uint32_t active,
                               const unsigned char* buf,
                               uint32_t* out_dim, int* out_extra) {
    if (active > (uint32_t)(POS_COUNT - 1)) return 0;   /* 越界即非本格式 */
    if (active == 0 && size == 16) {                    /* 空文件：无质心，维度无关 */
        *out_dim = (uint32_t)PM_NODE_FEATURE_DIM;
        *out_extra = 0;
        return 1;
    }

    uint32_t sol_dim[EMERGENT_POS_EXTRA_MAX + 1];
    int      sol_extra[EMERGENT_POS_EXTRA_MAX + 1];
    int nsol = 0;

    for (int e = 0; e <= EMERGENT_POS_EXTRA_MAX; e++) {
        long long num = (long long)size - 16
                      - 12LL * (long long)active - 44LL * (long long)e;
        long long den = 4LL * ((long long)active + (long long)e);
        if (den <= 0 || num <= 0) continue;
        if (num % den != 0) continue;
        long long d = num / den;
        if (d < 1 || d > 65536) continue;
        size_t off_e = (size_t)12 + (size_t)active * (12u + 4u * (size_t)d);
        if (off_e + 4 > size) continue;
        int e_at = 0;
        memcpy(&e_at, buf + off_e, sizeof(int));
        if (e_at != e) continue;                        /* 二次校验 */
        sol_dim[nsol] = (uint32_t)d;
        sol_extra[nsol] = e;
        nsol++;
        if (nsol > EMERGENT_POS_EXTRA_MAX) break;
    }

    if (nsol == 0) return 0;
    if (nsol == 1) { *out_dim = sol_dim[0]; *out_extra = sol_extra[0]; return 1; }

    int pick = -1, cur = 0;
    for (int i = 0; i < nsol; i++)
        if (sol_dim[i] == (uint32_t)PM_NODE_FEATURE_DIM) { pick = i; cur++; }
    if (cur == 1) { *out_dim = sol_dim[pick]; *out_extra = sol_extra[pick]; return 1; }
    return 0;
}

/* 把文件里 file_dim 宽的中心向量装入当前 PM_NODE_FEATURE_DIM 宽的数组。
 * 同维直拷；跨维（只可能来自 v1 旧格式反推路径）按 min() 截断、不足补 0，
 * 上层已 WARN + 记账（语义不完整，锚点需重建）。 */
static void ep_load_centroid(float* dst, const unsigned char* src,
                             uint32_t file_dim, float* scratch) {
    if (file_dim == (uint32_t)PM_NODE_FEATURE_DIM) {
        memcpy(dst, src, (size_t)PM_NODE_FEATURE_DIM * sizeof(float));
        return;
    }
    memcpy(scratch, src, (size_t)file_dim * sizeof(float));
    uint32_t n = file_dim < (uint32_t)PM_NODE_FEATURE_DIM
               ? file_dim : (uint32_t)PM_NODE_FEATURE_DIM;
    for (uint32_t d = 0; d < n; d++) dst[d] = scratch[d];
    for (uint32_t d = n; d < (uint32_t)PM_NODE_FEATURE_DIM; d++) dst[d] = 0.0f;
}

int emergent_pos_load(EmergentPOS* ep, const char* filepath) {
    if (!ep) return 0;
    /* v0.6 步 3：与 save 同一条闸门 —— 别让非中文实例去读中文锚点文件。 */
    if (!filepath && ep->lang != PM_LANG_ZH) {
        fprintf(stderr, "[EmergentPOS] 拒绝用默认路径加载非中文实例(lang=%s)"
                        " —— 请显式传 filepath\n", pm_lang_name(ep->lang));
        return -1;
    }
    const char* path = filepath ? filepath : EMERGENT_POS_DEFAULT_FILE;

    FILE* f = fopen(path, "rb");
    if (!f) return 0; /* 文件不存在是正常情况，返回 0 */

    /* v0.5.30 P0b: 整文件读入 —— 维度闸门必须知道「文件实际长度」才能
     * 反推维度、并校验头自述维度是否与内容自洽（见 ep_infer_legacy_dim）。 */
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 无法定位文件尾 (%s)"
                " → 拒绝\n", __LINE__, path, strerror(errno));
        fclose(f);
        return -1;
    }
    long fsz_l = ftell(f);
    if (fsz_l < 0) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 无法取文件长度 (%s)"
                " → 拒绝\n", __LINE__, path, strerror(errno));
        fclose(f);
        return -1;
    }
    size_t size = (size_t)fsz_l;
    rewind(f);
    unsigned char* buf = (unsigned char*)malloc(size ? size : 1);
    if (!buf) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 内存不足 (%zu 字节)"
                " → 拒绝\n", __LINE__, path, size);
        fclose(f);
        return -1;
    }
    if (size > 0 && fread(buf, 1, size, f) != size) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 读取失败 期望=%zu 字节"
                " 实际读入不足 (%s) → 拒绝\n", __LINE__, path, size, strerror(errno));
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    /* ---- 文件头：magic + version（+ v2 的 dim）---- */
    if (size < 12) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 文件截断 期望>=12 字节"
                " 实际=%zu 字节（连 magic+version+active_count 都不够）→ 拒绝\n",
                __LINE__, path, size);
        free(buf);
        return -1;
    }
    uint32_t magic = 0, version = 0;
    memcpy(&magic, buf, 4);
    memcpy(&version, buf + 4, 4);
    if (magic != EMERGENT_POS_MAGIC) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s magic 不符 期望=%08x(PMEP)"
                " 实际=%08x → 拒绝（非本格式文件）\n",
                __LINE__, path, EMERGENT_POS_MAGIC, magic);
        free(buf);
        return -1;
    }

    uint32_t file_dim = 0;
    size_t   hdr      = 0;   /* 读 active_count 的偏移（= 头长度） */
    int      legacy_inferred = 0;

    if (version == EMERGENT_POS_VERSION_DIM) {
        /* 新头：维度由文件自述，必须等于当前二进制维度（否则内容宽度不符） */
        if (size < 12) {
            fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 文件截断 期望>=12 字节"
                    " 实际=%zu 字节（v2 头缺 dim 字段）→ 拒绝\n", __LINE__, path, size);
            free(buf);
            return -1;
        }
        memcpy(&file_dim, buf + 8, 4);
        hdr = 12;   /* magic(4)+version(4)+dim(4)；紧随其后是 active_count */
        if (file_dim != (uint32_t)PM_NODE_FEATURE_DIM) {
            fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 维度不符 期望=%d"
                    "（当前二进制 PM_NODE_FEATURE_DIM）实际=%u（文件头自述）→ 拒绝\n",
                    __LINE__, path, PM_NODE_FEATURE_DIM, file_dim);
            free(buf);
            return -1;
        }
    } else if (version == EMERGENT_POS_VERSION_LEGACY) {
        /* 旧头：无维度字段 = 明确的旧格式 ⇒ 按 size 反推 + WARN + 记账（不拒绝）。
         * ⛔ 绝不"无维度字段就一刀切拒绝"——那会把手上所有历史文件全废掉。 */
        uint32_t active_probe = 0;
        memcpy(&active_probe, buf + 8, 4);
        int extra_probe = 0;
        if (!ep_infer_legacy_dim(size, active_probe, buf, &file_dim, &extra_probe)) {
            fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 无法按 size 反推维度"
                    " 期望=size==16+12*active+44*extra+4*dim*(active+extra)"
                    " 实际=size=%zu active=%u → 拒绝（size 与任何候选维度都不自洽，"
                    "疑为截断/损坏）\n", __LINE__, path, size, active_probe);
            free(buf);
            return -1;
        }
        legacy_inferred = 1;
        hdr = 8;    /* magic(4)+version(4)；v1 无 dim，紧随其后就是 active_count */
        g_ep_legacy_dim_inferred++;   /* 记账 */
        fprintf(stderr, "[EmergentPOS] WARN " __FILE__ ":%d: %s 旧格式文件(无维度字段,"
                "version=%u) 按 size 反推维度 dim=%u (期望=%d=当前二进制) size=%zu active=%u"
                " extra=%d → 按反推维度读取并记账(本进程累计第 %lu 次)%s\n",
                __LINE__, path, EMERGENT_POS_VERSION_LEGACY, file_dim, PM_NODE_FEATURE_DIM,
                size, active_probe, extra_probe, g_ep_legacy_dim_inferred,
                (file_dim == (uint32_t)PM_NODE_FEATURE_DIM) ? ""
                    : " ⚠ 维度不一致：跨维按 min() 截断/补零载入，锚点语义不完整，"
                      "请在下次训练/重启后重建并落盘");
    } else {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 版本未知 期望=%u(旧格式)"
                "|%u(带维度) 实际=%u → 拒绝\n", __LINE__, path,
                EMERGENT_POS_VERSION_LEGACY, EMERGENT_POS_VERSION_DIM, version);
        free(buf);
        return -1;
    }

    /* ---- 记录区 ---- */
    if (hdr + 4 > size) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 文件截断 期望>=%zu 字节"
                "（缺 active_count 字段）实际=%zu 字节 → 拒绝\n", __LINE__, path, hdr + 4, size);
        free(buf);
        return -1;
    }
    uint32_t active_count = 0;
    memcpy(&active_count, buf + hdr, 4);
    hdr += 4;
    if (active_count > (uint32_t)(POS_COUNT - 1)) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s active_count 越界 期望=0..%d"
                " 实际=%u → 拒绝\n", __LINE__, path, POS_COUNT - 1, active_count);
        free(buf);
        return -1;
    }

    size_t anchor_rec = 12u + 4u * (size_t)file_dim;   /* tag+centroid+member+stab */
    size_t extra_rec  = 44u + 4u * (size_t)file_dim;   /* id+centroid+member+coh+label32 */

    float* scratch = NULL;   /* file_dim 宽的临时中心（仅跨维度时需要） */
    if (file_dim != (uint32_t)PM_NODE_FEATURE_DIM) {
        scratch = (float*)malloc((size_t)file_dim * sizeof(float));
        if (!scratch) {
            fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 内存不足"
                    "（跨维临时缓冲 %u 维）→ 拒绝\n", __LINE__, path, file_dim);
            free(buf);
            return -1;
        }
    }

    /* A-P1-5 fix: 先清零再累加——此前只 ++ 不清零，重复 load 会累加越界写 */
    ep->extra_class_count = 0;
    int loaded = 0;
    for (uint32_t i = 0; i < active_count; i++) {
        if (hdr + anchor_rec > size) {
            fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 文件截断 期望>=%zu 字节"
                    "（第 %u/%u 个锚点记录，记录宽=%zu）实际=%zu 字节 → 拒绝\n",
                    __LINE__, path, hdr + anchor_rec, i + 1, active_count, anchor_rec, size);
            goto reject;
        }
        int tag = 0;
        memcpy(&tag, buf + hdr, 4);
        hdr += 4;
        if (tag <= POS_UNKNOWN || tag >= POS_COUNT) {
            fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 锚点 tag 越界 期望=%d..%d"
                    " 实际=%d（第 %u 个锚点记录）→ 拒绝\n",
                    __LINE__, path, POS_UNKNOWN + 1, POS_COUNT - 1, tag, i + 1);
            goto reject;
        }
        POSAnchor* anchor = &ep->anchors[tag];
        ep_load_centroid(anchor->centroid, buf + hdr, file_dim, scratch);
        hdr += 4u * (size_t)file_dim;
        memcpy(&anchor->member_count, buf + hdr, 4); hdr += 4;
        memcpy(&anchor->centroid_stability, buf + hdr, 4); hdr += 4;
        anchor->is_active = 1;
        loaded++;
    }

    /* 读取额外词类 */
    if (hdr + 4 > size) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 文件截断 期望>=%zu 字节"
                "（缺 extra_count 字段）实际=%zu 字节 → 拒绝\n", __LINE__, path, hdr + 4, size);
        goto reject;
    }
    int extra_count = 0;
    memcpy(&extra_count, buf + hdr, 4);
    hdr += 4;
    if (extra_count < 0 || extra_count > EMERGENT_POS_EXTRA_MAX) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s extra_count 越界 期望=0..%d"
                " 实际=%d → 拒绝\n", __LINE__, path, EMERGENT_POS_EXTRA_MAX, extra_count);
        goto reject;
    }

    for (int ei = 0; ei < extra_count; ei++) {
        if (hdr + extra_rec > size) {
            fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 文件截断 期望>=%zu 字节"
                    "（第 %d/%d 个额外词类记录，记录宽=%zu）实际=%zu 字节 → 拒绝\n",
                    __LINE__, path, hdr + extra_rec, ei + 1, extra_count, extra_rec, size);
            goto reject;
        }
        memcpy(&ep->extra_classes[ei].class_id, buf + hdr, 4); hdr += 4;
        ep_load_centroid(ep->extra_classes[ei].centroid, buf + hdr, file_dim, scratch);
        hdr += 4u * (size_t)file_dim;
        memcpy(&ep->extra_classes[ei].member_count, buf + hdr, 4); hdr += 4;
        memcpy(&ep->extra_classes[ei].coherence, buf + hdr, 4); hdr += 4;
        memcpy(ep->extra_classes[ei].label_hint, buf + hdr, 32); hdr += 32;
        ep->extra_classes[ei].is_active = 1;
        ep->extra_class_count++;
    }

    /* 整文件必须被恰好消费完：多出字节 = 头自述维度/计数与内容不符
     * （例如头写 256 而内容按 512 写），少字节 = 截断 —— 二者都拒绝。 */
    if (hdr != size) {
        fprintf(stderr, "[EmergentPOS] 加载失败 " __FILE__ ":%d: %s 文件长度不符 期望=%zu 字节"
                "（按头自述：dim=%u active=%u extra=%d；记录区起点来源=%s）实际=%zu 字节"
                " → 拒绝\n", __LINE__, path, hdr, file_dim, active_count, extra_count,
                legacy_inferred ? "size反推" : "头内dim", size);
        goto reject;
    }

    free(scratch);
    free(buf);
    fprintf(stderr, "[EmergentPOS] 加载完成: %d 硬编码锚点 + %d 额外词类 ← %s"
            "（维度=%u%s）\n", loaded, ep->extra_class_count, path, file_dim,
            legacy_inferred ? "，旧格式按 size 反推" : "");
    return loaded + ep->extra_class_count;

reject:
    free(scratch);
    free(buf);
    return -1;
}

/* ================================================================
 *  获取锚点信息
 * ================================================================ */

const POSAnchor* emergent_pos_get_anchor(EmergentPOS* ep, POSTag tag) {
    if (!ep || tag <= POS_UNKNOWN || tag >= POS_COUNT) return NULL;
    return &ep->anchors[tag];
}

int emergent_pos_anchor_count(EmergentPOS* ep) {
    if (!ep) return 0;
    int count = 0;
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        if (ep->anchors[tag].is_active) count++;
    }
    return count;
}

const char* emergent_pos_class_name(EmergentPOS* ep, int class_id) {
    if (!ep || class_id <= 0) return "?";
    if (class_id < POS_COUNT) return ep->anchors[class_id].label_cn;
    /* 额外词类 */
    int ei = class_id - POS_COUNT;
    if (ei < ep->extra_class_count && ep->extra_classes[ei].is_active)
        return ep->extra_classes[ei].label_hint;
    return "?";
}

/* ================================================================
 * v0.5.19: 分布签名（语法词类的正确尺子——pro 评审 08-14）
 * 语义特征学不出分布类（可替换性）；语法类的本质是"上下文分布相同"。
 * dist_sig[26] = 左邻POS分布[0..10] + 右邻POS分布[11..21] + 位置先验[22..25]
 * ================================================================ */

/* v2.1 阶段0-A 回退开关：1=真分布(decay-all+bump,只写[0..21])，0=旧行为(只增不减+[22..25]+自计数) */
#ifndef DIST_SIG_TRUE_DISTRIBUTION
#define DIST_SIG_TRUE_DISTRIBUTION 1
#endif

void emergent_pos_update_dist_sig(ReasoningNode* node, int left_pos,
                                  int right_pos, int pos_flags) {
    if (!node) return;
    float lr = 0.05f;  /* EMA 学习率——在线累积（不事后批量） */
#if DIST_SIG_TRUE_DISTRIBUTION
    /* v2.1 阶段0-A：真分布——每个 token 出现先全槽衰减，再抬观察槽。
     * 只写 [0..21]（左邻 POS [0..10] + 右邻 POS [11..21]）：
     *   - 邻词是种子（left/right_pos 为 1..10 的 POS 标签）→ 抬对应槽；
     *   - 邻词非种子/无邻（POS_UNKNOWN 或 -1）→ 只全衰减、不抬，正确吃掉未知邻词质量。
     * [22][23] 交回 funcword_record_position 独占；[24][25]（动词后/前）废弃。
     * dist_sig_count 由 funcword_record_position 独占，本函数不再 ++（避免喂料双计数）。 */
    /* v2.2 修复（2026-10-30）：衰减与抬升必须成对。
     * 原写法是「无条件衰减 22 槽 + 有条件抬 1 槽」——种子只覆盖 2.8%，
     * 97% 的更新只衰减不抬升 ⇒ dist_sig 被自己抹成 0（实测「山/看/不」
     * 累计几十次样本、dist_sig 仍全零，就是这个）。
     * 现在：左右邻 POS 全都未知时整次跳过，样本无效就不参与。 */
    {
        int lk = (left_pos  >= POS_NOUN && left_pos  < POS_COUNT);
        int rk = (right_pos >= POS_NOUN && right_pos < POS_COUNT);
        if (!lk && !rk) { (void)pos_flags; return; }
        for (int d = 0; d < 22; d++)
            node->dist_sig[d] += lr * (0.0f - node->dist_sig[d]);
        if (lk)
            node->dist_sig[left_pos] += lr * (1.0f - node->dist_sig[left_pos]);
        if (rk)
            node->dist_sig[11 + right_pos] += lr * (1.0f - node->dist_sig[11 + right_pos]);
        (void)pos_flags;
    }
#else
    /* 旧行为（回退保留）：只增不减（饱和 one-vector）+ [22..25] 位置位 + 自计数 */
    if (left_pos >= 0 && left_pos < POS_COUNT)
        node->dist_sig[left_pos] += lr * (1.0f - node->dist_sig[left_pos]);
    if (right_pos >= 0 && right_pos < POS_COUNT)
        node->dist_sig[11 + right_pos] += lr * (1.0f - node->dist_sig[11 + right_pos]);
    for (int i = 0; i < 4; i++)
        if (pos_flags & (1 << i))
            node->dist_sig[22 + i] += lr * (1.0f - node->dist_sig[22 + i]);
    node->dist_sig_count++;
#endif
}

void emergent_pos_diag_dist_clusters(EmergentPOS* ep, MasterTopology* master) {
    if (!ep || !master) return;
    static int diag_cnt = 0;
    if (++diag_cnt % 100 != 0) return;   /* 每100次调用打一次诊断 */
    SubTopology* vsub = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vsub || !vsub->net) return;
    HuarongTopologyNet* vnet = vsub->net;

    /* 对每个锚点类：种子词的 dist_sig 均值作分布中心 → 找最相似 top3（非种子） */
    for (int a = 0; a < ep->anchor_count; a++) {
        POSAnchor* anchor = &ep->anchors[a];
        if (!anchor || anchor->seed_count == 0) continue;
        float ctr[26] = {0};
        int found = 0;
        for (int i = 0; i < vnet->node_count; i++) {
            ReasoningNode* nd = vnet->nodes[i];
            if (!nd || !nd->concept || nd->dist_sig_count < 20) continue;
            for (int s = 0; s < anchor->seed_count; s++) {
                if (anchor->seeds[s] && strcmp_null(nd->concept, anchor->seeds[s]) == 0) {
                    for (int d = 0; d < 26; d++) ctr[d] += nd->dist_sig[d];
                    found++;
                    break;
                }
            }
        }
        if (found < 1) continue;
        for (int d = 0; d < 26; d++) ctr[d] /= (float)found;

        char top_names[3][64] = {{0},{0},{0}};
        float top_sim[3] = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < vnet->node_count; i++) {
            ReasoningNode* nd = vnet->nodes[i];
            if (!nd || !nd->concept || nd->dist_sig_count < 20) continue;
            int is_seed = 0;
            for (int s = 0; s < anchor->seed_count; s++)
                if (anchor->seeds[s] && strcmp_null(nd->concept, anchor->seeds[s]) == 0) {
                    is_seed = 1; break;
                }
            if (is_seed) continue;
            float dot = 0.0f, na = 0.0f, nb = 0.0f;
            for (int d = 0; d < 26; d++) {
                dot += ctr[d] * nd->dist_sig[d];
                na += ctr[d] * ctr[d];
                nb += nd->dist_sig[d] * nd->dist_sig[d];
            }
            float sim = (na > 0.0f && nb > 0.0f) ?
                dot / (sqrtf(na) * sqrtf(nb)) : 0.0f;
            for (int k = 0; k < 3; k++) {
                if (sim > top_sim[k]) {
                    for (int j = 2; j > k; j--) {
                        top_sim[j] = top_sim[j-1];
                        strncpy(top_names[j], top_names[j-1], 63);
                    }
                    top_sim[k] = sim;
                    strncpy(top_names[k], nd->concept, 63);
                    top_names[k][63] = '\0';
                    break;
                }
            }
        }
        LOG_INFO("[DistSig诊断] 类[%s]: 分布最近={%s(%.2f),%s(%.2f),%s(%.2f)}",
                 anchor->label_cn,
                 top_names[0], top_sim[0],
                 top_names[1], top_sim[1],
                 top_names[2], top_sim[2]);
    }
}

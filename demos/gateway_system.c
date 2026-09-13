/**
 * @file gateway_system.c
 * @brief PivotMind HTTP Gateway — 系统初始化 / 保存关闭
 *
 * 由 demos/pivotmind_gateway.c 拆分而来（v0.5.25 P2-6）。
 * 共享类型与原型见 gateway_internal.h。
 */

#include "pivotmind_paths.h"
#include "gateway_internal.h"
#include <sys/stat.h>   /* stat：判定既有种子是否有内容 */

/* D1: 记忆种子加载状态。加载失败后，任何"用空内存覆盖有效种子"的存盘都要被拦下。
 * 这两个文件级静态量在 gw_system_init 写、gw_system_shutdown 读，均为主线程路径。 */
static int g_memory_seed_load_ok   = 1;   /* 1=加载成功/无异常；0=加载失败 */
static int g_memory_seed_had_data  = 0;   /* 1=既有种子含记录（>16B 即不止 footer） */

static long gw_file_size_or_neg1(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;   /* 不存在 */
    return (long)st.st_size;
}

// ==================== 系统初始化 (复用 digital_life 逻辑) ====================

static int gw_system_init(GatewaySystem* gw) {
    printf("[gateway] 初始化 PivotMind 引擎...\n");

    // 1. 记忆系统
    gw->memory = memory_system_create(500, 2000, 5000);
    if (!gw->memory) { fprintf(stderr, "[gateway] 记忆系统创建失败\n"); return -1; }
    printf("[gateway]   记忆系统就绪\n");

    // 2. 多拓扑网络
    gw->topology = master_topology_create(16);
    if (!gw->topology) { fprintf(stderr, "[gateway] 拓扑网络创建失败\n"); return -1; }

    master_add_sub_topology(gw->topology, TOPO_VOCABULARY, "词汇拓扑", 100000, 10);
    master_add_sub_topology(gw->topology, TOPO_SEMANTIC,  "语义拓扑", 50000, 9);
    master_add_sub_topology(gw->topology, TOPO_EMOTION,   "情绪拓扑", 4000, 8);
    master_add_sub_topology(gw->topology, TOPO_SYNTAX,    "语法拓扑", 1000, 7);
    master_add_sub_topology(gw->topology, TOPO_CONTEXT,  "上下文拓扑", 1000, 6);
    master_add_sub_topology(gw->topology, TOPO_DOMAIN,    "领域拓扑", 1000, 5);
    master_add_sub_topology(gw->topology, TOPO_PRAGMA,   "语用拓扑", 1000, 4);
    master_add_sub_topology(gw->topology, TOPO_CULTURE,  "文化拓扑", 1000, 3);
    master_add_sub_topology(gw->topology, TOPO_CONCEPT,  "概念拓扑", 50000, 9);
    master_add_sub_topology(gw->topology, TOPO_MASTER,   "主拓扑", 100, 0);
    master_add_sub_topology(gw->topology, TOPO_TEMPLATE, "模板拓扑", 20000, 8);

    // 初始知识
    SubTopology* vocab = master_get_sub_topology_by_type(gw->topology, TOPO_VOCABULARY);
    SubTopology* semantic = master_get_sub_topology_by_type(gw->topology, TOPO_SEMANTIC);
    if (vocab && semantic) {
        huarong_net_add_node(vocab->net, "我", NULL, 0);
        huarong_net_add_node(vocab->net, "你", NULL, 0);
        huarong_net_add_node(vocab->net, "是", NULL, 0);
        huarong_net_add_node(vocab->net, "什么", NULL, 0);
        huarong_net_add_node(vocab->net, "学习", NULL, 0);
        huarong_net_add_node(vocab->net, "知道", NULL, 0);
        huarong_net_add_node(vocab->net, "帮助", NULL, 0);
        huarong_net_add_node(semantic->net, "自我", NULL, 0);
        huarong_net_add_node(semantic->net, "他人", NULL, 0);
        huarong_net_add_node(semantic->net, "存在", NULL, 0);
        huarong_net_add_node(semantic->net, "知识", NULL, 0);
        huarong_net_add_node(semantic->net, "理解", NULL, 0);
        huarong_net_add_node(semantic->net, "协助", NULL, 0);
        huarong_net_add_connection(vocab->net, 0, 0, 0.9f);
        huarong_net_add_connection(vocab->net, 1, 1, 0.9f);
        huarong_net_add_connection(vocab->net, 4, 3, 0.8f);
        huarong_net_add_connection(vocab->net, 5, 4, 0.8f);
        huarong_net_add_connection(vocab->net, 6, 5, 0.8f);
    }
    printf("[gateway]   认知网络就绪 (%d 拓扑)\n", gw->topology->sub_topo_count);

    // 3. 因果图
    gw->causal_graph = causal_graph_create(1000, 5000);
    if (!gw->causal_graph) { fprintf(stderr, "[gateway] 因果图创建失败\n"); return -1; }
    printf("[gateway]   因果图就绪\n");

    // 4. 学习器
    gw->learner = active_learner_create(gw->topology, gw->memory);
    if (!gw->learner) { fprintf(stderr, "[gateway] 学习器创建失败\n"); return -1; }
    active_learner_set_interval(gw->learner, 300);
    printf("[gateway]   学习器就绪 (间隔: 300s)\n");

    // 5. 前额叶（对话系统+认知调度）
    gw->prefrontal = prefrontal_create(gw->topology, gw->memory, gw->causal_graph, gw->learner);
    if (!gw->prefrontal) { fprintf(stderr, "[gateway] 前额叶创建失败\n"); return -1; }
    gw->dialog = prefrontal_dialog(gw->prefrontal);  /* 兼容旧代码 */
    printf("[gateway]   前额叶就绪\n");

    // 5b. QA 记忆检索（扩散/prefrontal 无产出时的兜底）
    gw->qa_memory = qa_memory_create("corpus/xiaohuangji_pipe.txt", 500000);
    if (!gw->qa_memory) {
        printf("[gateway]   QA记忆: 未找到语料或加载失败，继续运行\n");
    } else {
        printf("[gateway]   QA记忆就绪 (%d 对)\n", qa_memory_count(gw->qa_memory));
    }

    // 脑干
    gw->brainstem = brainstem_create(gw->topology, gw->memory, gw->dialog->cognitive_state);
    if (!gw->brainstem) { fprintf(stderr, "[gateway] 脑干创建失败\n"); return -1; }
    printf("[gateway]   脑干就绪\n");

    // 丘脑调度器
    gw->thalamus = thalamus_create();
    if (!gw->thalamus) { fprintf(stderr, "[gateway] 丘脑创建失败\n"); return -1; }
    printf("[gateway]   丘脑就绪\n");

    // 初始化爬虫框架（禁止 robots.txt 检查：AI 学习系统不做商业爬虫合规）
    {
        CrawlPolicy policy = CRAWL_POLICY_DEFAULT;
        policy.respect_robots = 0;  /* 搜狗 /sie? 等路径被 robots.txt 禁了 */
        web_fetch_init(&policy);
    }

    // 感觉皮层（自主语料输送口）
    fprintf(stderr, "[gateway]   创建感觉皮层...\n");
    gw->perception = perception_create(gw->topology, gw->memory, gw->learner, NULL);
    if (!gw->perception) { fprintf(stderr, "[gateway] 感觉皮层创建失败\n"); return -1; }
    /* g_perception 已由 perception_create 自动设置 */
    fprintf(stderr, "[gateway]   感觉皮层就绪\n");

    /* C2 配套: 初始化期间收到退出信号就提前收手。此时 gw 上已建对象全部由
     * gw_system_shutdown 逐项判 NULL 统一销毁，不会泄漏；好处是 H2 的
     * pthread_join(init_thread) 立刻返回，不必等完整加载流程跑完。
     * 尤其重要：绝不能让 learn_queue_init() 发生在 learn_queue_shutdown()
     * 之后（旧代码会因此重建 worker，而没人再 join 它们）。 */
    if (gw->shutdown_requested) {
        fprintf(stderr, "[gateway] 初始化被取消（收到退出信号），提前结束\n");
        return -1;
    }

    /* v0.5.9: 学习队列初始化（2 个 worker 消费 /learn 任务） */
    learn_queue_init();

    // 海马体（记忆+巩固，感知联动通过丘脑信号总线）
    fprintf(stderr, "[gateway]   创建海马体...\n");
    gw->hippocampus = hippocampus_create(gw->topology, gw->memory, gw->thalamus);
    if (!gw->hippocampus) { fprintf(stderr, "[gateway] 海马体创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   海马体就绪\n");

    // 小脑（资源平衡）
    fprintf(stderr, "[gateway]   创建小脑...\n");
    gw->cerebellum = cerebellum_create();
    if (!gw->cerebellum) { fprintf(stderr, "[gateway] 小脑创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   小脑就绪\n");

    // 杏仁核（情绪/文化效价调控）
    fprintf(stderr, "[gateway]   创建杏仁核...\n");
    gw->amygdala = amygdala_create(gw->topology, gw->thalamus, gw->dialog->cognitive_state);
    if (!gw->amygdala) { fprintf(stderr, "[gateway] 杏仁核创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   杏仁核就绪\n");

    /* 将认知状态指针注入拓扑，供所有模块通过 master->cognitive_state_ptr 访问 */
    gw->topology->cognitive_state_ptr = gw->dialog->cognitive_state;

    /* ── v0.3 新脑区 ── */
    // 前额叶执行器（推理编排引擎 — 任务分解/子目标调度）
    fprintf(stderr, "[gateway]   创建前额叶执行器 (v0.3)...\n");
    gw->pfe = pfe_create(gw->topology, gw->thalamus);
    if (!gw->pfe) { fprintf(stderr, "[gateway] 前额叶执行器创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   前额叶执行器就绪 (decompose_depth=%d)\n",
            gw->pfe->max_decompose_depth);

    // 想法竞争竞技场（IdeaArena — 多候选多维度竞争选择）
    fprintf(stderr, "[gateway]   创建想法竞技场 (v0.3)...\n");
    gw->arena = idea_arena_create();
    if (!gw->arena) { fprintf(stderr, "[gateway] 想法竞技场创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   想法竞技场就绪 (max_candidates=%d)\n", ARENA_MAX_CANDIDATES);

    /* ── v0.4 新脑区 ── */
    // 布罗卡区（句式模板构建与衰减自调度）
    fprintf(stderr, "[gateway]   创建布罗卡区 (v0.4)...\n");
    gw->broca = broca_create(gw->topology);
    if (!gw->broca) { fprintf(stderr, "[gateway] 布罗卡区创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   布罗卡区就绪 (interval=%d ticks)\n", gw->broca->build_interval_ticks);

    // 下丘脑（需求/动机调控 — 昼夜耦合 + 对话事件调制）
    fprintf(stderr, "[gateway]   创建下丘脑 (v0.4)...\n");
    gw->hypothalamus = hypothalamus_create(gw->dialog->cognitive_state);
    if (!gw->hypothalamus) { fprintf(stderr, "[gateway] 下丘脑创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   下丘脑就绪\n");

    // 加载持久化数据
    fprintf(stderr, "[gateway]   加载持久化状态...\n");
    if (access(pm_file(PM_FILE_STATE), F_OK) == 0) {
        int loaded = master_load_state(gw->topology, pm_file(PM_FILE_STATE));

        /* v0.5.10 fix: 加载失败（数据不完整）时回退 SD 卡备份——
         * systemd SIGKILL 杀在存盘写一半 → 正式文件损坏（08-08 20:21:44
         * 实测 62,105 链接全丢）。备份目录滚动保留最近文件。 */
        if (loaded < 0) {
            fprintf(stderr, "[gateway]   ⚠ 主状态加载失败，尝试回退 SD 卡备份...\n");
            /* 路径 SSOT：备份目录 = <home>/data/backup（原为 /mnt/sdcard/pivotmind_backup） */
            char backup_dir[PM_PATH_MAX];
            char backup_path[1024];
            struct dirent** entries = NULL;
            int n;
            if (pm_data_path(backup_dir, sizeof backup_dir, "backup") < 0) {
                fprintf(stderr, "[gateway]   ⚠ 备份目录路径拼接失败，跳过回退\n");
                backup_dir[0] = '\0';   /* scandir 必失败 ⇒ 走「无可用备份」分支 */
            }
            n = scandir(backup_dir, &entries, NULL, alphasort);
            char newest[1024] = {0};
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    if (!entries[i]) continue;
                    const char* name = entries[i]->d_name;
                    if (strstr(name, "pivotmind_state.") && strstr(name, ".bak") &&
                        strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                        snprintf(backup_path, sizeof(backup_path), "%s/%s", backup_dir, name);
                        /* scandir alphasort = 按名字排序，.bak 带时间戳 → 最后一个最大 */
                        if (access(backup_path, R_OK) == 0) {
                            snprintf(newest, sizeof(newest), "%s", backup_path);
                        }
                    }
                    free(entries[i]);
                }
                free(entries);
            }
            if (newest[0]) {
                fprintf(stderr, "[gateway]   回退到备份: %s\n", newest);
                loaded = master_load_state(gw->topology, newest);
                if (loaded > 0) {
                    /* 回退成功后立即原子存盘回主路径，后续正常滚动 */
                    master_save_state(gw->topology, pm_file(PM_FILE_STATE));
                }
            } else {
                fprintf(stderr, "[gateway]   ⚠ 无可用备份，以空状态启动（知识可能已丢失）\n");
            }
        }

        /* v0.5.7: 状态加载后强制初始化 POS 锚点中心——懒初始化只在
         * emergent_pos_tag 调用时触发，启动后无对话则永不触发 →
         * 锚点 0 激活 → POS 从未工作（实测"0 硬编码锚点"）。
         * 加载后词汇拓扑就绪，立即用种子词初始化中心向量 */
        if (loaded > 0) {
            int pos_init = cc_init_emergent_pos(gw->prefrontal->controller, "zh");
            fprintf(stderr, "[gateway] POS 锚点初始化: %d 个\n", pos_init);
        }
        if (loaded >= 0) fprintf(stderr, "[gateway]   加载拓扑状态: %d 节点\n", loaded);
    }

    /* v0.5.8: 把涌现词类系统挂给感知皮层 → 文章阅读器。
     * 打通 feed→POS 池管道：喂料路径把未分类新词送入 POS 池，
     * 池满触发聚类 → 额外词类涌现 → 语法拓扑获得原料。 */
    if (gw->perception && gw->prefrontal && gw->prefrontal->controller) {
        perception_set_emergent_pos(gw->perception,
                                    gw->prefrontal->controller->emergent_pos);
        fprintf(stderr, "[gateway]   POS 池管道已挂载 → 感知皮层\n");
    }

    int feat_loaded = load_features(gw->topology, pm_file(PM_FILE_FEATURES));
    if (feat_loaded > 0) fprintf(stderr, "[gateway]   加载特征向量: %d 节点\n", feat_loaded);
    else { int initted = init_random_features(gw->topology); fprintf(stderr, "[gateway]   初始化特征向量: %d 节点\n", initted); }


    /* 跨拓扑连接已在 master_load_state 中加载，无需重复 */
    fprintf(stderr, "[gateway]   跨拓扑连接已随状态加载 (节点=%d)\n",
            gw->topology->cross_link_count);

    fprintf(stderr, "[gateway]   加载记忆种子...\n");
    /* D1 fix: 必须消费返回值——绝不用"加载失败后的空内存"覆盖有效种子。
     * 同时记录既有文件尺寸：只有"确实有内容可能被毁"时才禁止存盘；
     * 不存在或 0 字节的种子允许本次正常写入（无害且可自愈）。 */
    {
        long seed_sz = gw_file_size_or_neg1(pm_file(PM_FILE_MEMORY_SEED));
        int loaded = memory_load_seed(gw->memory, pm_file(PM_FILE_MEMORY_SEED));
        g_memory_seed_load_ok  = (loaded >= 0) ? 1 : 0;
        g_memory_seed_had_data = (seed_sz > 16);   /* 16B = 仅 footer 的空种子 */
        if (loaded < 0) {
            fprintf(stderr, "[gateway]   ⚠ 记忆种子加载失败(返回 %d)，本次退出将"
                    "拒绝覆盖 memory_seed.dat（防止空状态永久覆盖有效种子）\n", loaded);
        } else {
            fprintf(stderr, "[gateway]   记忆种子就绪 (%d 条)\n", loaded);
        }
    }

    // 模板拓扑 (懒加载：启动时不全量构建，边用边积累)
    SubTopology* tpl = master_get_sub_topology_by_type(gw->topology, TOPO_TEMPLATE);
    if (tpl && tpl->net && tpl->net->node_count > 0) {
        fprintf(stderr, "[gateway]   模板拓扑就绪 (%d 节点)\n", tpl->net->node_count);
        gw->topology->use_template_voting = 1;
    } else {
        fprintf(stderr, "[gateway]   模板拓扑空，将在对话中逐步构建\n");
    }
    // 无论模板是否就绪，都开启投票（空模板时自动降级为无模板）
    gw->topology->use_template_voting = 1;

    fprintf(stderr, "[gateway]   创建节点缓存...\n");
    /* v0.5.13 fix: node_cap 按实际节点数+30%——此前 max_nodes+10000 在节点
     * 超容量时 cap < 实际节点 → bitmap/offsets 数组越界 + 每次启动"新建"
     * 截断冻结库（08-12 实测：cap=110000 < 120444 节点，thaw 全走"从未保存过"分支）
     * v0.5.17 fix: cap 改固定 200000——动态 cap（节点数×1.3）随节点增长每次漂移
     * → file_nodes != node_cap → wb+ 截断重建冻结库 → 冻结边导出 0 → 存盘丢
     * 冻结边（08-13 实测：23万边 → 6万边）。固定 cap 保证启动间不重建。 */
    gw->brain_cache = node_cache_create(pm_file(PM_FILE_BRAIN_CACHE), 200000);
    if (!gw->brain_cache) {
        fprintf(stderr, "[gateway] 大脑缓存创建失败\n");
        return -1;
    }
    fprintf(stderr, "[gateway]   节点缓存就绪\n");

    /* 注入缓存到所有子拓扑的哈希表，启用自动解冻 */
    for (int t = 0; t < gw->topology->sub_topo_count; t++) {
        SubTopology* sub = gw->topology->sub_topologies[t];
        if (sub && sub->node_hash)
            node_hash_set_cache(sub->node_hash, gw->brain_cache);
    }

    /* ================================================================
     *  丘脑信号总线 — 注册所有脑区 + 工具组件 + 拓扑归属
     * ================================================================ */

    // 注册脑区实例
    thalamus_register_region(gw->thalamus, THAL_PREFRONTAL,  gw->prefrontal);
    thalamus_register_region(gw->thalamus, THAL_HIPPOCAMPUS, gw->hippocampus);
    thalamus_register_region(gw->thalamus, THAL_PERCEPTION,  gw->perception);
    thalamus_register_region(gw->thalamus, THAL_CEREBELLUM,  gw->cerebellum);
    thalamus_register_region(gw->thalamus, THAL_AMYGDALA,    gw->amygdala);
    thalamus_register_region(gw->thalamus, THAL_PREF_EXEC,  gw->pfe);           /* v0.3 前额叶执行器 */
    thalamus_register_region(gw->thalamus, THAL_DMN,         NULL);              /* DMN 是无状态函数 */
    thalamus_register_region(gw->thalamus, THAL_BROCA,       gw->broca);         /* v0.4 Broca 有状态 */
    thalamus_register_region(gw->thalamus, THAL_HYPOTHALAMUS, gw->hypothalamus); /* v0.4 下丘脑 */

    // 注册工具组件
    thalamus_register_utility(gw->thalamus, THAL_UTIL_NODE_CACHE,      gw->brain_cache);
    thalamus_register_utility(gw->thalamus, THAL_UTIL_COGNITIVE_CTRL,  gw->prefrontal->controller);
    thalamus_register_utility(gw->thalamus, THAL_UTIL_SELF_LEARNER,   NULL);  /* self_learner 在后面注册 */
    thalamus_register_utility(gw->thalamus, THAL_UTIL_TOPO_BRAIN,     NULL);  /* topo_brain 在后面注册 */
    thalamus_register_utility(gw->thalamus, THAL_UTIL_IDEA_ARENA,      gw->arena); /* v0.3 */

    /* 根据运行时配置禁用脑区 */
    if (gw->config && gw->config->loaded) {
        if (!gw->config->brain_regions.perception)   thalamus_enable_region(gw->thalamus, THAL_PERCEPTION, 0);
        if (!gw->config->brain_regions.hippocampus)  thalamus_enable_region(gw->thalamus, THAL_HIPPOCAMPUS, 0);
        if (!gw->config->brain_regions.dmn)          thalamus_enable_region(gw->thalamus, THAL_DMN, 0);
        if (!gw->config->brain_regions.broca)        thalamus_enable_region(gw->thalamus, THAL_BROCA, 0);
        if (!gw->config->brain_regions.cerebellum)   thalamus_enable_region(gw->thalamus, THAL_CEREBELLUM, 0);
        if (!gw->config->brain_regions.amygdala)     thalamus_enable_region(gw->thalamus, THAL_AMYGDALA, 0);
        if (!gw->config->brain_regions.hypothalamus) thalamus_enable_region(gw->thalamus, THAL_HYPOTHALAMUS, 0);
        if (!gw->config->brain_regions.visual_cortex) thalamus_enable_region(gw->thalamus, THAL_VISUAL_CORTEX, 0);
    }

    // 设置子拓扑按脑区归属（每个脑区只负责自己的子拓扑）
    {
        int prefrontal_topo[]  = {TOPO_VOCABULARY, TOPO_SEMANTIC, TOPO_PRAGMA, TOPO_CONCEPT, TOPO_DOMAIN};
        int hippocampus_topo[] = {TOPO_CONTEXT, TOPO_DOMAIN};
        thalamus_set_partition(gw->thalamus, THAL_PREFRONTAL,  prefrontal_topo,  5);
        int broca_topo[]       = {TOPO_SYNTAX, TOPO_TEMPLATE};
        int amygdala_topo[]    = {TOPO_EMOTION, TOPO_CULTURE};
        int perception_topo[]  = {TOPO_VISUAL};                      /* v0.5 视觉拓扑归属感知 */
        thalamus_set_partition(gw->thalamus, THAL_PREFRONTAL,  prefrontal_topo,  4);
        thalamus_set_partition(gw->thalamus, THAL_HIPPOCAMPUS, hippocampus_topo, 2);
        thalamus_set_partition(gw->thalamus, THAL_BROCA,       broca_topo,       2);
        thalamus_set_partition(gw->thalamus, THAL_AMYGDALA,    amygdala_topo,    2);
        thalamus_set_partition(gw->thalamus, THAL_PERCEPTION,  perception_topo,  1); /* v0.5 */
        /* 小脑无专属拓扑（全局监控角色） */
    }

    // 绑定丘脑到脑干（脑干通过丘脑获取所有其他脑区的引用）
    brainstem_set_thalamus(gw->brainstem, gw->thalamus);

    /* v0.5.7: 注册大脑缓存到丘脑工具槽——brainstem 冻结/解冻
     * 通过 THAL_UTIL_NODE_CACHE 取用（此前从未注册，node_cache_freeze
     * 收到 NULL 空转，冻结机制实际未生效——假日志） */
    if (gw->brain_cache) {
        thalamus_register_utility(gw->thalamus, THAL_UTIL_NODE_CACHE, gw->brain_cache);
        /* v0.5.7: 注入 master——存盘时导出冻结边用 */
        gw->topology->node_cache = gw->brain_cache;
    }
    brainstem_set_verbose(gw->brainstem, 1);  /* 开启脑区日志 */

    printf("[gateway]   丘脑信号总线就绪 (%d 脑区, %d 工具槽)\n",
           THAL_SUBSYSTEM_COUNT, THAL_UTIL_COUNT);

    // 自主学习器
    {
        gw->self_learner = self_learner_create(gw->topology, NULL);
        if (gw->self_learner) {
            printf("[gateway]   自主学习器就绪\n");
            /* 通过丘洞注册，而非 brainstem_set_self_learner */
            thalamus_register_utility(gw->thalamus, THAL_UTIL_SELF_LEARNER, gw->self_learner);
        }
    }

    /* ── v0.5 视觉皮层脑区 ── */
    {
        VisualCortexConfig vcc = VISUAL_CORTEX_DEFAULT_CONFIG;
        vcc.verbose = 1;
        gw->visual_cortex = visual_cortex_create(gw->topology, &vcc);
        if (gw->visual_cortex) {
            /* 内部 MediaReader 也自动绑定丘脑 */
            MediaReader* mr = visual_cortex_get_media_reader(gw->visual_cortex);
            if (mr) media_reader_set_thalamus(mr, gw->thalamus);

            /* 注册为脑区: 脑干将通过丘脑调度 */
            thalamus_register_region(gw->thalamus, THAL_VISUAL_CORTEX, gw->visual_cortex);
            printf("[gateway]   视觉皮层脑区就绪 (v0.5, TOPO_VISUAL=%d)\n", TOPO_VISUAL);
        } else {
            fprintf(stderr, "[gateway] 视觉皮层创建失败，继续运行\n");
        }
    }

    /* C2 配套: 启动脑干前再查一次取消标志（这一步之后才开始有后台线程写拓扑） */
    if (gw->shutdown_requested) {
        fprintf(stderr, "[gateway] 初始化被取消（收到退出信号），提前结束\n");
        return -1;
    }

    /* 学习已由脑干统一调度 */
    brainstem_start(gw->brainstem);

    gw->start_time = time(NULL);
    gw->engine_ready = 1;  // 引擎初始化完成，可接受请求
    printf("[gateway] ===== 引擎就绪，开始接受请求 =====\n");

    // 训练模式: 引擎就绪后自动开始喂料
    printf("[DEBUG] train_mode_flag=%d, corpus=%s, topology=%p\n", gw->train_mode_flag, gw->train_config.corpus_path ? gw->train_config.corpus_path : "NULL", (void*)gw->topology);
    if (gw->train_mode_flag && gw->topology) {
        gw->train_mode = train_mode_create(gw->topology, gw->memory, gw->learner, gw->train_config);
        __sync_synchronize();  /* ARM 弱内存序: 确保 train_mode 对所有线程可见 */
        if (gw->train_mode) {
            train_mode_set_thalamus(gw->train_mode, gw->thalamus);
            /* v2.1 阶段0-A：注入 EmergentPOS 到训练模式，喂料路径累积 dist_sig[0..21]
             * （种子词可信标签）。不注入则喂料仅做 funcword_record_position（旧行为）。 */
            if (gw->prefrontal && gw->prefrontal->controller &&
                gw->prefrontal->controller->emergent_pos) {
                train_mode_set_emergent_pos(gw->train_mode,
                                            gw->prefrontal->controller->emergent_pos);
            }
            /* v0.5: 媒体格式训练需要 VisualCortex */
            if (gw->visual_cortex)
                train_mode_set_visual_cortex(gw->train_mode, gw->visual_cortex);
            train_mode_start(gw->train_mode);
        } else {
            fprintf(stderr, "[gateway] 训练模式创建失败\n");
        }
    }

    // 学习调度器（始终启动，后台自学习循环）
    {
        SchedulerConfig scfg = SCHEDULER_DEFAULT_CONFIG;
        if (gw->train_config.corpus_path)
            scfg.batch_corpus_path = gw->train_config.corpus_path;
        gw->scheduler = learning_scheduler_create(gw->topology, gw->memory,
                                                   gw->learner, &scfg);
        if (gw->scheduler) {
            learning_scheduler_start(gw->scheduler);
            printf("[gateway]   学习调度器已启动 (自学习=%d次/轮, 语料=%s)\n",
                   scfg.self_learn_cycles,
                   scfg.batch_corpus_path ? scfg.batch_corpus_path : "无(仅自学习)");
        }
    }

    // 脑区索引（9+1 脑区，词性涌现模块）
    {
        gw->topo_brain = topobrain_create(65536);  // 预分配 64K 节点
        if (gw->topo_brain) {
            printf("[gateway]   脑区索引就绪 (9+1 脑区)\n");
            /* 通过丘洞注册，而非 brainstem_set_topo_brain */
            thalamus_register_utility(gw->thalamus, THAL_UTIL_TOPO_BRAIN, gw->topo_brain);
        }
    }
    printf("[gateway] PivotMind 引擎就绪\n");

    return 0;
}

/* pthread_create 兼容包装 — 消除 cast-function-type 警告 */
void* gw_system_init_thread(void* arg) {
    gw_system_init((GatewaySystem*)arg);
    return NULL;
}

// ==================== 保存并关闭 ====================

void gw_system_shutdown(GatewaySystem* gw) {
    if (!gw) return;
    printf("[gateway] 正在关闭...\n");

    /* 0. v0.5.25 fix: 先停学习 worker（置 stop + join），再拆解任何资源。
     * 旧实现从不置 g_learn_q.stop、也不 join g_learn_workers：worker 循环持有
     * g_gw，消费任务时访问 gw->topology / gw->perception / gw->prefrontal，
     * 而下面会对这些对象 destroy → 后台 worker 踩已释放内存（use-after-free）。
     * 必须置于最前：此刻 gw 各对象仍全部有效，worker 可安全排空在队任务。 */
    learn_queue_shutdown();

    // 1. 停止脑干 → 冻结所有活性节点 → 确保状态完整
    if (gw->brainstem) brainstem_stop(gw->brainstem);

    /* 1b. P1-2: 存盘之前必须停掉**所有仍在写拓扑的后台线程**。
     * 旧版只停了学习 worker 与脑干，把调度器线程（learning_scheduler.c:385）
     * 和感知 worker（perception.c:271）留到存盘之后才 destroy →
     * master_save_state 遍历子拓扑期间 node_count/activation/边表被并发修改，
     * 存盘内容撕裂甚至越界（节点在遍历途中被回收）。
     * 前置条件：本函数此时不能有连接线程在跑（H2 已逐槽 join，见其自检）。 */
    if (gw->scheduler) {
        printf("[gateway]   停止学习调度器 (存盘前)...\n");
        learning_scheduler_stop(gw->scheduler);   /* 只停线程 + join，不销毁对象 */
    }
    if (gw->perception) {
        /* perception 只有 perception_destroy 内部才有"置停+join"（perception.c:283-294），
         * 所以这里提前销毁并置 NULL：后面第 6 步的 perception_destroy 会因 NULL 跳过，
         * 不会二次销毁。附带好处：destroy 尾部的 article_flush 会把这一批新词写进
         * 拓扑，正好被紧随其后的存盘保存下来。 */
        printf("[gateway]   停止感觉皮层 worker (存盘前)...\n");
        perception_destroy(gw->perception);
        gw->perception = NULL;
    }

    // 2. 保存完整状态到主文件（空启动保护：总节点 < 20 时跳过，防止覆盖有效存盘）
    if (gw->topology) {
        int total = master_count_total_nodes(gw->topology);
        if (total >= 20) {
            int saved = master_save_state(gw->topology, pm_file(PM_FILE_STATE));
            if (saved >= 0) printf("[gateway]   保存拓扑状态: %d 节点\n", saved);
            int feat_saved = save_features(gw->topology, pm_file(PM_FILE_FEATURES));
            if (feat_saved > 0) printf("[gateway]   保存特征: %d 节点\n", feat_saved);
            int cross_saved = save_cross_edges(gw->topology, pm_file(PM_FILE_CROSS_EDGES));
            if (cross_saved > 0) printf("[gateway]   保存跨拓扑连接: %d 条\n", cross_saved);
        } else {
            printf("[gateway]   跳过存盘 (总节点=%d < 门卫阈值 20，避免覆盖有效数据)\n", total);
        }
    }
    if (gw->memory) {
        /* D1 门卫：与拓扑 total>=20 门卫对等。种子真正的"内容"是 LTM 条目数，
         * 故门卫不看字节、看条数（阈值 MEMORY_SEED_MIN_ENTRIES=1，见下）：
         *   ① 本次加载失败 且 既有文件有记录 → 绝不覆盖（核心保命线）；
         *   ② 本次 0 条 且 既有文件有记录   → 绝不覆盖（空状态不得顶掉有效种子）。
         * 其余情况（首次启动 / 既有文件本无数据 / 正常有内容）照常保存。 */
        int ltm_count = gw->memory->permanent_memory
                        ? gw->memory->permanent_memory->size : 0;
        const int MEMORY_SEED_MIN_ENTRIES = 1;   /* 门卫下限：条目数 < 此值即视为空状态 */
        int would_destroy = g_memory_seed_had_data &&
                            (g_memory_seed_load_ok == 0 ||
                             ltm_count < MEMORY_SEED_MIN_ENTRIES);
        if (would_destroy) {
            fprintf(stderr, "[gateway]   ⚠ 跳过保存记忆种子: 加载失败或仅 %d 条，"
                    "拒绝覆盖既有有效种子\n", ltm_count);
        } else {
            int saved = memory_save_seed(gw->memory, pm_file(PM_FILE_MEMORY_SEED));
            if (saved >= 0) printf("[gateway]   保存记忆种子: %d 条\n", saved);
            else fprintf(stderr, "[gateway]   ⚠ 保存记忆种子失败 (返回 %d)\n", saved);
        }
    }

    // 3. 删除临时状态文件（brain_state.dat 只是脑干运行缓存，主状态已在上面保存）
    remove(pm_file(PM_FILE_BRAIN_CACHE));
    printf("[gateway]   清理临时状态文件\n");

    // 4. 销毁学习调度器（线程已在 1b 停掉并 join，这里只释放对象；
    //    destroy 内部会再调一次 stop，已由 H7 改成幂等）
    if (gw->scheduler) {
        printf("[gateway]   销毁学习调度器...\n");
        learning_scheduler_destroy(gw->scheduler);
        gw->scheduler = NULL;
    }

    // 5. 脑区索引
    if (gw->topo_brain) {
        topobrain_destroy(gw->topo_brain);
        gw->topo_brain = NULL;
    }

    // 6. 销毁资源（brainstem 已在上方 stop，这里只 destroy）
    /* v0.5 视觉皮层脑区 (内部自动销毁 MediaReader) */
    if (gw->visual_cortex)  { visual_cortex_destroy(gw->visual_cortex); gw->visual_cortex = NULL; }
    if (gw->brain_cache) { node_cache_destroy(gw->brain_cache); gw->brain_cache = NULL; }
    if (gw->self_learner) { self_learner_destroy(gw->self_learner); gw->self_learner = NULL; }
    /* P2-7: 每一项销毁后立刻置 NULL —— 旧版大部分保留野指针，一旦
     * gw_system_shutdown 被调用第二次（或与仍在跑的 init 线程交错）就是
     * 双重释放。置 NULL 后二次调用是安全的空操作。 */
    if (gw->amygdala)    { amygdala_destroy(gw->amygdala);       gw->amygdala = NULL; }
    if (gw->hippocampus) { hippocampus_destroy(gw->hippocampus); gw->hippocampus = NULL; }
    if (gw->cerebellum)  { cerebellum_destroy(gw->cerebellum);   gw->cerebellum = NULL; }
    if (gw->qa_memory)   { qa_memory_destroy(gw->qa_memory);     gw->qa_memory = NULL; }
    if (gw->thalamus)    { thalamus_destroy(gw->thalamus);       gw->thalamus = NULL; }
    if (gw->perception)  { perception_destroy(gw->perception);   gw->perception = NULL; }
    web_fetch_destroy();  /* 爬虫框架 */
    if (gw->brainstem)   { brainstem_destroy(gw->brainstem);     gw->brainstem = NULL; }
    if (gw->pfe)         { pfe_destroy(gw->pfe);                 gw->pfe = NULL; }       /* v0.3 */
    if (gw->arena)       { idea_arena_destroy(gw->arena);        gw->arena = NULL; }     /* v0.3 */
    if (gw->hypothalamus){ hypothalamus_destroy(gw->hypothalamus); gw->hypothalamus = NULL; } /* v0.4 */
    if (gw->broca)       { broca_destroy(gw->broca);             gw->broca = NULL; }     /* v0.4 */
    if (gw->learner)     { active_learner_destroy(gw->learner);  gw->learner = NULL; }
    if (gw->prefrontal)  { prefrontal_destroy(gw->prefrontal);   gw->prefrontal = NULL; }
    gw->dialog = NULL;        /* 别名（prefrontal->dialog），已随 prefrontal 释放 */
    if (gw->causal_graph){ causal_graph_destroy(gw->causal_graph); gw->causal_graph = NULL; }
    if (gw->topology)    { master_topology_destroy(gw->topology); gw->topology = NULL; }
    if (gw->memory)      { memory_system_destroy(gw->memory);     gw->memory = NULL; }
    if (gw->config)      { config_destroy(gw->config);            gw->config = NULL; }

    printf("[gateway] 已关闭 (运行 %lld 秒, 对话 %lld 轮)\n",
           (long long)(time(NULL) - gw->start_time), (long long)gw->total_dialogs);
}

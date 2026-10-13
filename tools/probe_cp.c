/**
 * probe_cp.c — 字对表读数器 + 左右熵（只读，串键 v2）
 *
 * 判据演进（每一版都由数据判定，见 CX-13）：
 *   v1 双向往返相对强度>=3.0  → 实测全表最大 2.53，永不达成（判据与数据不符）
 *   v2 次数>=20 且 PMI>=3.0   → 真词进来，但跨词边界碎片（们学/果如）也进来
 *   v3 加左右熵：真词左邻/右邻分布散（熵高），碎片被单一上下文钉死（熵低）
 *
 * 左右熵定义（纯计数）：
 *   H_left(A,B)  = -Σ_X p(X|A) log2 p(X|A)，X 取所有"X 在 A 前"的字
 *   H_right(A,B) = -Σ_Y p(Y|B) log2 p(Y|B)，Y 取所有"B 在 Y 前"的字
 *
 * 用法: probe_cp [top_n] [关键词] [最小左右熵]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pivotmind_paths.h"

#define CP_MAGIC 0x504D4350u
#define MAXENT   200000

typedef struct { char a[8]; char b[8]; unsigned int c; } Ent;
static Ent* E; static int NE = 0;
static char* USED;

typedef struct { char k[8]; unsigned long long v; } Sum;
static Sum* SUM; static int NSUM = 0, CAPSUM = 0;

static unsigned long long sum_of(const char* a) {
    for (int i = 0; i < NSUM; i++) if (strcmp(SUM[i].k, a) == 0) return SUM[i].v;
    return 0;
}
static void sum_add(const char* a, unsigned int c) {
    for (int i = 0; i < NSUM; i++) if (strcmp(SUM[i].k, a) == 0) { SUM[i].v += c; return; }
    if (NSUM >= CAPSUM) { CAPSUM = CAPSUM ? CAPSUM * 2 : 4096; SUM = (Sum*)realloc(SUM, sizeof(Sum) * (size_t)CAPSUM); }
    snprintf(SUM[NSUM].k, 8, "%s", a);
    SUM[NSUM].v = c; NSUM++;
}
static int is_cjk3(const char* s) {   /* 按码点判：U+4E00..U+9FFF 才算中文单字 */
    if (!s) return 0;
    size_t n = strlen(s);
    if (n != 3) return 0;
    unsigned cp = (((unsigned char)s[0] & 0x0F) << 12) |
                  (((unsigned char)s[1] & 0x3F) << 6) |
                   ((unsigned char)s[2] & 0x3F);
    return cp >= 0x4E00 && cp <= 0x9FFF;
}

static unsigned int rev_lookup(const char* a, const char* b) {
    for (int i = 0; i < NE; i++)
        if (E[i].c && strcmp(E[i].a, b) == 0 && strcmp(E[i].b, a) == 0) return E[i].c;
    return 0;
}
/* 左熵：所有 (X, A) 的计数分布熵 */
static double h_left(const char* A) {
    unsigned long long tot = 0;
    for (int i = 0; i < NE; i++) if (E[i].c && is_cjk3(E[i].a) && strcmp(E[i].b, A) == 0) tot += E[i].c;
    if (!tot) return 0.0;
    double h = 0;
    for (int i = 0; i < NE; i++) {
        if (!E[i].c || !is_cjk3(E[i].a) || strcmp(E[i].b, A) != 0) continue;
        double p = (double)E[i].c / (double)tot;
        h -= p * log2(p);
    }
    return h;
}
/* 右熵：所有 (B, Y) 的计数分布熵 */
static double h_right(const char* B) {
    unsigned long long tot = 0;
    for (int i = 0; i < NE; i++) if (E[i].c && is_cjk3(E[i].b) && strcmp(E[i].a, B) == 0) tot += E[i].c;
    if (!tot) return 0.0;
    double h = 0;
    for (int i = 0; i < NE; i++) {
        if (!E[i].c || !is_cjk3(E[i].b) || strcmp(E[i].a, B) != 0) continue;
        double p = (double)E[i].c / (double)tot;
        h -= p * log2(p);
    }
    return h;
}

int main(int argc, char** argv) {
    int topn = (argc > 1) ? atoi(argv[1]) : 30;
    const char* filt = (argc > 2 && argv[2][0]) ? argv[2] : NULL;
    double minh = (argc > 3) ? atof(argv[3]) : 0.0;

    const char* path = pm_file(PM_FILE_CHARPAIRS);
    printf("字对表: %s\n", path ? path : "(null)");
    if (!path) return 1;
    FILE* fp = fopen(path, "rb");
    if (!fp) { printf("× 打不开\n"); return 1; }
    unsigned int hdr[3];
    if (fread(hdr, 4, 3, fp) != 3 || hdr[0] != CP_MAGIC) { printf("× 头无效（旧格式？）\n"); fclose(fp); return 1; }
    unsigned int n = hdr[2];
    E = (Ent*)calloc(n ? n : 1, sizeof(Ent));
    USED = (char*)calloc(n ? n : 1, 1);
    for (unsigned int i = 0; i < n && NE < MAXENT; i++) {
        unsigned char la = 0, lb = 0;
        if (fread(&la, 1, 1, fp) != 1 || la >= 8) break;
        if (fread(E[NE].a, 1, la, fp) != la) break; E[NE].a[la] = '\0';
        if (fread(&lb, 1, 1, fp) != 1 || lb >= 8) break;
        if (fread(E[NE].b, 1, lb, fp) != lb) break; E[NE].b[lb] = '\0';
        if (fread(&E[NE].c, 4, 1, fp) != 1) break;
        NE++;
    }
    fclose(fp);
    unsigned long long total = 0;
    for (int i = 0; i < NE; i++) { sum_add(E[i].a, E[i].c); total += E[i].c; }
    printf("✓ %d 条 · 总计数 %llu · 串键（无需状态文件）\n", NE, total);
    printf("判据：次数>=20 且 PMI>=3.0 且 min(左熵,右熵)>=%.2f\n\n", minh);
    printf("  %-8s %6s %8s %7s %7s %7s  %s\n", "字对", "次数", "PMI", "左熵", "右熵", "min", "过判据");

    int shown = 0, pass = 0;
    for (int rank = 0; rank < NE; rank++) {
        int bi = -1; unsigned int bc = 0;
        for (int i = 0; i < NE; i++) if (!USED[i] && E[i].c > bc) { bc = E[i].c; bi = i; }
        if (bi < 0) break;
        USED[bi] = 1;
        const char* ea = E[bi].a; const char* eb = E[bi].b; unsigned int ec = E[bi].c;
        unsigned long long oa = sum_of(ea), ob = sum_of(eb);
        unsigned int rc = rev_lookup(ea, eb);
        double pmi = (total && oa && ob) ? log2(((double)ec / (double)total) /
                     (((double)oa / (double)total) * ((double)ob / (double)total))) : 0.0;
        double hl = h_left(ea), hr = h_right(eb), mn = hl < hr ? hl : hr;
        if (!is_cjk3(ea) || !is_cjk3(eb)) continue;   /* 标点/非中文单字不参与成词 */
        int ok = (ec >= 20) && (pmi >= 3.0) && (mn >= minh);
        if (ok) pass++;
        if (filt && !strstr(ea, filt) && !strstr(eb, filt)) continue;
        if (shown < topn) {
            char w[16]; snprintf(w, sizeof(w), "%s%s", ea, eb);
            printf("  %-10s %5u %8.2f %7.2f %7.2f %7.2f  %s\n", w, ec, pmi, hl, hr, mn, ok ? "YES" : "");
            shown++;
        }
    }
    printf("\n⇒ 过判据 = %d 个（阈值 min熵>=%.2f）\n", pass, minh);
    free(SUM); free(E); free(USED);
    return 0;
}

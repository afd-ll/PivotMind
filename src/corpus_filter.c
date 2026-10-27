/**
 * @file corpus_filter.c
 * @brief 见 include/corpus_filter.h —— 繁体 / 文言 / 无实义 的入口过滤
 *
 * 繁体表口径：只收「繁体写法与简体不同、且在繁体文本里常见」的字。
 * 同一个字简繁同形（如「中」「人」）绝不入表 —— 那会把正常简体语料误杀。
 */

#include "corpus_filter.h"
#include "word_prob.h"
#include "error.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

/* 常用繁体字（用空格分隔的 3 字节 UTF-8 单字；仅收简体写法不同的） */
static const char TRAD_CHARS[] =
    "們 這 說 時 會 來 對 開 關 學 國 語 麼 樣 裡 為 與 從 過 還 讓 點 電 腦 網 絡 "
    "車 馬 長 門 問 間 陽 陰 龍 鳳 愛 體 經 書 買 賣 聽 話 見 識 記 設 計 機 業 務 "
    "東 兩 嚴 舉 義 習 鄉 爭 於 雲 亞 產 親 僅 儀 價 眾 優 傳 傷 倫 偽 餘 側 偵 債 "
    "兒 蘭 興 養 獸 內 冊 寫 軍 農 決 況 凍 淨 減 幾 凱 擊 劃 劉 則 剛 創 刪 別 劍 "
    "劇 勸 辦 動 勢 匯 區 醫 華 協 單 廠 廳 歷 壓 參 雙 發 變 敘 疊 葉 號 嘆 嚇 嗎 "
    "啟 團 園 圍 圖 圓 聖 場 壞 塊 堅 壇 墳 壘 壯 聲 殼 處 備 復 夠 頭 誇 奪 奮 獎 "
    "妝 婦 媽 孫 寧 寶 實 寵 審 憲 宮 寬 賓 尋 導 屬 島 峽 崗 嶺 幣 帥 師 帳 帶 幫 "
    "幹 廣 莊 慶 廟 廢 異 棄 張 彎 彈 強 歸 當 錄 徹 徑 徵 憶 憂 懷 態 總 戀 應 懶 "
    "懸 懼 慘 戲 戶 執 擴 掃 揚 擾 撫 搶 護 報 擔 擬 擁 撥 擇 掛 採 揮 損 換 據 擠 "
    "攝 擺 搖 攤 撐 撒 擋 敗 敵 數 齋 鬥 斬 斷 無 舊 曠 晝 顯 曬 曉 暈 暢 曆 極 構 "
    "槍 標 樞 樹 橋 橫 檢 櫃 檔 權 歡 歐 歲 殘 殺 毀 氣 漢 湯 溝 沒 淪 滅 滿 漁 漲 "
    "漸 潔 濁 濃 濕 濟 濤 濫 濱 灣 災 煙 煩 熱 燈 燒 燭 爐 爺 牆 狀 獨 獄 獅 獲 猶 "
    "獻 環 瑪 畫 療 瘋 癢 癡 盡 監 盤 確 礙 礎 礦 碼 禮 禍 禪 離 種 積 稱 穩 窮 竊 "
    "競 筆 節 範 築 簡 籃 籌 簽 類 糧 糾 紀 約 紅 紋 納 紙 級 紛 緊 緒 線 練 組 細 "
    "終 結 絕 統 絲 綠 維 罰 罷 羅 聯 職 肅 脅 脈 腳 脫 臉 臨 艙 藝 蘇 虛 蟲 補 裝 "
    "複 襲 規 視 覺 觀 訂 認 討 訓 議 訊 講 許 論 訪 證 評 譯 讀 讚 豐 豬 貓 貝 負 "
    "財 貢 貧 貨 販 貪 責 貴 貸 費 貼 貿 賀 資 賊 賜 賞 賠 賢 賤 賦 質 賭 賴 購 賽 "
    "贊 贈 趕 跡 踐 蹤 軌 軟 軸 較 載 輔 輕 輛 輝 輩 輪 輯 輸 轉 轟 辭 邊 遼 達 遷 "
    "運 進 遠 違 連 週 逛 逢 遲 遺 適 選 鄧 鄭 鄰 醜 釋 針 釘 鈔 鈴 銀 銅 銳 銷 鋪 "
    "錢 錦 錯 鍋 鍵 鍾 鐘 鐵 鑄 鑑 閃 閉 閣 閥 閱 闊 陣 階 陸 隊 隨 險 隱 難 雛 雜 "
    "雞 霧 靈 靜 響 頁 頂 項 順 須 預 頑 頒 頓 頗 領 頸 頻 顆 題 額 顏 願 顧 飛 飯 "
    "飲 飽 飾 餅 餐 館 駐 駕 騎 驗 騙 驚 髮 鬆 鬧 魚 鮮 鳥 鳴 鴨 鷹 麥 麵 黃 黨 齊 "
    "齒 龜 隻 臺 陳";

#define CF_TBL_SIZE 4096
#define CF_MAX_SAMPLES 32
#define CF_MAX_LINE   4096   /* 字节上限：心理语料答案是长段（800 汉字≈2.4KB），512 会误杀 */

static char g_tbl[CF_TBL_SIZE][4];        /* 3 字节码 + NUL */
static int  g_tbl_init = 0;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static unsigned long long g_stats[5] = {0, 0, 0, 0, 0};
static char g_samples[CF_MAX_SAMPLES][64];
static int  g_sample_n = 0;

static unsigned int _h3(const char* k) {
    unsigned int h = 2166136261u;
    for (int i = 0; i < 3; i++) { h ^= (unsigned char)k[i]; h *= 16777619u; }
    return h;
}

static void _ins(const char* k) {
    unsigned int h = _h3(k) & (CF_TBL_SIZE - 1);
    for (int i = 0; i < CF_TBL_SIZE; i++) {
        int j = (int)((h + (unsigned int)i) & (CF_TBL_SIZE - 1));
        if (!g_tbl[j][0]) { memcpy(g_tbl[j], k, 3); g_tbl[j][3] = 0; return; }
        if (memcmp(g_tbl[j], k, 3) == 0) return;
    }
}

static void _build(void) {
    const char* p = TRAD_CHARS;
    while (*p) {
        if ((unsigned char)p[0] == ' ') { p++; continue; }
        if ((unsigned char)p[0] < 0x80) { p++; continue; }
        _ins(p);
        p += 3;
    }
    g_tbl_init = 1;
}

int cf_is_trad_char(const char* ch3) {
    if (!ch3 || strncmp(ch3, "", 1) == 0) return 0;
    if ((unsigned char)ch3[0] < 0x80) return 0;
    pthread_once(&g_once, _build);
    if (!g_tbl_init) return 0;
    unsigned int h = _h3(ch3) & (CF_TBL_SIZE - 1);
    for (int i = 0; i < CF_TBL_SIZE; i++) {
        int j = (int)((h + (unsigned int)i) & (CF_TBL_SIZE - 1));
        if (!g_tbl[j][0]) return 0;
        if (memcmp(g_tbl[j], ch3, 3) == 0) return 1;
    }
    return 0;
}

void cf_stats(unsigned long long out[5]) {
    if (!out) return;
    for (int i = 0; i < 5; i++) out[i] = g_stats[i];
}

void cf_dump_samples(int n) {
    int cnt = g_sample_n < n ? g_sample_n : n;
    for (int i = 0; i < cnt; i++)
        LOG_INFO("[语料过滤] 拒收样本: %s", g_samples[i]);
}

static void _sample(const char* text) {
    if (g_sample_n >= CF_MAX_SAMPLES) return;
    snprintf(g_samples[g_sample_n], sizeof(g_samples[0]), "%.60s", text ? text : "");
    g_sample_n++;
}

/* 统计：繁体命中 / 文言标记 / 汉字数 */
static const char* const k_classical_marks[9] = {"之", "乎", "者", "也", "矣", "焉", "哉", "兮", "曰"};

static int _scan(const char* text, int* trad_hit, int* classical) {
    *trad_hit = 0;
    *classical = 0;
    int hanzi = 0;
    const char* p = text;
    while (p && *p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) { p++; continue; }
        int len = (c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 0));
        if (len != 3) { p += (len ? len : 1); continue; }
        unsigned int cp = ((unsigned int)(p[0] & 0x0F) << 12)
                        | ((unsigned int)(p[1] & 0x3F) << 6)
                        |  (unsigned int)(p[2] & 0x3F);
        if (cp >= 0x4E00 && cp <= 0x9FFF) {
            hanzi++;
            if (!*trad_hit && cf_is_trad_char(p)) *trad_hit = 1;
            for (int k = 0; k < 9; k++)
                if (memcmp(p, k_classical_marks[k], 3) == 0) { (*classical)++; break; }
        }
        p += 3;
    }
    return hanzi;
}

int cf_accept_text(const char* text, int* why) {
    int r = CF_OK;
    if (!text || !text[0]) {
        r = CF_NO_CONTENT;
    } else if (strlen(text) > CF_MAX_LINE) {
        r = CF_TOO_LONG;
    } else if (!wp_text_has_content(text)) {
        r = CF_NO_CONTENT;
    } else {
        int trad = 0, cls = 0;
        int hanzi = _scan(text, &trad, &cls);
        if (trad) r = CF_TRAD;
        else if (hanzi >= 10 && cls >= 3 && cls * 100 / hanzi >= 8) r = CF_CLASSICAL;
    }
    if (r) { g_stats[r]++; _sample(text); }
    if (why) *why = r;
    return r == CF_OK;
}

int cf_accept_qa(const char* question, const char* answer, int* why) {
    int w1 = 0, w2 = 0;
    int ok1 = cf_accept_text(question, &w1);
    int ok2 = cf_accept_text(answer, &w2);
    int r = w1 ? w1 : w2;
    if (why) *why = r;
    return ok1 && ok2;
}

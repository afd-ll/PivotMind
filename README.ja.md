<div align="center">

# 玄枢 PivotMind

### 脳に着想を得た意味連想エンジン
**ピュアC · AIフレームワーク依存ゼロ · ARM組み込みボードで動作**

[English](README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Русский](README.ru.md)

[![Version](https://img.shields.io/badge/version-v0.4.8-blue.svg)](changelogs/)
[![License](https://img.shields.io/badge/license-Apache%202.0-green.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C-99%2B-orange.svg)](https://en.wikipedia.org/wiki/C99)
[![Platform](https://img.shields.io/badge/ARM-RK3399%20%7C%20x86__64-lightgrey.svg)](#running-on)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-success.svg)](#quick-start)

> 知能とは行列演算の積み重ねではなく、
> 推論ネットワークを伝播する活性化の波紋である。

</div>

---

## PivotMind とは

PivotMind は [TraceWisdomNetwork](#tracewisdomnetwork) +
[Hebbian Learning](#core-mechanisms) +
[Multi-Layer Diffusion Reasoning](#multi-layer-diffusion-engine)
の上に構築された**脳に着想を得た認知エンジン**です。
Transformer なし。埋め込みベクトルなし。誤差逆伝播なし。
あるのはノードとエッジ、活性化と減衰だけ — 絶え間なく刻む背景クロックによって駆動されます。

**現行バージョン: v0.4.8** — 13 の脳領域/サブシステム（全実装済み）、創発的品詞システム、並列マルチ学習器、PFE 推論、512 次元特徴ベクトル。

**コード規模: 85 ソースファイル(~48,600 行C) + 89 ヘッダー(~12,600 行) + ツール/テスト/デモ(~13,000 行) = 約 74,000 行。**

### TraceWisdomNetwork

各概念はノードです。同時出現がエッジを生成します。エッジは三重属性を保持します:
**重み × 信頼度 × 動機バイアス**。
11 個のサブトポロジ(語彙 / 意味 / 感情 / 構文 / 文脈 / 領域 / 語用論 / 文化 / 概念 / マスター / テンプレート)
はそれぞれ独立した推論ネットワークを形成し、O(1) 隣接リストによるクロストポロジリンクで相互接続されます。
活性化は層を跨いで同時に拡散し、競合によって勝者が出力として選択されます。

### なぜこのアプローチか

| 従来の LLM             | PivotMind                                                  |
|------------------------|------------------------------------------------------------|
| トークン予測、ステートレス | ノード活性化、持続的な内部状態                          |
| 勾配ベースのオフライン一括学習 | Hebbian オンライン + Skip-gram 事前学習             |
| 単一の埋め込み空間     | 11 個の独立したサブトポロジ + 512次元特徴ベクトル         |
| ニューラルネットのブラックボックス | 明示的なノード-エッジ経路、完全追跡可能         |
| GPU + 大容量 VRAM 必須 | pthread + OpenMP のみ、ARM 組み込みで動作              |
| 推論と学習が分離       | 会話がそのまま学習になる                                  |
| 生理的認識なし         | 内受容的自己モニタリング、3 段階ヘルス応答               |
| 訓練後は凍結           | 24時間365日の継続的バックグラウンド学習                    |

---

## 脳領域アーキテクチャ

PivotMind は哺乳類の皮質機能区分をモデル化します — 13 の脳領域/サブシステムがそれぞれ専任の責務を持ち、Thalamus 信号バスを介して通信します。**全13領域が完全実装済みで、スタブコードはゼロです。**

```
                          ┌──────────────────────┐
                          │   Prefrontal Cortex    │ ← 対話 / 意思決定エントリ
                          │  + Prefrontal Exec PFE │ ← 6モード推論オーケストレータ
                          └──────────┬───────────┘
                                     │ Signal Bus
        ┌────────┬────────┬─────────┼─────────┬────────┬────────┬────────┐
        ▼        ▼        ▼         ▼         ▼        ▼        ▼        ▼
   ┌────────┐┌──────┐┌──────┐┌──────────┐┌──────┐┌──────┐┌──────┐┌──────────┐
   │Hippo-  ││ DMN  ││Amyg- ││ Perception││Broca ││Cere- ││Brain-││Hypothal- │
   │campus  ││      ││dala  ││  Cortex   ││      ││bellum││stem  ││  amus    │
   │Memory  ││Dream ││Emo-  ││  Web     ││Template││BPTT  ││Circ- ││ Drives   │
   │Consol. ││      ││tion  ││  Search  ││Builder││Tuner ││adian ││          │
   └────────┘└──────┘└──────┘└──────────┘└──────┘└──────┘└──────┘└──────────┘
                                     │
                          ┌──────────┴──────────┐
                          │    Thalamus           │ ← Signal Bus + Resource Gate
                          └─────────────────────┘
```

| 脳領域                  | ファイル                     | 行数  | 機能                                                                |
|------------------------|----------------------------|------|---------------------------------------------------------------------|
| **Prefrontal**         | `prefrontal.c`             | 132  | 対話生成、拡散 → ACC 適応ゲーティング                               |
| **Prefrontal Exec**    | `prefrontal_executive.c`   | 1,502| 6 モード推論オーケストレーション、タスク分解、競合解決                   |
| **Hippocampus**        | `hippocampus.c`            | 135  | 記憶定着、QA リプレイ、知覚連動                                     |
| **DMN**                | `dmn.c`                    | 46   | Default Mode Network: 夢想的連想、アイドル探索                      |
| **Amygdala**           | `amygdala.c`               | 97   | 感情価サンプリング、探索 / 利用バランス                              |
| **Perception Cortex**  | `perception.c`             | 838  | Web 検索(Sogou+Bing デュアルプロバイダ)、article_reader パイプライン|
| **Broca's Area**       | `broca.c`                  | 56   | テンプレート自動構築と減衰スケジューリング                          |
| **Cerebellum**         | `cerebellum.c`             | 80   | BPTT 微調整、CPU/メモリ資源保護                                     |
| **Hypothalamus**       | `hypothalamus.c`           | 149  | 4次元ドライブ動力学(好奇/取得/社交/快適)、概日リズム連動            |
| **Thalamus**           | `thalamus.c`               | 540  | 信号バス、リソースゲーティング、領域間ルーティング、ツールスロット    |
| **Brainstem**          | `brainstem.c`              | 613  | 概日ハートビート、活性化減衰、自発的活性化、ヒープ監視               |
| **Cingulate (ACC)**    | `cingulate.c`              | 223  | 4 次元シーケンス評価(意味 + テンプレート + 感情 + 長さ)             |
| **IdeaArena**          | `idea_arena.c`             | 722  | 複数候補 5 次元競合選択、側方抑制、ドーパミン変調                   |
| **Reticular**          | `reticular.c`              | 133  | 覚醒/覚醒度調節                                                      |

---

## コアメカニズム

### 多層拡散エンジン

入力はスライディングウィンドウでトークン化され、層を跨いで同時に拡散します:

- **Vocabulary** — 直接的なリテラルマッチング、高速想起
- **Semantic** — 11 サブトポロジを跨ぐクロストポロジ連想
- **Template** — 構文パターン認識、コネクタ挿入のガイド
- **Emotion** — 価数 × 覚醒度の重み付け、候補優先度の調整

**v0.4.8 改善**: 機能語フィルタリング — `is_function_word()` が~130 語の中英機能語をチェックし、パイプラインの3段階（アクティブセット更新、重み付きスコアリング、出力）でフィルタリングして、高接続性の機能語が出力を支配するのを防止します。側方抑制により内容語の多様性を確保します。

### 推論オーケストレーション (PFE)

Prefrontal Executive は自動的に質問の複雑さを評価し、6 つの推論モードのいずれかにマッチングします:

| モード       | トリガーキーワード          | 戦略                                       |
|--------------|----------------------------|--------------------------------------------|
| DIRECT       | default                    | 単一拡散連想                               |
| DECOMPOSE    | why / because              | 定義 → 因果 → 統合                         |
| COMPARE      | compare / difference       | 属性抽出 → 対比                             |
| HOWTO        | how to                     | 前提条件 → ステップシーケンス              |
| ABDUCE       | what if / assume           | ベースライン → 連鎖反応                    |
| ANALOGY      | analogy / similar          | 構造マッピング                             |

サブゴールは再帰的に分解されます(深さは設定可能)。競合検出 + IdeaArena 5D競合により最適経路を選択し、説明可能な推論チェーンを生成します。戦略重みは EMA 自己学習 + 永続化をサポートします。

### 創発的品詞システム (Emergent POS) **NEW v0.4.3** *(英中版の詳細を参照)*

人間は単語クラスごとに3〜5個の「シードアンカー」のみを提供します(~50語、中英各セット)。システムはシード単語の512次元Hebbian特徴ベクトルからアンカー重心を初期化します。実行時、新出単語はコサイン類似度で最も近いクラスに分類され、EMAで重心が微調整されます。未分類プールが10語を超えると新しい単語クラスが創発的に出現します。

### 内受容的自己モニタリング

RSS メモリ、接続増加率、推論遅延を継続的に監視し、3 段階応答を行います:

| レベル         | 状態        | アクション                                                     |
|---------------|------------|----------------------------------------------------------------|
| 🟢 GREEN      | 正常        | 通常動作                                                       |
| 🟡 YELLOW     | 警告        | ログ警告 + 学習閾値を上昇                                      |
| 🔴 RED        | 危険        | 緊急保存 + 弱いエッジを一括剪定                                |

---

## 学習システム *(英中版の詳細を参照)*

### 事前学習システム

Skip-gram / CBOW 単語埋め込み事前学習(1,624行): 動的窓サイズ、ネガティブサンプリング、モーメンタム加速、勾配クリッピング、学習率スケジューリング。

### 学習器マトリクス

| 学習器 | 方式 | 説明 |
|--------|------|------|
| **Autonomic** | Hebbian オンライン | 共起強化、16シャード並列更新 |
| **Active** | 24/7 バックグラウンド | 知識自動取得、概念関係分析 |
| **Self** | 好奇心駆動 | 好奇心サンプリング → 深層歩行 → 知識レビュー → 自己修正 |
| **BPTT** | 時系列逆伝播 | RNN + Linear、Adam 最適化 |

### 破局的忘却防止

EWC（弾性重み統合）に基づく: Fisher情報行列でパラメータ重要度をマークし、新規学習時に既存知識を選択的に保護。

---

## ニューラルネットワークサブシステム *(英中版の詳細を参照)*

**テンソル**: 多次元テンソル作成/破棄/ブロードキャスト/クローン/ビュー  
**レイヤー**: 8種(LINEAR/RELU/SIGMOID/TANH/SOFTMAX/DROPOUT/EMBEDDING/SIMPLE_RNN)  
**LSTM** (713行): 完全LSTM、双方向、レイヤー正規化  
**GRU** (621行): 完全GRU、更新/リセットゲート、双方向  
**モデル**: 多層スタッキング、フォワードパス、MSE損失、直列化  
**トレーナー**: ミニバッチ訓練、学習率スケジューリング  
**最適化器**: SGD / Adam(β1=0.9, β2=0.999) / RMSprop  
**量子化**: FP16/INT8/INT4/INT2 精度削減  
**剪定**: MAGNITUDE/RANDOM/GRADIENT/STRUCTURED 戦略  
**アテンション**: Bahdanau/Luong/自己注意/マルチヘッド注意

---

## クイックスタート

### ビルド

```bash
# GCC + pthread + OpenMP が必要(libcurl + openssl 必要、他は依存ゼロ)
make all

# ARM クロスコンパイル
make CC=aarch64-linux-gnu-gcc all

# デバッグビルド(ASAN アドレス/UB 検出付き)
make asan

# 全ユニットテスト実行
make test
```

### 実行

```bash
# インタラクティブゲートウェイ(推奨)
./build/bin/pivotmind_gateway

# CLI インタラクティブモード
./build/bin/digital_life
```

ゲートウェイはデフォルトで `:8080` でリッスンします(自動更新JS付きHTMLダッシュボード)。

### API 例

```bash
# 質問する
curl -X POST http://localhost:8080/chat \
  -H "Content-Type: application/json" \
  -d '{"query":"What is consciousness?"}'

# 学習素材を与える
curl -X POST http://localhost:8080/learn \
  -H "Content-Type: application/json" \
  -d '{"text":"Consciousness is the subjective experience produced by neural networks."}'

# 状態確認
curl http://localhost:8080/status

# ヘルスチェック
curl http://localhost:8080/health
```

### ビルドターゲット

| コマンド              | 説明                                          |
|----------------------|-----------------------------------------------|
| `make all`           | 全ターゲットをビルド                          |
| `make gateway`       | HTTP ゲートウェイのみビルド                   |
| `make digital-life`  | CLI インタラクティブ版をビルド                |
| `make seed-builder`  | シードトポロジツールをビルド                  |
| `make debug-seed`    | デバッグ用シードツールをビルド                |
| `make batch-learn`   | バッチ学習ツール                              |
| `make corpus-train`  | コーパス訓練ツール                            |
| `make template-build`| テンプレート構築ツール                        |
| `make test-dialog`   | 対話テストツール                              |
| `make clean`         | ビルド成果物を削除                            |
| `make test`          | 全ユニットテスト実行                          |

---

## プロジェクト構成

```
pivotmind/
├── src/               # 85 のコアソースファイル(~48,600 行 C)
├── include/           # 89 のヘッダファイル(~12,600 行)
├── demos/             # ゲートウェイとインタラクティブエントリ
├── tools/             # 57 ツール(訓練/デバッグ/データ処理/コーパスDL)
├── tests/             # ユニットテスト + 統合テスト + フィクスチャ
├── scripts/           # 自動化スクリプト(フィード、知識DL等)
├── changelogs/        # 44 バージョン変更履歴(000-043)
├── docs/              # アーキテクチャドキュメントと図
├── data/              # ランタイムデータ(hermes 知識ベース 25MB等)
└── libs/              # サードパーティライブラリ
```

---

## バージョン履歴

| バージョン   | 主な内容                                                                                |
|-------------|-----------------------------------------------------------------------------------------|
| v0.1.x      | 基本ウォーク推論、競合キュー、状態永続化                                                |
| v0.2.x      | 多層拡散、Hippocampus / DMN / Perception、内受容的モニタリング                          |
| **v0.3.0**  | Prefrontal Executive(6 モード推論)、IdeaArena 5D、戦略重み自己学習                      |
| **v0.4.0**  | コード簡素化、脳領域境界修正、Broca アップグレード、Hypothalamus/Thalamus/Brainstem 新設 |
| **v0.4.1**  | Web fetch リファクタ(libcurl)、Bing/Bing News 追加、ニュースタイマー、海外コンプライアンス |
| **v0.4.2**  | realloc ダングリングポインタ全面修正(15+箇所)、4ラウンドメモリ安全監査                |
| **v0.4.3**  | **創発的品詞システム** — シードアンカー + 512次元特徴クラスタリング                    |
| **v0.4.8**  | 拡散エンジン機能語フィルタ(~130語)、クロスレイヤーインデックス修正、double-free 修正  |

> 詳細な変更履歴: [`changelogs/`](changelogs/) ディレクトリを参照

---

## 既知の制限

- **生成の流暢さ** — 連想経路の出力は LLM 生成テキストほど自然ではない(継続的に改善中)
- **GPU アクセラレーションなし** — 純粋な CPU + pthread + OpenMP
- **バイナリ状態ファイル** — アーキテクチャ間で非互換(x86_64 と ARM は非互換、JSON/MessagePack 形式を計画中)
- **シングルノード** — 分散マルチノードトポロジ未対応

---

## ロードマップ

- [ ] FPGA デプロイ(究極の目標: ハードウェアレベルのニューロモーフィックコンピューティング)
- [ ] 分散マルチノードトポロジ(デバイス間活性化伝播)
- [ ] 視覚 / 聴覚マルチモーダル入力インターフェース
- [ ] JSON/MessagePack テキスト形式永続化(クロスアーキテクチャ互換)

---

## コントリビューション

Issue と Pull Request を歓迎します。大きな変更を行う場合は、まず Issue を開いて変更内容を議論してください。

---

## ライセンス

[Apache License 2.0](LICENSE)

---

<a name="running-on"></a>
*現在 **EAIDK-610**(RK3399 ARM Cortex-A72、3.8GB RAM)上で稼働中。*
*目標: 組み込みハードウェアにデプロイ可能な自己維持型分散認知エンジン。*

<div align="center">

メンテナー：[陈道祥 (afd-ll)](https://github.com/afd-ll)

[⭐ このリポジトリに Star](https://github.com/afd-ll/PivotMind) · [🐛 バグ報告](https://github.com/afd-ll/PivotMind/issues) · [📖 ドキュメントを読む](ARCHITECTURE.md)

</div>

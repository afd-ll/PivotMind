<div align="center">

# 玄枢 PivotMind

### 脳に着想を得た意味連想エンジン
**Pure C · AI フレームワーク依存ゼロ · ARM 組み込みボードで動作**

[English](README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Русский](README.ru.md)

[![Version](https://img.shields.io/badge/version-v0.4.0-blue.svg)](changelogs/)
[![License](https://img.shields.io/badge/license-Apache%202.0-green.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C-99%2B-orange.svg)](https://en.wikipedia.org/wiki/C99)
[![Platform](https://img.shields.io/badge/ARM-RK3399%20%7C%20x86__64-lightgrey.svg)](#running-on)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-success.svg)](#quick-start)

> 知能とは行列演算の積み重ねではなく、
> 推論ネットワークを伝播する活性化の波紋である。

</div>

---

## PivotMind とは

PivotMind は [HuarongTopologyNet](#huarongtopologynet) +
[Hebbian Learning](#core-mechanisms) +
[Multi-Layer Diffusion Reasoning](#multi-layer-diffusion-engine)
の上に構築された**脳に着想を得た認知エンジン**です。
Transformer なし。埋め込みベクトルなし。誤差逆伝播なし。
あるのはノードとエッジ、活性化と減衰だけ — 絶え間なく刻む背景クロックによって駆動されます。

**現行バージョン: v0.4.0** — 10 の脳領域、PFE 推論、IdeaArena 競合、オンライン Hebbian 学習。

### HuarongTopologyNet

各概念はノードです。同時出現がエッジを生成します。エッジは三重属性を保持します:
**重み × 信頼度 × 動機バイアス**。
10 個のサブトポロジ(語彙 / 意味 / 感情 / 構文 / 文脈 / 領域 / 語用論 / 文化 / 概念 / テンプレート)
はそれぞれ独立した推論ネットワークを形成し、クロスリンクで相互接続されます。
活性化は層を跨いで同時に拡散し、競合によって勝者が出力として選択されます。

### なぜこのアプローチか

| 従来の LLM             | PivotMind                                                  |
|------------------------|------------------------------------------------------------|
| トークン予測、ステートレス | ノード活性化、持続的な内部状態                          |
| 勾配ベースのオフライン一括学習 | Hebbian オンラインリアルタイム学習                  |
| 単一の埋め込み空間     | 10 個の独立したサブトポロジ                                |
| ニューラルネットのブラックボックス | 明示的なノード-エッジ経路、完全追跡可能         |
| GPU + 大容量 VRAM 必須 | pthread + OpenMP のみ、ARM 組み込みで動作              |
| 推論と学習が分離       | 会話がそのまま学習になる                                  |
| 生理的認識なし         | 内受容的自己モニタリング、3 段階ヘルス応答               |

---

## 脳領域アーキテクチャ

PivotMind は哺乳類の皮質機能区分をモデル化します — 10 の脳領域がそれぞれ専任の責務を持ち、Thalamus 信号バスを介して通信します。

```
                          ┌──────────────────────┐
                          │   Prefrontal Cortex    │ ← 対話 / 意思決定エントリ
                          │  + Prefrontal Exec PFE │ ← 推論オーケストレータ
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

| 脳領域                  | ファイル                     | 機能                                                                |
|------------------------|----------------------------|---------------------------------------------------------------------|
| Prefrontal             | `prefrontal.c`             | 対話生成、拡散 → ACC 評価                                           |
| Prefrontal Exec        | `prefrontal_executive.c`   | 6 モード推論オーケストレーション、タスク分解                        |
| Hippocampus            | `hippocampus.c`            | 記憶定着、QA リプレイ、知覚連動                                     |
| DMN                    | `dmn.c`                    | Default Mode Network: 夢想的連想、アイドル探索                      |
| Amygdala               | `amygdala.c`               | 感情価サンプリング、探索 / 利用バランス                              |
| Perception Cortex      | `perception.c`             | Web 検索、article_reader セマンティックパイプライン                  |
| Broca's Area           | `broca.c`                  | テンプレート自動構築と減衰スケジューリング                          |
| Cerebellum             | `cerebellum.c`             | BPTT 微調整、ハードウェア資源保護                                   |
| Hypothalamus           | `hypothalamus.c`           | ドライブ動力学調整、概日リズム連動                                  |
| Thalamus               | `thalamus.c`               | 信号バス、リソースゲーティング、領域間ルーティング                  |
| Brainstem              | `brainstem.c`              | 概日ハートビート、活性化減衰、自発的活性化                          |
| Cingulate (ACC)        | `cingulate.c`              | 4 次元シーケンス評価(意味 + テンプレート + 感情 + 長さ)             |
| IdeaArena              | `idea_arena.c`             | 複数候補 5 次元競合選択                                             |

---

## コアメカニズム

### 多層拡散エンジン

入力はスライディングウィンドウでトークン化され、層を跨いで同時に拡散します:

- **Vocabulary** — 直接的なリテラルマッチング、高速想起
- **Semantic** — 10 サブトポロジを跨ぐクロストポロジ連想
- **Template** — 構文パターン認識、コネクタ挿入のガイド
- **Emotion** — 価数 × 覚醒度の重み付け、候補優先度の調整

側方抑制により出力の多様性を確保します。

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

サブゴールは再帰的に分解されます(深さは設定可能)。競合検出 + IdeaArena 競合により最適経路を選択し、説明可能な推論チェーンを生成します。

### 内受容的自己モニタリング

RSS メモリ、接続増加率、推論遅延を継続的に監視し、3 段階応答を行います:

| レベル         | 状態        | アクション                                                     |
|---------------|------------|----------------------------------------------------------------|
| 🟢 GREEN      | 正常        | 通常動作                                                       |
| 🟡 YELLOW     | 警告        | ログ警告 + 学習閾値を上昇                                      |
| 🔴 RED        | 危険        | 緊急保存 + 弱いエッジを一括剪定                                |

---

## クイックスタート

### ビルド

```bash
# GCC + pthread + OpenMP が必要(その他の依存なし)
make all

# ARM クロスコンパイル
make CC=aarch64-linux-gnu-gcc all
```

### 実行

```bash
# インタラクティブゲートウェイ(推奨)
./build/bin/pivotmind_gateway

# CLI インタラクティブモード
./build/bin/digital_life
```

ゲートウェイはデフォルトで `:8080` でリッスンします。

### API 例

```bash
# 質問する
curl -X POST http://localhost:8080/ask \
  -H "Content-Type: application/json" \
  -d '{"query":"What is consciousness?"}'

# 学習素材を与える
curl -X POST http://localhost:8080/learn \
  -H "Content-Type: application/json" \
  -d '{"text":"Consciousness is the subjective experience produced by neural networks in the brain."}'

# 状態確認
curl http://localhost:8080/status
```

### ビルドターゲット

| コマンド              | 説明                                          |
|----------------------|-----------------------------------------------|
| `make all`           | 全ターゲットをビルド                          |
| `make gateway`       | HTTP ゲートウェイのみビルド                   |
| `make digital-life`  | CLI インタラクティブ版をビルド                |
| `make seed-builder`  | シードトポロジツールをビルド                  |
| `make batch-learn`   | バッチ学習ツール                              |
| `make clean`         | ビルド成果物を削除                            |

---

## プロジェクト構成

```
pivotmind/
├── src/               # 82 のコアソースファイル
├── include/           # 86 のヘッダファイル
├── demos/             # ゲートウェイとインタラクティブエントリ
├── tools/             # 学習 / デバッグ / データ処理ツール
├── tests/             # ユニットテスト(PFE テスト 23 件、合格率 100%)
├── scripts/           # 自動化スクリプト
├── changelogs/        # バージョン変更履歴
├── docs/              # アーキテクチャドキュメントと図
└── archived/          # 過去バージョンのアーカイブ
```

---

## バージョン履歴

| バージョン   | 主な内容                                                                                |
|-------------|-----------------------------------------------------------------------------------------|
| v0.1.x      | 基本ウォーク推論、競合キュー、状態永続化                                                |
| v0.2.x      | 多層拡散、Hippocampus / DMN / Perception、内受容的モニタリング                          |
| **v0.3.0**  | Prefrontal Executive(6 モード推論)、IdeaArena 5D、戦略重み自己学習                      |
| **v0.4.0**  | コード簡素化(約 200 行)、11 件のルックアップ統合、Broca アップグレード、Hypothalamus 新設 |

> 詳細な変更履歴: [`changelogs/`](changelogs/) ディレクトリを参照

---

## 既知の制限

- **中国語ファースト** — 文字レベルのトークン化は中国語に自然に適合し、英語 / 混合入力の体験は限定的
- **応答の流暢さ** — 連想経路の出力は LLM 生成テキストほど自然ではない(継続的に改善中)
- **GPU アクセラレーションなし** — 純粋な CPU + pthread + OpenMP
- **バイナリ状態ファイル** — アーキテクチャ間で非互換(x86_64 と ARM は非互換)

---

## ロードマップ

- [ ] FPGA デプロイ(究極の目標: ハードウェアレベルのニューロモーフィックコンピューティング)
- [ ] 分散マルチノードトポロジ(デバイス間活性化伝播)
- [ ] 視覚 / 聴覚マルチモーダル入力インターフェース
- [ ] スケジュール自動保存

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

[⭐ このリポジトリに Star](https://github.com/afd-ll/PivotMind) · [🐛 バグ報告](https://github.com/afd-ll/PivotMind/issues) · [📖 ドキュメントを読む](ARCHITECTURE.md)

</div>

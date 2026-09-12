# `n.q` のソース — `.pt` → `n.q` 変換ツール

**English summary**: This directory is the complete, self-contained source of the tool that converts the
upstream Ultralytics **YOLO26n** checkpoint (`yolo26n.pt`) into `n.q`, the quantized/re-laid-out weight
blob loaded by this project's FPGA accelerator. `n.q` is a derivative work of AGPL-3.0 weights, so this
tool is shipped as its **Corresponding Source**. It needs only a C++17 compiler and CMake — no PyTorch,
no ultralytics, no OpenCV, no OpenCL, and none of the HLS kernel sources. With the bundled sidecar
`data_shift_y26_640.txt` it reproduces the distributed `n.q` **byte-for-byte**
(md5 `3a83aeb5d10ec13130998904dfee7732`). Build and run: see "ビルド" / "実行" below.

---

配布アーカイブ(`y7-public-<tag>.tar.gz`)の `home/yolov7/data26_live/n.q` および
`home/yolov7/data26_640/n.q` は、上流 Ultralytics **YOLO26n** の学習済み重み(`yolo26n.pt`)を
本 PL(FPGA)向けに量子化・再配置したもの = **AGPL-3.0 の重みの派生物**である。

本ディレクトリは、その変換を行うツールの**全ソース**であり、`n.q` の
**対応ソース(Corresponding Source)**として同梱している
([../../LICENSE](../../LICENSE) / [../../THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) §1)。

## 上流の重み(入力)

| 項目 | 値 |
|---|---|
| ファイル | `yolo26n.pt`(Ultralytics YOLO26n) |
| 入手元 | https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo26n.pt |
| sha256 | `9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef` |
| ライセンス | AGPL-3.0(Ultralytics) |

モデル構造と学習済み重みは **Ultralytics** による(上流 = https://github.com/ultralytics/ultralytics)。
本プロジェクトは Ultralytics の公式物ではなく、提携・承認も受けていない。

## 構成

| ファイル | 役割 |
|---|---|
| `src/nq_main.cpp` | CLI(`--pt` / `--gen` ほか) |
| `src/nq.cpp` | 生成器本体。層列の構築、重み/bias の量子化と pack、n.q の書き出し |
| `src/pt_reader.cpp` | `.pt`(= ZIP + pickle)の自前パーサ。torch 不要 |
| `src/nq_support.cpp` | 生成器が参照する最小の外部シンボル(割当状態・既定 `data_shift` 表・`file_save`) |
| `include/nq_defs.h` | 型と定数(`struct Layer` / `LayerType` / メモリマップ / `NqConfig` / `FP8` / bump allocator / `relayout_liveness`) |
| `include/pt_reader.h`, `include/pt_format.h` | `.pt` パーサの API と pickle/ZIP 形式の定義 |
| `data_shift_y26_640.txt` | **sidecar**(量子化シフト表)。640×640 用の正本、md5 `8c41789e6349ef3c2806157f2bfefb6d` |
| `CMakeLists.txt` | 生成器だけをビルドする独立 target |

`include/nq_defs.h` は、本プロジェクト内部で HLS kernel と host が共用する巨大ヘッダから
**生成器が参照する宣言だけ**を抜き出した trim 版である。HLS kernel の実装、`ap_int`/`ap_uint`
エミュレーション、`hls::stream` shim、OpenCL host ラッパ、推論用の `swish`/`sigmoid` LUT は含まない。
`GMEM_T`(512bit = 64B の 1 word)は内部版では `ap_uint<512>` だが、生成器は `sizeof()` と
64B 境界計算にしか使わないので、ここでは POD(`unsigned char[64]`)に置き換えてある
(`n.q` の中身は変わらない)。

## ビルド

依存は **C++17 コンパイラ + CMake 3.13 以上**のみ。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="\
  -DY26_NETH=640u -DY26_NETW=640u -DGMEM_DATA64_SIZE_MB=512 -DHEAD_RESERVE_FRONT \
  -DHB_TOP_WORDS=524288u -DBATCH_IN_BASE=278528 -DBATCH_OUT_BASE=432128 -DMGR_CYC_BASE=456704"
cmake --build build -j"$(nproc)"
```

この `-D` 群が**入力幾何(640×640)とメモリマップ**を決める。値が 1 つでも違うと別の `n.q` になる。

## 実行

生成器は sidecar と既定出力先を **`.pt` と同じディレクトリ**から探すので、作業ディレクトリに
`.pt` と sidecar(名前は `data_shift_y26.txt`)を置く。

```bash
mkdir -p work
cp /path/to/yolo26n.pt           work/yolo26n.pt
cp data_shift_y26_640.txt        work/data_shift_y26.txt     # ★名前を変える

env WEIGHT_K3_INT8=0 \
    RELAYOUT_SELFCHECK=1 OVERREAD_CHECK=1 \
    MAP_PIN_HEADS=131,136,140,145,149,154 MAP_PIN_HEADS_BASE=457216 \
    ./build/nq --pt work/yolo26n.pt --gen work/n.q

md5sum work/n.q
# 3a83aeb5d10ec13130998904dfee7732     ← 配布 n.q と byte 一致
```

その他のモード:

```bash
./build/nq --pt work/yolo26n.pt --nq-dump work/n.q      # 既存 n.q の層ヘッダ一覧
./build/nq --pt work/yolo26n.pt --dump-shift work/n.q out.txt  # n.q から sidecar を書き出す
./build/nq --pt work/yolo26n.pt --gen-layers            # 生成せず層構成だけ検査
./build/nq --pt work/yolo26n.pt                         # .pt の階層ダンプのみ
```

## 再現条件(4 つ)

1 つ欠けても別のファイルになる。

| # | 条件 | 値 |
|---|---|---|
| ① | sidecar が正本 | `data_shift_y26_640.txt` = md5 `8c41789e6349ef3c2806157f2bfefb6d`。`.pt` と同じ dir に `data_shift_y26.txt` として置く |
| ② | ビルド定義 | 上記「ビルド」の `-D` 群(`Y26_NETH/NETW=640u`、`GMEM_DATA64_SIZE_MB=512`、`HEAD_RESERVE_FRONT`、`HB_TOP_WORDS=524288u`、`BATCH_IN_BASE=278528`、`BATCH_OUT_BASE=432128`、`MGR_CYC_BASE=456704`) |
| ③ | 量子化 | `WEIGHT_K3_INT8=0`(全層 int16) |
| ④ | head pin | `MAP_PIN_HEADS=131,136,140,145,149,154` + `MAP_PIN_HEADS_BASE=457216`。**列挙順(昇順)が配置を決める** |

`RELAYOUT_SELFCHECK=1` / `OVERREAD_CHECK=1` は配置の自己検証で、出力バイトには影響しない。

期待値: **md5 `3a83aeb5d10ec13130998904dfee7732`**(5,506,816 バイト)。

★④ を付けた構成では生成ログに

```
[CEIL] ✗ 層 peak=484352 > BATCH_IN_BASE=278528 ...
```

が**出るのが正常**。6 つの head を I/O 窓の上 `[457216, 483072)` へ固定配置する構成で、
この配置を前提にした推論 host とセットで採用している(配布物がこの組み合わせ)。

## sidecar(`data_shift_y26_640.txt`)について

sidecar は各 conv 路の固定小数シフト(`zoomin` / `zoomout`)表で、**キャリブレーション用画像集合**から
算出したものである。**同梱の sidecar を使う限り、`n.q` の再現は C++ だけで完結する**。

sidecar 自体を**作り直す**には、`.pt` を PyTorch で読み込んで各層の活性化レンジを測る Python
(torch + ultralytics)が必要で、**そのスクリプトは本ディレクトリには含まれない**。
同梱の sidecar がその出力であり、再現の入力として固定されている。

## `n.q` のレイアウト(概要)

すべて **64B(512bit)word 単位**。

```
[層 0 ヘッダ 8 word = 512B]
  ├ word0 = struct Layer (64B: id / type / in / out / is[4] / os[4] / p[4] / flags / recipe)
  └ word1..7 = 予約(0)
[Conv 層のみ] bias 領域 (conv.bias word)     … int16 × 出力ch(padding 込み)
              weight 領域 (conv.weight word) … 量子化済み重みを PL の読み順に pack
[層 1 ヘッダ 8 word] …
  …
[Finish 層 × 8 = 512B]   ← 末尾 sentinel(層連鎖の終端)
```

- `in` / `out` は data buffer 内の位置で、**4KB(64 word)単位**で格納する(実 word = 値 × 64)。
- `out` の割り当ては liveness ベースの再配置(`relayout_liveness`)が決める。`MAP_PIN_HEADS` は
  6 つの出力 head を固定アドレスへ先に確保し、後続層に踏ませない。
- Conv の重みは 3×3 / 1×1(tap-fold・dense32)/ depthwise で pack の形が違う。
  どれを使うかは層ヘッダの `flags`(bit2/bit3)に焼かれる。

## ライセンス

本ディレクトリのソースは**本プロジェクトの著作物**で、**AGPL-3.0**
([../../LICENSE](../../LICENSE))。各ファイル先頭に
`SPDX-License-Identifier: AGPL-3.0-or-later` を付けてある。

出力である `n.q` は上流 Ultralytics YOLO26n の重みの派生物でもあるため、**AGPL-3.0**
([../../THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) §1)。

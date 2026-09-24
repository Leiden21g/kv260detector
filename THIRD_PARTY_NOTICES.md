# 第三者物の告知(THIRD_PARTY_NOTICES)

本 repo の**自作部分**は GNU Affero General Public License v3.0(AGPL-3.0)で公開する(全文 = [LICENSE](LICENSE))。
本書は、本 repo が**同梱する**、または**配布バイナリ(GitHub Release asset)に含む/前提とする**第三者物の一覧である。
第三者物にはそれぞれのライセンスが適用され、本 repo の AGPL-3.0 はそれらを上書きしない。

**本書に出てくる `package/…` は、配布アーカイブ `y7-public-<tag>.tar.gz`(GitHub Release の asset)を
展開した中のパス**であり、repo にはそれらのファイルは含まれない([README.md](README.md)「配布物」)。

**表記**

- 「確認済」= 上流の実物(ソース・ライセンスファイル・配布物の md5 など)と突き合わせて確かめた事実。
- 「要確認」= 未確認、または公開前に判断が要るもの。
- 「(見解)」= ライセンスの解釈や法的な扱いについての**非専門家の整理(推測)**。法的助言ではない。公開前に必要なら専門家に確認すること。

---

## 0. 一覧

| # | 物 | 本 repo での扱い | ライセンス | 状態 |
|---|---|---|---|---|
| 1 | Ultralytics YOLO26n の重み → `n.q` ほか重み派生ファイル | 配布バイナリ(Release asset) | AGPL-3.0(Ultralytics) | ★**決定済(2026-09-12)**: 対応ソース = `.pt` → `n.q` 変換ツールを同梱 = [src/nq/](src/nq/)。配布 `n.q`(md5 `3a83aeb5`)の byte-exact 再生成を確認済 |
| 2 | `allegro_dvt.ko`(Allegro DVT VCU エンコーダ driver、patch 適用ビルド) | 配布バイナリ(Release asset) | GPL-2.0 | ★**決定済(2026-09-12)**: 案 A を実施 — 対応ソース = [src/allegro-dvt/](src/allegro-dvt/) + 別 asset の source tarball |
| 3 | `al5e.fw` / `al5e_b.fw`(VCU microcode v2019.2) | 配布バイナリ(Release asset) | Allegro DVT2 の許諾(MIT 型 + 使用先制限) | 確認済 |
| 4 | PL bitstream `kv260_fan_vcu.bit.bin` / xclbin `binary_container_1.bin` | 配布バイナリ(Release asset) | **本プロジェクトの著作物 = 独自条件(AGPL-3.0 ではない)** + AMD(Xilinx)IP の生成物 | ★**決定済(2026-09-12)**: 重み非含有・Ultralytics 由来コード無しを確認済、独自条件で配布。**AMD IP の再配布条件も EULA 原文で確認済**(§4-5。§3(a)(3)C が bitstream 形式での配布を明文で許す。残る論点 4 つは同節末尾) |
| 5 | `combined_allegro.dtbo`(PL overlay) | 配布バイナリ(Release asset) | 自作 = AGPL-3.0(AMD ツール生成 DT を元に再構成) | ★**決定済(2026-09-12)**: ソース同梱 = [src/dtbo/](src/dtbo/)。`dtc` で byte-exact 再生成を確認済 |
| 6 | MediaMTX v1.19.2 | **同梱しない**(上流から取得) | MIT | 確認済 |
| 7 | XRT 2.20.0 | 同梱しない(board に dnf 導入)。推論 host がヘッダを include し動的リンク | Apache-2.0(user space) | 確認済 |
| 8 | OpenCV / GStreamer / gst-rtsp-server / PyGObject / Python ほか | 同梱しない(board に dnf 導入) | 各上流(下記) | 参照のみ |
| 9 | ソース内の取り込み物(8x8 bitmap font、COCO-80 クラス名) | `live/inference_host/include/y26_live.h` | Public Domain / COCO データセットのラベル名 | font は確認済 / クラス名も出所を確認済(§9。著作物性は低いと整理 = 見解) |
| 10 | AMD 公式 SD イメージ・Boot FW・PetaLinux SDK・Vitis/Vivado | 同梱しない(各自 AMD から入手) | AMD の各ライセンス | 参照のみ |
| 11 | AP1302 カメラ ISP firmware `ap1302_ar1335_single_fw.bin` | **同梱しない**(利用者が上流から取得) | ON Semiconductor AP1302 ISP Firmware License Agreement(専用 EULA) | ★**確定(2026-09-12)**: 出所・ライセンスとも確認済、同梱しない方針で決定 |

---

## 1. Ultralytics YOLO26n の重み(`n.q` ほか)

- **元の重み**: Ultralytics の `yolo26n.pt`。入手元 = https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo26n.pt
  (sha256 `9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef`)。**確認済**: 変換に使った `.pt` の sha256 が
  上記 release asset の digest と一致する。
- **本 repo の派生物**: `package/home/yolov7/data26_live/n.q`、`package/home/yolov7/data26_640/` の `n.q`・`p5cls_w.bin`・
  `w_psa_pe_*.bin`・`data_shift_y26_640.txt`。いずれも上記の重みを本 PL 向けに**量子化・再配置したもの**(= 派生物)。
  git では追跡しておらず、Release asset として配布している。
- **ライセンス**: Ultralytics のコードと学習済みモデルは **AGPL-3.0**(Ultralytics の表記。確認済: ultralytics 8.4.56 の
  配布物の `License: AGPL-3.0` と同梱 LICENSE。本 repo の [LICENSE](LICENSE) はその LICENSE とバイト一致する GNU 公式全文)。
  (見解)`n.q` 等は AGPL-3.0 の重みの派生物なので、AGPL-3.0 で配布する。
- **帰属**: YOLO26 のモデル構造と学習済み重みは **Ultralytics** による。上流 = https://github.com/ultralytics/ultralytics
  / https://docs.ultralytics.com 。本 repo は Ultralytics の公式物ではなく、Ultralytics とは無関係(提携・承認を受けていない)。
- **Enterprise License**: Ultralytics は AGPL-3.0 の義務(ソース公開等)を負わずに商用利用するための
  **Ultralytics Enterprise License** を別途提供している(https://www.ultralytics.com/license)。本 repo の成果物を
  AGPL-3.0 の条件に従えない形で使いたい場合は、Ultralytics との個別契約が必要になる可能性がある(見解)。
- **推論後処理**: `live/inference_host/` の head decode / letterbox(パディング色 114 など)は、Ultralytics の仕様に
  出力を合わせた独自実装。Ultralytics のソースコードをコピーした箇所は、grep の範囲では見つかっていない(確認済: 著作権表記・
  SPDX・上流由来の記述の検索で該当なし)。
- **対応ソース**: ★**決定済(2026-09-12)**。AGPL-3.0 の「対応ソース(Corresponding Source)」は「改変に適した形式」を
  要求する。`n.q` については、元の `.pt`(上流で入手可能)に加えて **`.pt` → `n.q` の変換ツールまで同梱する**
  ことにした = [src/nq/](src/nq/)(生成器の全ソース + 量子化 sidecar `data_shift_y26_640.txt`)。
  HLS kernel ソースは含まない(§4 のとおり bit/xclbin は AGPL 対象外)。依存は C++17 のみで、torch / ultralytics /
  OpenCV / OpenCL は不要。**確認済**: クリーンな環境で cmake ビルド → 実行し、配布 `n.q`
  (v1.1: md5 `2c6a15bc77fe1a6471c382c83a3e6377`、5,506,816 バイト。v1.0 は `3a83aeb5d10ec13130998904dfee7732`)を `cmp` 差分 0 = **byte-exact に再生成できる**
  (再現条件と手順は [src/nq/README.md](src/nq/README.md))。
  ★sidecar `data_shift_y26_640.txt` 自体を作り直すキャリブレーション Python(torch + ultralytics 必要)は
  同梱していない。sidecar はその出力として固定入力の形で同梱してある。

## 2. `allegro_dvt.ko`(VCU H.264 エンコーダ driver)

- **確認済(配布物の modinfo)**: md5 `73ceb61fb3eff5174634f717fb066275`(= `package/MD5SUMS.txt` の `home/allegro_dvt.ko`)、
  sha256 `080cf20f6d823aceb3f836463ae015b10dc5dffa79dd5fe056902460880ce62d`、104,992 バイト。
  ```
  description:    Allegro DVT encoder driver
  author:         Michael Tretter <kernel@pengutronix.de>
  license:        GPL
  alias:          of:N*T*Callegro,al5e-1.1
  name:           allegro
  vermagic:       6.12.40-xilinx-g31626ef92ff1 SMP mod_unload aarch64
  ```
- **上流**: Linux kernel mainline の `drivers/media/platform/allegro-dvt/`(Pengutronix 作)。本 ko は **AMD(Xilinx)の
  kernel tree `linux-xlnx` のコミット `31626ef92ff1`**(board の kernel `6.12.40-xilinx-g31626ef92ff1` と同じコミット)の
  同 dir から、次の 10 ファイルを取得してビルドしたもの:
  `allegro-core.c` `allegro-mail.c` `allegro-mail.h` `nal-h264.c` `nal-h264.h` `nal-hevc.c` `nal-hevc.h` `nal-rbsp.c` `nal-rbsp.h` `Makefile`
  (https://github.com/Xilinx/linux-xlnx/tree/31626ef92ff1/drivers/media/platform/allegro-dvt)。
  各ファイルのヘッダは `SPDX-License-Identifier: GPL-2.0`、`Copyright (C) 2019(-2020) Pengutronix`、`MODULE_LICENSE("GPL")`(確認済)。
  モジュール名は上流どおり `allegro`(ファイル名だけ `allegro_dvt.ko` に改名して配置)。
- **本 repo 側の変更(patch)**: `allegro-core.c` の `allegro_encoder_buffer_init()` に 1 箇所。KV260 の VCU は
  `ENC_BUFFER_EN=false`(`VCU_MEMORY_DEPTH` = 0)なので、そのときは encoder buffer を無効として扱う(無いと
  channel 作成が `resource unavailable (8e)` で失敗する):
  ```diff
   	err = regmap_read(settings, VCU_MEMORY_DEPTH, &memory_depth);
   	if (err < 0)
   		return err;
  +	if (memory_depth == 0)
  +		return -ENODEV;
   	err = regmap_read(settings, VCU_NUM_CORE, &num_cores);
  ```
  確認済: 上流 `31626ef92ff1` の `allegro-core.c` にこの分岐は無い(= 本 repo 側の変更)。ビルドは
  [src/allegro-dvt/build_allegro_dvt.sh](src/allegro-dvt/build_allegro_dvt.sh) と同じ手順(上記 10 ファイルの取得 →
  patch 適用 → board kernel の build tree で `modules_prepare` → `UTS_RELEASE` を board の vermagic に固定 →
  `make M=... CONFIG_VIDEO_ALLEGRO_DVT=m modules`)で行った。
  ko のバイナリから patch の有無は直接は読めない(配布 ko が patch 版であることはビルド記録と board 実機動作に基づく)。
- **ライセンス**: GPL-2.0(only)。本 repo の AGPL-3.0 は**この ko には適用されない**。
  (見解)ko は独立したカーネルモジュールで、本 repo の他の部分とは別プログラムとして同梱する「集合物(aggregate)」扱い。
- **義務(見解)**: GPL-2.0 のバイナリを配布する者は、**対応する完全なソース**(上記 10 ファイル + patch + ビルドに使った
  スクリプト)を、バイナリに添えるか、書面の申し出(3 年間有効)で提供する必要がある(GPL-2.0 §3)。
  上流 URL を示すだけでは §3 の要件を満たすかが曖昧(見解)。
- **提供方法 = ★案 A を実施(2026-09-12)**:
  - **repo / 配布アーカイブに同梱**するもの = [src/allegro-dvt/](src/allegro-dvt/)(配布アーカイブでは `src/allegro-dvt/`)。
    中身 = 上流の特定(repo・コミット・10 ファイル)、`upstream_sources.sha256`(上流 10 ファイルの sha256)、
    patch(`0001-allegro-dvt-kv260-disable-encoder-buffer.patch` = 正規の unified diff。`patch -p1` / `-p5` で当たる)、
    `build_allegro_dvt.sh`(取得 → sha256 照合 → patch → クロスビルド → vermagic 確認。**開発機パスを含まない**)、
    および手順の説明([src/allegro-dvt/README.md](src/allegro-dvt/README.md))。
  - **ko と同じ GitHub Release に並べる別 asset** = `allegro-dvt-gpl-src-31626ef92ff1.tar.gz`。
    中身 = 上記一式 + **上流 10 ファイルの実体**(未改変)+ patch 適用済み `allegro-core.c` + `SHA256SUMS`。
    ★これにより**ネットワーク無しでビルドが完結する**(`SRCDIR=<展開先>/upstream bash build_allegro_dvt.sh`)。
    生成は開発 repo の `scripts/package_allegro_dvt_source.sh`(上流取得 → sha256 照合 → patch 実適用を検証 → tar)。
  - kernel の設定・build tree は board の公式 kernel(`6.12.40-xilinx-g31626ef92ff1`)のものを使う。
  - ★**運用上の義務**: ko を含む release を作るたびに、この source tarball を**同じ release の asset として必ず並べる**
    (GPL-2.0 §3。配布アーカイブの `RELEASE_NOTES.md` にもこの注意を出している)。
- **patch の同一性について**: 同梱 patch は上流 `31626ef92ff1` の `allegro-core.c` に対して `patch` でクリーンに当たり、
  適用結果が `patched/allegro-core.c` とバイト一致することを tarball 生成時に毎回検証している(確認済)。
  配布 ko のビルド時に当てた変更とはコメント文の分量だけが違い、**コンパイル結果に影響する差は無い**
  (追加した実行文は `if (memory_depth == 0) return -ENODEV;` の 1 つだけ)。
- **状態**: ★**解決済(2026-09-12)**。出所・patch 内容・ライセンス・提供方法のすべてが確定し、ソースも用意した。
  残る運用上の注意は上記「運用上の義務」だけ。

## 3. `al5e.fw` / `al5e_b.fw`(VCU microcode)

- **確認済**: 配布物(`package/MD5SUMS.txt` の `firmware/al5e.fw` = `6f0aaf8144ff7dcef430e6a8c27b6177`、126,572 バイト /
  `firmware/al5e_b.fw` = `fa9cb5231cdda894328caeac77b81457`、14,680 バイト)は、上流
  **Xilinx/vcu-firmware** のタグ `xilinx-v2019.2`(ブランチ `release-2019.2`)の `1.0.0/lib/firmware/al5e.fw` /
  `al5e_b.fw` と **md5 が一致**する(https://github.com/Xilinx/vcu-firmware)。
- **ライセンス(確認済)**: 同 repo の `LICENSE`(Allegro DVT2)。MIT 型の許諾で再配布可、ただし
  (a) **著作権表示と許諾表示を全コピーに含めること**、(b) **使用は Xilinx デバイス上/Xilinx デバイスと通信する用途に限る**、
  (c) 逆コンパイル・改変等の禁止、の条件が付く。
- **義務**: Release asset として配布するときは、下記の許諾文を同梱する(本書を同梱すれば足りる見込み。見解)。
  (見解)この firmware は VCU の MCU 上で動く別プログラムで、本 repo の AGPL-3.0 部分とは集合物の関係。AGPL-3.0 は適用されない。
- 上流 `LICENSE` 全文(原文。空行のみ詰めた):

```
/******************************************************************************
*
* Copyright (C) 2017 Allegro DVT2.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX OR ALLEGRO DVT2 BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
*
* Except as contained in this notice, the name of Allegro DVT2 shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Allegro DVT2.
*
* Licensee shall not decrypt, decompile, reverse-engineer, disassemble, or
* otherwise reduce to a human-perceivable form, or modify or alter, any portion
* of the Licensed Materials that are provided by Xilinx in object code, encrypted
* or other obfuscated form.
*
******************************************************************************/
```

- 参考: AMD 公式 SD の dnf パッケージ `vcu-firmware` / `kernel-module-vcu`(AMD 版 fw と driver)は本構成では使わない
  (`doc/setup.md` §2-1)。

## 4. PL bitstream / xclbin(Vitis / Vivado の生成物)

- 対象: `firmware/kv260_fan_vcu.bit.bin`(bit 8ec4bc3b)/ `firmware/xilinx/yolov7/binary_container_1.bin`(xclbin 14279337)。
- 中身: 自作の HLS 推論 kernel(Vitis HLS 2025.2 で合成)と、AMD(Xilinx)の IP / platform を Vivado で統合した生成物。
  PL overlay(§5)から読み取れる範囲では、少なくとも VCU(`xlnx,vcu-1.2`)、MIPI CSI-2 RX Subsystem、Video Frame Buffer Write、
  AXI IIC、AXI Interrupt Controller、AXI4-Stream FIFO、および XRT(zocl)用の platform 部分を含む。

### 4-1. 重みを含まないこと(確認済)

- **bit / xclbin のどちらにも学習済み重みは入っていない**。重みは**すべて実行時に `n.q`(§1)から
  param GMEM 経由で読む**。kernel 側に焼き込まれた重みテーブルは存在しない。
- 要素数 100 以上の const 配列は HLS kernel ソースに存在せず、唯一の大きな定数表 `attn_exp_lut[4096]`
  (PSA attention の softmax 用 exp LUT)は**数式由来**である(ヘッダ冒頭に生成式
  `attn_exp_lut[i] = round(exp(-i/256) * 2^15)` を明記。合成で `exp()` ハードを避けるための const 化)。
- モデル構造も `n.q` 駆動で、層 id を直書きした分岐は無い(層種別 enum と `n.q` 内の層記述で駆動)。
  YOLO26 を前提とした DRAM レイアウト定数と説明コメントはあるが、上流コードの複製ではない。
- これが AGPL 切り分けの要点である: **AGPL-3.0 の原因は Ultralytics の重み(= `n.q`)であり、
  bit/xclbin はその重みを 1 バイトも含まない。**

### 4-2. Ultralytics 由来コードを含まないこと(確認済)

- 確認方法: HLS kernel のソース一式(推論 kernel 本体・その FIFO/型ヘッダ・上記 exp LUT ヘッダ)に対し、
  `grep -niE 'ultralytics|yolov8|non_max_suppression|make_anchors|dist2bbox'` を実行して**該当 0 件**。
  著作権表記・SPDX 行・「上流から移植」等の記述も見つかっていない。
- YOLO26 は NMS-free のため、Ultralytics 側の `non_max_suppression` / DFL / `make_anchors` / `dist2bbox` に
  相当する実装はそもそも存在しない。
- 推論 host 側(`live/inference_host/`)についても §1「推論後処理」および §9 のとおり。

### 4-3. host と kernel の関係(確認済)

- host(`live/inference_host/`)と kernel は**別バイナリ**で、結合は **XRT 経由のみ**
  (`load_xclbin` → `set_arg` / `run.start()`、BO と `xrt::ip`)。host が kernel の宣言をリンクしているわけではない。
- 両者が共有しているのは `n.q` のバイナリレイアウトと DRAM メモリマップという**ファイルフォーマット / ABI** だけである。

### 4-4. ライセンス表明(本プロジェクトの著作物としての条件)

bit/xclbin は本プロジェクトの著作物であり、**AGPL-3.0 では配布しない**。次の独自条件で提供する
(第三者 IP を含む生成物にコピーレフトを掛けると §4-5 の AMD IP 部分と整合しないため)。

```
PL bitstream / xclbin 再配布条件

Copyright (c) 2026 Leiden21g. All rights reserved.

1. 本ファイル(kv260_fan_vcu.bit.bin / binary_container_1.bin)は、配布アーカイブの一部として
   バイナリ形式のまま再配布してよい。本告知(THIRD_PARTY_NOTICES.md)を添えること。
2. 改変、リバースエンジニアリング、逆アセンブル、および PL の実体(回路・ネットリスト・ソース)の
   抽出を意図した解析は認めない。
3. 本ファイルは AMD(Xilinx)の IP を含む生成物である。使用・再配布にあたっては AMD の該当する
   ライセンス条件にも従うこと(§4-5)。とくに本ファイルは **AMD(Xilinx)のデバイス(KV260 等)を
   プログラムする目的でのみ**使用・再配布できる(AMD EULA §3(a)(3)C / §4(a))。
4. 無保証。本ファイルの使用から生じるいかなる損害についても著作者は責任を負わない。
```

### 4-5. AMD IP の再配布条件(★条文を確認済 = 2026-09-12)

bit/xclbin には、AMD(Xilinx)がツールに同梱して提供する IP core の生成物が含まれる。その再配布条件を
**AMD の EULA 原文で確認した**(以前の本節は「条文を確認していない」としていた)。

- **参照した条文(確認済)**: Vitis 2025.2 のインストールに含まれる *XILINX, INC. END USER LICENSE AGREEMENT*。
  - `/opt/Xilinx/2025.2/.xinstall/2025.2.1/data/unified_xilinx_eulas_vitis.txt`
    (対象 Software = Vitis Unified Software Platform / Vitis HLS / Vivado ML Edition / Vitis Model Composer。
    末尾の版表記 `2025.09.30`、md5 `0379c6c7491bd0e194ed3bfc2475eb40`、26,651 バイト)
  - 同内容の公開 PDF = **UG1593 Vivado ML End User License Agreement**
    https://download.amd.com/docnav/documents/eula/ug1593_vivado_ml_eulas_2025.2.pdf
  - **確認済**: 本節が引用する §3(a)(3)C・§4(a)・§4(b) は、インストール内の Vitis 変種 / Vivado 変種 /
    Bootgen 変種で**文言が一致する**(差分は対象ツールの一覧・輸出管理条項・末尾の日付だけ)。
    したがって Vivado で bit → `v++` で xclbin → Bootgen で `.bit.bin` のどの経路でも配布条項は同じ。

**§3(a)(3)C Distribution Rights(原文)**

> C.	Distribution Rights.  Licensee may distribute Distributable Components (including when modified
> per Section 3(a)(3)B (Modification Rights) above), provided that either (a) such Distributable
> Components have been converted into a machine executable form, such as in the case of a Core or
> Design Element, a Bitstream, and in the case of a Driver, in compiled object code form; or (b) the
> recipient thereof has a valid license from Xilinx to use the same Software.  In all cases
> Distributable Components shall be distributed only for use to program a Xilinx Device.

要約: AMD 提供の IP core を含む成果物は、**bitstream のような機械実行形式に変換された形であれば第三者に
配布してよい**。ただし配布は「**AMD(Xilinx)デバイスをプログラムする用途に限る**」。

**§4(a) Special Use Restrictions(原文、抜粋)**

> No right is granted hereunder to use the Software or any Bitstream generated by use of the Software
> to program or develop designs for non-Xilinx Devices; …

要約: ツールおよび**その生成 Bitstream** を、AMD 以外のデバイス向けの設計に使う権利は与えられていない。

**§4(b)(i) General Restrictions(原文、抜粋)**

> (i) decompile, translate, reverse-engineer, disassemble, or otherwise reduce to human readable form
> the Software or the data files generated by the Software;

要約: ツールおよび**ツールが生成したデータファイル**のリバースエンジニアリングは許諾されていない。
§4-4 の条項 2(改変・リバースエンジニアリングを認めない)はこれと同じ方向である。

- **本件で使っている AMD IP はすべて「無償同梱(ライセンスキー不要)」の core である(確認済)**。
  §3(a)(3)A は対象を *"a 'Core' … includes only a 'Core' provided by Xilinx in the Software tools listed
  above at no additional charge, and not any fee-based LogiCORE products"* と定めており、本件はこれに該当する。
  確認方法 = (1) キーが要る IP の一覧 `/opt/Xilinx/2025.2/data/ip/core_licenses/{Xilinx.lic,XilinxFree.lic}` に
  使用 IP が**1 つも載っていない**、(2) 使用 IP の `component.xml` に `licenseKey` 要素が**無い**
  (対照実験: キーが要る `cmac_usplus_v3_1` には 4 件ある = この判定に判別力があることを確認済)。
  有償 LogiCORE・評価版(hardware evaluation)ライセンスの IP は**使っていない**
  (傍証: 評価版 bitstream は数時間で停止するが、本構成は長時間の連続配信で動作している)。

- **AMD 自身の配布前例(確認済)**: AMD は Kria KV260 / KR260 用の PL firmware を prebuilt バイナリ
  (`*.bit.bin` / `*.xclbin`)で公開しており、ソースは付けていない(https://github.com/Xilinx/kria-apps-firmware)。
  その条件 `LICENSE-BINARIES` は「**バイナリ形式のみ・無改変での再配布を許す、BSD に似ているが BSD ではない
  独自の許諾**」で、条件は著作権表示の再掲と Xilinx 名の推奨利用の禁止の 2 点である。
  (見解)§4-4 の条件(バイナリのまま再配布可・改変/リバース禁止・無保証)はこれとほぼ同型である。

- **(見解)本 repo の整理**: 以上より、bit/xclbin を「**KV260(AMD Zynq UltraScale+ MPSoC)で使うための
  バイナリ**」として再配布することは AMD の EULA と整合すると理解している。利用者は本ファイルを
  **AMD 以外のデバイスをプログラムする目的で使ってはならない**(§3(a)(3)C / §4(a))。

- **★要確認として残る論点**(いずれも条文を読んだだけでは決まらない。必要なら専門家に確認すること):
  1. EULA が定めるのは **AMD 供給部分の配布**であって、自作 HLS kernel を含む**成果物全体**の条件ではない。
     全体を §4-4 の独自条件で配ることの妥当性は EULA からは出てこない。
  2. 「AMD デバイス上での使用に限る」という用途限定を、受領者に対してどこまで明記する必要があるか。
  3. §4(b)(i) のリバースエンジニアリング禁止を**受領者に引き継がせる義務**が条文上あるか
     (EULA に明示の flow-down 条項は**見つからなかった**が、「無い」と言い切るには読みに依存する)。
  4. ツール同梱の第三者 OSS 集(`Third_Party_Software_EULA.txt`、約 710 万行)は精読していない。
     IP の RTL ではなくツール実行環境の OSS が主と見ているが、未確認。

### 4-6. AGPL-3.0 の対応ソース義務との関係(見解)

(見解)AGPL-3.0 の「対応ソース(Corresponding Source)」の義務は bit/xclbin には及ばないと整理している。理由:

1. bit/xclbin は AGPL 対象物(Ultralytics 由来の重み = `n.q`)の**コードも重みも含まない独立の著作物**である(§4-1、§4-2)。
2. host と kernel は XRT 越しの**別バイナリ**で、リンクによる結合は無い。共有されているのはファイルフォーマット /
   ABI だけである(§4-3)。
3. AGPL 部分(`n.q` 等)と同一の配布アーカイブに入るのは、GPL でいう**「単なる集合(mere aggregation)」**に
   あたると整理できる(§2 の `allegro_dvt.ko`、§3 の `al5e.fw` と同じ扱い)。

★**これは非専門家の整理であり、最終的な法的判断は専門家の確認が望ましい。**

### 4-7. HLS kernel ソースの公開方針(決定済)

- **HLS kernel のソースと Vivado/Vitis の platform 構築手順は公開しない**(本 repo に含まれていない)。
  「PL(bit / xclbin)と `n.q` の再生成は本 repo の範囲外」と [README.md](README.md) に既記であり、本節はそれと整合する。
- §4-6 のとおり、これらは AGPL-3.0 の対応ソースには当たらないと整理している(見解)。
- 利用者は配布アーカイブの bit/xclbin をそのまま使う(再生成は不要)。

## 5. `combined_allegro.dtbo`(PL overlay)

- 自作の device tree overlay。ただし camera / probe / XRT(zocl)部分は AMD のツールが生成した overlay を decompile して
  再構成し、VCU 部分は AMD の VCU binding を元に `allegro,al5e-1.1` binding へ書き換えたもの(開発記録による)。
- **ライセンス**: 本プロジェクトの著作物として **AGPL-3.0**([LICENSE](LICENSE))。
- ★**ソース同梱 = 実施済(2026-09-12)**: ソースは [src/dtbo/combined_allegro.dtso](src/dtbo/combined_allegro.dtso)
  (配布アーカイブでは `src/dtbo/`)。**確認済**: `dtc -@ -I dts -O dtb` で配布 dtbo と **byte-exact に一致**する
  (md5 `9dd5895218d5295bced3c17cf9439a3b` / 11,257 バイト、DTC 1.6.1)。配布アーカイブの生成スクリプトが毎回この
  一致を検査する。
  (以前の本節には「ソースが本 repo に無い」とあったが、**誤り**だった。開発 repo の
  `scripts/fan_platform/combined_vcu_yolo_allegro.dtso` として最初から存在しており、`src/dtbo/` のものは
  そのヘッダコメントを公開向けに書き直しただけで、DT の本体は**バイト同一**である。)
- **AMD 生成 DT を元にした部分について**(見解): 元にしたのは AMD のツール(Vitis / DTG)が**本プロジェクトの
  ハードウェア設計から生成した** device tree であり、ノード名・`compatible` 文字列・レジスタアドレスといった
  binding の記述が主である。**確認済**: AMD のツールが生成した DT(`XSCT 2025.2` 出力)に入っているのは
  「`CAUTION: This file is automatically generated by Xilinx.` + 版と日付」のコメントだけで、
  **著作権表示もライセンス条文も無い**。配布 dtbo 自体にもそうした文字列は無い(コメントは dtb に残らない)。
  **要確認**として残るのはこの点の法的評価(AMD ツールの生成物に AMD の権利が及ぶかどうか)だけで、
  ソースの所在としては解決している。なお bitstream に含まれる AMD IP の再配布条件は別論点(§4-5)。

## 6. MediaMTX

- v1.19.2 `linux_arm64`(https://github.com/bluenviron/mediamtx/releases)。**MIT License**(確認済: 上流 `v1.19.2` の `LICENSE`)。
- 本 repo・配布アーカイブのどちらにも**同梱しない**。利用者が上流から取得して展開先の `home/mediamtx` に置く(手順 = `doc/setup.md` §3-0。
  上流の `checksums.sha256` で検証し、`package/MD5SUMS.txt` の md5 と一致することを確認済)。
- 設定ファイル `package/home/mediamtx_detect.yml` は自作。
- (見解)同梱しないので MIT の表示義務は本 repo には生じない。将来 Release asset に含める場合は上流 `LICENSE` を同梱すること。

## 7. XRT(Xilinx Runtime)

- 2.20.0。board には AMD EDF の feed から `dnf install xrt` で導入する(同梱しない。`doc/setup.md` §2-1)。
- 推論 host(`live/inference_host/`)が `xrt/xrt_kernel.h` 等のヘッダを include し、`libxrt_coreutil` / `libxilinxopencl` に
  動的リンクする。
- **ライセンス(確認済)**: XRT の user space コードは **Apache-2.0**(上流 `LICENSE` の表記)。kernel driver(zocl 等)は GPL-2.0。
  (見解)Apache-2.0 は AGPL-3.0(GPLv3 系)と両立する。

## 8. board 側で dnf 導入するもの・ビルド時に使うもの(同梱しない。参照のみ)

`doc/setup.md` §2-1 で board に導入するもの、`live/build_live.sh` がリンクするもの。いずれも同梱・再配布しない。
ライセンスは各上流の一般的な表記を記したもので、本 repo で個別に条文を確認したものではない(参照のみ)。

| 物 | 使う所 | ライセンス(上流の表記) |
|---|---|---|
| OpenCV 4.9(`libopencv-core409` / `libopencv-imgproc409`) | `camera_preprocess` が動的リンク | Apache-2.0(OpenCV 4.5 以降。GitHub の repo 表示で確認) |
| GStreamer 1.0 / plugins-base / good / bad、gst-rtsp-server | `rtsp_detect_server.py` | LGPL-2.1-or-later(plugin により別ライセンスあり) |
| PyGObject(`python3-pygobject`)、GObject Introspection | 同上 | LGPL-2.1-or-later |
| Python 3 | launcher・web・rtsp のスクリプト | PSF License |
| NumPy | `verify_preprocess_c.py`(PC 側の検証のみ) | BSD-3-Clause |
| glibc / libstdc++ / libgcc(PetaLinux SDK の sysroot) | 全 C/C++ バイナリが動的リンク | LGPL-2.1-or-later / GPL-3.0 + GCC Runtime Library Exception |
| Linux UAPI ヘッダ(`linux/videodev2.h` 等) | `capture_daemon`・`vcu_stream` | GPL-2.0 WITH Linux-syscall-note |
| `devmem2`、`curl` | 補助ツール | 各上流 |

## 9. ソース内に取り込んだ第三者由来のデータ

- **8x8 bitmap font**(`live/inference_host/include/y26_live.h` の `FONT8`): `font8x8_basic`
  (https://github.com/dhepper/font8x8)由来。上流の表記は **Public Domain**(IBM の VGA フォント由来の public domain データを
  元にしたもの)。確認済: 上流 README / ヘッダの `License: Public Domain`。ソース内のコメントにも public domain と記載済。
- **COCO-80 クラス名**(同ファイルの `COCO80`): **出所 = COCO(Common Objects in Context)データセットの
  80 個の物体カテゴリ名**。実体は `"person"`, `"bicycle"`, `"car"`, … という**一般的な英単語 80 個の配列**で、
  検出結果の `cls` index を人間に読める名前へ変換するためだけに使っている(確認済: 実体を目視、
  COCO の標準 80 カテゴリと一致。順序も COCO 標準の並び)。
  この並びは Ultralytics に限らず COCO を扱う実装で広く共通して使われるもので、**Ultralytics 固有の著作物とは
  考えにくい**(見解: 単語の列挙であり、選択・配列もデータセット側で定まっているため創作性は低い)。
  なお同ファイルにはこの表の出所表記が無いので、必要ならコメントで「COCO データセットのカテゴリ名」と
  補うのが親切(★ただし `y26_live.h` は board 配備品と md5 を突き合わせているため、変更は
  `live/MD5SUMS.expect.txt` の更新とセットで行うこと)。
- 上記以外に、`live/` と配布アーカイブ(`home/` を含む)のテキストファイルで、第三者の著作権表記・SPDX 行・
  「上流から移植」等の記述は見つかっていない(確認済: grep。対象 = .c/.cpp/.h/.py/.sh/.env/.yml/.service/.md)。

## 10. AMD から各自入手するもの(同梱しない。参照のみ)

- Kria 汎用 Starter Kit 組込み Linux 2025.2(wic)、K26 Boot FW v1.06、ZynqMP common image の PetaLinux SDK(`sdk.sh`)、
  Vitis / Vivado 2025.2。いずれも AMD のアカウントとライセンスの下で各自入手する(`doc/setup.md` §1・§3-9)。
- ⚠ **訂正(2026-09-12)**: 本節にはかつて「AP1302 の firmware(`ap1302_ar1335_single_fw.bin` 等)は board の rootfs に
  含まれるものを使う」と書いていたが、これは**事実誤認**だった。素の公式 SD イメージには含まれず、AMD EDF 25.11 の dnf feed
  にも無いことが 2026-09-12 の実機検証で判明した(`dnf provides '*/ap1302_ar1335_single_fw.bin'` は No matches)。
  正しい扱いは **§11** に分離して記載した。

## 11. AP1302 カメラ ISP firmware(`ap1302_ar1335_single_fw.bin`)

- **用途**: KV260 の IAS コネクタ **J7** に挿した AP1302 + AR1335 カメラモジュールを使う場合にのみ必要。
  これが `/lib/firmware/` に無いと AP1302 の driver が probe に失敗し(`Direct firmware load ... failed with error -2`)、
  `/dev/media0` が生えない。カメラを使わない構成では不要。
- **本 repo での扱い**: 本 repo・配布アーカイブのどちらにも**同梱しない**(★**2026-09-12 に方針確定**)。
  利用者が上流から各自取得する(手順 = [doc/setup.md](doc/setup.md) §2-2)。
- **上流(確認済)**: **https://github.com/Xilinx/ap1302-firmware** の `main` ブランチ(実体の最終更新コミット
  `3482046f28bfedbb59e3f7321bef4f0b148d0765`、2024-01-25)の `ap1302_ar1335_single_fw.bin`。
  本構成で動作を確認したファイル(79,324 バイト / md5 `23adc4be340a6bbcc4a7ba562c7b2889` /
  sha256 `2dd09e34c68eb2e9ff2b488c9b7fb6d77f4673bff9c1af167d9d466e795ec1c2`)と、上流から実際に取得したファイルが
  **md5 バイト一致**することを確認済。
- **著作権者 / 位置づけ(確認済)**: 著作権者は **ON Semiconductor**(Semiconductor Components Industries, LLC)。
  **AMD(Xilinx)は著作権者ではなく、許諾を受けた再配布者**である(上流 repo は AMD が運用しているが、ライセンスを
  与えているのは ON Semiconductor)。
- **ライセンス(確認済)**: 同 repo の `LICENSE.txt` = 「**ON Semiconductor AP1302 ISP Firmware License Agreement**」。
  OSI 承認のオープンソースライセンスではない専用 EULA で、GitHub のライセンス判定も `NOASSERTION`。主な条件:
  - **バイナリ形式のみ**配布可。ソース形式・RTL 形式での提供は禁止。
  - 再配布するときは、製品に組み込むか、**契約書の写しを同梱**すること。
  - 用途は **ON Semiconductor の AP1302 と組み合わせる目的に限る**(Xilinx SOM Starter Kit での使用は明示的に許諾されている)。
  - 著作権表示の保持。無保証。**High Risk Use**(原子力・航空・軍事・医療 等)向けではない。
  - **§2.1(d)**: 「GPL・LGPL・MPL・Apache その他あらゆるオープンソースライセンスの対象となり得る行為」を禁止。
  - §12 秘密保持。§5.1 契約期間は 3 年(以後自動更新)。
- **同梱しない理由(見解)**: 次の 4 点から、同梱せず利用者取得とするのが安全と判断した。
  1. **§2.1(d) との衝突回避**。本 repo の自作部分は AGPL-3.0 で配布しており、AGPL-3.0 の配布物に同梱すると
     「オープンソースライセンスの対象となり得る行為」に当たると読まれる恐れがある(集合物にすぎないという整理も
     あり得るが、条文の文言が広く、リスクを取る必要がない)。
  2. 再配布するなら **EULA 全文(約 20KB)の同梱**が必要になるが、利用者が上流から 1 コマンドで取得できる以上、不要。
  3. **3 年の契約期間**(§5.1)という不確実性を、本配布物に持ち込まない。
  4. MediaMTX(§6)など、**既に「同梱しない」としている第三者物の扱いと整合**する。
  ★ これは `al5e.fw`(§3)を**同梱している**のと意図的に扱いを変えている。`al5e.fw` の許諾は MIT 型で再配布が
  明示的に許されており、許諾文の同梱だけで条件を満たせるため。
- **入手手順**: [doc/setup.md](doc/setup.md) §2-2(ライセンス条項の取得 → 同意 → コミット permalink で firmware 取得 →
  md5 検証 → `/lib/firmware/` へ配置 → reboot)。
- **要確認**: この `main` 版は、**reboot の約半分で dmesg に firmware CRC mismatch が出る**既知の不安定さがある
  (driver が引き直して撮像自体は正常。[doc/setup.md](doc/setup.md) §3-2 の注記)。同 repo の `xlnx_rel_v2022.1`
  ブランチ版(79,276 バイト / md5 `17d6726a888e8683f6fa5c82af702c28`、AMD smartcam 2022.1 経路用)でこれが改善するかは
  **未検証**。なお版を取り違えると `CRC mismatch: expected 0xa152` で全黒になるため、本構成では `main` 版を使うこと。

---

## 公開前に決めること(要確認のまとめ)

1. ~~`allegro_dvt.ko` の GPL-2.0 対応ソースの提供方法~~ → ★**決定済・実施済(2026-09-12)= 案 A**(§2)。
   repo / 配布アーカイブに [src/allegro-dvt/](src/allegro-dvt/)(上流の特定 + sha256 + patch + `build_allegro_dvt.sh`)を同梱し、
   上流ソースの実体は **ko と同じ Release の別 asset** `allegro-dvt-gpl-src-31626ef92ff1.tar.gz` として並べる。
   ★**残る運用上の義務 = ko を含む release を作るたびに、この source tarball を必ず同じ release に並べること**。
2. ~~`n.q` の AGPL-3.0 対応ソースの範囲(変換ツールを含めるか)~~ → ★**決定済・実施済(2026-09-12)= 同梱する**(§1)。
   [src/nq/](src/nq/) に `.pt` → `n.q` 変換ツールの全ソースを置いた。**確認済**: この dir だけで
   (上流 `.pt` + 同梱 sidecar を入力に)配布 `n.q` を **byte-exact に再生成できる**(`cmp` 差分 0、
   依存は C++17 のみ)。sidecar を作り直す calibration Python(torch + ultralytics)は同梱していない。
3. ~~bit/xclbin のライセンス表明と、自作 HLS kernel ソースを公開するか~~ → ★**決定済(2026-09-12)**:
   bit/xclbin は **AGPL-3.0 ではなく独自条件**で配布(条件文 = §4-4)。**HLS kernel ソースは公開しない**(§4-7、
   [README.md](README.md) の「PL の再生成は範囲外」と整合)。根拠 = bit/xclbin は重みも Ultralytics 由来コードも
   含まず(§4-1・§4-2)、host とは XRT 越しの別バイナリ(§4-3)であり、AGPL 部分とは集合物の関係(§4-6、見解)。
   ★最終的な法的判断は専門家の確認が望ましい。
3-b. ~~bitstream に含まれる **AMD IP の再配布条件**~~ → ★**条文を確認済(2026-09-12)**(§4-5):
   AMD の EULA **§3(a)(3)C** が、同梱 IP core を含む成果物を **bitstream 等の機械実行形式で第三者に配布すること
   を明文で許している**(用途は「AMD デバイスをプログラムすること」に限定)。本件の IP はすべて無償同梱
   (ライセンスキー不要)であることも確認済。AMD 自身も KV260 用 PL firmware をバイナリのみで配布している前例がある。
   ★**残る論点は 4 つ**(成果物全体の条件は EULA では決まらない / 用途限定を受領者にどう明記するか /
   リバースエンジニアリング禁止の flow-down 義務の有無 / ツール同梱 OSS 集の未精読)= §4-5 末尾。
4. ~~`combined_allegro.dtbo` のソース(`.dtso`)を同梱するか~~ → ★**決定済・実施済(2026-09-12)= 同梱する**(§5)。
   [src/dtbo/combined_allegro.dtso](src/dtbo/combined_allegro.dtso)。`dtc` で配布 dtbo を **byte-exact に再生成**できることを
   確認済(md5 `9dd58952`)で、配布アーカイブの生成スクリプトが毎回この一致を検査する。
   ★この項目に書いていた「ソースが本 repo に無い」は**誤り**だった(最初から `scripts/fan_platform/` に存在した)。
   残るのは、元にした AMD ツール生成 DT の法的評価(§5 の最終項)だけ。
5. ~~配布する推論 host ELF(`c6b433a0`)が公開ソースからビルドできない~~ → ★**解決済(2026-09-12)**:
   **配布物の推論 host ELF を公開ソース版に統一した**(v1.0 = `3f586c9d`、v1.1 = `5446141e`)。本 repo の `live/inference_host/`
   (簡素化版)を `SOURCE_DATE_EPOCH` pin でビルドすると **byte 再現する**ことを実測で確認し
   (`live/MD5SUMS.expect.txt` / `y26_live_recipe.env` の `EXPECT_HOST_ELF_MD5`)、release 生成時に
   その実体を差し込む(`scripts/package_public_release.sh` §4c)。期待値と違う ELF を差し込もうとすると
   生成が止まるので、「公開ソースからビルドできない ELF」を再び配ることはない。
   ★**board 実機への差し替えも完了(2026-09-12)**: 置換前後の canary がいずれも **GOLD 6/6 一致**
   (139.19 → 139.56 ms/img)、live 10 分 6000 枚で **fps 9.88〜9.92**、無接触 14 点の md5 不変。
   = **配布物・公開ソース・board 実機の 3 者が一致した**(検証の詳細な記録は開発側にあり、公開物には含まれない)。
6. AGPL-3.0 §13(ネットワーク越しの利用者へのソース提供)→ ★**実装済(2026-09-12)**:
   視聴ページ(`:8890` = `live/web/web_mode_server.py`)に**対応ソースの在処を常時表示する**ようにした。
   URL は env **`Y7_SOURCE_URL`** で与える(launcher `run_live_rtsp_stream.sh` が webmode unit へ渡す)。
   未設定のときはリンクを出さず「AGPL-3.0(対応ソース = 配布物同梱の LICENSE / README 参照)」と表示する。
   ★**公開 repo の URL は確定済** = https://github.com/Leiden21g/kv260detector(2026-09-12、25c で作成)。
   `Y7_SOURCE_URL` にこれを設定すればリンクが出る(board の再配備は不要)。改変して配信する場合は自分の公開先を設定すること。
   なお `:8889` は MediaMTX 自身のページなので、本プロジェクトが表示を足せるのは `:8890` だけ。
   ★**board 実機への配備も完了(2026-09-12)**: 実測で URL 未設定時は
   `AGPL-3.0(対応ソース = 配布物同梱の LICENSE / README 参照)`、`Y7_SOURCE_URL` 設定時は
   そのリンクが出ることを確認した(**board 再配備なしに切り替わる**ことも実証済)。
7. ~~AP1302 の firmware(§11)を同梱しない方針で確定してよいか~~ → ★**決定済(2026-09-12、ユーザ判断)= 同梱しない**。
   利用者が上流(Xilinx/ap1302-firmware)から取得し、ライセンスに同意する形とする(手順 = [doc/setup.md](doc/setup.md) §2-2)。
   これにより EULA 全文(約 20KB)の同梱も、§2.1(d)(オープンソースライセンスの対象となり得る行為の禁止)との
   関係整理も**不要**になる。★同梱へ方針変更する場合は、その 2 点を先に解決すること。

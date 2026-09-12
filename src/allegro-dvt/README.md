# `allegro_dvt.ko` の対応ソース(GPL-2.0)

配布アーカイブ(`y7-public-<tag>.tar.gz`)に含まれる `home/allegro_dvt.ko` は **GPL-2.0 のカーネル
モジュール**である。本ディレクトリは、その **対応するソース(Corresponding Source)** への到達手段
一式 — 上流ソースの特定(repo・コミット・ファイル・sha256)、本プロジェクトの変更(patch)、
ビルドスクリプト — を提供する。

本ディレクトリの内容(`build_allegro_dvt.sh` / patch)は、上流と同じ **GPL-2.0 (only)** で提供する。
本 repo 全体の AGPL-3.0 はこのモジュールには適用されない(詳細 = [../../THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) §2)。

> **English summary.** `home/allegro_dvt.ko` in the release archive is a GPL-2.0 Linux kernel module:
> the mainline `allegro-dvt` V4L2 encoder driver (Copyright (C) 2019-2020 Pengutronix,
> Michael Tretter <kernel@pengutronix.de>), taken from Xilinx `linux-xlnx` commit `31626ef92ff1`,
> with one local fix for the KV260 VCU (see the patch in this directory). This directory is the
> Corresponding Source: upstream identification with checksums, the patch, and the build script.
> Everything here is GPL-2.0 (only). The project's AGPL-3.0 licence does not apply to this module.

---

## 1. 中身

| ファイル | 内容 |
|---|---|
| `0001-allegro-dvt-kv260-disable-encoder-buffer.patch` | 本プロジェクトによる変更(1 箇所)。`patch -p1`(kernel tree のルートから)または `patch -p5`(driver ディレクトリ内)で適用できる unified diff |
| `upstream_sources.sha256` | 上流 10 ファイルの sha256(取得物の同一性確認用) |
| `build_allegro_dvt.sh` | 取得 → 照合 → patch → クロスビルド → vermagic 確認を行うスクリプト |

上流ソースの**実体**は、配布ポリシー上 2 経路のいずれでも入手できる:

1. **同じ GitHub Release の asset** `allegro-dvt-gpl-src-31626ef92ff1.tar.gz`
   (上流 10 ファイル + patch 適用済み `allegro-core.c` + 本ディレクトリ一式。オフラインで完結する)
2. **上流から直接**
   https://github.com/Xilinx/linux-xlnx/tree/31626ef92ff1/drivers/media/platform/allegro-dvt
   (`build_allegro_dvt.sh` が既定でこちらを取得し、`upstream_sources.sha256` と照合する)

## 2. 上流の特定

| 項目 | 値 |
|---|---|
| 上流 repo | https://github.com/Xilinx/linux-xlnx |
| コミット | `31626ef92ff1`(board の kernel `6.12.40-xilinx-g31626ef92ff1` と同一コミット) |
| ディレクトリ | `drivers/media/platform/allegro-dvt/` |
| ファイル(10) | `allegro-core.c` `allegro-mail.c` `allegro-mail.h` `nal-h264.c` `nal-h264.h` `nal-hevc.c` `nal-hevc.h` `nal-rbsp.c` `nal-rbsp.h` `Makefile` |
| ライセンス | 各ファイル先頭 `SPDX-License-Identifier: GPL-2.0` / `MODULE_LICENSE("GPL")` |
| 著作権 | Copyright (C) 2019(-2020) Pengutronix, Michael Tretter \<kernel@pengutronix.de\> |
| kconfig | `CONFIG_VIDEO_ALLEGRO_DVT=m` |
| モジュール名 | 上流どおり `allegro`(board 上のファイル名だけ `allegro_dvt.ko` に改めて配置している) |

## 3. 本プロジェクトによる変更(1 箇所)

`allegro-core.c` の `allegro_encoder_buffer_init()` に、`VCU_MEMORY_DEPTH == 0` のとき `-ENODEV`
を返す分岐を追加した。

理由: KV260 の PL に載せた VCU IP は `ENC_BUFFER_EN=false` で合成されており、`VCU_MEMORY_DEPTH`
が 0 を返す。上流のコードはそのまま進んで `buffer->size = color_depth * 32 * memory_depth = 0`
を計算し、`create_channel` が `encoder_buffer_size=0` を MCU に送るため、channel 作成が
`resource unavailable (8e)` で失敗して encode ができない。`-ENODEV` を返すと
`has_encoder_buffer` が false のままになり、`create_channel` は encoder buffer を無効(`-1`)
として送る。encoder buffer は DDR 帯域削減のための任意機能なので、無効でも H.264/HEVC の
encode 自体は正常に動作する。

適用方法:

```bash
# kernel tree のルートから
patch -p1 < 0001-allegro-dvt-kv260-disable-encoder-buffer.patch

# driver のファイルだけを集めたディレクトリ内で
patch -p5 < 0001-allegro-dvt-kv260-disable-encoder-buffer.patch
```

## 4. ビルド

```bash
KSRC=<sysroot>/usr/lib/modules/6.12.40-xilinx-g31626ef92ff1/build \
CROSS_COMPILE=aarch64-linux-gnu- \
  bash build_allegro_dvt.sh
```

必要なもの(いずれも利用者側で用意する):

- **aarch64 クロスツールチェイン**。配布 ko は gcc 13.4.0(AMD Vitis/PetaLinux 2025.2 同梱の
  `aarch64-linux-gnu-`)でビルドした。
- **board kernel `6.12.40-xilinx-g31626ef92ff1` の build tree**(headers-only でよいが `.config` と
  `Module.symvers` が必要)。AMD の KV260 用 platform の sysroot 配下
  `usr/lib/modules/6.12.40-xilinx-g31626ef92ff1/build` がこれに当たる。あるいは linux-xlnx
  `31626ef92ff1` を board と同じ `.config` で `make modules_prepare` したツリーでもよい。
- `curl`(オフラインなら `SRCDIR=` で上流ソースのディレクトリを渡せば不要)、`make`、`patch`。

スクリプトが行う非自明なこと:

- **`vermagic` の固定**。クロス環境では `scripts/setlocalversion` が git 由来の `-g31626ef92ff1`
  suffix を付けられず、vermagic が board と食い違って `insmod` が弾かれる。
  そのため `include/generated/utsrelease.h` の `UTS_RELEASE` を board の値で直接書く。
- **build tree の複製**。ドライブ mount 上の tree は `scripts/` の exec bit を失っていることが
  あるため、native fs に複製して `chmod +x` し直す。

★**バイナリの md5 一致は保証しない**。コンパイラ版や build tree の差でバイナリは変わる(機能は同じ)。
配布 ko は md5 `73ceb61fb3eff5174634f717fb066275` / sha256
`080cf20f6d823aceb3f836463ae015b10dc5dffa79dd5fe056902460880ce62d` / 104,992 バイト。

## 5. あわせて必要なもの(本ディレクトリの対象外)

このモジュールだけでは encode できない。VCU の MCU 上で動く microcode が別途必要で、
配布アーカイブの `firmware/al5e.fw` / `firmware/al5e_b.fw` がそれである
(上流 = Xilinx/vcu-firmware タグ `release-2019.2` の `1.0.0/lib/firmware/`。
ライセンスは Allegro DVT2 の許諾 = [../../THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) §3)。

board 上で driver を使うための device tree(`allegro,al5e-1.1` ノード、`xlnx,vcu-settings` syscon
など)は PL overlay 側にあり、そのソースは [../dtbo/](../dtbo/) にある。

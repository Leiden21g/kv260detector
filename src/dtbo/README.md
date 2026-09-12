# `combined_allegro.dtbo` のソース

配布アーカイブ(`y7-public-<tag>.tar.gz`)に含まれる `firmware/combined_allegro.dtbo` /
`home/combined_allegro.dtbo` は、本ディレクトリの [`combined_allegro.dtso`](combined_allegro.dtso)
をコンパイルしたものである。本プロジェクトの著作物で、ライセンスは AGPL-3.0
([../../LICENSE](../../LICENSE) / [../../THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) §5)。

## 再生成(byte-exact)

```bash
dtc -@ -I dts -O dtb -o combined_allegro.dtbo combined_allegro.dtso
md5sum combined_allegro.dtbo
# 9dd5895218d5295bced3c17cf9439a3b   (11,257 バイト)
```

`dtc` は AMD Vitis / PetaLinux 同梱のもの(**DTC 1.6.1** で上の md5 を確認済)か、distro の
`device-tree-compiler` パッケージのもの。`-@`(`__symbols__` を出す)は overlay に必須。

★dtc の版が違うと出力バイトは変わりうる(意味は同じ)。配布 dtbo と md5 を一致させたい場合は
DTC 1.6.1 を使うこと。

## この overlay が何を expose するか

単一 bitstream `kv260_fan_vcu.bit.bin` 上の 4 系統を **1 枚の overlay で同時に**出す。
別々の overlay にすると、VCU 単独 overlay では zocl が出ず host が `No such device index 0`
になる、といった具合に片方しか動かない。

| fragment | 対象 | 中身 |
|---|---|---|
| `fragment@0` | `&fpga_full` | `firmware-name = "kv260_fan_vcu.bit.bin"`、fabric clock(`clocking0/1`、100MHz)、`afi0`、`misc_clk_0`、**`zyxclmm_drm`(zocl = 推論 kernel の XRT device)** |
| `fragment@1` | `&amba` | probe(`axi_intc@80000000` / `axi_fifo_mm_s@80010000`)、camera(`i2c@80030000` + mux + AP1302 `isp@3c` / `csiss@80020000` / `fb_wr@80040000` / `isp_vcap_csi`)、VCU(`vcu@80140000` + `al5e@80109000` + `vcu_settings@80141000` syscon) |

## encoder node が mainline allegro-dvt 向けである点

`al5e@80109000` は AMD の proprietary driver 向けではなく、**mainline の `allegro-dvt` driver**
([../allegro-dvt/](../allegro-dvt/))が要求する binding に合わせてある:

| 項目 | 値 | 理由 |
|---|---|---|
| `compatible` | `allegro,al5e-1.1` | mainline driver の of_match |
| `reg` / `reg-names` | `regs`(0x80109000, 0x1000)+ `sram`(0x80100000, 0x9000) | mainline は 2 領域を名前で引く |
| `clock-names` | `core_clk`, `mcu_clk` | 同 driver の `clk_get` 名 |
| `assigned-clock-rates` | `<667000000 444000000>` | VCU core / MCU |
| `xlnx,vcu-settings` | `<&vcu_settings>`(syscon) | **無いと `allegro_runtime_resume` が `ERR_PTR` を `regmap_read` して oops する** |

## board への配置

`install.sh` が `/lib/firmware/combined_allegro.dtbo` と、dfx-mgr 用の
`/lib/firmware/xilinx/yolov7vcu/yolov7vcu.dtbo` へ置く。
★**dtbo を差し替えたら reboot が必要**(overlay ディレクトリの `rmdir` は dfx-mgr を wedge させる。
[../../doc/setup.md](../../doc/setup.md) §3-2)。

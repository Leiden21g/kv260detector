# KV260 YOLO26n ライブ検出 Web 配信

AMD Kria **KV260** Starter Kit と AP1302 カメラモジュール(KV260 付属 IAS モジュール)で、カメラ映像を
**PL(FPGA)上の YOLO26n(640×640)**で推論し、検出枠を重ねた映像を **VCU の H.264** で符号化して
RTSP で出し、MediaMTX 経由でブラウザ(WebRTC / HLS)へ配信する一式。重みは Ultralytics の YOLO26n を
本 PL 向けに量子化したもの(`n.q`)。到達点はブラウザで `http://<board>:8889/detect` に検出オーバレイ付きの
ライブ映像(約 9.5 fps)が見える状態(`doc/setup.md`)。

```
カメラ(AP1302)→ capture_daemon → camera_preprocess → PL 推論(YOLO26n)→ 検出 overlay
  → vcu_stream(VCU H.264)→ rtsp_detect_server.py(RTSP :8554)→ MediaMTX(WebRTC :8889 / HLS :8888)
                                                     web_mode_server.py(視聴ページ :8890)
```

## 構成

| dir | 役割 | 詳細 |
|---|---|---|
| [`live/`](live/) | board 上で動くプログラムの**ソース**(capture / preprocess / 推論 host / vcu_stream / rtsp / web / launcher)と、aarch64 クロスビルド(`build_live.sh`) | [live/README.md](live/README.md) |
| [`doc/`](doc/) | 起動 SD の準備から配信の起動・停止・確認までの手順 | [doc/setup.md](doc/setup.md) |

**本 repo にはソースと手順だけを置く。** KV260 へ転送する配信一式(バイナリ + launcher + fan 制御 +
配置スクリプト `install.sh`)は repo に含めず、**配布アーカイブ**として別に配る(下の「配布物」)。

## 前提

- Kria KV260 Starter Kit + AP1302 カメラモジュール(IAS0 = J7)。
- AMD 公式の Kria 汎用 Starter Kit 組込み Linux 2025.2(kernel `6.12.40-xilinx-g31626ef92ff1`)、XRT 2.20.0。
  同梱の `allegro_dvt.ko` はこの kernel 専用(vermagic 一致が必須)。
- PL(bit / xclbin)・`n.q`・推論 host は**セットで一致**している必要がある(単体差替不可。配布アーカイブ同梱の
  `README.md`「版の整合」)。

## クイックスタート

手順の全体と注意点は **[doc/setup.md](doc/setup.md)**。流れだけ示す:

1. 起動 SD を用意する(AMD 公式 2025.2 wic、Boot FW v1.06 以上)— §1
2. 初回ログイン、ssh 公開鍵、パスワード不要な sudo、`dnf install xrt gstreamer1.0 …` — §2
3. 配布アーカイブ `y7-public-<tag>.tar.gz` を取得して展開し、MediaMTX v1.19.2 `linux_arm64` を上流から
   取得して展開先の `home/mediamtx` に置く — §3-0
4. 展開先で `md5sum -c MD5SUMS.txt` → `bash install.sh <user>@<board>` で配置 → reboot — §3-1
5. `bash ~/vcu_enc_setup.sh`(reboot 後の PL bring-up)→ 任意で canary 確認 — §3-2, §3-3
6. `run_live_rtsp_stream.sh` と `browser_stream_mediamtx.sh` で配信開始、`http://<board>:8889/detect` を開く — §3-4
7. 停止は必ず stop フラグ経由(走行中の推論 host を kill すると PL の CU が壊れ、復旧は reboot のみ)— §3-6

再ビルドは [live/README.md](live/README.md)(PetaLinux 2025.2 common SDK の sysroot が要る)。
PL(bit / xclbin)と `n.q` の再生成は本 repo の範囲外。

## 配布物(board へ配置する一式)

board へ配置する一式は **repo に含めない**。`.tar.gz` 1 本にまとめ、GitHub Release の asset として配る予定
(★この asset はまだ公開していない。以下はその前提で書いた手順)。

- ファイル名は `y7-public-<tag>.tar.gz`。`<tag>` には採用ビルドの **xclbin の md5 先頭 8 桁**が入る
  (本書が対象とする採用ビルドは xclbin `14279337` = `y7-public-14279337.tar.gz`)。
- 中身: 配置バイナリ一式(`firmware/` = bit / xclbin / `al5e*.fw` / dtbo、`home/` = launcher・`allegro_dvt.ko`・
  `vcu_stream`・推論 host ELF・`n.q` / `x.bin`、`fan/` = fan 制御 unit)+ 配置スクリプト **`install.sh`** +
  同梱 **`README.md`**(中身・配置先・版の整合)+ **`MD5SUMS.txt`** + **`LICENSE`** + **`THIRD_PARTY_NOTICES.md`** +
  **`src/`** = 同梱バイナリの**対応ソース**(`src/allegro-dvt/` = `allegro_dvt.ko` の GPL-2.0 対応ソース、
  `src/dtbo/` = `combined_allegro.dtbo` の `.dtso`。いずれも repo の [src/](src/) と同じもの)。
- 使い方: 展開 → `home/mediamtx` を上流から取得して置く → `md5sum -c MD5SUMS.txt` →
  `bash install.sh <user>@<board>`。詳細は [doc/setup.md](doc/setup.md) §3-0〜§3-1。
- 版の正は同梱の `MD5SUMS.txt`。PL(bit / xclbin)・`n.q`・推論 host ELF はセットで一致していること。
- **MediaMTX は同梱も再配布もしない**。上流の GitHub Release から取得する(`doc/setup.md` §3-0)。
- `capture_daemon` / `camera_preprocess` / `vcu_stream` は本 repo の `live/` から再ビルドすると asset 同梱の
  配備品と byte 一致する(`live/MD5SUMS.expect.txt`)。PL(bit / xclbin)と `n.q` の再生成は本 repo の範囲外。
- `allegro_dvt.ko`(GPL-2.0)と `combined_allegro.dtbo` の**対応ソース**は [src/](src/) にある。
  `combined_allegro.dtbo` は `dtc` だけで byte-exact に再生成できる(`src/dtbo/`)。`allegro_dvt.ko` の上流
  ソース実体は、ko と同じ Release の別 asset `allegro-dvt-gpl-src-31626ef92ff1.tar.gz` として配る
  (オフラインでビルドできる形。`src/allegro-dvt/README.md`)。

## ライセンス

- 本 repo の自作部分は **GNU Affero General Public License v3.0(AGPL-3.0)** — [LICENSE](LICENSE)。
- 重み(`n.q` ほか)は **Ultralytics の YOLO26n**(AGPL-3.0)の量子化派生物。YOLO26 のモデルと学習済み重みは
  Ultralytics(https://github.com/ultralytics/ultralytics)による。本 repo は Ultralytics の公式物ではなく、
  Ultralytics・AMD とは無関係。AGPL-3.0 の条件に従えない商用利用には Ultralytics の Enterprise License が関係しうる
  (https://www.ultralytics.com/license)。
- 第三者物(`allegro_dvt.ko` = GPL-2.0、VCU firmware `al5e*.fw` = Allegro DVT2 の許諾、MediaMTX = MIT、XRT = Apache-2.0、
  bit/xclbin に含まれる AMD IP など)はそれぞれのライセンスに従う。一覧・入手元・義務と未確認事項は
  **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**。

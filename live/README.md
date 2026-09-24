# live — KV260 Web 配信パイプラインの board 側プログラム(ソース)

カメラ(AP1302)→ 前処理 → PL 推論(YOLO26n、640×640)→ 検出 overlay → VCU H.264 → RTSP → MediaMTX(WebRTC/HLS)
を board 上で回す **ホスト側プログラム一式のソース**。board へ配置するバイナリ一式は repo には無く、配布アーカイブ
`y7-public-<tag>.tar.gz`(GitHub Release の asset)として配る(`../README.md`「配布物」)。本 dir はその再ビルド用。

```
カメラ /dev/video0 ──capture_daemon──▶ /dev/shm/live.nv12(640² NV12、seq 付き atomic 差替)
                                          │
                     camera_preprocess --daemon ──▶ /tmp/lv/live.bin(量子化済 HWC int16、3.3MB)
                                          │
   yololoop(推論 host ELF、1 起動 2000 枚 ping-pong streaming) ──▶ /tmp/overlay_live.nv12(検出枠つき)
                                          │
                      vcu_stream ──▶ /tmp/h264.fifo(H.264 Annex-B、VCU /dev/video1)
                                          │
              rtsp_detect_server.py(gst-rtsp-server、ENC=vcufifo) ──▶ rtsp://board:8554/detect
                                          │
                mediamtx(再エンコード無しリマックス) ──▶ http://board:8889/detect (WebRTC) / :8888 (HLS)
                web_mode_server.py ──▶ http://board:8890/ (視聴ページ + 全景/切り抜き/pan ボタン)
```

| ディレクトリ | 中身 | board 上の配置 | 役割 |
|---|---|---|---|
| `capture/capture_daemon.c` | C | `~/yolov7/capture_daemon` | V4L2 で 3840×2160 を取り込み 640² へ変換して `/dev/shm/live.nv12` を publish。`--pub-square 640 --mode-file /tmp/lv/capmode`(全景/切り抜き/pan)、`--fps` で write 間引き |
| `preprocess/camera_preprocess.cpp` | C++/OpenCV | `~/yolov7/camera_preprocess` | NV12 → letterbox → 量子化 HWC16。`--daemon` で常駐(seq 差分で間引き)。同 dir の `verify_preprocess_c.py` + `camera_preprocess.py`(python 参照実装)+ `chw_load_ref.py` は x86 での byte-exact 検証用(board 不要。下記) |
| `vcu_stream/vcu_stream.cpp` + `include/vcu_enc.h` | C++ | `~/vcu_stream` | overlay NV12 を VCU(allegro-dvt、V4L2 M2M)で H.264 化し fifo へ |
| `rtsp/rtsp_detect_server.py` | python/gst | `~/yolov7/rtsp_detect_server.py` | fifo の H.264 を RTSP(8554)で配信(appsrc bridge) |
| `web/web_mode_server.py`, `web/geo640.py` | python | `~/yolov7/` | :8890 の視聴ページと表示モード切替(`/tmp/lv/ovmode`, `/tmp/lv/capmode`) |
| `launcher/run_live_rtsp_stream.sh` | bash | `~/run_live_rtsp_stream.sh` | 上記を systemd transient unit(capdlive/ppdaemon/yololoop/vcustream/rtspdetect/webmode/probedrain)として起動する本体 |
| `launcher/browser_stream_mediamtx.sh` | bash | `~/browser_stream_mediamtx.sh` | MediaMTX を mediamtx unit で起動(WebRTC :8889 / HLS :8888) |
| `launcher/vcu_enc_setup.sh` | bash | `~/vcu_enc_setup.sh` | reboot 後の PL bring-up(allegro ko + combined_allegro overlay = VCU+camera+yolo 統合 bit を焼く) |
| `launcher/probe_drain_daemon.sh` | bash | `~/probe_drain_daemon.sh` | probe FIFO 定期 flush(re-arm wedge 予防) |
| `inference_host/` | C++ | `~/yolov7/yolov7_host_overlap_camlive_geo640x640_ovl` | PL 推論 host(XRT)。★2026-09-10 に**配信専用へ簡素化 + 改名して追跡に復帰**: `src/y26_live.cpp` 1 本 + `include/y26_live.h` 1 本(旧 2 ソース + 16 ヘッダ 12,411 行 → 1,483 行)。`y26_live_recipe.env` が -D 群の正(下記) |

## ★推論 host — 配信専用へ簡素化して追跡に復帰(2026-09-10)

2026-09-08 に一度 `.gitignore` で追跡外にした(元の実装は 12,411 行あり、Web 配信専用の公開 repo には過大だったため)。
**2026-09-10 に「Web 配信で実際に通る経路」だけへ削り、board 実機で byte-exact を確認したうえで追跡に戻した。**

| | 旧 | 新 |
|---|---|---|
| ソース | `src/host.cpp` 869 行 + `src/tasks.cpp` 2,104 行 | **`src/y26_live.cpp` 778 行のみ**(2 本を統合し改名) |
| ヘッダ | 16 本(うち実使用 8 本 = 5,900 行) | **`include/y26_live.h` 1 本 = 705 行** |
| 外部依存 | PetaLinux SDK + **Vitis include(ap_int.h)** | **SDK だけ**(`GMEM_T` を 64B POD 化して ap_uint を排除) |
| ELF `.text` | 150,706 B | **63,658 B**(−58%) |

落としたのは live で一度も通らない経路: 静的 batch 検証 / `diff_tensor` / self-consistency(Add・Concat・
MaxPool・Resize・Split)/ PSA golden 突合 / HB ring / PROBE63 / DBG_L50 / `LAYER_MAP` / `DUMP_*` /
`SEED_RUN` / `GATE_A_TEST` / `STDIN_DAEMON` / `PHASE3_PROF` の計測一式 / SERIAL 経路 / 設計A'(`NQ_INPUT_ADDR`)/
`BUF_RING` / in-host v4l2 capture / in-host VCU encode / `P5CLS_FLOAT` / mid-chain seed。
本番 launcher が必ず立てる env(`OVERLAY_DEFER` `VCU_KEEP_P4` `GO_FLUSH=cvac`)は分岐ごと畳んだ。
⚠ **`FILELIST` と `DUMP_HEADS_PF` は必須**(無いと起動時 abort)。
★`DUMP_HEADS_NOFILE` の分岐だけは**残した**: head の `<out>_L<id>.bin` 書き出しは
`canary_golden.sh` が 6 head の md5 を GOLD と突合する唯一の出口で、簡素化 host の byte-exact 検証に要る。

### board 実測(2026-09-10、全段 PASS)

簡素化 host の canary が **GOLD 6/6 完全一致**(`d5053fa6 d4f8cc60 de3383a1 1c5155a5 13a60293 f30905c4`、v1.0 の n.q 3a83aeb5 での値、
selfcons 6/6)= 推論結果は 1 bit も変わっていない。**137.82 ms/img(旧 140.67 から −2.0%)**、
live 20 分で **fps 9.90〜9.95**(旧 9.63〜9.87)、8 unit active、`:8889/detect/`=200、`:8890`=200、
NRestarts 0、事後 canary も GOLD 一致。

挙動差は 3 点。①最終層出力の per-image dump(`/tmp/lv/o`)を廃止 = 毎フレームの device→host sync 1 回と
ファイル書込 1 回が消えた(検出には未使用)。**上の −2.0% はこれが出所**。②`GO_FLUSH` 未指定時の既定が
evict → cvac。③`VCU_KEEP_P4` 未指定でも P4 cls を殺さない。②③は本番 launcher が両方とも明示指定するので
live の挙動は不変。


追跡するのは capture / preprocess / vcu_stream / rtsp / web / launcher の 6 段 + `inference_host/` の
4 ファイル(`src/y26_live.cpp` / `include/y26_live.h` / `y26_live_recipe.env` / `geo640.env`)と `build_live.sh`。
`out/` 等のビルド生成物は `.gitignore` 対象。

⚠ 簡素化前の `inference_host/` は kernel(HLS)側の宣言・native forward・検証経路まで抱えた開発用の実装
そのものだった。**簡素化後は別物**で、それらは本 repo には無い(Web 配信に要らないため落とした)。
推論 host のバイナリ(配備済み ELF)は配布アーカイブの `home/yolov7/` に入っており、配信を動かすだけならビルドは要らない。

## ビルド(PC / WSL、aarch64 クロス)

board に compiler は無い。PetaLinux 2025.2 common SDK の sysroot(`sdk.sh -y -d <SDK> -p` で展開)を使う。

```bash
SDK=<SDK 展開先> bash build_live.sh all      # → out/(配信 3 本 + host)
#   個別: bash build_live.sh capture|preprocess|vcu_stream|host
```

`inference_host/` は追跡下にあるので clone すれば host も一緒にビルドされる。dir を消した環境では
`all` は配信 3 本だけを作り host は skip する(`build_live.sh` の do_host が dir の有無で分岐)。

- ★2026-09-10: 推論 host の **Vitis include 依存は解消**(`VITIS_INC` 不要)。SDK sysroot だけで 4 本とも通る。
- `camera_preprocess` は `-lopencv_imgproc -lopencv_core` の最小リンク(全 .so リンクは起動 0.66s に劣化)。
- ★2026-09-10: **決定論ビルド**。推論 host の `__DATE__`/`__TIME__` は `y26_live_recipe.env` の `SOURCE_DATE_EPOCH_DEFAULT` で固定されるので、同じ SDK なら md5 は毎回同じ。期待値は `MD5SUMS.expect.txt`(ビルド末尾で自動突合)。
  `BASE_DEF` のうち生成コードに効くのは幾何/配置の 7 個だけ(実測: 残りを外しても `.text` md5 が一致)。
  ★2026-09-12: **配布物の推論 host ELF は本 dir のビルド結果(`3f586c9d`)に統一した**
  (release 生成時に差し込む = `scripts/package_public_release.sh` §4c)。これで配布バイナリと
  公開ソースが一致する(AGPL-3.0 の対応ソース)。それ以前に配布/配備していた `c6b433a0` は
  簡素化前(host.cpp 63c7657b / tasks.cpp 98a74755)のもので、簡素化版とはバイナリが違った
  (**推論結果は board canary で GOLD 一致を確認済**)。
- ★2026-09-24(v1.1): 推論 host の PE 重み(PSA の位置項)を int16 へ詰める処理を**飽和**にした
  (旧版は範囲外の 2 値で符号が反転していた)。PE ファイルが無い region は警告を出す。配布 ELF = `5446141e`。
  同時に n.q を 2c6a15bc へ更新したので canary GOLD は `0cd128f1 ec9ef88d 4b33a187 28ec7e43 9bf53b2a ef000a69` に変わる。
- 本 dir から再ビルドした配信 3 本(`capture_daemon` 7a323056 / `camera_preprocess` b2a09f41 / `vcu_stream` 01c52c91)は
  **配備品と byte 一致**(2026-09-10 確認。期待 md5 と経緯は `MD5SUMS.expect.txt`。`vcu_stream` は同日に board 側を
  再現可能な動的リンク版へ差し替えて一致させた)。推論 host も 2026-09-12 に配布物側を本 dir の
  ビルド結果へ統一したので、**4 本すべてが本 dir から再現できる**(board 実機の ELF を
  `3f586c9d` へ差し替えるには canary が要る = 別作業)。配備品そのものは配布アーカイブに入っている。

## 前処理の x86 検証(`preprocess/verify_preprocess_c.py`)

board 不要。合成 NV12 を作り、`camera_preprocess.cpp` を x86 でビルドしたバイナリの出力と、
同 dir の python 参照実装(`camera_preprocess.py`)の golden とを byte 比較する。

```bash
g++ -O2 -o /tmp/camera_preprocess preprocess/camera_preprocess.cpp $(pkg-config --cflags --libs opencv4)
python3 preprocess/verify_preprocess_c.py --bin /tmp/camera_preprocess --hwc16
```

- 必要なもの: python3 + NumPy + `cv2`、x86 の OpenCV(4.6 で確認)、同 dir の `camera_preprocess.py`。
  `--hwc16`(量子化済 HWC int16 経路も比較)を使うときは同 dir の `chw_load_ref.py` も要る(それ以外では未使用)。
- 幾何は引数で渡す(`--cam W H stride` / `--size H W`)。`--size` 省略時は 384×640。
  `camera_preprocess.py` は同 dir に `geo640.py`/`geo640.env` があればそこから既定幾何を取るが、無くても動く(任意依存)。

## 起動・停止(要点。手順の全体は `../doc/setup.md`)

```bash
ssh <user>@<board> 'bash ~/vcu_enc_setup.sh'                                   # reboot 後 1 回
ssh <user>@<board> 'cd ~ && HOST=./yolov7_host_overlap_camlive_geo640x640_ovl bash ~/run_live_rtsp_stream.sh'
ssh <user>@<board> 'bash ~/browser_stream_mediamtx.sh'
# 停止は必ず stop フラグ経由(走行中 host の kill は PL を壊す)
ssh <user>@<board> 'touch /tmp/lv/stop; for w in $(seq 1 200); do pgrep yolov7_host >/dev/null || break; sleep 2; done;
  for u in yololoop capdlive ppdaemon vcustream rtspdetect webmode mediamtx; do sudo systemctl stop $u; done; rm -f /tmp/lv/stop'
```

幾何・fps 蓋・gate は `~/yolov7/geo640.env` が唯一の源(launcher が source する)。直書きしない。

## ライセンス

AGPL-3.0([../LICENSE](../LICENSE))。取り込んだ第三者物(8x8 font 等)とリンク先ライブラリ(XRT / OpenCV 等)は
[../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。

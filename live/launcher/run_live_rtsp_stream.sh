#!/bin/bash
# run_live_rtsp_stream.sh — [board] camera → preprocess → HW 推論(full-HW head, L154 も HW)
#   → overlay → VCU H.264 → fifo → RTSP。
#   ★1 起動 = N 枚 ping-pong streaming(mid-run refill)= host 再起動コストが消える。
#   PC 視聴: vlc rtsp://<board>:8554/detect  /  ブラウザ: http://<board>:8890/(:8889 を埋め込み)
#
# ★仕組み(2026-07-12 実機確定):
#   - ~~in-host の LIVE_SHM 取込みは壊れている~~ → **2026-07-17b 解決**(真因 = host が camera_preprocess へ
#     geometry を渡さず 720p frame を 1080p として読み rc=1 → x.bin fallback。live_input_not_reaching_kernel §8)。
#     現在は in-host LIVE_SHM でも外部 preprocess+FILELIST と byte-exact 一致する。
#     ただし本 script は **FILELIST 方式のまま**(pp を別プロセスにする方が PS 負荷を分散でき、実績もある)。
#   - **FILELIST の全行を同一パス /tmp/lv/live.bin に向ける**。host の refill(image k+2)は
#     FILELIST のパスを **その時点で** chw_load する → 外部 preprocess デーモンが同パスを atomic に
#     差し替え続ければ、1 起動のまま毎フレーム最新カメラ画像が入る = 真の連続 streaming。
#   - kernel/host とも BATCH_IN_BASE=165888(層 peak 162512 の上)= ping-pong slot0 の破壊が根治済。
#   実証: N=20 で 11-20 枚目も 1 周目と byte-exact 一致 / PL 243.8ms/枚。
#   (真因と実証の記録は本 script の外に置いてある)
#
# 前提: 配布物の PL(bit / xclbin)・n.q・推論 host ELF が**セットで一致**していること
#   (版の正 = 配布アーカイブ同梱の MD5SUMS.txt)、および VCU bring-up 済(vcu_enc_setup.sh)。
# env: NIMG(1起動の枚数, 既定 400) / DURATION(秒, 既定 1800) / FPS(既定 5) / CONF(既定 0.25)
#      HOST(推論 host ELF。既定 = 下記)
#      Y7_SOURCE_URL(視聴ページ :8890 に出す対応ソースの URL。AGPL-3.0 §13。未設定ならリンクを出さない)
set +e
# ★ユーザ非依存化(2026-09-06): board ユーザは petalinux でも amd-edf でもよい。実行ユーザの home を基準にする。
BU="$(id -un)"; BH="/home/$BU"
cd "$BH/yolov7" || exit 1
FIFO=/tmp/h264.fifo; OVL=/tmp/overlay_live.nv12; sudo rm -f "$OVL"
# ── ★geo640 H4(2026-08-28): 推論入力幾何(NETH×NETW)の唯一の源 = ~/yolov7/geo640.env ──
#   無ければ現行 384×640。CAM_W/H・WEB_W/H・ppdaemon の --size・FPS/CAP_FPS の蓋を全てここから導出する(直書き禁止)。
for _g in "${GEO640_ENV:-}" ./geo640.env ./scripts/geo640.env; do [ -n "$_g" ] && [ -f "$_g" ] && { . "$_g"; echo "[live] geo640.env: $_g"; break; }; done
Y26_NETH="${Y26_NETH:-384}"; Y26_NETW="${Y26_NETW:-640}"
echo "[live] geo: NETW x NETH = ${Y26_NETW}x${Y26_NETH} (host ELF はこの幾何の -D で組んだもの(recipe の GEO640_NETH/NETW)を使うこと)"
FPS="${FPS:-${GEO640_FPS_CAP:-8}}"; CONF="${CONF:-0.25}"; DURATION="${DURATION:-0}"; NIMG="${NIMG:-2000}"
# ★★2026-08-02: DURATION の既定を 1800 → **0(無期限)** に変更した。
#   理由 = 満了は「yololoop だけ落ちて下流 5 unit が凍結画を配信し続ける」半停止を生み、
#   これが 3 回再発した(2026-08-01 に 2 回 / 08-02 に 1 回 = 約 3 時間放置)。
#   runbook に手順を書いても、一度きりの中継を仕掛けても、6 時間ごとに同じ穴が開いた。
#   ⇒ **満了そのものを無くす**のが恒久策。DURATION=<秒> を明示すれば従来どおり有期。
#   ★停止は従来どおり **stop フラグ経由**(/tmp/lv/stop)。mid-run kill は厳禁のまま。
# ★既定は配備されている推論 host ELF(2026-09-12 修正。旧既定 ./yolov7_host_n200 は board に無く
#   env 未指定だと起動できなかった)。別名を置いて試すときは HOST= で上書きする。
HOST="${HOST:-./yolov7_host_overlap_camlive_geo640x640_ovl}"
# ── OVERLAY_DEFER(overlap host 専用)──
#   PHASE3_OVERLAP build の host(*_ovl)では head snapshot + deferred decode で PS 仕事を
#   PL と重畳し 2.92→5.74fps(2026-07-15 実測)。SERIAL build では無視される(host 側 guard 済)。
#   overlap host を HOST に指定した時は既定 ON。
#   ★req8(2026-08-29): 640 ELF 名が *_ovl* に該当せず DEFER が付かず overlay 133ms が直列だった(b3 phase D)。
#     env OVERLAY_DEFER 未指定なら「host 名 *_ovl* または geo640.env の幾何が 384x640 以外」で 1(384 *_ovl* は不変)。
case "$HOST" in *_ovl*) OVERLAY_DEFER="${OVERLAY_DEFER:-1}" ;; esac
if [ "${Y26_NETH}x${Y26_NETW}" != "384x640" ]; then OVERLAY_DEFER="${OVERLAY_DEFER:-1}"; fi
OVLDEF_ENV=""; [ -n "${OVERLAY_DEFER:-}" ] && OVLDEF_ENV="OVERLAY_DEFER=$OVERLAY_DEFER"
# ★req8: ppdaemon の publish 形式。geo640.env の GEO640_PP_HWC16=1(または env PP_HWC16=1)で camera_preprocess が
#   host chw_load と byte 同一の量子化済 HWC int16(H*W*4*2B)を live.bin に書く(host はサイズ自動判別・変換無し)。未指定は従来 float。
# ── ★req10 gate 恒久化(2026-08-29、640 live の A/B で G5 PASS)──
#   640 live で A/B PASS した host env gate を launcher 既定にする。**基準幾何 384x640(geo384.env / env 無し)では
#   従来どおり全て未設定 = 展開結果不変**。幾何≠384x640 のとき既定 GO_FLUSH=cvac / SNAP_THREADS=2 / PP_HWC16=1
#   (geo640.env の GEO640_GO_FLUSH / GEO640_SNAP_THREADS / GEO640_HEAD_SKIP_P4CLS / GEO640_PP_HWC16 があればそれが勝つ、
#   起動 env が最優先 = 個別 off は GO_FLUSH=evict / SNAP_THREADS=1 / PP_HWC16=0)。host は getenv で読む。
#   ★HEAD_SKIP_P4CLS は本 script が host に VCU_KEEP_P4=1 を付けるため live では host 側 gate で常に自動無効(no-op)
#     = 既定は空(渡さない)、env で明示された時だけ透過する。実効 gate は GO_FLUSH/SNAP_THREADS の 2 つ。
if [ "${Y26_NETH}x${Y26_NETW}" != "384x640" ]; then
  GO_FLUSH="${GO_FLUSH:-${GEO640_GO_FLUSH:-cvac}}"; SNAP_THREADS="${SNAP_THREADS:-${GEO640_SNAP_THREADS:-2}}"
  HEAD_SKIP_P4CLS="${HEAD_SKIP_P4CLS:-${GEO640_HEAD_SKIP_P4CLS:-}}"; PP_HWC16="${PP_HWC16:-${GEO640_PP_HWC16:-1}}"
fi
# ★2026-09-04: GO_EARLY 恒久配線(headpin_live2_640_report_20260903.md 実証済 gate)。
#   geo640.env の GEO640_GO_EARLY があればそれ、無ければ起動 env の GO_EARLY を通す(既定は空=従来動作)。
GO_EARLY="${GO_EARLY:-${GEO640_GO_EARLY:-}}"
GATE_ENV=""
[ -n "${GO_FLUSH:-}" ] && GATE_ENV="$GATE_ENV GO_FLUSH=$GO_FLUSH"
[ -n "${SNAP_THREADS:-}" ] && GATE_ENV="$GATE_ENV SNAP_THREADS=$SNAP_THREADS"
[ -n "${HEAD_SKIP_P4CLS:-}" ] && GATE_ENV="$GATE_ENV HEAD_SKIP_P4CLS=$HEAD_SKIP_P4CLS"
[ -n "${GO_EARLY:-}" ] && GATE_ENV="$GATE_ENV GO_EARLY=$GO_EARLY"
PP_HWC16="${PP_HWC16:-${GEO640_PP_HWC16:-0}}"
if [ "$PP_HWC16" = 1 ]; then PP_OUT_OPT="--out-hwc16"; else PP_OUT_OPT="--out"; fi
echo "[live] geo: gates GO_FLUSH=${GO_FLUSH:-unset} SNAP_THREADS=${SNAP_THREADS:-unset} HEAD_SKIP_P4CLS=${HEAD_SKIP_P4CLS:-unset}(VCU_KEEP_P4=1 の live では no-op) PP_HWC16=${PP_HWC16:-0} OVERLAY_DEFER=${OVERLAY_DEFER:-unset} GO_EARLY=${GO_EARLY:-unset}"
# ── カメラ画質(AP1302 ISP)──
#   既定(exposure=12 FULL_AUTO / gamma=4096)は逆光シーンで前景が黒潰れ 30-40% し、検出が 0 件になる。
#   AE=9(AUTO_BV_EXP_TIME: 露光時間優先)+ gamma 3x で影を持ち上げると、黒潰れ 0% かつ
#   白飛びも 4.5%→2.9% に改善する(実測)。saturation は gamma 持ち上げによる退色の補償。
#   ★exposure は露出値でなく AE_CTRL のモード番号。有効値は 0-3(manual)と 9/10/11/12(auto)のみ。
#     4-8 は未定義=不定動作なので使わないこと(ap1302.c: ap1302_set_exposure(…, s32 mode))。
CAM_EXPOSURE="${CAM_EXPOSURE:-9}"; CAM_GAMMA="${CAM_GAMMA:-12288}"; CAM_SATURATION="${CAM_SATURATION:-6144}"
# ── キャプチャ解像度 ──
#   推論入力は letterbox 384x640 に縮小されるので、1080p の画素の大半は捨てられている。
#   720p にしても検出はほぼ不変(同じ 16:9 = letterbox 数式が同一)で、capture_daemon の
#   frame write が 6.22MB→2.76MB(2.25 分の 1)になり CPU が大きく空く。
#   AP1302 の scaler は 24x16〜4224x4092 を自由に出せる(PL 側に scaler は無い)。
#   ★stride は PL frmbuf の quirk(NV12 で 2×幅)で決まるので、推測せず実 frame サイズから逆算する。
#
#   ★AP1302 の source pad は「capd が完全に停止して stream が stall した後」でないと変わらない。
#     pkill -9 直後に media-ctl -V を叩くと **rc=0 を返すのに効かず** 1920x1080 に居座り、
#     frmbuf は 1080p サイズの frame を出し続ける(= stride 逆算が壊れて画が破綻する)。
#     ap1302_s_stream() は 0→1 遷移でのみ ap1302_configure() を呼ぶため(ap1302.c)。
#     → 下の設定ブロックで **--get-v4l2 で source pad を検証し、通らなければ retry/abort** する。
#   PL frmbuf の stride は 3840 固定(1080p 由来)。720p でも 3840 のままなので、
#   frame = 3840 × 720 × 3/2 = 4147200B。stride は推測せず実 frame サイズから逆算する。
#   ★geo640: NETH==NETW(640×640 化)のときは CAM=NETW×NETH(AP1302 が推論幾何を直接出す)、それ以外は従来 720p。
if [ "$Y26_NETH" = "$Y26_NETW" ]; then CAM_W="${CAM_W:-$Y26_NETW}"; CAM_H="${CAM_H:-$Y26_NETH}"
else CAM_W="${CAM_W:-1280}"; CAM_H="${CAM_H:-720}"; fi
# ★GEO640_CAPD_PUB(2026-09-04、web「切り抜き」ボタン): 取込 pad 3840×2160、publish は capd が 640² へ変換。
#   下流は publish 幾何 = CAM_W/H(640²)のまま無変更。⚠2880×2160 は frame 供給停止 — 使うな。
CAPTURE_W=$CAM_W; CAPTURE_H=$CAM_H; CAPD_PUB_ARGS=""; WEBMODE_ENV=""
if [ "${GEO640_CAPD_PUB:-0}" = 1 ] && [ "$Y26_NETH" = "$Y26_NETW" ]; then
  CAPTURE_W=3840; CAPTURE_H=2160
  CAPD_PUB_ARGS="--pub-square $Y26_NETW --mode-file /tmp/lv/capmode"
  WEBMODE_ENV="--setenv=CAPMODE=1 --setenv=CAPPAN_MAXX=$(( (CAPTURE_W - Y26_NETW) / 2 )) --setenv=CAPPAN_MAXY=$(( (CAPTURE_H - Y26_NETH) / 2 ))"
  # ★ここで WEBMODE_ENV を**上書き**するので、Y7_SOURCE_URL の追記はこの if の後で行う(下記)。
  mkdir -p /tmp/lv; [ -s /tmp/lv/capmode ] || echo wide > /tmp/lv/capmode
fi
# ★AGPL-3.0 §13(2026-09-12): 視聴ページ(:8890)に対応ソースの URL を出す。
#   未設定なら web 側はリンクを出さず「同梱 LICENSE 参照」の表示だけになる。
#   ★capmode 分岐が WEBMODE_ENV を上書きするので、**追記はこの位置**でなければ消える。
[ -n "${Y7_SOURCE_URL:-}" ] && WEBMODE_ENV="$WEBMODE_ENV --setenv=Y7_SOURCE_URL=$Y7_SOURCE_URL"
# ★geo640: 配信幾何 = 推論幾何(NETW×NETH)。vcustream/rtspdetect の 640 384 直書きを置換
WEB_W=$Y26_NETW; WEB_H=$Y26_NETH
# ── capd の書き出しレート ──
#   capd の CPU は「1 frame ごとの 4MB write + rename ×2」が主。capture path は帯域律速
#   (~36-37 MB/s)なので **解像度を下げても fps が上がって同じだけ食う**(720p で capd 単独 96-99%)。
#   下流は 検出 ~2.5fps / RTSP 5fps しか要らないので、**write を間引く**のが唯一の実効レバー。
#   DQBUF/QBUF は毎フレーム続く(止めると V4L2 キューが詰まり CSI が wedge する)。
CAP_FPS="${CAP_FPS:-${GEO640_CAP_FPS:-9}}"
# ── VCU の GOP(= IDR 間隔、フレーム単位)──
#   旧既定 25 は FPS=5 では **IDR が 5 秒に 1 枚**しかなく、ブラウザ(WebRTC/HLS)は視聴開始時に
#   最大 5 秒 黒画面になる。5 なら 1 秒。1080p@5fps では帯域増もわずか。
VCU_GOP="${VCU_GOP:-5}"
LOG=/tmp/live_rtsp_stream.log; : > "$LOG"; exec >>"$LOG" 2>&1
# ── zocl kds_poll の busy-poll 緩和 ──
#   既定 kds_interval=0 は CU 走行中 kds_poll が 1 core を 100% 消費する busy-poll。
#   1(ms)で 100%→~53% に半減し、Δ/frame はむしろ改善(252→243ms, 2026-07-15 実測。
#   競合減の方が 1ms poll 遅延より効く)。sysfs は reboot で 0 に戻るため毎回設定する。
KDS_IV="${KDS_IV:-1}"
echo "$KDS_IV" | sudo tee /sys/devices/platform/fpga-region/fpga-region:zyxclmm_drm/kds_interval >/dev/null 2>&1 \
  && echo "[live] kds_interval=$KDS_IV (busy-poll 緩和)"
echo "[live] $(date +%T) xclbin=$(md5sum /lib/firmware/xilinx/yolov7/binary_container_1.bin|cut -c1-8) nq=$(md5sum data26_live/n.q|cut -c1-8) host=$(md5sum $HOST|cut -c1-8)"
[ -e /dev/video1 ] || { echo "[live] ✗ /dev/video1 無(VCU 未 bring-up)"; exit 2; }

# ★★yololoop を素で systemctl stop すると **走行中の host を SIGTERM で殺す**。これは
#   「run 中の kill 厳禁(CU wedge → reboot)」そのもので、**CU 劣化の主因**(2026-07-14 実証:
#   解像度切替のたびに再起動していたら劣化間隔が 3h15m→70分→34分→8分 と縮んだ)。
#   → stop フラグを立てて **現在の host run が自然終了するのを待ってから** unit を止める。
mkdir -p /tmp/lv
if [ "$(systemctl is-active yololoop 2>/dev/null)" = active ]; then
  touch /tmp/lv/stop
  echo "[live] 走行中の host の自然終了を待つ(mid-run kill は CU を壊す)…"
  # ★pgrep -f は使うな: 起動時の `env HOST=./yolov7_host_ibrt2 …` が自分のコマンド行に含まれ
  #   **自分自身にマッチ**して永久に抜けない。comm(プロセス名)マッチにすること。
  for w in $(seq 1 400); do pgrep "yolov7_host" >/dev/null || break; sleep 2; done
  if pgrep "yolov7_host" >/dev/null; then
    echo "[live] ⚠ host が 800s 経っても終わらない → 強制停止する。**CU 劣化の可能性あり=golden canary を確認せよ**"
  else
    echo "[live] ✔ host は自然終了した(CU 安全)"
  fi
fi
for u in capdlive ppdaemon vcustream rtspdetect yololoop probedrain webmode; do sudo systemctl stop $u 2>/dev/null; sudo systemctl reset-failed $u 2>/dev/null; done
rm -f /tmp/lv/stop
# ★capture_daemon が完全に死ぬまで待つ。生き残っていると sensor が streaming のままになり、
#   後段の media-ctl -V(解像度変更)が rc=0 のまま無視される。パターンは [d] で自マッチを避ける。
sudo pkill -f "capture_[d]aemon" 2>/dev/null
for w in $(seq 1 40); do pgrep -f "capture_[d]aemon" >/dev/null || break; sleep 0.25; done
pgrep -f "capture_[d]aemon" >/dev/null && { sudo pkill -9 -f "capture_[d]aemon"; sleep 1; }
sleep 1
mkdir -p /tmp/lv

# ── camera + capture daemon ──
# ★entity 名 "ap1302.<i2c bus>-003c" の bus 番号は kernel/DT で変わる(PetaLinux 2025.2=4、AMD EDF 25.11=3)。
#   ハードコードせず media-ctl -p から検出する(2026-09-06 EDF 対応)。
AP1302_ENT=""
for i in $(seq 1 60); do AP1302_ENT=$(media-ctl -d /dev/media0 -p 2>/dev/null | grep -oE "ap1302\.[0-9]+-003c" | head -1); [ -n "$AP1302_ENT" ] && break; sleep 0.5; done
[ -n "$AP1302_ENT" ] || { echo "[live] ✗ media0 に ap1302.*-003c entity が無い"; exit 5; }
echo "[live] ap1302 entity=$AP1302_ENT"
# ★source pad を設定 → get で検証(rc=0 を信用しない)。効くまで最大 5 回。
for try in 1 2 3 4 5; do
  sudo media-ctl -d /dev/media0 -V "\"$AP1302_ENT\":2 [fmt:VYYUYY8_1X24/${CAPTURE_W}x${CAPTURE_H}]" 2>/dev/null
  GOT=$(media-ctl -d /dev/media0 --get-v4l2 "\"$AP1302_ENT\":2" 2>/dev/null | grep -o '[0-9]*x[0-9]*' | head -1)
  [ "$GOT" = "${CAPTURE_W}x${CAPTURE_H}" ] && break
  echo "[live] media-ctl 未反映(try $try: got=$GOT) → retry"; sleep 1
done
[ "$GOT" = "${CAPTURE_W}x${CAPTURE_H}" ] || { echo "[live] ✗ AP1302 source pad が ${CAPTURE_W}x${CAPTURE_H} にならない (got=$GOT)"; exit 5; }
v4l2-ctl -d /dev/video0 --set-fmt-video=width=$CAPTURE_W,height=$CAPTURE_H,pixelformat=NV12 2>/dev/null
v4l2-ctl -d /dev/video0 \
  --set-ctrl=exposure="$CAM_EXPOSURE",gamma="$CAM_GAMMA",saturation="$CAM_SATURATION" 2>/dev/null
echo "[live] cam: ${CAM_W}x${CAM_H} $(v4l2-ctl -d /dev/video0 --get-ctrl=exposure,gamma,saturation 2>/dev/null | tr '\n' ' ')"
# ★前回 run の live.nv12 を必ず消す。残っていると「ファイルがある」で即 break し、**旧解像度の
#   frame サイズから stride を誤算出**する(720p 化で踏んだ: 旧 1080p の 6220800B から stride=5760
#   と誤算 → camera_preprocess にゴミ stride を渡して検出 300 件暴走)。
sudo rm -f /dev/shm/live.nv12 /dev/shm/live.nv12.seq /dev/shm/live.nv12.tmp
# ★A-13(2026-07-19 本番投入): --mmap = 常時 mmap slot の RENAME_EXCHANGE publish で
#   capd CPU 1/3.4(20260718e §1)。reader 契約不変(<out> は常に完結した通常ファイル)。
#   capture_daemon は --mmap 対応版(旧 capture_daemon_a13)であること。
sudo systemd-run --unit=capdlive --collect ./capture_daemon --dev /dev/video0 --w $CAPTURE_W --h $CAPTURE_H \
  --fps "$CAP_FPS" --out /dev/shm/live.nv12 --mmap $CAPD_PUB_ARGS >/dev/null 2>&1
for w in $(seq 1 200); do [ -s /dev/shm/live.nv12 ] && break; sleep 0.1; done
[ -s /dev/shm/live.nv12 ] || { echo "[live] ✗ capd が frame を出さない"; exit 6; }

# ★実 frame サイズから stride を逆算(NV12: size = stride × H × 3/2)。
#   PL frmbuf の stride は 3840 固定 → 1080p: 6220800B / 720p: 4147200B。どちらも stride=3840。
FRAMESZ=$(stat -c%s /dev/shm/live.nv12 2>/dev/null)
CAM_STRIDE=$(( FRAMESZ / (CAM_H * 3 / 2) ))
[ "$CAM_STRIDE" -lt "$CAM_W" ] && { echo "[live] ✗ stride 異常: frame=$FRAMESZ stride=$CAM_STRIDE < W=$CAM_W"; exit 4; }
echo "[live] capd seq=$(cat /dev/shm/live.nv12.seq 2>/dev/null) frame=${FRAMESZ}B stride=${CAM_STRIDE}"

# ── probe FIFO drain(必須) ──
sudo systemd-run --unit=probedrain --collect bash -c \
  'if [ -x /usr/sbin/devmem ]; then while true; do /usr/sbin/devmem 0x80010018 32 0x000000A5; sleep 1; done; else exec python3 '"$BH"'/probe_drain.py; fi' >/dev/null 2>&1

# ── ★前処理デーモン: 最新カメラ frame を /tmp/lv/live.bin へ atomic に差し替え続ける ──
#   ★A-15 フリーラン抑制(2026-07-17b): 従来は while true で **同じ frame を延々と再前処理**していた
#   (capd は --fps=$CAP_FPS で間引くのに pp は無制限 = 新 frame 1 枚あたり数回~十数回の無駄打ち)。
#   capd は frame の rename **後**に <out>.seq を atomic 更新する(capture_daemon.c:154-157)ため、
#   「seq が変わった = 新 frame が既に置換済み」を reader が信頼できる。これは capd 側が最初から
#   reader の新 frame 検知用に用意していた仕組み(capture_daemon.c:9)= 消費側が未実装だっただけ。
#   ⚠ .seq が無い/読めない時は従来どおり毎回処理する(= 旧動作に安全に fallback)。
#   ⚠ 待ちは sleep 0.02(=最悪 20ms の追加遅延)。capd 既定 6fps(167ms 間隔)に対し十分小さい。
#   ★#17(2026-09-04)常駐 daemon 版への切替。camera_preprocess が `--daemon` を持つなら、
#   bash ループ + 毎 frame exec + cp をやめ、**1 プロセス常駐**にする(board 実測の内訳:
#   exec 固定費 14.4% / cp 10.0% / float 中間 = ppdaemon 1 core のうち約 0.31 core)。
#   seq 差分間引き・publish 順(f.nv12 を先)・出力 byte は従来と同一(daemon 側で同じ式)。
#   PP_DAEMON=0 で従来ループへ明示的に戻せる。バイナリが未対応なら自動で従来ループ。
PP_DAEMON="${PP_DAEMON:-${GEO640_PP_DAEMON:-auto}}"
PPD_OK=0
if [ "$PP_DAEMON" != 0 ] && (cd $BH/yolov7 && ./camera_preprocess 2>&1 | grep -q -- '--daemon'); then PPD_OK=1; fi
if [ "$PPD_OK" = 1 ]; then
  echo "[live] pp: 常駐 daemon 版を使用(--daemon。PP_DAEMON=0 で従来ループ)"
  sudo systemd-run --unit=ppdaemon --collect $BH/yolov7/camera_preprocess --daemon \
    --nv12 /dev/shm/live.nv12 --seq /dev/shm/live.nv12.seq --bg-out /tmp/lv/f.nv12 \
    "$PP_OUT_OPT" /tmp/lv/live.bin \
    --width "$CAM_W" --height "$CAM_H" --stride "$CAM_STRIDE" --size "$Y26_NETH" "$Y26_NETW" >/dev/null 2>&1
else
sudo systemd-run --unit=ppdaemon --collect bash -c '
cd '"$BH"'/yolov7
last=""
while true; do
  seq=""; read -r seq < /dev/shm/live.nv12.seq 2>/dev/null
  if [ -n "$seq" ] && [ "$seq" = "$last" ]; then sleep 0.02; continue; fi   # 新 frame 無し=何もしない
  last="$seq"
  cp /dev/shm/live.nv12 /tmp/lv/f.nv12.tmp 2>/dev/null
  ./camera_preprocess --nv12 /tmp/lv/f.nv12.tmp '"$PP_OUT_OPT"' /tmp/lv/live.bin.tmp \
      --width '"$CAM_W"' --height '"$CAM_H"' --stride '"$CAM_STRIDE"' --size '"$Y26_NETH"' '"$Y26_NETW"' >/dev/null 2>&1 \
    && { mv -f /tmp/lv/f.nv12.tmp /tmp/lv/f.nv12; mv -f /tmp/lv/live.bin.tmp /tmp/lv/live.bin; }
done' >/dev/null 2>&1
fi
for w in $(seq 1 100); do [ -s /tmp/lv/live.bin ] && break; sleep 0.1; done
echo "[live] pp daemon: live.bin $(stat -c%s /tmp/lv/live.bin 2>/dev/null) bytes (publish=$( [ "$PP_HWC16" = 1 ] && echo "hwc16 期待 $(( Y26_NETH * Y26_NETW * 8 ))B" || echo "float 期待 $(( 3 * Y26_NETH * Y26_NETW * 4 ))B" ), OVERLAY_DEFER=${OVERLAY_DEFER:-unset})"

# ── FILELIST: 全 N 行を同一の live.bin に向ける(refill が毎回最新を読む) ──
[ -s /tmp/lv/ovmode ] || echo wide > /tmp/lv/ovmode
: > /tmp/lv/list.txt
for i in $(seq 1 "$NIMG"); do echo "/tmp/lv/live.bin /tmp/lv/o" >> /tmp/lv/list.txt; done

# ── 初回 overlay(vcu_stream 起動に必要) ──
sudo env OVERLAY_WEB=1 OVERLAY_MODE_FILE=/tmp/lv/ovmode OVERLAY_BG_PAIR=1 PERSIST_NIMG=2 STREAM_NIMG_RT=2 GOAXIS_EN=1 PINGPONG_EN=1 $OVLDEF_ENV $GATE_ENV \
  FILELIST=/tmp/lv/list.txt DUMP_HEADS_PF=131,136,140,145,149,154 DUMP_HEADS_NOFILE=1 \
  OVERLAY_NV12=/dev/shm/live.nv12 OVERLAY_OUT="$OVL" OVERLAY_W=$CAM_W OVERLAY_H=$CAM_H \
  BOX_SCALES=2048,2048,1024 CLS_SCALES=512,128,128 CONF="$CONF" VCU_KEEP_P4=1 \
  timeout --kill-after=8 300 "$HOST" ./data26_live 0 155 2>&1 | grep -a 'OVERLAY:' | tail -1
[ -s "$OVL" ] || { echo "[live] ✗ overlay 未生成"; exit 3; }
echo "[live] overlay: $(stat -c%s "$OVL")B (期待 $(( WEB_W * WEB_H * 3 / 2 ))B = packed ${WEB_W}x${WEB_H})"

# ── vcu_stream + RTSP ──
[ -p "$FIFO" ] || { rm -f "$FIFO"; mkfifo "$FIFO"; }
sudo systemd-run --unit=vcustream --collect $BH/vcu_stream "$OVL" $WEB_W $WEB_H "$FIFO" "$FPS" 3000000 "$VCU_GOP" /dev/video1 >/dev/null 2>&1
sudo systemd-run --unit=rtspdetect --collect --uid=$BU --gid=$BU \
  --setenv=ENC=vcufifo --setenv=H264_FIFO="$FIFO" --setenv=FPS="$FPS" --setenv=WIDTH=$WEB_W --setenv=HEIGHT=$WEB_H \
  python3 $BH/yolov7/rtsp_detect_server.py >/dev/null 2>&1
sudo systemd-run --unit=webmode --collect --uid=$BU --gid=$BU $WEBMODE_ENV \
  python3 $BH/yolov7/web_mode_server.py >/dev/null 2>&1

# ── ★検出ループ: 1 起動 = NIMG 枚 streaming(host 起動コストは NIMG 枚に償却) ──
# ★ループ条件: DURATION=0/inf なら無期限。それ以外は従来どおり経過秒で打ち切る。
case "$DURATION" in
  0|inf|infinite) LOOPCOND='true';                                            DURTXT='無期限(満了なし)';;
  *)              LOOPCOND='[ $(( $(date +%s) - T0 )) -lt '"$DURATION"' ]';    DURTXT="${DURATION}s で自然終了";;
esac
echo "[live] yololoop: $DURTXT / Restart=on-failure"
# ★Restart は **on-failure** であって always ではない。
#   always にすると stop フラグでの**正常終了まで再起動**されてしまい、
#   「走行中 host の自然終了を待ってから畳む」= mid-run kill 回避の手順が機能しなくなる
#   (再起動 → フラグを見て即終了 → 再起動 … で start rate limit に当たって failed になる)。
#   on-failure なら crash/OOM だけ拾い、stop フラグと systemctl stop は素通しになる。
sudo systemd-run --unit=yololoop --collect -p Restart=on-failure -p RestartSec=10 bash -c '
cd '"$BH"'/yolov7
T0=$(date +%s); it=0; frames=0
while '"$LOOPCOND"'; do
  [ -f /tmp/lv/stop ] && { echo "[yololoop] stop フラグ検知 → 次の host は起動しない"; break; }
  it=$((it+1))
  D=$(env OVERLAY_WEB=1 OVERLAY_MODE_FILE=/tmp/lv/ovmode OVERLAY_BG_PAIR=1 PERSIST_NIMG='"$NIMG"' STREAM_NIMG_RT='"$NIMG"' GOAXIS_EN=1 PINGPONG_EN=1 '"$OVLDEF_ENV"' '"$GATE_ENV"' \
    FILELIST=/tmp/lv/list.txt DUMP_HEADS_PF=131,136,140,145,149,154 DUMP_HEADS_NOFILE=1 \
    OVERLAY_NV12=/dev/shm/live.nv12 OVERLAY_OUT='"$OVL"' OVERLAY_W='"$CAM_W"' OVERLAY_H='"$CAM_H"' \
    BOX_SCALES=2048,2048,1024 CLS_SCALES=512,128,128 CONF='"$CONF"' VCU_KEEP_P4=1 \
    timeout --kill-after=8 600 '"$HOST"' ./data26_live 0 155 2>&1 | grep -aco "OVERLAY:")
  frames=$((frames+D))
  echo "run $it t=$(( $(date +%s) - T0 ))s frames=$frames (+$D)"
done
echo "[yololoop] 終了 it=$it frames=$frames"' >/dev/null 2>&1

sleep 30
echo "[live] units: capd=$(systemctl is-active capdlive) pp=$(systemctl is-active ppdaemon) vcustream=$(systemctl is-active vcustream) rtsp=$(systemctl is-active rtspdetect) loop=$(systemctl is-active yololoop)"
# ★board 自身のアドレスを出す(2026-09-12: 開発機の固定 IP 直書きをやめた)。
_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"; : "${_IP:=<board>}"
echo "[live] RTSP: rtsp://$_IP:8554/detect   Web: http://$_IP:8890/"
echo "[live] DONE $(date +%T)"

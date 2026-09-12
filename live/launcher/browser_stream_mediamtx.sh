#!/bin/bash
# browser_stream_mediamtx.sh — [board] 既存の検出 RTSP を **ブラウザで見える形**に再発行する。
#
#   仕組み: MediaMTX(静的 Go バイナリ 1 個)が rtsp://127.0.0.1:8554/detect を読み、
#           **WebRTC(WHEP)と HLS へリマックス**する。★再エンコードしない = CPU をほぼ食わない
#           (H.264 Baseline のまま素通し)。既存の RTSP 配信・VLC 視聴はそのまま並存する。
#
#   視聴:   WebRTC  http://<board>:8889/detect   ← 推奨(遅延 ~1s、内蔵プレイヤー)
#           HLS     http://<board>:8888/detect   ← フォールバック(遅延 数秒)
#
#   ★前提: run_live_rtsp_stream.sh を **VCU_GOP=5**(既定)で起動していること。
#     GOP=25(旧既定)だと FPS=5 では IDR が 5 秒に 1 枚しかなく、ブラウザは視聴開始時に
#     最大 5 秒 黒画面になる。
#
#   env: MTX(既定 /home/petalinux/mediamtx) / SRC(既定 rtsp://127.0.0.1:8554/detect)
#        BOARD_IP(WebRTC の ICE host candidate に載せる。既定は自動検出)
set +e
BU="$(id -un)"; BH="/home/$BU"   # ★ユーザ非依存(petalinux / amd-edf)
MTX="${MTX:-$BH/mediamtx}"
SRC="${SRC:-rtsp://127.0.0.1:8554/detect}"
BOARD_IP="${BOARD_IP:-$(ip -4 route get 1.1.1.1 2>/dev/null | grep -o 'src [0-9.]*' | cut -d' ' -f2)}"
CFG=$BH/mediamtx_detect.yml

[ -x "$MTX" ] || { echo "✗ mediamtx が無い: $MTX"; exit 1; }

# ★mediamtx 自身の RTSP サーバは切る(既存の検出 RTSP と 8554 が衝突するため)。
cat > "$CFG" <<EOF
logLevel: info
rtsp: no
rtmp: no
srt: no

hls: yes
hlsAddress: :8888
hlsVariant: fmp4
hlsAlwaysRemux: yes

webrtc: yes
webrtcAddress: :8889
webrtcLocalUDPAddress: :8189
webrtcAdditionalHosts: [${BOARD_IP}]

api: no
metrics: no

paths:
  detect:
    source: ${SRC}
    sourceOnDemand: no
EOF

sudo systemctl stop mediamtx 2>/dev/null; sudo systemctl reset-failed mediamtx 2>/dev/null
sudo systemd-run --unit=mediamtx --collect --uid=$BU --gid=$BU \
  "$MTX" "$CFG" >/dev/null 2>&1

for w in $(seq 1 40); do
  if command -v curl >/dev/null; then curl -sf -o /dev/null "http://127.0.0.1:8889/detect" && break
  else python3 -c "import urllib.request,sys; urllib.request.urlopen('http://127.0.0.1:8889/detect',timeout=2)" 2>/dev/null && break; fi
  sleep 0.5
done

echo "[browser] mediamtx=$(systemctl is-active mediamtx)"
echo "[browser] WebRTC : http://${BOARD_IP}:8889/detect   ★推奨(遅延 ~1s)"
echo "[browser] HLS    : http://${BOARD_IP}:8888/detect   (フォールバック)"
echo "[browser] RTSP   : rtsp://${BOARD_IP}:8554/detect   (従来どおり並存)"

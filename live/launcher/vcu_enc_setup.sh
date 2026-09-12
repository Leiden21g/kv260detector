#!/bin/bash
# vcu_enc_setup.sh — [board] clean boot から VCU H.264 encoder(allegro-dvt)を bring-up する。
#   ★reboot 後に走らせる(insmod/overlay load は永続しない)。encoder 完動手順は
#     採用構成 = allegro.ko + v2019.2 fw + combined_allegro(syscon)+ encbuf patch。
#
#   前提(board ~/ に配置済):
#     ~/allegro_dvt.ko          (patched, md5 73ceb61f: cross-build + encoder-buffer patch)
#     ~/combined_allegro.dtbo   (md5 9dd58952: allegro binding + regs/sram split + assigned-clock-rates + vcu-settings syscon)
#     /lib/firmware/al5e.fw,al5e_b.fw  (v2019.2: 126572B/14680B。元 board fw は .bak_board 退避済)
#     /lib/firmware/kv260_fan_vcu.bit.bin (fb18e112: VCU+camera+yolo 統合 200MHz)
#
#   ★重要: PL overlay の rmdir は of_overlay_remove で kernel crash → 別 overlay を stack せず、
#     combined_allegro だけを clean base(ttc0pwm のみ)から load。変更したい時は reboot→直接 apply。
set -u
KO="${KO:-$HOME/allegro_dvt.ko}"
DTBO_SRC="${DTBO_SRC:-$HOME/combined_allegro.dtbo}"
OVL=/sys/kernel/config/device-tree/overlays

# ★2026-09-06 AMD EDF 25.11(dfx-mgr あり)対応: configfs へ直接 apply すると 7.8MB bit で kernel Oops
#   (fpga_mgr_load→cma_heap_map_dma_buf)を踏んだ実績がある。dfx-mgr(xmutil)がある image では
#   /lib/firmware/xilinx/yolov7vcu/(install.sh が作る: bit + dtbo + shell.json)を `xmutil loadapp` で載せる。
#   ⚠ Oops が出たら reboot せず(hang する)、dmesg を保存して電源再投入。
APP="${APP:-yolov7vcu}"
if command -v xmutil >/dev/null 2>&1 && [ -d /lib/firmware/xilinx/$APP ]; then
  echo "[vcu-enc] dfx-mgr 経路(xmutil loadapp $APP)"
  lsmod | grep -q '^allegro' || sudo insmod "$KO" || { echo "  insmod 失敗"; exit 1; }
  cur="$(ls "$OVL" 2>/dev/null | tr '\n' ' ')"; echo "  overlays(before): $cur"
  case "$cur" in *${APP}_image*) echo "  既に $APP がロード済";;
    "") sudo xmutil loadapp "$APP" || { echo "  loadapp 失敗"; sudo dmesg | tail -20; exit 1; };;
    *)  sudo xmutil unloadapp && sudo xmutil loadapp "$APP" || { echo "  unload/loadapp 失敗"; sudo dmesg | tail -20; exit 1; };;
  esac
  sleep 2
  lsmod | grep -q '^zocl' || sudo modprobe zocl 2>/dev/null
  echo "[vcu-enc] status=$(ls "$OVL" | tr '\n' ' ') fpga=$(cat /sys/class/fpga_manager/fpga0/state) fan=$(systemctl is-active y7-fan)"
  if [ -e /dev/video1 ]; then echo "  ✓ VCU H.264 encoder ready(/dev/video1) camera=$( [ -e /dev/media0 ] && echo /dev/media0 || echo 無し) dri=$(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
  else echo "  ✗ /dev/video1 無"; sudo dmesg | grep -iE "allegro|firmware|resource|fpga" | tail -6; exit 1; fi
  exit 0
fi
echo "[vcu-enc] clean base 確認(callegro/yolov7 が既 load なら reboot 推奨)"
cur="$(ls "$OVL" 2>/dev/null | tr '\n' ' ')"; echo "  overlays: $cur"
case "$cur" in *yolov7*|*callegro*|*combined*) echo "  ⚠ PL overlay 既 load。rmdir 厳禁 → reboot してから再実行推奨"; ;; esac

echo "[vcu-enc] insmod allegro-dvt"
lsmod | grep -q '^allegro' || sudo insmod "$KO" || { echo "  insmod 失敗"; exit 1; }

echo "[vcu-enc] combined_allegro overlay load(direct apply)"
sudo cp -f "$DTBO_SRC" /lib/firmware/combined_allegro.dtbo
if [ ! -d "$OVL/callegro" ]; then
  sudo mkdir -p "$OVL/callegro"
  echo -n combined_allegro.dtbo | sudo tee "$OVL/callegro/path" >/dev/null
  i=0; while [ "$(cat "$OVL/callegro/status" 2>/dev/null)" != applied ] && [ $i -lt 500 ]; do sleep 0.01; i=$((i+1)); done
fi
sleep 2
echo "[vcu-enc] status=$(cat "$OVL/callegro/status" 2>/dev/null) fpga=$(cat /sys/class/fpga_manager/fpga0/state) fan=$(systemctl is-active y7-fan)"

echo "[vcu-enc] encoder device:"
if [ -e /dev/video1 ]; then
  v4l2-ctl -d /dev/video1 --info 2>/dev/null | grep -iE "Card type|Driver name"
  echo "  ✓ VCU H.264 encoder ready(/dev/video1, v4l2h264enc)"
  echo "  test: gst-launch-1.0 videotestsrc num-buffers=60 ! video/x-raw,format=NV12,width=1280,height=720 ! v4l2h264enc ! filesink location=/tmp/t.h264"
else
  echo "  ✗ /dev/video1 無 → dmesg | grep -i allegro で診断(runtime_resume crash / resource unavailable 等)"
  sudo dmesg | grep -iE "allegro|firmware|resource" | tail -4
fi

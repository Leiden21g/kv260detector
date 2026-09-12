#!/bin/bash
# build_live.sh — Web 配信パイプラインの board 側プログラムを PC(WSL)で aarch64 クロスビルドする。
#   board(KV260 / PetaLinux 2025.2)には compiler が無い。PetaLinux 2025.2 common SDK の sysroot を使う。
#
#   前提: SDK=<展開先> (PetaLinux 2025.2 common SDK を sdk.sh -y -d <展開先> -p で展開済、environment-setup-* あり)
#         ★2026-09-10: 推論 host も **Vitis include 不要**になった(GMEM_T を 64B POD 化)。SDK だけで全部通る。
#   使い方: bash build_live.sh [all|capture|preprocess|vcu_stream|host]   → out/ に生成
#   ★2026-09-10: **決定論ビルド**にした。SOURCE_DATE_EPOCH から __DATE__/__TIME__ を固定注入するので
#     同じソース + 同じ SDK なら **何度ビルドしても md5 が一致する**(実測: 1.2s 空けた 2 連続で同値)。
#     期待値は MD5SUMS.expect.txt。ビルド末尾で自動突合する(不一致は ✗ 表示、rc は 0 のまま)。
#     ⚠ SOURCE_DATE_EPOCH を変えると host ELF の md5 だけ変わる(起動ログの build_id 文字列に入るため)。
#       既定値 = y26_live_recipe.env の SOURCE_DATE_EPOCH。動かすなら期待値も一緒に更新すること。
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="${SDK:-}"   # ★必須。SDK=<展開先> で渡す(既定値は持たない = 環境固有のパスを書かない)
OUT="${OUT:-$HERE/out}"; mkdir -p "$OUT"
WHAT="${1:-all}"

[ -n "$SDK" ] || { echo "✗ SDK 未指定: SDK=<PetaLinux 2025.2 common SDK の展開先> bash $0 ${WHAT}"; exit 1; }
[ -f "$SDK/environment-setup-cortexa72-cortexa53-amd-linux" ] || { echo "✗ SDK 無し: $SDK(sdk.sh -y -d $SDK -p で展開)"; exit 1; }
# shellcheck disable=SC1090
source "$SDK/environment-setup-cortexa72-cortexa53-amd-linux"   # $CC/$CXX = aarch64 + --sysroot
SYSROOT="$SDK/sysroots/cortexa72-cortexa53-amd-linux"

do_capture(){
  echo "[build] capture_daemon(V4L2 取込 daemon)"
  # ★フラグは board 配備品(7a323056)を byte 再現する組み合わせに固定してある。触ると md5 が動く:
  #     -std=c11 → gnu11/既定(gnu17) にすると別バイナリ / -lpthread を足すとさらに別バイナリ
  #   (ソースは pthread を 1 箇所も使っていないので -lpthread はもともと不要だった)
  $CC -O2 -std=c11 "$HERE/capture/capture_daemon.c" -o "$OUT/capture_daemon"
}
do_preprocess(){
  echo "[build] camera_preprocess(NV12→letterbox→量子化 HWC16、OpenCV 最小リンク)"
  # ★-lopencv_imgproc -lopencv_core のみ(pkg-config --libs は全 .so をリンクし起動 0.66s に劣化する)
  $CXX -O2 -std=c++17 "$HERE/preprocess/camera_preprocess.cpp" -o "$OUT/camera_preprocess" \
    -I"$SYSROOT/usr/include/opencv4" -lopencv_imgproc -lopencv_core
}
do_vcu_stream(){
  echo "[build] vcu_stream(VCU H.264 常駐 encoder → fifo)"
  $CXX -O2 -std=c++17 -I"$HERE/vcu_stream" "$HERE/vcu_stream/vcu_stream.cpp" -o "$OUT/vcu_stream"
}
do_host(){
  # ★inference_host/ は追跡下(2026-09-10 に配信専用へ簡素化して復帰)。dir を消した環境では skip する。
  if [ ! -d "$HERE/inference_host" ]; then
    echo "[build] host: inference_host/ が無いので skip(配信 3 本だけ作る)"
    return 0
  fi
  echo "[build] 推論 host(配信専用。src/y26_live.cpp 1 本 + include/y26_live.h 1 本)"
  # y26_live_recipe.env の BASE_DEF を使う(ELF 同一性の記録)。★2026-09-10 の配信専用化で
  #   ・src/host.cpp + src/tasks.cpp は src/y26_live.cpp へ統合(2026-09-10 に改名)
  #   ・ヘッダは y26_live.h の 1 本(GMEM_T を 64B POD 化したので **Vitis include は不要**)
  #   ・BASE_DEF のうち生成コードに効くのは幾何/配置の 7 個だけ(y26_live_recipe.env 末尾の注記)
  # shellcheck disable=SC1090
  . "$HERE/inference_host/y26_live_recipe.env"
  local IH="$HERE/inference_host" BD="$OUT/host_build"; mkdir -p "$BD"
  local COMMON="-std=c++17 -I$IH/include -I$SYSROOT/usr/include/xrt"
  # ★決定論化: y26_live.cpp は __DATE__/__TIME__ を起動ログの build_id に埋める唯一の箇所(:332 と :774)。
  #   固定しないと **1 秒違うだけで ELF の md5 が変わる**(実測 869d241c / e8467b8a)。
  #   固定すると obj も ELF も byte 一致する(実測 48987813 / 3f586c9d、cwd を変えても同値)。
  local EPOCH="${SOURCE_DATE_EPOCH:-${SOURCE_DATE_EPOCH_DEFAULT:-}}"
  local -a PIN=()   # ★配列で渡す(文字列 + eval だと "Sep 10 2026" の空白で語分割されて壊れる)
  if [ -n "$EPOCH" ]; then
    PIN=( "-D__DATE__=\"$(date -u -d "@$EPOCH" +'%b %e %Y')\"" \
          "-D__TIME__=\"$(date -u -d "@$EPOCH" +'%H:%M:%S')\"" -Wno-builtin-macro-redefined )
    echo "[build]   build_id 固定: SOURCE_DATE_EPOCH=$EPOCH ($(date -u -d "@$EPOCH" +'%b %e %Y %H:%M:%S') UTC)"
  else
    echo "[build]   ⚠ SOURCE_DATE_EPOCH 未設定 → build_id は現在時刻 = ELF md5 は再現しない"
  fi
  # shellcheck disable=SC2086
  $CXX $COMMON $BASE_DEF "${PIN[@]}" -c -o "$BD/y26_live.o" "$IH/src/y26_live.cpp"
  $CXX -o "$OUT/$HOST_ELF" "$BD/y26_live.o" -luuid -lxrt_coreutil -lxilinxopencl -lpthread -lrt -ldl -lcrypt -lstdc++
}
case "$WHAT" in
  all) do_capture; do_preprocess; do_vcu_stream; do_host ;;
  capture) do_capture ;; preprocess) do_preprocess ;; vcu_stream) do_vcu_stream ;; host) do_host ;;
  *) echo "usage: $0 [all|capture|preprocess|vcu_stream|host]"; exit 2 ;;
esac
echo "[build] done → $OUT"; (cd "$OUT" && md5sum * 2>/dev/null | cut -c1-8,33- || true)

# ── 期待 md5 との突合(決定論ビルドの検算)───────────────────────────────────
#   MD5SUMS.expect.txt = このレシピで作られるはずの 4 本の md5。board 配備品と一致するものは
#   その旨も書いてある。ここが ✗ なら「ソース or SDK or フラグ or SOURCE_DATE_EPOCH が動いた」。
EXPECT="$HERE/MD5SUMS.expect.txt"
if [ -f "$EXPECT" ]; then
  echo "[build] 期待 md5 突合 ($(basename "$EXPECT"))"
  ng=0
  while read -r want name _rest; do
    case "$want" in \#*|"") continue ;; esac
    [ -f "$OUT/$name" ] || continue
    got="$(md5sum "$OUT/$name" | cut -d" " -f1)"
    if [ "$got" = "$want" ]; then echo "  ✔ $name  ${got:0:8}"
    else echo "  ✗ $name  期待 ${want:0:8} / 実際 ${got:0:8}"; ng=1; fi
  done < "$EXPECT"
  [ "$ng" = 0 ] || echo "  ⚠ 不一致あり — MD5SUMS.expect.txt の注記を読んでから期待値を更新すること"
fi

#!/bin/bash
# build_allegro_dvt.sh — 配布している `allegro_dvt.ko` を再現ビルドするスクリプト
#   (GPL-2.0 の「対応するソース(Corresponding Source)」の一部。
#    ../../THIRD_PARTY_NOTICES.md §2 / README.md を参照)
#
# 何をするか:
#   ① 上流 linux-xlnx @ ${SHA} の drivers/media/platform/allegro-dvt/ から 10 ファイルを取得
#   ② sha256 を upstream_sources.sha256 と照合(ネットワーク取得物の同一性確認)
#   ③ KV260 用 patch(0001-allegro-dvt-kv260-disable-encoder-buffer.patch)を適用
#   ④ board kernel の build tree(headers-only)で out-of-tree module としてビルド
#   ⑤ vermagic が board の ${REL} に一致することを確認
#
# 出力: ${WORK}/src/allegro.ko
#        → board へは `allegro_dvt.ko` という名前で置く(モジュール名自体は上流どおり `allegro`)。
#
# 前提(いずれも利用者側で用意するもの):
#   - aarch64 クロスツールチェイン(配布 ko は gcc 13.4.0 = Xilinx 2025.2 同梱のものでビルドした)
#   - board kernel ${REL} の build tree(headers-only でよい。.config と Module.symvers が要る)
#     入手口: AMD の KV260 用 PetaLinux/Vitis platform の sysroot
#             `<sysroot>/usr/lib/modules/${REL}/build`、または linux-xlnx @ ${SHA} を
#             board と同じ .config で `make modules_prepare` したツリー。
#   - curl, python3(照合用), make, patch
#
# 使い方:
#   KSRC=/path/to/usr/lib/modules/6.12.40-xilinx-g31626ef92ff1/build \
#   CROSS_COMPILE=aarch64-linux-gnu- \
#     bash build_allegro_dvt.sh
#
#   オフライン(上流 10 ファイルを手元に持っている場合):
#   SRCDIR=/path/to/allegro-dvt-sources KSRC=... bash build_allegro_dvt.sh
#   (GPL ソース tarball `allegro-dvt-gpl-src-<SHA>.tar.gz` の `upstream/` がこれに当たる)
set -euo pipefail

SHA="${SHA:-31626ef92ff1}"                  # board kernel の git SHA(uname -r の -g<SHA>)
REL="${REL:-6.12.40-xilinx-g31626ef92ff1}"  # board の vermagic
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
WORK="${WORK:-$(mktemp -d -t allegro_dvt_build.XXXXXX)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH="${PATCH:-$HERE/0001-allegro-dvt-kv260-disable-encoder-buffer.patch}"
SHAFILE="${SHAFILE:-$HERE/upstream_sources.sha256}"
SRCDIR="${SRCDIR:-}"                        # 指定時は上流取得をせずここから複製する
BASE="https://raw.githubusercontent.com/Xilinx/linux-xlnx/${SHA}/drivers/media/platform/allegro-dvt"
FILES=(allegro-core.c allegro-mail.c allegro-mail.h nal-h264.c nal-h264.h \
       nal-hevc.c nal-hevc.h nal-rbsp.c nal-rbsp.h Makefile)

die(){ echo "[allegro] ✗ $*" >&2; exit 1; }
log(){ echo "[allegro] $*"; }

[[ -n "${KSRC:-}" ]] || die "KSRC(board kernel の build tree)を指定すること。
  例: KSRC=<sysroot>/usr/lib/modules/${REL}/build"
[[ -d "$KSRC" ]] || die "KSRC が無い: $KSRC"
[[ -f "$KSRC/Makefile" ]] || die "KSRC が kernel build tree でない(Makefile 無し): $KSRC"
[[ -f "$PATCH" ]] || die "patch が無い: $PATCH"
command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1 \
  || die "クロスコンパイラが PATH に無い: ${CROSS_COMPILE}gcc"

mkdir -p "$WORK/src" "$WORK/kbuild"
log "WORK=$WORK  KSRC=$KSRC  REL=$REL  SHA=$SHA"

# ── ① 上流ソースの用意 ─────────────────────────────────────────────────────────
if [[ -n "$SRCDIR" ]]; then
  log "上流ソースを $SRCDIR から複製(オフライン)"
  for f in "${FILES[@]}"; do
    [[ -f "$SRCDIR/$f" ]] || die "SRCDIR に $f が無い: $SRCDIR"
    cp -f "$SRCDIR/$f" "$WORK/src/$f"
  done
else
  log "上流 linux-xlnx @ $SHA から 10 ファイルを取得"
  for f in "${FILES[@]}"; do
    curl -sfL "$BASE/$f" -o "$WORK/src/$f" || die "取得失敗: $BASE/$f"
  done
fi

# ── ② sha256 照合 ──────────────────────────────────────────────────────────────
if [[ -f "$SHAFILE" ]]; then
  ( cd "$WORK/src" && sha256sum -c --quiet "$SHAFILE" ) \
    || die "上流ソースの sha256 が $SHAFILE と一致しない。
  SHA=$SHA を変えた場合は期待値も変わる(その場合は照合を飛ばすため SHAFILE= を空で渡す)。"
  log "✔ 上流 10 ファイルの sha256 一致"
else
  log "(sha256 照合はスキップ: $SHAFILE 無し)"
fi

# ── ③ KV260 patch 適用 ─────────────────────────────────────────────────────────
#   patch は kernel tree からの相対パス(a/drivers/media/platform/allegro-dvt/…)なので
#   -p5 でファイル名部分だけにする。
if grep -q 'memory_depth == 0' "$WORK/src/allegro-core.c"; then
  log "patch 既適用"
else
  ( cd "$WORK/src" && patch -p5 --no-backup-if-mismatch < "$PATCH" ) \
    || die "patch 適用に失敗($PATCH)"
  log "✔ KV260 encoder-buffer patch 適用"
fi
grep -q 'memory_depth == 0' "$WORK/src/allegro-core.c" || die "patch 後も該当行が無い"

# ── ④ build tree を複製してビルド ──────────────────────────────────────────────
#   ドライブ mount 上の build tree は exec bit を失っている場合があるので native fs へ複製する。
if [[ ! -e "$WORK/kbuild/Makefile" ]]; then cp -a "$KSRC/." "$WORK/kbuild/"; fi
find "$WORK/kbuild/scripts" "$WORK/kbuild/arch" -name '*.sh' -exec chmod +x {} \; 2>/dev/null || true

make -C "$WORK/kbuild" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" modules_prepare

# ★vermagic を board に一致させる。cross 環境では setlocalversion が git 由来の
#   `-g<SHA>` suffix を付けられず vermagic が board と食い違い insmod が弾かれるため、
#   UTS_RELEASE を直接書く。
printf '#define UTS_RELEASE "%s"\n' "$REL" > "$WORK/kbuild/include/generated/utsrelease.h"

rm -f "$WORK"/src/allegro.mod.* "$WORK"/src/allegro.ko 2>/dev/null || true
make -C "$WORK/kbuild" M="$WORK/src" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
     CONFIG_VIDEO_ALLEGRO_DVT=m modules

[[ -f "$WORK/src/allegro.ko" ]] || die "allegro.ko が生成されなかった"

# ── ⑤ vermagic / alias の確認 ──────────────────────────────────────────────────
MI="$WORK/modinfo.bin"
"${CROSS_COMPILE}objcopy" -O binary --only-section=.modinfo "$WORK/src/allegro.ko" "$MI"
GOT_VM=$(tr '\0' '\n' < "$MI" | sed -n 's/^vermagic=//p' | head -1)
log "vermagic: $GOT_VM"
log "alias   : $(tr '\0' '\n' < "$MI" | grep 'allegro,al5e' | head -1)"
[[ "$GOT_VM" == "$REL "* || "$GOT_VM" == "$REL" ]] \
  || die "vermagic が board($REL)と一致しない: $GOT_VM"

log "✔ 完成: $WORK/src/allegro.ko"
cat <<EOS

board への配置:
  scp $WORK/src/allegro.ko <user>@<board>:~/allegro_dvt.ko
  (insmod は launcher の live/launcher/vcu_enc_setup.sh が行う)

★配布 ko との md5 一致は保証しない。コンパイラ版・build tree の差で
  バイナリは変わる(機能は同じ)。配布 ko の md5 は
  73ceb61fb3eff5174634f717fb066275(104,992 バイト)。

別途必要な VCU microcode(本スクリプトの対象外、配布物に同梱):
  Xilinx/vcu-firmware tag release-2019.2 の
  1.0.0/lib/firmware/al5e.fw (126,572B) / al5e_b.fw (14,680B)
EOS

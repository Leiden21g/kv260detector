#!/usr/bin/env python3
# chw_load_ref.py — 推論 host の chw_load(x.bin → kernel 入力 slot)の python 参照実装(x86、board 不要)。
#
#   x.bin(float32 CHW 3×H×W、camera_preprocess の出力)→ host が kernel 入力 slot に置く
#   HWC int16 [H][W][4] を byte 単位で再現する。
#     o[y][x][c] = int16( work * (1 << (8 + shift0)) )   (float 乗算 → 0 方向切捨て)
#     o[y][x][3] = 0                                     (c==2 のとき次要素を 0 埋め)
#     ★quirk: 旧 while(!feof) 由来の「EOF 後 1 周」。最終 float 値 work を
#       count==3*H*W の位置 = (c=3,y=0,x=0) に書く → o[0][0][3] = int16(last*(1<<(8+shift0)))。
#       proven golden(head byte-exact)維持のため host は意図的にこれを残しており、参照も再現する。
#   shift0 = 推論 host の data_shift[0][0](先頭 = 6 → ×16384)。
#
#   使い方: chw_load_ref.py x.bin out.hwc [--neth 384] [--netw 640] [--shift 6]
#     x.bin は float CHW(3*H*W*4B)か、req8 の量子化済 HWC int16(H*W*4*2B、そのまま通す)のどちらか。
#   終了コード: 0=OK / 3=入力バイト数が期待 3*H*W*4 とも H*W*4*2 とも不一致(640 化の「旧 x.bin 検出」基準)。
#   NETH/NETW の既定は 384×640。640 構成では geo640.env の値を引数で渡す(直書き禁止)。
import sys, os, argparse, numpy as np


def chw_load_ref(x, H, W, shift0):
    """x: float32 [3*H*W] → int16 [H][W][4](推論 host の chw_load 等価)。"""
    n = 3 * H * W
    assert x.size == n
    scale = np.float32(1 << (8 + shift0))
    # C: o = work*(1<<(8+s)) は float×float(int→float 昇格)→ int16 変換(0 方向切捨て)
    q = np.trunc(x.astype(np.float32) * scale).astype(np.int64)
    if q.min() < -32768 or q.max() > 32767:
        raise SystemExit(f"!! int16 範囲外 {q.min()}..{q.max()}(x.bin が /255 正規化されていない?)")
    q = q.astype(np.int16).reshape(3, H, W)
    o = np.zeros((H, W, 4), np.int16)
    o[:, :, 0:3] = q.transpose(1, 2, 0)
    # EOF 後 1 周 quirk: work=x[-1] を count=n の位置 (c=3, y=0, x=0) に書く
    o[0, 0, 3] = np.int16(np.trunc(np.float32(x[-1]) * scale))
    return o


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xbin")
    ap.add_argument("out")
    ap.add_argument("--neth", type=int, default=384)
    ap.add_argument("--netw", type=int, default=640)
    ap.add_argument("--shift", type=int, default=6, help="推論 host の data_shift[0][0]")
    a = ap.parse_args()
    H, W = a.neth, a.netw
    expect = 3 * H * W * 4
    expect16 = H * W * 4 * 2
    sz = os.path.getsize(a.xbin)
    # ★req8(2026-08-29): hwc16 入力(camera_preprocess --out-hwc16 = 量子化済 HWC int16)は host と同じくサイズで判別し
    #   そのまま通す(推論 host の chw_load の hwc16 直読みと同じ = 変換無し)。float 経路の出力と byte-exact であるべき。
    if sz == expect16 and expect16 != expect:
        o = np.fromfile(a.xbin, np.int16).reshape(H, W, 4)
        o.tofile(a.out)
        print(f"ok {a.xbin} ({sz}B, hwc16 直読み) -> {a.out} int16 [{H}][{W}][4] = {o.nbytes}B o[0][0][3]={int(o[0,0,3])}")
        return
    if sz != expect:
        print(f"!! {a.xbin}: {sz}B != 期待 {expect}B (3*{H}*{W}*4)  → 幾何不一致(旧 x.bin?)", file=sys.stderr)
        sys.exit(3)
    x = np.fromfile(a.xbin, np.float32)
    o = chw_load_ref(x, H, W, a.shift)
    o.tofile(a.out)
    print(f"ok {a.xbin} ({sz}B) -> {a.out} int16 [{H}][{W}][4] = {o.nbytes}B  "
          f"min={x.min():.4f} max={x.max():.4f} shift0={a.shift} o[0][0][3]={int(o[0,0,3])}")


if __name__ == "__main__":
    main()

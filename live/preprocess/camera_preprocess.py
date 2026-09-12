#!/usr/bin/env python3
# camera_preprocess.py — KV260 IAS カメラの NV12 frame を yolo26 host 入力 x.bin に変換。
#
#   live e2e の PS 前処理(board の camera_preprocess.cpp と同じ変換の python 参照実装)。
#   board 経路:  v4l2-ctl --stream-to=frame.nv12 (1920×1080 NV12, stride=3840)
#              → 本スクリプト → x.bin (float32 CHW 1×3×384×640, RGB, /255, pad114)
#              → board host (./yolov7) が読み込み内部で *16384 量子化 → kernel 投入。
#
#   ★letterbox は mAP 評価 harness と同一式 = board の x.bin と byte 一致(同一量子化前提)。
#   ★de-stride は frmbuf の NV12 stride(既定 3840 = 2×幅)のパディング除去。
#
#   使い方:
#     # 単一 frame:  v4l2-ctl ... --stream-count=1 --stream-to=/tmp/f.nv12
#     python3 camera_preprocess.py --nv12 /tmp/f.nv12 --out /tmp/x.bin [--png /tmp/f.png]
#     # multi-frame raw (--stream-count=N を 1 ファイルに連結したもの) の k 枚目:
#     python3 camera_preprocess.py --nv12 /tmp/f.nv12 --frame 5 --out /tmp/x.bin
import sys, os, argparse, numpy as np
import cv2


def letterbox(img, size):
    """img(HWC BGR uint8) -> (x[1,3,H,W] float RGB/255, r, padx, pady)。
    ★mAP 評価 harness の letterbox と数値完全一致(board host と同一前処理)。self-contained。"""
    H, W = size
    h0, w0 = img.shape[:2]
    r = min(H / h0, W / w0)
    nh, nw = int(round(h0 * r)), int(round(w0 * r))
    res = cv2.resize(img, (nw, nh))
    padx, pady = (W - nw) // 2, (H - nh) // 2
    canvas = np.full((H, W, 3), 114, np.uint8)
    canvas[pady:pady + nh, padx:padx + nw] = res
    x = canvas[:, :, ::-1].transpose(2, 0, 1)[None].astype(np.float32) / 255.0
    return x, r, padx, pady


def unletterbox(boxes_xyxy, r, padx, pady):
    """letterbox 座標 [N,4] -> 元画像座標(検出枠の戻し)。"""
    b = boxes_xyxy.copy()
    b[:, [0, 2]] = (b[:, [0, 2]] - padx) / r
    b[:, [1, 3]] = (b[:, [1, 3]] - pady) / r
    return b


def chw_to_hwc16(x, H, W, shift0=6):
    """float32 CHW [3*H*W] → int16 HWC [H][W][4](推論 host の chw_load 等価 = chw_load_ref.py と同式)。
    値 = trunc(x * 2^(8+shift0))(C の float 積 → int16 変換)、c=3 は 0、EOF quirk o[0][0][3] = 最終 float の量子化値。"""
    n = 3 * H * W
    assert x.size == n, (x.size, n)
    scale = np.float32(1 << (8 + shift0))
    q = np.trunc(x.astype(np.float32) * scale).astype(np.int16).reshape(3, H, W)
    o = np.zeros((H, W, 4), np.int16)
    o[:, :, 0:3] = q.transpose(1, 2, 0)
    o[0, 0, 3] = np.int16(np.trunc(np.float32(x[-1]) * scale))
    return o


def nv12_to_bgr(raw, W, H, S):
    """de-stride NV12(stride S, 2×幅パディング)→ BGR uint8 HWC。"""
    frame = W * H * 3 // 2 if S == W else S * H * 3 // 2  # padding 無し/有り両対応
    y  = raw[:S * H].reshape(H, S)[:, :W]
    uv = raw[S * H:S * H + S * (H // 2)].reshape(H // 2, S)[:, :W]
    nv = np.concatenate([y.reshape(-1), uv.reshape(-1)]).astype(np.uint8)
    return cv2.cvtColor(nv.reshape(H * 3 // 2, W), cv2.COLOR_YUV2BGR_NV12)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nv12", required=True, help="v4l2-ctl --stream-to で吐いた NV12 raw")
    ap.add_argument("--out", default=None, help="出力 x.bin (float32 CHW)")
    # ★req8(2026-08-29): host chw_load と byte 同一の HWC int16 [H][W][4](量子化済)を直接書く。C++ 版 --out-hwc16 と同等。
    ap.add_argument("--out-hwc16", default=None, help="出力 HWC int16 [H][W][4](host chw_load 等価、EOF quirk o[0][0][3] 込み)")
    ap.add_argument("--shift0", type=int, default=6, help="量子化 shift(推論 host の data_shift[0][0]=6 → ×16384)")
    ap.add_argument("--width",  type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--stride", type=int, default=3840, help="frmbuf NV12 stride(=2×幅)。padding 無し時は --stride=幅")
    # ★既定の letterbox 幾何は、同 dir に geo640.py / geo640.env があればそこから取る(任意)。無ければ 384×640。
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from geo640 import net_hw
        _def_hw = list(net_hw())
    except ImportError:
        _def_hw = [384, 640]
    ap.add_argument("--size", type=int, nargs=2, default=_def_hw, help="letterbox H W (既定 = geo640.env の NETH NETW、無ければ 384 640)")
    ap.add_argument("--frame", type=int, default=0, help="連結 raw の k 枚目(0始まり)")
    ap.add_argument("--png", default=None, help="検証用: de-stride 後の元画像を PNG 保存")
    ap.add_argument("--png-lb", default=None, help="検証用: letterbox 済(モデル入力)を PNG 保存")
    a = ap.parse_args()
    if not a.out and not a.out_hwc16:
        ap.error("--out か --out-hwc16 の少なくとも一方が必要")

    W, H, S = a.width, a.height, a.stride
    frame_bytes = S * H * 3 // 2  # NV12: Y=S*H + UV=S*(H/2)
    raw_all = np.fromfile(a.nv12, np.uint8)
    nframes = raw_all.size // frame_bytes
    if nframes == 0:
        sys.exit(f"!! {a.nv12} が 1 frame ({frame_bytes}B) に満たない: {raw_all.size}B "
                 f"(W={W} H={H} stride={S} 想定)")
    if a.frame >= nframes:
        sys.exit(f"!! --frame {a.frame} >= 含有 frame 数 {nframes}")
    raw = raw_all[a.frame * frame_bytes:(a.frame + 1) * frame_bytes]

    bgr = nv12_to_bgr(raw, W, H, S)
    if a.png:
        cv2.imwrite(a.png, bgr); print(f"  元画像(de-stride) -> {a.png}  mean={bgr.mean():.1f}")

    x, r, padx, pady = letterbox(bgr, tuple(a.size))   # x[1,3,H,W] float32 RGB/255
    if a.out:
        np.ascontiguousarray(x[0].astype(np.float32)).tofile(a.out)
    if a.out_hwc16:
        o16 = chw_to_hwc16(np.ascontiguousarray(x[0].astype(np.float32)).reshape(-1), a.size[0], a.size[1], a.shift0)
        o16.tofile(a.out_hwc16)
        print(f"  hwc16 int16 [{a.size[0]}][{a.size[1]}][4] ({o16.nbytes}B, shift0={a.shift0}, o[0][0][3]={int(o16[0,0,3])}) -> {a.out_hwc16}")

    if a.png_lb:
        lb = (x[0].transpose(1, 2, 0)[:, :, ::-1] * 255).astype(np.uint8)  # RGB->BGR で可視化
        cv2.imwrite(a.png_lb, lb); print(f"  letterbox 入力 -> {a.png_lb}")

    print(f"✔ frame {a.frame}/{nframes}  src {W}×{H} (stride {S}) -> "
          f"x.bin {a.size[0]}×{a.size[1]} float32 CHW  -> {a.out}  "
          f"({x[0].size*4}B, scale r={r:.4f} pad=({padx},{pady}))")
    # 検出枠の元画像への戻し変換は unletterbox(boxes, r, padx, pady)。


if __name__ == "__main__":
    main()

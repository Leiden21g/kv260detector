#!/usr/bin/env python3
# verify_preprocess_c.py — camera_preprocess.cpp が camera_preprocess.py と byte-exact かを x86 で検証。
#
#   board 不要。合成 NV12(既定 1920×1080 stride3840, 構造ある決定的パターン)を生成 → python golden x.bin
#   ⇔ C++ バイナリ出力 x.bin を bit 比較。複数 frame / 複数パターンで網羅。
#
#   x86 OpenCV C++(4.6)と python cv2(4.13)が byte 一致 = アルゴリズム正当性 + resize/cvtColor の
#   バージョン非依存性を同時に裏付ける(board の最終 golden 一致は board 復帰後ゲート)。
#
#   ★2026-08-28 H4-T0: 640×640 化に向けて幾何を引数化(既定は現行 1080p→384×640 のまま)。
#     --size H W      letterbox 出力(既定 384 640。640×640 化では 640 640)
#     --cam  W H S    カメラ幾何 幅/高さ/NV12 stride(既定 1920 1080 3840)
#     --bin  PATH     C++ バイナリ(既定 ../out/camera_preprocess = build_live.sh preprocess の出力。第 1 位置引数でも可)
#     --expect-identity  r==1 / pad==0 を assert し、resize を経ない直変換とも byte 一致を要求
#                        (640×640 入力 → 640×640 のとき、letterbox が恒等になることの証明)
#   例(H4 追加ケース):
#     verify_preprocess_c.py --cam 1280 720 2560 --size 640 640          # 720p → 640² letterbox
#     verify_preprocess_c.py --cam 640 640 1280 --size 640 640 --expect-identity
#     verify_preprocess_c.py --cam 640 640 640  --size 640 640 --expect-identity   # stride=幅(padding 無し)
import os, sys, argparse, subprocess, tempfile, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import camera_preprocess as pp   # letterbox / nv12_to_bgr を golden として再利用

DEF_W, DEF_H, DEF_S = 1920, 1080, 3840
DEF_SIZE = (384, 640)


def synth_nv12(seed, nframes, W, H, S):
    """構造ある決定的 NV12 raw(stride S, 右 S-W は padding ゴミ=de-stride 検証。S==W なら padding 無し)。"""
    rng = np.random.default_rng(seed)
    fb = S * H * 3 // 2
    buf = np.empty(fb * nframes, np.uint8)
    for k in range(nframes):
        off = k * fb
        # Y 平面: 勾配 + frame 依存オフセット + ノイズ(左 W のみ意味、右 S-W は乱数 padding)
        ygrad = (np.add.outer(np.arange(H), np.arange(S)) + 37 * k) % 256
        ynoise = rng.integers(0, 256, size=(H, S), dtype=np.uint8)
        yplane = ((ygrad.astype(np.uint16) + ynoise) % 256).astype(np.uint8)
        if S > W:
            yplane[:, W:] = rng.integers(0, 256, size=(H, S - W), dtype=np.uint8)  # padding ゴミ
        buf[off:off + S * H] = yplane.reshape(-1)
        # UV 平面: H/2 行、左 W が NV12 interleave、右は padding ゴミ
        uv = rng.integers(0, 256, size=(H // 2, S), dtype=np.uint8)
        uv[:, :W] = ((np.add.outer(np.arange(H // 2), np.arange(W)) * 3 + 11 * k) % 256).astype(np.uint8)
        buf[off + S * H:off + fb] = uv.reshape(-1)
    return buf


def golden_xbin(nv12_path, frame, W, H, S, size):
    fb = S * H * 3 // 2
    raw_all = np.fromfile(nv12_path, np.uint8)
    raw = raw_all[frame * fb:(frame + 1) * fb]
    bgr = pp.nv12_to_bgr(raw, W, H, S)
    x, r, padx, pady = pp.letterbox(bgr, size)
    return np.ascontiguousarray(x[0].astype(np.float32)), bgr, r, padx, pady


def identity_xbin(bgr):
    """resize/pad を一切経ない直変換(BGR→RGB, CHW, /255)。letterbox が恒等のときの独立 golden。"""
    return np.ascontiguousarray(bgr[:, :, ::-1].transpose(2, 0, 1).astype(np.float32) / 255.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin_pos", nargs="?", default=None, help="(旧互換) C++ バイナリ path")
    ap.add_argument("--bin", default=None, help="C++ バイナリ(既定 ../out/camera_preprocess)")
    ap.add_argument("--size", type=int, nargs=2, default=list(DEF_SIZE), metavar=("H", "W"))
    ap.add_argument("--cam", type=int, nargs=3, default=[DEF_W, DEF_H, DEF_S], metavar=("W", "H", "S"))
    ap.add_argument("--seeds", type=int, nargs="+", default=[1, 7, 42])
    ap.add_argument("--nframes", type=int, default=3)
    ap.add_argument("--expect-identity", action="store_true",
                    help="r==1 かつ pad==0 を assert し、resize 無し直変換とも一致を要求")
    # ★req8: --hwc16 で C++ --out-hwc16 / python --out-hwc16 / chw_load_ref(golden float→int16)の 3 者 byte-exact も検証
    ap.add_argument("--hwc16", action="store_true", help="量子化済 HWC int16 経路(--out-hwc16)も python⇔C++⇔chw_load_ref で比較")
    ap.add_argument("--shift0", type=int, default=6)
    a = ap.parse_args()
    cbin = a.bin or a.bin_pos or os.path.join(HERE, "camera_preprocess")
    if not os.path.exists(cbin):
        sys.exit(f"!! C++ バイナリが無い: {cbin}  (先に build してください)")
    W, H, S = a.cam
    size = tuple(a.size)
    if S < W or S % 2 or W % 2 or H % 2:
        sys.exit(f"!! カメラ幾何が不正: W={W} H={H} S={S}(S>=W, 全て偶数)")
    print(f"cam {W}x{H} stride {S} -> letterbox {size[0]}x{size[1]}  bin={cbin}"
          + ("  [expect-identity]" if a.expect_identity else ""))
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        for seed in a.seeds:
            nframes = a.nframes
            nv = os.path.join(td, f"s{seed}.nv12")
            synth_nv12(seed, nframes, W, H, S).tofile(nv)
            for frame in range(nframes):
                gold, bgr, r, padx, pady = golden_xbin(nv, frame, W, H, S, size)
                if a.expect_identity:
                    if not (r == 1.0 and padx == 0 and pady == 0):
                        print(f"  ✗ seed{seed} frame{frame}: letterbox が恒等でない r={r} pad=({padx},{pady})")
                        fails += 1; continue
                    ident = identity_xbin(bgr)
                    if not np.array_equal(gold.reshape(-1), ident.reshape(-1)):
                        print(f"  ✗ seed{seed} frame{frame}: letterbox(golden) != resize 無し直変換"); fails += 1; continue
                cout = os.path.join(td, f"s{seed}_f{frame}_c.bin")
                subprocess.run([cbin, "--nv12", nv, "--out", cout, "--frame", str(frame),
                                "--width", str(W), "--height", str(H), "--stride", str(S),
                                "--size", str(size[0]), str(size[1])],
                               check=True, stderr=subprocess.DEVNULL)
                cdat = np.fromfile(cout, np.float32)
                gold = gold.reshape(-1)
                if cdat.shape != gold.shape:
                    print(f"  ✗ seed{seed} frame{frame}: shape {cdat.shape} != {gold.shape}"); fails += 1; continue
                exact = np.array_equal(cdat, gold)
                nbad = int((cdat != gold).sum())
                tag = f" r={r:.4f} pad=({padx},{pady})"
                if exact:
                    print(f"  ✓ seed{seed} frame{frame}: BYTE-EXACT ({gold.size} float){tag}")
                else:
                    maxabs = float(np.abs(cdat - gold).max())
                    print(f"  ✗ seed{seed} frame{frame}: {nbad}/{gold.size} 不一致 maxΔ={maxabs:.3e}{tag}"); fails += 1
                if a.hwc16:
                    # golden = chw_load_ref(float 経路の host 等価)。C++ --out-hwc16 と python --out-hwc16 の両方がこれと一致すること。
                    import chw_load_ref as cref
                    g16 = cref.chw_load_ref(gold, size[0], size[1], a.shift0)
                    c16p = os.path.join(td, f"s{seed}_f{frame}_c.hwc16")
                    subprocess.run([cbin, "--nv12", nv, "--out-hwc16", c16p, "--frame", str(frame),
                                    "--width", str(W), "--height", str(H), "--stride", str(S),
                                    "--size", str(size[0]), str(size[1]), "--shift0", str(a.shift0)],
                                   check=True, stderr=subprocess.DEVNULL)
                    c16 = np.fromfile(c16p, np.int16)
                    p16p = os.path.join(td, f"s{seed}_f{frame}_p.hwc16")
                    subprocess.run([sys.executable, os.path.join(HERE, "camera_preprocess.py"), "--nv12", nv, "--out-hwc16", p16p,
                                    "--frame", str(frame), "--width", str(W), "--height", str(H), "--stride", str(S),
                                    "--size", str(size[0]), str(size[1]), "--shift0", str(a.shift0)],
                                   check=True, stdout=subprocess.DEVNULL)
                    p16 = np.fromfile(p16p, np.int16)
                    exp_n = size[0] * size[1] * 4
                    okc = c16.size == exp_n and np.array_equal(c16, g16.reshape(-1))
                    okp = p16.size == exp_n and np.array_equal(p16, g16.reshape(-1))
                    if okc and okp:
                        print(f"  ✓ seed{seed} frame{frame}: hwc16 BYTE-EXACT C++==py==chw_load_ref ({exp_n} int16 = {exp_n*2}B, o[0][0][3]={int(g16[0,0,3])})")
                    else:
                        print(f"  ✗ seed{seed} frame{frame}: hwc16 不一致 C++ {'ok' if okc else 'NG'} / py {'ok' if okp else 'NG'}"); fails += 1
    print("=" * 56)
    print("RESULT:", "ALL BYTE-EXACT ✅" if fails == 0 else f"{fails} mismatch ❌")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()

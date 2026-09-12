#!/usr/bin/env python3
"""geo640.py — python スクリプト向けの入力幾何(H, W)取得。正本 = scripts/geo640.env(無ければ 384×640)。
   使い方: from geo640 import net_hw; H, W = net_hw()
"""
import os, re
_DEF = (384, 640)
def net_hw(env_path=None):
    p = env_path or os.environ.get("GEO640_ENV") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "geo640.env")   # env GEO640_ENV で候補 env を差せる
    h, w = _DEF
    try:
        for line in open(p, encoding="utf-8"):
            m = re.match(r"^\s*Y26_NET([HW])\s*=\s*(\d+)", line)
            if m:
                if m.group(1) == "H": h = int(m.group(2))
                else:                 w = int(m.group(2))
    except FileNotFoundError:
        pass
    return h, w
if __name__ == "__main__":
    print("%d %d" % net_hw())

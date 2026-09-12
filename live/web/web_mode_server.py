#!/usr/bin/env python3
# web_mode_server.py — [board] live 配信の表示モード切替 UI(2026-08-12)。
#   overlay の「全景(縮小 letterbox)/中央ズーム(等倍切り出し)」を Web ボタンで動的切替する。
#   仕組み: /mode?set=wide|crop が MODE_FILE(/tmp/lv/ovmode)を atomic に書き、
#   host(tasks.cpp overlay_frame)が毎フレーム読む(env OVERLAY_MODE_FILE)。
#   配信系 unit(vcustream/rtspdetect/mediamtx)は出力幾何 640x384 不変のため無再起動で切替可。
#   / は視聴ページ(mediamtx WebRTC :8889/detect を iframe 埋め込み+切替ボタン)。
#   iframe がブロックされる環境では :8889 を別タブで開き、本ページはボタンだけ使えば良い。
#   起動(launcher が systemd-run): python3 ~/yolov7/web_mode_server.py  (env PORT / MODE_FILE)
import os, sys, html, http.server, socketserver, urllib.parse

PORT = int(os.environ.get("PORT", "8890"))
MODE_FILE = os.environ.get("MODE_FILE", "/tmp/lv/ovmode")
# ★AGPL-3.0 §13(2026-09-12): 本ページはネットワーク越しに第三者へ提供されうるので、
#   対応ソースの在処を視聴者に示す。URL は env `Y7_SOURCE_URL` で与える(launcher が渡す)。
#   未設定のときはリンクを出さず、「同梱の LICENSE を見よ」という表示だけにする
#   (公開 repo の URL が確定していない間のため。URL を設定すれば board 再配備なしでリンクが出る)。
SOURCE_URL = os.environ.get("Y7_SOURCE_URL", "").strip()
# ★geo640 H4(2026-08-28): 推論幾何 NETH==NETW(640×640 化 = 推論=配信=等倍)のときは「中央ズーム(crop)」の
#   意味が消える(plan §1-5 既定 = 撤去)→ wide のみ。幾何は geo640.py(同 dir の geo640.env)から。
#   geo640.py が無い(現行 board 配置)ときは幾何不明 = 従来どおり wide/crop 両方。
try:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from geo640 import net_hw
    _NETH, _NETW = net_hw()
    SQUARE = (_NETH == _NETW)
except ImportError:
    SQUARE = False
MODES = ("wide",) if SQUARE else ("wide", "crop")

def get_mode():
    try:
        m = open(MODE_FILE).read().strip()
        return m if m in MODES else "wide"        # SQUARE では crop も wide 扱い(残置 ovmode の無害化)
    except OSError:
        return "wide"

def set_mode(m):
    tmp = MODE_FILE + ".tmp"
    with open(tmp, "w") as f:
        f.write(m)
    os.replace(tmp, MODE_FILE)          # atomic(host は毎フレーム読むので途中状態を見せない)

# ★capmode(2026-09-04、web「切り抜き」ボタン): capd の publish 変換モードを切替える。
#   wide = 全景(大 pad 中央正方を 640² へ縮小)/ zoom = 中央 640² の等倍級 crop。
#   /tmp/lv/capmode を capture_daemon(--mode-file)が毎 publish 読む = 配信・推論とも瞬時切替。
#   env CAPMODE=1(launcher が GEO640_CAPD_PUB=1 のとき設定)でボタンを出す。
#   根拠 = 2026-09-04 のカメラ probe(zoom_absolute は疑似ズーム / subdev crop 不受理)。
CAPMODE_ON = os.environ.get("CAPMODE", "0") == "1"
CAPMODE_FILE = os.environ.get("CAPMODE_FILE", "/tmp/lv/capmode")
CAPMODES = ("wide", "zoom")
# ★pan(2026-09-04): 切り抜き窓の位置を上下左右ボタンで動かす。file 書式 = "<mode> <dx> <dy>"
#   (dx/dy = 中央からの px オフセット)。capd(--mode-file)が毎 publish 読んで clamp する。
#   可動域の既定は取込 3840x2160 / 窓 640² のとき (3840-640)/2=1600、(2160-640)/2=760。
CAPPAN_MAXX = int(os.environ.get("CAPPAN_MAXX", "1600"))
CAPPAN_MAXY = int(os.environ.get("CAPPAN_MAXY", "760"))
CAPPAN_STEP = int(os.environ.get("CAPPAN_STEP", "160"))

def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)

def get_capstate():
    """(mode, dx, dy) を返す。旧形式("zoom" だけ)も読める。"""
    try:
        t = open(CAPMODE_FILE).read().split()
    except OSError:
        return ("wide", 0, 0)
    m = t[0] if t and t[0] in CAPMODES else "wide"
    dx = dy = 0
    if len(t) >= 3:
        try:
            dx, dy = int(t[1]), int(t[2])
        except ValueError:
            dx = dy = 0
    return (m, _clamp(dx, -CAPPAN_MAXX, CAPPAN_MAXX), _clamp(dy, -CAPPAN_MAXY, CAPPAN_MAXY))

def set_capstate(m, dx, dy):
    dx = _clamp(int(dx), -CAPPAN_MAXX, CAPPAN_MAXX) & ~1   # NV12 = 偶数原点(capd 側でも clamp/整列)
    dy = _clamp(int(dy), -CAPPAN_MAXY, CAPPAN_MAXY) & ~1
    tmp = CAPMODE_FILE + ".tmp"
    with open(tmp, "w") as f:
        f.write("%s %d %d" % (m, dx, dy))
    os.replace(tmp, CAPMODE_FILE)          # atomic(capd は毎 frame 読む)
    return (m, dx, dy)

def get_capmode():
    return get_capstate()[0]

# AGPL-3.0 §13 の表示(bar 右端)。URL は env で与えられたときだけリンクにする。
#   ★URL は HTML 属性に入るので必ず escape する(視聴者に出す文字列)。
if SOURCE_URL:
    SRCLINK = ('AGPL-3.0 / ソース: <a href="%s" target=_blank rel=noopener>%s</a>'
               % (html.escape(SOURCE_URL, quote=True), html.escape(SOURCE_URL)))
else:
    SRCLINK = "AGPL-3.0(対応ソース = 配布物同梱の LICENSE / README 参照)"

PAGE = """<!doctype html><html lang=ja><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>KV260 live 表示モード</title>
<style>
 body{margin:0;background:#111;color:#eee;font-family:sans-serif}
 .bar{padding:8px;display:flex;gap:8px;align-items:center}
 button{font-size:16px;padding:6px 18px;border-radius:6px;border:1px solid #555;background:#222;color:#eee;cursor:pointer}
 button.on{background:#2c7;color:#000;font-weight:bold}
 .src{margin-left:auto;font-size:12px;color:#888}
 .src a{color:#8bf}
 iframe{border:0;width:100vw;height:calc(100vh - 52px)}
</style>
<div class=bar>
 %CAPBTNS%
 %OVBTNS%
 <span id=st></span>
 <span class=src>%SRCLINK%</span>
</div>
%PANBAR%
<iframe src="http://%HOST%:8889/detect" allow="autoplay"></iframe>
<script>
const CAP=%CAP%;
async function refresh(){
 if(CAP){
  const t=(await (await fetch('/capmode')).text()).trim().split(' ');
  const c=t[0], dx=(t[1]|0), dy=(t[2]|0);
  for(const id of ['cwide','czoom']){ const b=document.getElementById(id); if(b) b.className=(id==='c'+((c==='zoom')?'zoom':'wide'))?'on':''; }
  const pb=document.getElementById('pan'); if(pb) pb.style.display=(c==='zoom')?'flex':'none';
  document.getElementById('st').textContent='カメラ: '+(c==='zoom'?('切り抜き(ズーム) 位置 x'+(dx>=0?'+':'')+dx+' y'+(dy>=0?'+':'')+dy):'全景');
 }else{
  const m=(await (await fetch('/mode')).text()).trim();
  for(const id of %MODELIST%){ const b=document.getElementById(id); if(b) b.className=(id===m)?'on':''; }
  document.getElementById('st').textContent='現在: '+(m==='crop'?'中央ズーム':'全景');
 }
}
async function setm(m){ await fetch('/mode?set='+m); refresh(); }
async function setc(m){ await fetch('/capmode?set='+m); refresh(); }
async function pan(d){ await fetch('/cappan?dir='+d); refresh(); }
refresh(); setInterval(refresh,3000);
</script>"""

class H(http.server.BaseHTTPRequestHandler):
    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/mode":
            if "set" in q and q["set"][0] in MODES:
                set_mode(q["set"][0])
            self._send(200, get_mode().encode(), "text/plain")
            return
        if u.path == "/capmode":
            m, dx, dy = get_capstate()
            if "set" in q and q["set"][0] in CAPMODES:
                m, dx, dy = set_capstate(q["set"][0], dx, dy)   # 位置は保持したままモードだけ変える
            self._send(200, ("%s %d %d" % (m, dx, dy)).encode(), "text/plain")
            return
        if u.path == "/cappan":
            m, dx, dy = get_capstate()
            d = q.get("dir", [""])[0]
            st = CAPPAN_STEP
            try:
                st = max(2, int(q.get("step", [CAPPAN_STEP])[0]))
            except ValueError:
                pass
            if   d == "left":   dx -= st
            elif d == "right":  dx += st
            elif d == "up":     dy -= st
            elif d == "down":   dy += st
            elif d == "center": dx = dy = 0
            m, dx, dy = set_capstate(m, dx, dy)
            self._send(200, ("%s %d %d" % (m, dx, dy)).encode(), "text/plain")
            return
        if u.path == "/":
            # Host ヘッダが無い場合の fallback。★開発機の固定 IP は書かない(2026-09-12)。
            host = (self.headers.get("Host") or "localhost").split(":")[0]
            capbtns = ("<button id=cwide onclick=\"setc('wide')\">全景</button>"
                       "<button id=czoom onclick=\"setc('zoom')\">切り抜き(ズーム)</button>") if CAPMODE_ON else ""
            ovbtns = "" if CAPMODE_ON else (
                "<button id=wide onclick=\"setm('wide')\">全景(縮小)</button>"
                + ("" if SQUARE else "<button id=crop onclick=\"setm('crop')\">中央ズーム(等倍)</button>"))
            panbar = ("<div class=bar id=pan style=\"display:none\">"
                      "<button onclick=\"pan('left')\">←</button>"
                      "<button onclick=\"pan('up')\">↑</button>"
                      "<button onclick=\"pan('down')\">↓</button>"
                      "<button onclick=\"pan('right')\">→</button>"
                      "<button onclick=\"pan('center')\">中央</button>"
                      "</div>") if CAPMODE_ON else ""
            page = (PAGE.replace("%HOST%", host)
                        .replace("%PANBAR%", panbar)
                        .replace("%CAPBTNS%", capbtns)
                        .replace("%OVBTNS%", ovbtns)
                        .replace("%CAP%", "true" if CAPMODE_ON else "false")
                        .replace("%MODELIST%", repr(list(MODES)))
                        .replace("%SRCLINK%", SRCLINK))
            self._send(200, page.encode(), "text/html; charset=utf-8")
            return
        self._send(404, b"not found", "text/plain")

    def log_message(self, *a):   # journal を汚さない
        pass

os.makedirs(os.path.dirname(MODE_FILE), exist_ok=True)
if CAPMODE_ON:
    _m, _dx, _dy = get_capstate()
    set_capstate(_m, _dx, _dy)      # 旧形式("zoom" のみ)を "<mode> <dx> <dy>" へ正規化
if not os.path.exists(MODE_FILE) or get_mode() not in MODES or (SQUARE and open(MODE_FILE).read().strip() != "wide"):
    set_mode("wide")            # 幾何が等倍なら残置 crop を wide へ正規化(host は毎フレーム読む)

with socketserver.ThreadingTCPServer(("", PORT), H) as s:
    s.daemon_threads = True
    print(f"[webmode] :{PORT} mode_file={MODE_FILE} mode={get_mode()}", flush=True)
    s.serve_forever()

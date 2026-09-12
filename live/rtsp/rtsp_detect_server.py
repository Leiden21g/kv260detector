#!/usr/bin/env python3
# rtsp_detect_server.py — 検出オーバレイ JPEG を RTSP で配信(KV260 board-native)。
#   ★ENC で encoder 段を切替: jpeg(MJPEG)/ vcu(VCU H.264, allegro-dvt)/ omx(VCU H.264, OMX)。
#
#   設計 = MJPEG 版(2026-06-23)を土台に、検出 H.264 で帯域を削減する版(2026-07-04)へ発展させたもの。
#   共通 src = overlay_decode_daemon.py が出力する JPEG($OVERLAY, atomic mv で差替)を multifilesrc(loop)で読む。
#
#   ★ENC=jpeg : multifilesrc→jpegparse→rtpjpegpay(無再エンコード=CPU 軽、帯域=MJPEG のまま)。
#   ★ENC=vcu/omx : ★appsrc bridge(2026-07-05 実装)=
#       独立した「encoder pipeline」(multifilesrc→jpegdec→v4l2h264enc→h264parse→appsink)を常時走らせ、
#       RTSP factory 側は caps を明示設定した appsrc→rtph264pay にする。
#       → gst-rtsp-server は SDP 生成時に M2M HW encoder を PAUSED 遷移させる必要が無くなり、
#         「Retrieving media info」stall(v4l2h264enc の caps ネゴ deadlock)を回避する。
#       appsink の H.264 buffer を接続中の全 appsrc へ push。encoder は /dev/video1 を常時保持。
#       ★VCU encoder の実出力 = profile=baseline/level=4/byte-stream/alignment=au(board 実測 2026-07-05)。
#         profile=main を caps 強制すると not-negotiated になるので指定しない。
#
#   起動(board, systemd-run 推奨):
#     sudo systemd-run --unit=rtspdetect --collect --uid=petalinux --gid=petalinux \
#       --setenv=OVERLAY=/tmp/latest_overlay.jpg --setenv=FPS=5 --setenv=ENC=vcu \
#       python3 /home/petalinux/yolov7/rtsp_detect_server.py
#   受信(PC): ENC=jpeg → vlc rtsp://192.168.0.35:8554/detect
#             ENC=vcu/omx(H.264)→ ffplay -fflags nobuffer rtsp://192.168.0.35:8554/detect
#   loopback(board): ENC=jpeg → ...! rtpjpegdepay ! jpegdec ! fakesink
#                    ENC=vcu/omx → gst-launch-1.0 rtspsrc location=rtsp://127.0.0.1:8554/detect \
#                                   ! rtph264depay ! h264parse ! avdec_h264 ! fakesink num-buffers=10
import gi, os, subprocess, threading
gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
from gi.repository import Gst, GstRtspServer, GLib

OVERLAY = os.environ.get("OVERLAY", "/tmp/latest_overlay.jpg")
FPS     = int(os.environ.get("FPS", "5"))
PORT    = os.environ.get("PORT", "8554")
MOUNT   = os.environ.get("MOUNT", "/detect")
ENC     = os.environ.get("ENC", "jpeg").lower()   # jpeg | vcu | omx | vcufifo
WIDTH   = int(os.environ.get("WIDTH", "1920"))
HEIGHT  = int(os.environ.get("HEIGHT", "1080"))
H264_FIFO = os.environ.get("H264_FIFO", "/tmp/h264.fifo")   # ENC=vcufifo: host(vcu_stream)が流す H.264 fifo

Gst.init(None)

# VCU(allegro-dvt / OMX)encoder の H.264 出力 caps(board 実測: profile=baseline/level=4)。
# appsrc に明示設定して SDP を encoder 非起動で生成可能にする(= media info stall の回避点)。
H264_CAPS = (f"video/x-h264,stream-format=(string)byte-stream,alignment=(string)au,"
             f"profile=(string)baseline,level=(string)4,"
             f"width=(int){WIDTH},height=(int){HEIGHT},framerate=(fraction){FPS}/1")


class H264Encoder:
    """H.264 常駐ソース → appsink の buffer を接続中の appsrc 群へ push(appsrc bridge = SDP stall 回避)。
       source:
         'gst'  = 内部 encoder pipeline(multifilesrc jpg → jpegdec → v4l2h264enc/omxh264enc → h264parse)。
         'fifo' = ★host(vcu_stream, vcu_enc.h)が流す H.264 fifo を h264parse で受けるだけ(gst から M2M 排除)。
    """
    def __init__(self, venc=None, source="gst"):
        self.source = source
        if source == "fifo":
            # ★encode は host(vcu_stream)側。ここは H.264 を parse して appsrc へ橋渡すのみ。
            #   sync=false = fifo の到着 pace(vcu_stream が FPS で送出)に従う。
            launch = (f"filesrc location={H264_FIFO} "
                      f"! h264parse config-interval=1 "
                      f"! video/x-h264,stream-format=byte-stream,alignment=au "
                      f"! appsink name=asink emit-signals=true sync=false max-buffers=4 drop=false")
        else:
            launch = (f"multifilesrc location={OVERLAY} loop=true caps=image/jpeg,framerate={FPS}/1 "
                      f"! jpegparse ! jpegdec ! videoconvert ! {venc} "
                      f"! video/x-h264,stream-format=byte-stream,alignment=au "
                      f"! h264parse config-interval=1 "
                      # sync=true = multifilesrc の framerate(FPS)で real-time pace(帯域を FPS に一致させる)。
                      # push 時に PTS/DTS をクリアするので client 側 clock mismatch は起きない。
                      f"! appsink name=asink emit-signals=true sync=true max-buffers=2 drop=true")
        self.pipe = Gst.parse_launch(launch)
        asink = self.pipe.get_by_name("asink")
        asink.connect("new-sample", self._on_sample)
        self._lock = threading.Lock()
        self._srcs = []          # 接続中クライアントの appsrc 群
        self._pushed = 0

    def _on_sample(self, sink):
        smp = sink.emit("pull-sample")
        if smp is not None:
            src_buf = smp.get_buffer()
            with self._lock:
                srcs = list(self._srcs)
            for s in srcs:
                # ★別 pipeline 由来の PTS/DTS をクリア → RTSP appsrc(do-timestamp=true)が
                #   自 pipeline の running-time で付け直す(clock mismatch で RTP に破棄される問題の回避)。
                b = src_buf.copy()
                b.pts = Gst.CLOCK_TIME_NONE
                b.dts = Gst.CLOCK_TIME_NONE
                b.duration = Gst.CLOCK_TIME_NONE
                try: s.emit("push-buffer", b)
                except Exception: pass
            self._pushed += 1
        return Gst.FlowReturn.OK

    def start(self):
        # ★fifo ソースは host が window 境界(=1プロセス化 VCU_H264_FIFO 経路)で writer を閉じると
        #   filesrc が EOS する。bus watch で EOS/ERROR を捉え NULL→PLAYING で fifo を再 open し、
        #   次 window の H.264 受信を継続(appsrc bridge=client 側 RTSP は無停止)。vcu_stream 常駐経路
        #   では EOS は起きない為この watch は無害。
        if self.source == "fifo":
            bus = self.pipe.get_bus()
            bus.add_signal_watch()
            bus.connect("message::eos",   self._on_reader_end)
            bus.connect("message::error", self._on_reader_end)
        self.pipe.set_state(Gst.State.PLAYING)
        print(f"[enc] H.264 encoder pipeline PLAYING(venc 常駐, {WIDTH}x{HEIGHT}@{FPS})", flush=True)

    def _on_reader_end(self, bus, msg):
        # host が window 境界で fifo を閉じた(EOS)/ 一時的 error。fifo を再 open し次 window を待つ。
        print(f"[enc] fifo reader {msg.type.first_value_nick} → 再 open(次 window 待ち)", flush=True)
        self.pipe.set_state(Gst.State.NULL)
        self.pipe.set_state(Gst.State.PLAYING)

    def register(self, appsrc):
        with self._lock: self._srcs.append(appsrc)
        print(f"[enc] client appsrc 登録(接続中={len(self._srcs)})", flush=True)

    def unregister(self, appsrc):
        with self._lock:
            if appsrc in self._srcs: self._srcs.remove(appsrc)
        print(f"[enc] client appsrc 解除(接続中={len(self._srcs)})", flush=True)


def _on_media_configure(factory, media, enc):
    """RTSP media 構築時(client 接続時)= appsrc に caps 設定 + encoder へ登録。"""
    el = media.get_element()
    appsrc = el.get_by_name("src")
    appsrc.set_property("caps", Gst.Caps.from_string(H264_CAPS))
    appsrc.set_property("format", Gst.Format.TIME)
    appsrc.set_property("is-live", True)
    appsrc.set_property("do-timestamp", True)
    appsrc.set_property("block", False)
    enc.register(appsrc)
    media.connect("unprepared", lambda m: enc.unregister(appsrc))


def ensure_placeholder():
    """$OVERLAY が無ければ multifilesrc が start できないので黒 JPEG を 1 枚生成して置く。"""
    if os.path.exists(OVERLAY):
        return
    try:
        subprocess.run(
            ["gst-launch-1.0", "videotestsrc", "pattern=black", "num-buffers=1",
             "!", f"video/x-raw,width={WIDTH},height={HEIGHT}", "!", "jpegenc", "!",
             "filesink", f"location={OVERLAY}"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception as e:
        print(f"[warn] placeholder 生成失敗: {e}", flush=True)


def main():
    if ENC != "vcufifo":              # vcufifo は fifo が唯一のソース(JPEG placeholder 不要)
        ensure_placeholder()
    server = GstRtspServer.RTSPServer()
    server.set_service(PORT)
    factory = GstRtspServer.RTSPMediaFactory()
    factory.set_shared(True)                 # 複数クライアントで 1 パイプライン共有

    if ENC == "jpeg":
        launch = (f"( multifilesrc location={OVERLAY} loop=true caps=image/jpeg,framerate={FPS}/1 "
                  f"! jpegparse ! rtpjpegpay name=pay0 pt=26 )")
        factory.set_launch(launch)
    elif ENC in ("vcu", "omx", "vcufifo"):
        if ENC == "vcufifo":
            enc = H264Encoder(source="fifo")          # ★host(vcu_stream)の H.264 fifo を受ける
            launch = f"appsrc bridge ← H.264 fifo({H264_FIFO}) [encode=host/vcu_enc.h]"
        else:
            venc = "v4l2h264enc" if ENC == "vcu" else "omxh264enc"
            enc = H264Encoder(venc=venc, source="gst")
            launch = f"appsrc bridge → {venc}"
        enc.start()
        # RTSP factory = appsrc(明示 caps)→ rtph264pay。encoder/source は factory の外(introspection 非対象)。
        factory.set_launch("( appsrc name=src ! h264parse config-interval=1 ! rtph264pay name=pay0 pt=96 config-interval=1 )")
        factory.connect("media-configure", lambda f, m: _on_media_configure(f, m, enc))
    else:
        raise SystemExit(f"[fatal] 未知 ENC={ENC}(jpeg|vcu|omx|vcufifo)")

    server.get_mount_points().add_factory(MOUNT, factory)
    sid = server.attach(None)
    codec = "MJPEG" if ENC == "jpeg" else f"H.264/{ENC}(appsrc bridge)"
    print(f"★ RTSP 配信開始: rtsp://0.0.0.0:{PORT}{MOUNT}  "
          f"({codec} @{FPS}fps, src={OVERLAY}, attach={sid}, "
          f"port={server.get_property('bound-port')})\n  launch: {launch}", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()

// vcu_stream.cpp — 常駐 VCU H.264 streamer(vcu_enc.h)。latest overlay NV12 を FPS で連続 encode し
//   fifo/stdout へ H.264 Annex-B を流す。RTSP 段(rtsp_detect_server.py ENC=vcufifo)が fifo を h264parse で受ける。
//   = encode 制御を完全に host(C++)側へ。gst から M2M encoder を排除(JPEG 往復 + SDP stall 消滅)。
//
//   使い方(board): ./vcu_stream <overlay.nv12> <W> <H> <out(fifo|-)> [fps=5] [bitrate=3000000] [gop=25] [dev=/dev/video1]
//     例: mkfifo /tmp/h264.fifo
//         ./vcu_stream /tmp/overlay_frontfix.nv12 1920 1080 /tmp/h264.fifo 5 3000000 25 &
//   overlay NV12 は検出 host が atomic に差替(無ければ直前 frame を継続 = MJPEG multifilesrc loop と同挙動)。
#include "../include/vcu_enc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <ctime>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int){ g_stop = 1; }

int main(int argc, char** argv){
  if (argc < 5){
    fprintf(stderr, "usage: %s <overlay.nv12> <W> <H> <out(fifo|-)> [fps=5] [bitrate=3000000] [gop=25] [dev=/dev/video1]\n", argv[0]);
    return 2;
  }
  const char* ovp = argv[1];
  int W = atoi(argv[2]), H = atoi(argv[3]);
  const char* outp = argv[4];
  int fps = argc > 5 ? atoi(argv[5]) : 5;
  int br  = argc > 6 ? atoi(argv[6]) : 3000000;
  int gop = argc > 7 ? atoi(argv[7]) : 25;
  const char* dev = argc > 8 ? argv[8] : "/dev/video1";
  if (fps < 1) fps = 1;

  signal(SIGINT,  on_sig);
  signal(SIGTERM, on_sig);
  signal(SIGPIPE, SIG_IGN);   // reader 切断で write EPIPE → 落とさず reopen

  const size_t need = (size_t)W * H * 3 / 2;
  std::vector<uint8_t> frame(need, 16);   // 初期 = 黒 luma(overlay 未生成時のプレースホルダ)
  { // 起動時に overlay があれば読む
    FILE* f = fopen(ovp, "rb");
    if (f){ if (fread(frame.data(), 1, need, f) != need) { /* 短小=placeholder 維持 */ } fclose(f); }
  }

  vcu::Encoder enc;
  // retry(allegro 初回 channel warm-up 対策): fresh reopen で最大 3 回。
  bool opened = false;
  for (int a = 0; a < 3 && !opened; a++){
    opened = enc.open_dev(dev, W, H, br, gop);
    if (!opened){ fprintf(stderr, "[stream] open_dev 失敗 attempt %d\n", a+1); enc.close_dev(); }
  }
  if (!opened){ fprintf(stderr, "[stream] %s open 不可(callegro 未 load?)\n", dev); return 1; }
  // warm-up: 初回 frame を 1 枚投入して pipeline を通す(初回 encode 空対策)。
  { std::vector<uint8_t> warm; enc.encode_frame(frame.data(), frame.size(), warm, 200); }

  // ★VCU_BENCH=N: encode 単体スループット計測(pacing/fifo 無で N frame を連続 encode)→ fps 表示して終了。
  if (const char* bn = getenv("VCU_BENCH")){
    int N = atoi(bn); if (N < 1) N = 300;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned long got = 0, bytes = 0;
    for (int i = 0; i < N; i++){ std::vector<uint8_t> au; enc.encode_frame(frame.data(), frame.size(), au, 0); got++; bytes += au.size(); }
    { std::vector<uint8_t> t; enc.flush(t); bytes += t.size(); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "[bench] VCU encode: %d frame in %.3fs = %.1f fps (%.2f Mbps 相当, %lu KB)\n",
            N, sec, N / sec, bytes * 8.0 / sec / 1e6, bytes / 1024);
    enc.close_dev(); return 0;
  }

  int outfd = -1;
  if (!strcmp(outp, "-")) outfd = STDOUT_FILENO;
  else { fprintf(stderr, "[stream] fifo open(reader 待ち): %s\n", outp); outfd = open(outp, O_WRONLY); }  // reader 接続まで block
  if (outfd < 0){ perror("[stream] out open"); return 1; }
  fprintf(stderr, "[stream] ▶ %dx%d @%dfps br=%d gop=%d dev=%s out=%s\n", W, H, fps, br, gop, dev, outp);

  const long period_ns = 1000000000L / fps;
  struct timespec ts;
  unsigned long nframe = 0, nbytes = 0;
  // ★st_mtime(整数秒)比較は 10fps 源に対し 1 回/秒しか変化を検知できず、
  //   「エンコード 10fps・内容 1fps」の配信になる(2026-08-12 実測)。ns 込みで比較する。
  struct timespec lastmt = {0, 0};

  while (!g_stop){
    // overlay NV12 が更新されていれば読み直す(mtime 変化時のみ)。
    struct stat st;
    if (stat(ovp, &st) == 0 &&
        (st.st_mtim.tv_sec != lastmt.tv_sec || st.st_mtim.tv_nsec != lastmt.tv_nsec) &&
        (size_t)st.st_size >= need){
      FILE* f = fopen(ovp, "rb");
      if (f){ if (fread(frame.data(), 1, need, f) == need) lastmt = st.st_mtim; fclose(f); }
    }
    std::vector<uint8_t> au;
    enc.encode_frame(frame.data(), frame.size(), au, 0);   // 即時に取れる H.264 AU を回収
    if (!au.empty()){
      ssize_t w = write(outfd, au.data(), au.size());
      if (w < 0){                       // reader 切断 → fifo 再 open(次 reader 待ち)
        if (strcmp(outp, "-")){ close(outfd); fprintf(stderr, "[stream] reader 切断 → fifo 再 open\n");
          outfd = open(outp, O_WRONLY); if (outfd < 0){ perror("[stream] reopen"); break; } }
      } else { nbytes += au.size(); }
    }
    nframe++;
    if ((nframe % (unsigned long)(fps * 10)) == 0)
      fprintf(stderr, "[stream] %lu frames, %lu KB 送出\n", nframe, nbytes / 1024);
    ts.tv_sec = 0; ts.tv_nsec = period_ns; nanosleep(&ts, nullptr);
  }
  std::vector<uint8_t> tail; enc.flush(tail);
  if (outfd >= 0 && strcmp(outp, "-")) close(outfd);
  enc.close_dev();
  fprintf(stderr, "[stream] 終了(%lu frames)\n", nframe);
  return 0;
}

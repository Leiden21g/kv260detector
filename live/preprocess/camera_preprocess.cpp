// camera_preprocess.cpp — camera_preprocess.py の C/OpenCV 等価実装(byte-exact ドロップイン)。
//
//   目的: KV260 board の live free-run feed で per-frame `python3 camera_preprocess.py` を spawn
//   していたのが律速(ARM Cortex-A53 で numpy/cv2 import だけ ~1.5s)。同一 CLI の C++ バイナリに
//   差し替え、interpreter/import 起動を排除して preprocess を ~数十ms に落とす。
//
//   処理 = camera_preprocess.py と完全一致:
//     NV12 raw(stride S=2×幅 padding)→ de-stride → cvtColor(YUV2BGR_NV12)
//     → letterbox(min-scale resize INTER_LINEAR + pad114)→ RGB/255 float32 CHW(1×3×H×W)。
//
//   ★byte-exact の肝:
//     - resize / cvtColor は OpenCV の同一 fixed-point backend(cv2 と同じ)→ uint8 が bit 一致。
//     - 最終の /255 は numpy の `astype(float32)/255.0`(= 単精度除算)を **(float)v/255.0f** で再現。
//       cv::dnn::blobFromImage は内部で double 乗算(v*(1/255))→ float cast のため 1ULP ずれ得る → 不使用。
//
//   CLI(python 版と同じ引数サブセット):
//     camera_preprocess --nv12 f.nv12 --out x.bin [--frame k] [--width 1920] [--height 1080]
//                       [--stride 3840] [--size 384 640] [--out-hwc16 live.bin] [--shift0 6]
//   ★req8(2026-08-29) --out-hwc16: host(src/host.cpp:chw_load)が x.bin から作る kernel 入力 **HWC int16 [H][W][4]**
//     (値 = (int16)(float * (1<<(8+shift0)))、c=3 は 0、EOF quirk o[0][0][3] = 最終 float の量子化値)を ppdaemon 側で
//     直接書く(サイズ H*W*4*2B = 640² で 3,276,800B)。host の chw_load はサイズで自動判別し fread 一発で取り込む
//     (float 4.9MB read + 1.2M 変換 → int16 3.3MB read)。shift0 = host の data_shift[0][0](既定 6)。
//     --out と --out-hwc16 は併用可(少なくとも一方が必須)。python 版 --out-hwc16 / chw_load_ref.py と byte-exact。
//
//   ★リンクは core+imgproc のみ(cvtColor/resize/Mat)。`pkg-config --libs opencv4` は opencv 全 ~40 .so を
//     リンク→起動時に全ロードで board 0.66s。最小リンクで board **0.088s**(19× vs python 1.71s)。
//   build(x86 検証): g++ -O2 -std=c++17 camera_preprocess.cpp -o camera_preprocess `pkg-config --cflags opencv4` -lopencv_imgproc -lopencv_core
//   build(board aarch64): 同じ 1 行を aarch64 の cross-compile で。sysroot に libopencv_core / libopencv_imgproc が要る(board 上で直接 g++ しても可)。
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <unistd.h>
#include <ctime>

static const char* arg_get(int argc, char** argv, const char* key, const char* def) {
    for (int i = 1; i < argc - 1; ++i)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return def;
}

// 24bit BMP writer(imgcodecs 非依存)。BMP = BGR bottom-up, row を 4byte 境界に pad → cv::Mat(BGR) を
// 下行から書くだけ。cv2.imread 互換。imwrite を link しないため起動 0.088s を維持。
static bool write_bmp24(const char* path, const cv::Mat& bgr) {
    const int W = bgr.cols, H = bgr.rows;
    const int rowbytes = W * 3, pad = (4 - (rowbytes & 3)) & 3, stride = rowbytes + pad;
    const unsigned dataSz = (unsigned)stride * H, fileSz = 54 + dataSz;
    unsigned char hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=fileSz; hdr[3]=fileSz>>8; hdr[4]=fileSz>>16; hdr[5]=fileSz>>24;
    hdr[10]=54;                                   // pixel data offset
    hdr[14]=40;                                   // DIB header size
    hdr[18]=W; hdr[19]=W>>8; hdr[20]=W>>16; hdr[21]=W>>24;
    hdr[22]=H; hdr[23]=H>>8; hdr[24]=H>>16; hdr[25]=H>>24;
    hdr[26]=1;                                    // planes
    hdr[28]=24;                                   // bpp
    hdr[34]=dataSz; hdr[35]=dataSz>>8; hdr[36]=dataSz>>16; hdr[37]=dataSz>>24;
    FILE* o = std::fopen(path, "wb");
    if (!o) return false;
    std::fwrite(hdr, 1, 54, o);
    std::vector<unsigned char> row(stride, 0);
    for (int y = H - 1; y >= 0; --y) {            // bottom-up
        std::memcpy(row.data(), bgr.ptr<unsigned char>(y), rowbytes);
        std::fwrite(row.data(), 1, stride, o);
    }
    std::fclose(o);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ★#17(2026-09-04)常駐 daemon 化 + float 中間の省略。
//   従来の board 運用は launcher の ppdaemon ループが **frame 毎に本バイナリを exec** していた
//   (board 実測 起動固定費 ~0.088s/回。capd 8fps では起動だけで CPU ~0.7 core)。加えて
//   `cp /dev/shm/live.nv12 /tmp/lv/f.nv12.tmp`(3.7MB)を挟むので、同じ frame を 2 回読んでいた。
//   --daemon は seq ファイルを自分で見張り、1 プロセスのまま
//     ① raw を 1 回だけ read → ② --bg-out(= f.nv12)へ atomic publish(cp を吸収)
//     → ③ 前処理 → ④ --out / --out-hwc16 へ atomic publish
//   を回す。**値の計算式は 1-shot 経路と 1 命令も変えていない**(byte-exact 維持)。
//   さらに `--out` 無し(hwc16 のみ)のときは float CHW 中間 vector(640² で 4.9MB)を作らず
//   同じ式で直接量子化する(= 同一 bit。float の中間丸めは (float)v/255.0f で共通)。
// ─────────────────────────────────────────────────────────────────────────────

struct PPCtx {                       // 反復間で使い回すバッファ群(daemon 時の malloc/free を消す)
    int W, H, S, OH, OW, shift0;
    const char* out_path;
    const char* hwc16_path;
    const char* png_path;
    std::vector<unsigned char> raw;
    cv::Mat nv12, bgr, res, canvas;
    std::vector<float> outf;         // --out 指定時のみ使う
    std::vector<int16_t> q;          // --out-hwc16 指定時のみ使う
    std::vector<unsigned char> iobuf;
};

// tmp へ書いて rename = reader からは常に完結したファイルに見える(atomic publish)
static bool write_atomic(const char* path, const void* data, size_t nbytes, bool atomic) {
    std::string tmp = path; if (atomic) tmp += ".tmp";
    FILE* o = std::fopen(tmp.c_str(), "wb");
    if (!o) { std::fprintf(stderr, "!! open %s 失敗\n", tmp.c_str()); return false; }
    bool ok = (std::fwrite(data, 1, nbytes, o) == nbytes);
    std::fclose(o);
    if (!ok) { std::fprintf(stderr, "!! write %s 失敗\n", tmp.c_str()); return false; }
    if (atomic && std::rename(tmp.c_str(), path) != 0) { std::fprintf(stderr, "!! rename %s 失敗\n", path); return false; }
    return true;
}

// raw(1 frame ぶんの NV12、stride S)→ 出力ファイル群。verbose=1-shot 時の従来 stderr を出す。
// 戻り値 = 成功可否。★計算式は従来コードと同一。
static bool pp_process(PPCtx& c, bool atomic, bool verbose, int frame_no, long nframes, double rscale_unused) {
    (void)rscale_unused;
    const int W = c.W, H = c.H, S = c.S, OH = c.OH, OW = c.OW;

    // ---- de-stride: stride S の Y(H 行)+ UV(H/2 行)を W 幅の連続 NV12 へ詰める(py: nv12_to_bgr)----
    if (c.nv12.empty()) c.nv12.create(H * 3 / 2, W, CV_8UC1);
    for (int r = 0; r < H; ++r)
        std::memcpy(c.nv12.ptr<uchar>(r), c.raw.data() + (size_t)r * S, W);
    for (int r = 0; r < H / 2; ++r)
        std::memcpy(c.nv12.ptr<uchar>(H + r), c.raw.data() + (size_t)S * H + (size_t)r * S, W);

    cv::cvtColor(c.nv12, c.bgr, cv::COLOR_YUV2BGR_NV12);

    // ---- --png: de-stride 後の元画像(BGR uint8 W×H)を保存(camera_detect.py --frame overlay 用)----
    if (c.png_path) write_bmp24(c.png_path, c.bgr);

    // ---- letterbox(py: letterbox)----
    const double rscale = std::min((double)OH / H, (double)OW / W);
    const int nh = (int)std::lround(H * rscale);
    const int nw = (int)std::lround(W * rscale);
    cv::resize(c.bgr, c.res, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);   // cv2.resize 既定 = INTER_LINEAR
    const int padx = (OW - nw) / 2, pady = (OH - nh) / 2;
    if (c.canvas.empty()) c.canvas.create(OH, OW, CV_8UC3);
    c.canvas.setTo(cv::Scalar(114, 114, 114));
    c.res.copyTo(c.canvas(cv::Rect(padx, pady, nw, nh)));

    const size_t HW = (size_t)OH * OW;
    const float qscale = (float)(1 << (8 + c.shift0));

    // ---- RGB/255 float32 CHW(py: canvas[:,:,::-1].transpose(2,0,1).astype(f32)/255.0)----
    //   out channel ch=0/1/2 = R/G/B = canvas(BGR) channel (2-ch)。除算は単精度で numpy と一致。
    //   ★--out 未指定(hwc16 のみ)のときは中間 vector を作らず、同じ式でその場で量子化する。
    const bool need_float = (c.out_path != nullptr);
    if (need_float && c.outf.size() != 3 * HW) c.outf.assign(3 * HW, 0.0f);
    if (c.hwc16_path && c.q.size() != HW * 4) c.q.assign(HW * 4, 0);
    float last_f = 0.0f;                                   // ch=2 の最終画素の float 値(EOF quirk 用)
    for (int ch = 0; ch < 3; ++ch) {
        const int src_c = 2 - ch;
        float* dst = need_float ? c.outf.data() + (size_t)ch * HW : nullptr;
        int16_t* qd = c.hwc16_path ? c.q.data() : nullptr;
        for (int y = 0; y < OH; ++y) {
            const cv::Vec3b* row = c.canvas.ptr<cv::Vec3b>(y);
            const size_t base = (size_t)y * OW;
            for (int x = 0; x < OW; ++x) {
                const float v = (float)row[x][src_c] / 255.0f;
                if (dst) dst[base + x] = v;
                if (qd)  qd[(base + x) * 4 + ch] = (int16_t)(v * qscale);
            }
        }
        if (ch == 2) {
            const cv::Vec3b* row = c.canvas.ptr<cv::Vec3b>(OH - 1);
            last_f = (float)row[OW - 1][0] / 255.0f;       // ch=2 の src_c=0 の最終画素
        }
    }

    if (c.out_path) {
        if (!write_atomic(c.out_path, c.outf.data(), c.outf.size() * sizeof(float), atomic)) return false;
        if (verbose)
            std::fprintf(stderr, "ok frame %d/%ld  %dx%d(stride %d) -> %dx%d float32 CHW (%zuB) r=%.4f pad=(%d,%d)\n",
                         frame_no, nframes, W, H, S, OH, OW, c.outf.size() * sizeof(float), rscale, padx, pady);
    }
    // ---- --out-hwc16: host.cpp:chw_load(float 経路)と byte 同一の HWC int16 [OH][OW][4](req8)----
    //   ★EOF quirk: host の旧 while(!feof) 由来で「最終 float 値」が (c=3,y=0,x=0) にもう 1 回書かれる。
    if (c.hwc16_path) {
        for (size_t i = 0; i < HW; ++i) c.q[i * 4 + 3] = 0;
        c.q[3] = (int16_t)(last_f * qscale);
        if (!write_atomic(c.hwc16_path, c.q.data(), c.q.size() * sizeof(int16_t), atomic)) return false;
        if (verbose)
            std::fprintf(stderr, "ok hwc16 %dx%d int16 [H][W][4] (%zuB) shift0=%d o[0][0][3]=%d -> %s\n",
                         OH, OW, c.q.size() * sizeof(int16_t), c.shift0, (int)c.q[3], c.hwc16_path);
    }
    return true;
}

// nv12_path から frame 番号 k を c.raw へ読む。ok なら true。
static bool pp_read_frame(PPCtx& c, const char* nv12_path, int frame, long* nframes_out, bool verbose) {
    const size_t frame_bytes = (size_t)c.S * c.H * 3 / 2;
    FILE* f = std::fopen(nv12_path, "rb");
    if (!f) { if (verbose) std::fprintf(stderr, "!! open %s 失敗\n", nv12_path); return false; }
    std::fseek(f, 0, SEEK_END); long fsz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    long nframes = (long)(fsz / frame_bytes);
    if (nframes_out) *nframes_out = nframes;
    if (nframes == 0) {
        if (verbose) std::fprintf(stderr, "!! %s が 1 frame(%zuB)に満たない: %ldB\n", nv12_path, frame_bytes, fsz);
        std::fclose(f); return false;
    }
    if (frame >= nframes) {
        if (verbose) std::fprintf(stderr, "!! --frame %d >= 含有 frame 数 %ld\n", frame, nframes);
        std::fclose(f); return false;
    }
    if (c.raw.size() != frame_bytes) c.raw.assign(frame_bytes, 0);
    std::fseek(f, (long)frame * frame_bytes, SEEK_SET);
    bool ok = (std::fread(c.raw.data(), 1, frame_bytes, f) == frame_bytes);
    if (!ok && verbose) std::fprintf(stderr, "!! read 失敗\n");
    std::fclose(f);
    return ok;
}

static long read_seq(const char* seq_path) {              // 読めない/空なら -1(= 従来どおり毎回処理)
    FILE* f = std::fopen(seq_path, "rb");
    if (!f) return -1;
    char b[64] = {0};
    size_t n = std::fread(b, 1, sizeof(b) - 1, f);
    std::fclose(f);
    if (n == 0) return -1;
    char* end = nullptr;
    long v = std::strtol(b, &end, 10);
    if (end == b) return -1;
    return v;
}

int main(int argc, char** argv) {
    const char* nv12_path = arg_get(argc, argv, "--nv12", nullptr);
    const char* out_path  = arg_get(argc, argv, "--out",  nullptr);
    const char* hwc16_path = arg_get(argc, argv, "--out-hwc16", nullptr);   // req8: 量子化済 HWC int16 publish
    const int   shift0     = std::atoi(arg_get(argc, argv, "--shift0", "6"));   // = host data_shift[0][0]
    bool daemon = false;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--daemon") == 0) daemon = true;
    const char* seq_path = arg_get(argc, argv, "--seq", nullptr);           // daemon: 新 frame 検知(capd の <out>.seq)
    const char* bg_path  = arg_get(argc, argv, "--bg-out", nullptr);        // daemon: 読んだ raw をそのまま対 publish(= 旧 cp)
    const int poll_ms    = std::atoi(arg_get(argc, argv, "--poll-ms", "20"));
    const int min_int_ms = std::atoi(arg_get(argc, argv, "--min-interval-ms", "0")); // >0 で処理間隔の下限(更なる間引き)
    if (!nv12_path || (!out_path && !hwc16_path)) {
        std::fprintf(stderr, "usage: %s --nv12 f.nv12 (--out x.bin | --out-hwc16 live.bin) "
                             "[--frame k] [--width 1920] [--height 1080] [--stride 3840] [--size H W] [--shift0 6]\n"
                             "       %s --daemon --nv12 /dev/shm/live.nv12 --seq /dev/shm/live.nv12.seq "
                             "[--bg-out /tmp/lv/f.nv12] [--poll-ms 20] [--min-interval-ms 0] ...\n", argv[0], argv[0]);
        return 2;
    }
    PPCtx c;
    c.W = std::atoi(arg_get(argc, argv, "--width",  "1920"));
    c.H = std::atoi(arg_get(argc, argv, "--height", "1080"));
    c.S = std::atoi(arg_get(argc, argv, "--stride", "3840"));   // NV12 stride(=2×幅 padding 既定)
    c.OH = 384; c.OW = 640;                                     // letterbox 出力(FPGA=384×640)
    for (int i = 1; i < argc - 2; ++i)
        if (std::strcmp(argv[i], "--size") == 0) { c.OH = std::atoi(argv[i + 1]); c.OW = std::atoi(argv[i + 2]); }
    c.shift0 = shift0;
    c.out_path = out_path; c.hwc16_path = hwc16_path;
    c.png_path = arg_get(argc, argv, "--png", nullptr);
    const int frame = std::atoi(arg_get(argc, argv, "--frame", "0"));

    if (!daemon) {
        // ---- 従来の 1-shot(★出力は非 atomic = 旧挙動そのまま。launcher 側が .tmp→mv している)----
        long nframes = 0;
        if (!pp_read_frame(c, nv12_path, frame, &nframes, true)) return 1;
        return pp_process(c, /*atomic=*/false, /*verbose=*/true, frame, nframes, 0.0) ? 0 : 1;
    }

    // ---- daemon: seq 差分で新 frame だけを処理し、対で atomic publish ----
    std::fprintf(stderr, "[pp-daemon] nv12=%s seq=%s bg=%s out=%s hwc16=%s %dx%d(stride %d) -> %dx%d poll=%dms min_int=%dms\n",
                 nv12_path, seq_path ? seq_path : "(none)", bg_path ? bg_path : "(none)",
                 out_path ? out_path : "(none)", hwc16_path ? hwc16_path : "(none)",
                 c.W, c.H, c.S, c.OH, c.OW, poll_ms, min_int_ms);
    long last_seq = -1;
    long long last_proc_ms = -1;
    unsigned long done = 0, fail = 0;
    for (;;) {
        if (seq_path) {
            const long s = read_seq(seq_path);
            // ⚠ seq が無い/読めない時は従来どおり毎回処理する(= 旧動作に安全に fallback)
            if (s >= 0 && s == last_seq) { usleep((useconds_t)poll_ms * 1000); continue; }
            if (s >= 0) last_seq = s;
        }
        if (min_int_ms > 0) {                       // 案A: host 消費 fps に合わせた追加間引き
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            const long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (last_proc_ms >= 0 && now_ms - last_proc_ms < min_int_ms) { usleep((useconds_t)poll_ms * 1000); continue; }
            last_proc_ms = now_ms;
        }
        if (!pp_read_frame(c, nv12_path, 0, nullptr, false)) { ++fail; usleep((useconds_t)poll_ms * 1000); continue; }
        // ★f.nv12(背景)を live.bin より **先に** publish(OVERLAY_BG_PAIR の前提。旧 launcher の cp+mv と同順)
        if (bg_path && !write_atomic(bg_path, c.raw.data(), c.raw.size(), true)) { ++fail; continue; }
        if (!pp_process(c, /*atomic=*/true, /*verbose=*/false, 0, 1, 0.0)) { ++fail; continue; }
        if ((++done % 200) == 0)
            std::fprintf(stderr, "[pp-daemon] done=%lu fail=%lu seq=%ld\n", done, fail, last_seq);
    }
    return 0;
}

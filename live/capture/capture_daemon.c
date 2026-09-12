// capture_daemon.c — KV260 IAS (AR1335+AP1302) の V4L2 stream を「1本だけ開きっぱなし」にして
//   最新フレームを atomic に共有ファイルへ書き出す常駐 daemon。
//
//   ★目的: free-run live camera の CSI wedge 回避。単発 v4l2-ctl の都度 stream-on/off が
//     AP1302 free-run で line buffer を溢れさせ wedge する。
//     本 daemon は STREAMON を 1 回だけ行い DQBUF/QBUF ループで drain し続ける=wedge しない。
//
//   出力: --out (既定 /dev/shm/live_latest.nv12) に最新 NV12 frame を tmp+rename で atomic 更新。
//         並びで <out>.seq に 10進フレーム番号(ASCII)を書く=reader が新フレーム検知に使える。
//   使い方: capture_daemon [--dev /dev/video0] [--out /dev/shm/live_latest.nv12] [--w 1920 --h 1080]
//                          [--fps N]
//   停止: SIGINT/SIGTERM で STREAMOFF→close。
//
//   ★--fps N(既定 0=無制限, 従来動作): shm への **書き出しだけ** を N fps に間引く。
//     DQBUF/QBUF は毎フレーム続ける(止めると V4L2 キューが詰まり CSI が wedge する)。
//     動機 = capd の CPU は「1 frame ごとの 4MB write + rename ×2(本体/seq)」が主で、
//     centre は帯域律速のため解像度を下げても fps が上がって同じだけ食う(720p: 9.5fps/37MB/s で
//     capd 単独 96-99%)。下流(検出 2.5fps / RTSP 5fps)は 5-6fps あれば足りるので、
//     write を間引くのが唯一の実効的な CPU レバー。
//
//   ★--mmap (env CAPD_MMAP=1, 既定 OFF = 従来動作): A-13 host CPU 削減。
//     従来 publish = open(O_TRUNC)+write(4MB)+close+rename。O_TRUNC が tmpfs page を毎フレーム
//     解放し write が新規 page を確保+zero+copy する = 4MB 分の page fault が毎フレーム走る。
//     mmap 版は「常時 mmap 済の slot を N 本」用意し memcpy で書いて renameat2(RENAME_EXCHANGE)
//     で <out> と入れ替える。page は起動時に 1 度だけ fault させる = 定常では memcpy + syscall 1 本。
//     ★reader 契約は不変: <out> は常に完結した 1 frame の通常ファイルで、publish は原子的。
//       publish 中の inode は <out> に無い(=reader が触れない)ので tearing しない。
//     slot 数 N(--slots, 既定 3)= 「reader が fd を握ったまま何 frame 耐えられるか」の猶予。
//     再利用は N-1 frame 後なので N=3 なら 2 frame 分の猶予がある(従来の unlink 方式は無限)。
// _GNU_SOURCE: renameat2/RENAME_EXCHANGE (A-13) に加え、-std=c11 下で ftruncate/clock_gettime 等の
// POSIX 宣言を有効化する(__STRICT_ANSI__ だと隠れて implicit-declaration になる)。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <linux/videodev2.h>

#define NBUF 4
static volatile int g_run = 1;
static void on_sig(int s){ (void)s; g_run = 0; }

static int xioctl(int fd, unsigned long req, void *arg){
    int r; do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

// ---------------------------------------------------------------------------
// ★A-13: mmap publish ring
//   不変条件: ring.map[i] は「今 ring.path[i] にある inode」の mapping。path[0] = <out>。
//   publish(j!=0) = memcpy → renameat2(EXCHANGE, path[j], path[0]) → map[0]<->map[j] を swap。
//   これで unlink/truncate/write を一切使わずに <out> を原子的に差し替えられる。
// ---------------------------------------------------------------------------
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif

#define MAXSLOT 8
typedef struct {
    int    n;                     // slot 数 (>=2)。0 = 無効(従来 write+rename 経路)
    int    next;                  // 次に書く staging slot (1..n-1)
    char   path[MAXSLOT][512];    // path[0] = out
    void  *map[MAXSLOT];
    int    fd[MAXSLOT];
    size_t cap;                   // mapping 長 = 起動時 imgsz
    size_t size[MAXSLOT];         // 各 inode の現ファイル長(bytesused 変化時のみ ftruncate)
} ring_t;

static int ring_exchange(const char *a, const char *b){
    return (int)syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, (unsigned int)RENAME_EXCHANGE);
}

// ---------------------------------------------------------------------------
// ★pub 変換(2026-09-04, web「切り抜き」ボタン): 大 pad(例 2880×2160 / 3840×2160)で取り込み、
//   publish 時に 640² へ変換して従来と同一レイアウト(stride 3840 の N×N NV12)で書く。
//   動機 = 2026-09-04 のカメラ crop 実測:
//     - AP1302 の zoom_absolute は疑似ズーム(実解像が増えない)、subdev crop は不受理
//     - cap-only は 4K でも ~27fps だが、フル frame の publish 書出は ~44MB/s 律速
//     → 「取込は大 pad・書出は 640² だけ」にすれば等倍級の切り抜きが帯域中立で得られる。
//   モード(--mode-file、毎 publish 読む。既定/読めない時 = wide):
//     wide = 中央 正方(min(W,H)²)を nearest で N² へ縮小(全景。従来 zoom256 の画角相当)
//     zoom = 中央 N² を等倍 crop(切り抜き)
//   ★reader 契約不変: 出力 frame は「stride 3840 × N 行 × NV12」= 現行本番の live.nv12 と同一。
// ---------------------------------------------------------------------------
typedef struct {
    int      N;            // 出力正方サイズ(0 = 無効 = 従来動作)
    int      dstride;      // 出力 stride(既定 3840)
    size_t   dbytes;       // = dstride*N*3/2
    int      sw, sh, sst;  // 取込幾何(G_FMT から)
    const char *mode_file; // wide|zoom
    uint16_t *cmap;        // wide 用 列 nearest map(Y: N 個 / UV: N 個(偶数化))
    uint8_t  *scratch;     // 非 ring 経路用の変換先
} pub_t;

// mode file 書式: "wide" | "zoom" | "zoom <dx> <dy>"(dx/dy = 中央からの crop 原点オフセット px)
//   ★pan(2026-09-04): zoom 時に web の上下左右ボタンで切り抜き位置を動かす。範囲は capd 側で
//     [0, sw-N] × [0, sh-N] へ clamp する(UI 側の clamp が甘くても窓は必ず frame 内)。
static int pub_mode_zoom(const pub_t *p, int *dx, int *dy){   // 1=zoom / 0=wide
    *dx = 0; *dy = 0;
    if (!p->mode_file) return 0;
    char b[64]; ssize_t n = 0;
    int fd = open(p->mode_file, O_RDONLY);
    if (fd >= 0){ n = read(fd, b, sizeof b - 1); close(fd); }
    if (n <= 0 || b[0] != 'z') return 0;
    b[n] = '\0';
    const char *s = b;
    while (*s && *s != ' ' && *s != '\n') s++;   // モード語をスキップ
    if (*s == ' '){ int a = 0, c = 0; if (sscanf(s, "%d %d", &a, &c) == 2){ *dx = a; *dy = c; } }
    return 1;
}

static int pub_init(pub_t *p, int N, int dstride, const char *mode_file, int sw, int sh, int sst){
    memset(p, 0, sizeof *p);
    if (N <= 0) return 0;
    if (sw < N || sh < N){ fprintf(stderr, "[capd] pub: 取込 %dx%d < %d → pub 無効(従来動作)\n", sw, sh, N); return 0; }
    p->N = N; p->dstride = dstride; p->dbytes = (size_t)dstride * N * 3 / 2;
    p->sw = sw; p->sh = sh; p->sst = sst; p->mode_file = mode_file;
    int Q = (sw < sh ? sw : sh) & ~1;           // wide の中央正方
    int qx0 = ((sw - Q) / 2) & ~1;
    p->cmap = malloc(sizeof(uint16_t) * N * 2);
    p->scratch = calloc(1, p->dbytes);
    if (!p->cmap || !p->scratch){ fprintf(stderr, "[capd] pub: malloc 失敗 → pub 無効\n"); p->N = 0; return 0; }
    for (int i = 0; i < N; i++){
        int sx = qx0 + (int)((long)i * Q / N);
        p->cmap[i]     = (uint16_t)sx;          // Y 列
        p->cmap[N + i] = (uint16_t)(sx & ~1);   // UV 列(U/V 対の先頭 = 偶数 byte)
    }
    fprintf(stderr, "[capd] pub: %dx%d(st%d) → %d²(st%d, %zuB) wide=Q%d zoom=中央crop mode=%s\n",
            sw, sh, sst, N, dstride, p->dbytes, Q, mode_file ? mode_file : "(固定 wide)");
    return 1;
}

// src(取込 frame)→ dst(N² stride-dstride NV12)。dst の 640B 超の残り byte は初期 0 のまま
// (ring slot / scratch とも起動時 memset 済 = per-frame では触らない)。
static void pub_xform(const pub_t *p, const uint8_t *src, uint8_t *dst){
    const int N = p->N, DS = p->dstride, SST = p->sst;
    const uint8_t *suv = src + (size_t)SST * p->sh;
    uint8_t *duv = dst + (size_t)DS * N;
    int dx = 0, dy = 0;
    if (pub_mode_zoom(p, &dx, &dy)){
        int x0 = ((p->sw - N) / 2) + dx;
        int y0 = ((p->sh - N) / 2) + dy;
        if (x0 < 0) x0 = 0; else if (x0 > p->sw - N) x0 = p->sw - N;
        if (y0 < 0) y0 = 0; else if (y0 > p->sh - N) y0 = p->sh - N;
        x0 &= ~1; y0 &= ~1;   // NV12: UV は 2×2 単位 = 原点は偶数でなければ色がずれる
        for (int r = 0; r < N; r++)
            memcpy(dst + (size_t)r * DS, src + (size_t)(y0 + r) * SST + x0, N);
        for (int r = 0; r < N / 2; r++)
            memcpy(duv + (size_t)r * DS, suv + (size_t)(y0 / 2 + r) * SST + x0, N);
    } else {
        const int Q = (p->sw < p->sh ? p->sw : p->sh) & ~1;
        const int qy0 = ((p->sh - Q) / 2) & ~1;
        const uint16_t *cy = p->cmap, *cuv = p->cmap + N;
        for (int r = 0; r < N; r++){
            const uint8_t *s = src + (size_t)(qy0 + (int)((long)r * Q / N)) * SST;
            uint8_t *d = dst + (size_t)r * DS;
            for (int i = 0; i < N; i++) d[i] = s[cy[i]];
        }
        for (int r = 0; r < N / 2; r++){
            const uint8_t *s = suv + (size_t)(qy0 / 2 + (int)((long)r * Q / N)) * SST;
            uint8_t *d = duv + (size_t)r * DS;
            for (int i = 0; i < N; i += 2){ int c = cuv[i]; d[i] = s[c]; d[i+1] = s[c+1]; }
        }
    }
}

// 成功=0。失敗時は ring->n=0 に落として呼び元が従来経路へフォールバックする。
// ★cap は「初フレームの実 bytesused」で決める(遅延 init)。定常では publish 長 == cap となり
//   ftruncate が 1 度も走らない = per-frame の page 解放/再 fault がゼロになる。
static int ring_init(ring_t *r, const char *out, int nslot, size_t imgsz){
    memset(r, 0, sizeof *r);
    if (nslot < 2) nslot = 2;
    if (nslot > MAXSLOT) nslot = MAXSLOT;
    r->cap = imgsz;
    for (int i = 0; i < nslot; i++){
        if (i == 0) snprintf(r->path[i], sizeof r->path[i], "%s", out);
        else        snprintf(r->path[i], sizeof r->path[i], "%s.slot%d", out, i);
        r->fd[i] = open(r->path[i], O_RDWR|O_CREAT, 0644);
        if (r->fd[i] < 0){ fprintf(stderr, "[capd] mmap ring: open %s: %s\n", r->path[i], strerror(errno)); return -1; }
        if (ftruncate(r->fd[i], (off_t)imgsz) < 0){ fprintf(stderr, "[capd] mmap ring: ftruncate: %s\n", strerror(errno)); return -1; }
        r->size[i] = imgsz;
        r->map[i] = mmap(NULL, imgsz, PROT_READ|PROT_WRITE, MAP_SHARED, r->fd[i], 0);
        if (r->map[i] == MAP_FAILED){ fprintf(stderr, "[capd] mmap ring: mmap: %s\n", strerror(errno)); r->map[i]=NULL; return -1; }
        memset(r->map[i], 0, imgsz);   // ★起動時に 1 度だけ page fault を払う(定常の per-frame コスト排除)
    }
    // RENAME_EXCHANGE が使えるか起動時に検証(古い kernel / 非対応 fs を即検出)。
    if (ring_exchange(r->path[1], r->path[0]) < 0){
        fprintf(stderr, "[capd] mmap ring: renameat2(RENAME_EXCHANGE): %s → 従来 write+rename へ fallback\n", strerror(errno));
        return -1;
    }
    { void *t = r->map[0]; r->map[0] = r->map[1]; r->map[1] = t; }
    { int t = r->fd[0]; r->fd[0] = r->fd[1]; r->fd[1] = t; }
    { size_t t = r->size[0]; r->size[0] = r->size[1]; r->size[1] = t; }
    r->n = nslot; r->next = 1;
    return 0;
}

static void ring_free(ring_t *r){
    for (int i = 0; i < r->n; i++){
        if (r->map[i]) munmap(r->map[i], r->cap);
        if (r->fd[i] >= 0) close(r->fd[i]);
        if (i > 0) unlink(r->path[i]);   // <out> は残す(reader が最後の frame を読める)
    }
    r->n = 0;
}

// frame を publish。成功=0 / 失敗=-1(呼び元は従来経路へ落とす)。
static int ring_publish(ring_t *r, const void *src, size_t n){
    int j = r->next;
    if (n > r->cap) n = r->cap;
    // ★memcpy の前に file 長を必ず cap へ戻す。縮んだままの inode に mapping 全域から書くと
    //   file 末尾を超えたページで SIGBUS になる(自己検証で実際に踏んだ)。
    if (r->size[j] != r->cap){
        if (ftruncate(r->fd[j], (off_t)r->cap) < 0) return -1;
        r->size[j] = r->cap;
    }
    memcpy(r->map[j], src, n);
    // 従来は write(n) ゆえ file 長 = bytesused。stat() で長さを見る reader のため一致させる。
    // 定常では n == cap なので ftruncate は 1 度も走らない。
    if (n != r->cap){
        if (ftruncate(r->fd[j], (off_t)n) < 0) return -1;
        r->size[j] = n;
    }
    if (ring_exchange(r->path[j], r->path[0]) < 0) return -1;
    { void *t = r->map[0]; r->map[0] = r->map[j]; r->map[j] = t; }
    { int t = r->fd[0]; r->fd[0] = r->fd[j]; r->fd[j] = t; }
    { size_t t = r->size[0]; r->size[0] = r->size[j]; r->size[j] = t; }
    r->next = (j + 1 >= r->n) ? 1 : j + 1;
    return 0;
}

// ★pub 用: staging slot の生ポインタを貸す/コミットする(ring_publish の分割形。
//   pub_xform が slot へ直接書く = scratch 経由の余分な 3.7MB memcpy を避ける)。
static void *ring_stage(ring_t *r){
    int j = r->next;
    if (r->size[j] != r->cap){
        if (ftruncate(r->fd[j], (off_t)r->cap) < 0) return NULL;
        r->size[j] = r->cap;
    }
    return r->map[j];
}
static int ring_commit(ring_t *r, size_t n){
    int j = r->next;
    if (n != r->cap){
        if (ftruncate(r->fd[j], (off_t)n) < 0) return -1;
        r->size[j] = n;
    }
    if (ring_exchange(r->path[j], r->path[0]) < 0) return -1;
    { void *t = r->map[0]; r->map[0] = r->map[j]; r->map[j] = t; }
    { int t = r->fd[0]; r->fd[0] = r->fd[j]; r->fd[j] = t; }
    { size_t t = r->size[0]; r->size[0] = r->size[j]; r->size[j] = t; }
    r->next = (j + 1 >= r->n) ? 1 : j + 1;
    return 0;
}

int main(int argc, char **argv){
    const char *dev = "/dev/video0";
    const char *out = "/dev/shm/live_latest.nv12";
    int W = 1920, H = 1080;
    double fps_limit = 0.0;                 // 0 = 無制限(従来動作)
    int use_mmap = 0;                       // ★A-13: 既定 OFF = 従来 write+rename と byte 等価
    int nslot = 3;
    int pub_n = 0, pub_stride = 3840;       // ★pub 変換(既定 OFF = 従来動作)
    const char *mode_file = NULL;
    if (getenv("CAPD_MMAP")) use_mmap = atoi(getenv("CAPD_MMAP"));
    if (getenv("CAPD_SLOTS")) nslot = atoi(getenv("CAPD_SLOTS"));
    for (int i = 1; i < argc; i++){
        if (!strcmp(argv[i], "--dev") && i+1<argc) dev = argv[++i];
        else if (!strcmp(argv[i], "--out") && i+1<argc) out = argv[++i];
        else if (!strcmp(argv[i], "--w") && i+1<argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--h") && i+1<argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i+1<argc) fps_limit = atof(argv[++i]);
        else if (!strcmp(argv[i], "--mmap")) use_mmap = 1;
        else if (!strcmp(argv[i], "--no-mmap")) use_mmap = 0;
        else if (!strcmp(argv[i], "--slots") && i+1<argc) nslot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pub-square") && i+1<argc) pub_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pub-stride") && i+1<argc) pub_stride = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode-file") && i+1<argc) mode_file = argv[++i];
    }
    const double min_period = fps_limit > 0.0 ? 1.0 / fps_limit : 0.0;
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    int fd = open(dev, O_RDWR);
    if (fd < 0){ fprintf(stderr, "open %s: %s\n", dev, strerror(errno)); return 2; }

    // ★KV260 xilinx-video capture は Multiplanar (CAPTURE_MPLANE)。NV12 contiguous = 1 plane。
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt);
    fmt.type = t;
    // ★2026-06-29 fix: 既定は G_FMT で「外部 v4l2-ctl --set-fmt-video が確立した format」を継承する。
    //   daemon 自前の S_FMT は ap1302 sensor s_stream を un-trigger し DQBUF 無 frame("no frame 10s")に
    //   なる(v4l2-ctl --stream-mmap は外部 set-fmt の format で stream し frame 取得=board 実証)。
    //   旧挙動(stride-3840 quirk を強制 S_FMT)は env CAPD_SFMT=1 で復活可。
    if (getenv("CAPD_SFMT") && atoi(getenv("CAPD_SFMT"))) {
        fmt.fmt.pix_mp.width = W; fmt.fmt.pix_mp.height = H;
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.num_planes = 1;
        fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 3840;   // KV260 IAS frmbuf stride-3840 quirk(実frame=6220800)
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 6220800;
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0){ fprintf(stderr, "S_FMT: %s\n", strerror(errno)); return 2; }
    } else {
        // G_FMT: 外部 set-fmt の format を読むだけ(s_stream を保つ)。
        if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0){ fprintf(stderr, "G_FMT: %s\n", strerror(errno)); return 2; }
    }
    unsigned nplanes = fmt.fmt.pix_mp.num_planes ? fmt.fmt.pix_mp.num_planes : 1;
    size_t imgsz = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (imgsz < (size_t)W*H*3/2) imgsz = (size_t)W*H*3/2;   // 安全下限(stride-3840 で 6220800)
    fprintf(stderr, "[capd] %s %ux%u NV12 mplane=%u sizeimage=%zu bpl=%u out=%s\n",
            dev, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, nplanes, imgsz,
            fmt.fmt.pix_mp.plane_fmt[0].bytesperline, out);
    // ★pub 変換 init(取込幾何は G_FMT の実値。bpl=0 の driver は sizeimage から逆算)
    pub_t pub;
    {
        int sw = (int)fmt.fmt.pix_mp.width, sh = (int)fmt.fmt.pix_mp.height;
        int sst = (int)fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        if (sst <= 0 && sh > 0) sst = (int)(imgsz * 2 / (3 * (size_t)sh));
        pub_init(&pub, pub_n, pub_stride, mode_file, sw, sh, sst);
    }

    struct v4l2_requestbuffers req; memset(&req, 0, sizeof req);
    req.count = NBUF; req.type = t; req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0){ fprintf(stderr, "REQBUFS: %s\n", strerror(errno)); return 2; }

    void *buf[NBUF]; size_t blen[NBUF];
    for (unsigned i = 0; i < req.count; i++){
        struct v4l2_buffer b; struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&b, 0, sizeof b); memset(planes, 0, sizeof planes);
        b.type = t; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.m.planes = planes; b.length = nplanes;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0){ fprintf(stderr, "QUERYBUF: %s\n", strerror(errno)); return 2; }
        blen[i] = b.m.planes[0].length;
        buf[i] = mmap(NULL, blen[i], PROT_READ|PROT_WRITE, MAP_SHARED, fd, b.m.planes[0].m.mem_offset);
        if (buf[i] == MAP_FAILED){ fprintf(stderr, "mmap: %s\n", strerror(errno)); return 2; }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0){ fprintf(stderr, "QBUF init: %s\n", strerror(errno)); return 2; }
    }

    // ★AP1302 は stream toggle に脆弱(off→on で sensor が止まる, 2026-06-24 実証)。
    //   ∴ 起動時 STREAMOFF は行わず STREAMON 1回のみ(toggle ゼロ)=sustained 維持の鍵。
    //   systemd 即exit 対策は startup STREAMOFF でなく下の poll/DQBUF エラー耐性で担保する。
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0){ fprintf(stderr, "STREAMON: %s\n", strerror(errno)); return 2; }
    fprintf(stderr, "[capd] STREAMON ok (mplane) — 永続 stream 開始\n");

    char tmp[512], seqp[512], seqtmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    snprintf(seqp, sizeof seqp, "%s.seq", out);
    snprintf(seqtmp, sizeof seqtmp, "%s.seq.tmp", out);

    // ★A-13: mmap publish ring。cap を実 bytesused に合わせるため初フレームまで init を遅延する。
    //   init 失敗は致命ではなく従来経路へ落とす(運用継続を優先)。
    ring_t ring; ring.n = 0;
    int ring_want = use_mmap;

    unsigned long seq = 0;
    unsigned long dq_total = 0;  // ★heartbeat: DQBUF 成功総数(stall の切り分け用)
    double last_wr = 0.0;        // 直近の shm 書き出し時刻(--fps 間引き用)
    int consec_err = 0;          // 連続エラー数(一過性は許容、持続時のみ exit)
    unsigned long stall_polls = 0;  // フレーム未着の poll 連続回数(sensor warm-up/flaky 許容)
    const int MAX_CONSEC_ERR = 50;
    while (g_run){
        // ★robust 化: poll() で readability を timeout 待ち。
        //   - timeout(=フレーム未着, sensor 非streaming): exit せず継続(warm-up/flaky 吸収)。
        //   - DQBUF エラー: 即 break せず再 QBUF して継続。連続 MAX_CONSEC_ERR 超で初めて exit。
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);   // 1s timeout
        if (pr == 0){ if((++stall_polls % 10)==0) fprintf(stderr, "[capd] no frame %lus (sensor streaming?)\n", stall_polls); continue; }
        if (pr < 0){ if (errno == EINTR) continue; fprintf(stderr, "[capd] poll: %s\n", strerror(errno)); if(++consec_err>MAX_CONSEC_ERR) break; continue; }
        stall_polls = 0;
        struct v4l2_buffer b; struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&b, 0, sizeof b); memset(planes, 0, sizeof planes);
        b.type = t; b.memory = V4L2_MEMORY_MMAP; b.m.planes = planes; b.length = nplanes;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0){
            if (errno == EAGAIN) continue;
            fprintf(stderr, "[capd] DQBUF: %s (consec=%d)\n", strerror(errno), consec_err+1);
            if (++consec_err > MAX_CONSEC_ERR){ fprintf(stderr, "[capd] DQBUF 連続失敗 %d → exit\n", consec_err); break; }
            continue;
        }
        consec_err = 0;
        // ★heartbeat(2026-09-04): 100 DQBUF 毎に dq/pub を刷る。大 pad 実験で「publish が 23 で
        //   止まるがログ無音」を踏んだため(DQBUF が止まったのか publish が止まったのかを分ける)。
        if ((++dq_total % 100) == 0)
            fprintf(stderr, "[capd] hb dq=%lu pub=%lu\n", dq_total, seq);
        // ★--fps: 書き出しだけ間引く(DQBUF/QBUF は毎フレーム続ける=キューを詰まらせない)。
        int do_write = 1;
        if (min_period > 0.0){
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            double t = now.tv_sec + now.tv_nsec * 1e-9;
            if (last_wr > 0.0 && (t - last_wr) < min_period) do_write = 0;
            else last_wr = t;
        }
        // 最新フレームを atomic publish。ring 有効時は mmap+RENAME_EXCHANGE、無効時は従来 tmp→rename。
        if (do_write){
            size_t n = b.m.planes[0].bytesused ? b.m.planes[0].bytesused : imgsz;
            const uint8_t *psrc = (const uint8_t *)buf[b.index];
            if (pub.N) n = pub.dbytes;   // ★pub: 出力は変換後レイアウト長(reader 契約 = 現行 640² frame と同一)
            int published = 0;
            if (ring_want){
                ring_want = 0;
                if (ring_init(&ring, out, nslot, n) == 0)
                    fprintf(stderr, "[capd] mmap publish ring: slots=%d cap=%zu (per-frame の write+rename と page 再確保を排除)\n", ring.n, ring.cap);
                else { ring_free(&ring); ring.n = 0;
                       fprintf(stderr, "[capd] mmap publish ring 初期化失敗 → 従来 write+rename で継続\n"); }
            }
            if (ring.n){
                if (pub.N){
                    // ★pub: slot へ直接変換書込(padding は init 時 0 のまま = 両モードとも書く byte 位置が同一)
                    void *d = ring_stage(&ring);
                    if (d){ pub_xform(&pub, psrc, (uint8_t *)d);
                            if (ring_commit(&ring, n) == 0) published = 1; }
                    if (!published){ fprintf(stderr, "[capd] ring pub 失敗(%s) → 従来 write へ降格\n", strerror(errno));
                                     ring_free(&ring); ring.n = 0; }
                } else {
                    if (ring_publish(&ring, psrc, n) == 0) published = 1;
                    else { fprintf(stderr, "[capd] ring publish 失敗(%s) → 従来 write+rename へ降格\n", strerror(errno));
                           ring_free(&ring); ring.n = 0; }
                }
            }
            if (!published){
                if (pub.N){ pub_xform(&pub, psrc, pub.scratch); psrc = pub.scratch; }
                int ofd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (ofd >= 0){
                    ssize_t w = write(ofd, psrc, n); (void)w;
                    close(ofd);
                    rename(tmp, out);
                    published = 1;
                }
            }
            // ★seq は frame publish の「後」に更新する(reader は seq 増加を新 frame 到着の合図に使う)。
            if (published){
                seq++;
                int sfd = open(seqtmp, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (sfd >= 0){ char sb[32]; int L = snprintf(sb, sizeof sb, "%lu\n", seq); ssize_t ww=write(sfd, sb, L); (void)ww; close(sfd); rename(seqtmp, seqp); }
            }
        }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0){
            fprintf(stderr, "[capd] QBUF: %s (consec=%d)\n", strerror(errno), consec_err+1);
            if (++consec_err > MAX_CONSEC_ERR) break;
        }
    }

    xioctl(fd, VIDIOC_STREAMOFF, &t);
    for (unsigned i = 0; i < req.count; i++) munmap(buf[i], blen[i]);
    close(fd);
    fprintf(stderr, "[capd] stopped after %lu frames\n", seq);
    return 0;
}

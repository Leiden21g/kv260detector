// vcu_enc.h — host-includable, 依存 = Linux V4L2 UAPI + libc のみ。VCU(allegro-dvt)H.264 encoder を
//   GStreamer 非依存で C++ から直接駆動する single-plane M2M ドライバ(/dev/video1)。
//   入力 = packed NV12(y26 overlay 出力とバイト一致)/ 出力 = H.264 Annex-B(byte-stream/au, SPS/PPS+IDR)。
//
//   ★board 実測 caps(2026-07-06):
//     Capabilities=0x84208000 = single-plane M2M(MPLANE でない)。
//     OUTPUT = NV12 のみ, bytesperline=W, sizeimage=W*H*3/2。CAPTURE = H264/HEVC。
//     H.264 profile=Baseline 固定。★video_bitrate 既定=240Mbps(max)= 必ず S_CTRL で下げる。
//
//   使い方(単発 = 1 frame):
//     vcu::Encoder enc;
//     if (enc.open_dev("/dev/video1", W, H, 3000000, 25)) {
//       std::vector<uint8_t> h264;
//       enc.encode_oneshot(nv12, W*H*3/2, h264);   // submit → ENC STOP → LAST まで drain
//       fwrite(h264.data(),1,h264.size(),fp); enc.close_dev();
//     }
//   使い方(連続 = stream): open_dev 後 per-frame submit(nv12) → drain(out); 最後に flush(out)。
#ifndef VCU_ENC_H
#define VCU_ENC_H
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <linux/videodev2.h>

namespace vcu {

struct MBuf { void* start = nullptr; size_t length = 0; };

class Encoder {
public:
  ~Encoder(){ close_dev(); }

  // dev=/dev/video1, WxH, bitrate[bps], gop[frames]。成功で true。
  bool open_dev(const char* dev, int W, int H, int bitrate = 3000000, int gop = 25){
    W_ = W; H_ = H; free_out_ = 0;   // 再 open 時の状態リセット(reopen retry 対応)
    fd_ = ::open(dev, O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0){ perror("[vcu] open"); return false; }
    // OUTPUT(=入力 NV12)
    v4l2_format of{}; of.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    of.fmt.pix.width = W; of.fmt.pix.height = H;
    of.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12; of.fmt.pix.field = V4L2_FIELD_NONE;
    of.fmt.pix.bytesperline = W;
    if (xioctl(VIDIOC_S_FMT, &of) < 0){ perror("[vcu] S_FMT OUTPUT"); return false; }
    obpl_  = of.fmt.pix.bytesperline ? of.fmt.pix.bytesperline : (unsigned)W;
    osize_ = of.fmt.pix.sizeimage ? of.fmt.pix.sizeimage : (size_t)W * H * 3 / 2;
    // CAPTURE(=出力 H264)
    v4l2_format cf{}; cf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cf.fmt.pix.width = W; cf.fmt.pix.height = H;
    cf.fmt.pix.pixelformat = V4L2_PIX_FMT_H264; cf.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(VIDIOC_S_FMT, &cf) < 0){ perror("[vcu] S_FMT CAPTURE"); return false; }
    csize_ = cf.fmt.pix.sizeimage ? cf.fmt.pix.sizeimage : (size_t)2 * 1024 * 1024;
    // ext-ctrls(★240M 既定を下げる)。失敗は継続(driver 差)。
    set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
    set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, bitrate);
    set_ctrl(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop);
    // buffers(MMAP)
    if (!reqbufs(V4L2_BUF_TYPE_VIDEO_OUTPUT, 2, ob_)) return false;
    if (!reqbufs(V4L2_BUF_TYPE_VIDEO_CAPTURE, 4, cb_)) return false;
    for (size_t i = 0; i < cb_.size(); i++) qbuf_cap((int)i);   // CAPTURE 全 queue
    if (!streamon(V4L2_BUF_TYPE_VIDEO_OUTPUT))  return false;
    if (!streamon(V4L2_BUF_TYPE_VIDEO_CAPTURE)) return false;
    return true;
  }

  // NV12 1 枚を OUTPUT queue へ。空き OUTPUT buf を選び(必要なら POLLOUT 待ち→DQBUF で回収)memcpy → QBUF。
  bool submit(const uint8_t* nv12, size_t len){
    int idx;
    if (free_out_ < (int)ob_.size()){ idx = free_out_++; }
    else {  // 全て in-flight → 完了 OUTPUT buf を回収(NONBLOCK なので先に POLLOUT 待ち)
      pollfd p{fd_, POLLOUT, 0}; poll(&p, 1, 1000);
      v4l2_buffer b{}; b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; b.memory = V4L2_MEMORY_MMAP;
      if (xioctl(VIDIOC_DQBUF, &b) < 0){ perror("[vcu] DQBUF OUTPUT"); return false; }
      idx = b.index;
    }
    uint8_t* dst = (uint8_t*)ob_[idx].start;
    if (obpl_ == (unsigned)W_){                 // packed = そのまま
      memcpy(dst, nv12, std::min(len, osize_));
    } else {                                    // driver stride 有 = 行ごと copy(Y then UV)
      for (int y = 0; y < H_; y++)     memcpy(dst + (size_t)y * obpl_, nv12 + (size_t)y * W_, W_);
      const uint8_t* su = nv12 + (size_t)W_ * H_; uint8_t* du = dst + (size_t)obpl_ * H_;
      for (int y = 0; y < H_ / 2; y++) memcpy(du + (size_t)y * obpl_, su + (size_t)y * W_, W_);
    }
    v4l2_buffer b{}; b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; b.memory = V4L2_MEMORY_MMAP;
    b.index = idx; b.bytesused = osize_; b.field = V4L2_FIELD_NONE;
    if (xioctl(VIDIOC_QBUF, &b) < 0){ perror("[vcu] QBUF OUTPUT"); return false; }
    return true;
  }

  // 準備できた CAPTURE(H264 AU)を回収して out へ append。LAST flag で打切り。
  void drain(std::vector<uint8_t>& out, int timeout_ms = 1000){
    for (;;){
      pollfd p{fd_, POLLIN, 0};
      if (poll(&p, 1, timeout_ms) <= 0) break;
      v4l2_buffer b{}; b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
      if (xioctl(VIDIOC_DQBUF, &b) < 0){ if (errno == EAGAIN) break; perror("[vcu] DQBUF CAP"); break; }
      if (b.bytesused){ uint8_t* s = (uint8_t*)cb_[b.index].start; out.insert(out.end(), s, s + b.bytesused); }
      bool last = (b.flags & V4L2_BUF_FLAG_LAST);
      qbuf_cap(b.index);
      if (last) break;
    }
  }

  // streaming: 1 frame を submit し、準備できた H.264 を out へ非ブロッキング回収(live 用途の本線)。
  bool encode_frame(const uint8_t* nv12, size_t len, std::vector<uint8_t>& out, int drain_ms = 0){
    if (!submit(nv12, len)) return false;
    drain(out, drain_ms);   // drain_ms=0 = 即時に取れる分だけ
    return true;
  }

  // flush: ENC STOP → LAST まで残り H.264 を drain。stream 終端で 1 度呼ぶ。
  void flush(std::vector<uint8_t>& out){
    v4l2_encoder_cmd c{}; c.cmd = V4L2_ENC_CMD_STOP;
    xioctl(VIDIOC_ENCODER_CMD, &c);   // 未対応 driver は無視 → drain の timeout で回収
    drain(out, 3000);
  }

  // 単発(allegro-dvt は encoder pipeline depth で 1 frame を flush しないので prime 枚数だけ同一 frame を
  //   投入して pipeline を通す)→ STOP flush。get_first_au=true で先頭 AU(SPS/PPS+IDR)を返す想定。
  bool encode_oneshot(const uint8_t* nv12, size_t len, std::vector<uint8_t>& out, int prime = 16){
    for (int i = 0; i < prime; i++){ if (!submit(nv12, len)) return false; drain(out, 0); }
    flush(out);
    return !out.empty();
  }

  void close_dev(){
    if (fd_ >= 0){
      int t;
      t = V4L2_BUF_TYPE_VIDEO_OUTPUT;  ioctl(fd_, VIDIOC_STREAMOFF, &t);
      t = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(fd_, VIDIOC_STREAMOFF, &t);
      for (auto& m : ob_) if (m.start && m.start != MAP_FAILED) munmap(m.start, m.length);
      for (auto& m : cb_) if (m.start && m.start != MAP_FAILED) munmap(m.start, m.length);
      ob_.clear(); cb_.clear();
      ::close(fd_); fd_ = -1;
    }
  }

  size_t out_sizeimage() const { return csize_; }

private:
  int fd_ = -1, W_ = 0, H_ = 0, free_out_ = 0;
  unsigned obpl_ = 0; size_t osize_ = 0, csize_ = 0;
  std::vector<MBuf> ob_, cb_;

  int xioctl(unsigned long req, void* arg){ int r; do { r = ioctl(fd_, req, arg); } while (r < 0 && errno == EINTR); return r; }
  void set_ctrl(unsigned id, int val){
    v4l2_control c{}; c.id = id; c.value = val;
    if (ioctl(fd_, VIDIOC_S_CTRL, &c) < 0) fprintf(stderr, "[vcu] S_CTRL 0x%x=%d 失敗(継続)\n", id, val);
  }
  bool reqbufs(int type, int count, std::vector<MBuf>& v){
    v4l2_requestbuffers r{}; r.count = count; r.type = type; r.memory = V4L2_MEMORY_MMAP;
    if (xioctl(VIDIOC_REQBUFS, &r) < 0){ perror("[vcu] REQBUFS"); return false; }
    v.assign(r.count, MBuf{});
    for (unsigned i = 0; i < r.count; i++){
      v4l2_buffer b{}; b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
      if (xioctl(VIDIOC_QUERYBUF, &b) < 0){ perror("[vcu] QUERYBUF"); return false; }
      v[i].length = b.length;
      v[i].start  = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
      if (v[i].start == MAP_FAILED){ perror("[vcu] mmap"); return false; }
    }
    return true;
  }
  void qbuf_cap(int idx){
    v4l2_buffer b{}; b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = idx;
    xioctl(VIDIOC_QBUF, &b);
  }
  bool streamon(int type){ if (xioctl(VIDIOC_STREAMON, &type) < 0){ perror("[vcu] STREAMON"); return false; } return true; }
};

} // namespace vcu
#endif

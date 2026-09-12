// y26_live.h — KV260 Web 配信用 推論 host の**唯一の**ヘッダ(2026-09-10)。
//   従来の tasks.h / clHost.h / tasks_fifo.h / tasks_kernel.h / y26_decode.h / vcu_enc.h /
//   p5cls_float.h / pt_reader.h(計 8 本、約 5,900 行)を 1 本に統合し、live 配信経路が使う分だけ残した。
//
//   ★方針(本 repo は Web 配信経路に必要なものだけを持つ):
//     - kernel(HLS)側の宣言・native forward・検証/ダンプ用 API は**一切持ち込まない**。
//     - GMEM_T は ap_uint<512> ではなく 64B の POD にした(host は sizeof とポインタ演算しか使わない)。
//       ⇒ **Vitis の include(ap_int.h 等)への依存が消えた** = ビルドに VITIS_INC 不要。
//     - 定数式は元ヘッダから行単位でそのまま持ってきている(値の同一性は scripts の cpp 展開比較で検証)。
//
//   構成: §1 メモリマップ定数 / §2 Layer と n.q configure / §3 batch(streaming)定数 /
//         §4 y26 tail decode + NV12 overlay / §5 ClHost(XRT)
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>

// ============================================================================
// §1 メモリマップ定数(旧 include/tasks.h より)
// ============================================================================
#define GMEM_BITWIDTH   (512)
// ★GMEM_T: 旧 `#define GMEM_T ap_uint<512>`。host は sizeof(64B)とポインタ演算しか使わないので
//   POD で置換する(ap_int.h = Vitis include 依存の唯一の理由だった)。sizeof/alignof は据置。
struct GMEM_T { uint8_t _b[64]; };
static_assert(sizeof(GMEM_T) == 64, "GMEM_T は 512bit = 64B 固定");


// ── BUILD ID と heartbeat ring の予約
#define HB_KERNEL_WORDS (GMEM_DATA64_SIZE*64/GMEM_BITWIDTH)   // BO 実語数 (64MB→131072 / 256MB→524288)
#ifndef HB_TOP_WORDS
#define HB_TOP_WORDS    262144u                               // ★固定上端 (BO サイズ非連動)。2026-06-21 16MB DDR削減 (旧524288=256MB)。ring/build_id/pe の anchor
// ⚠ yolo26 512MB linear K13 は層 footprint=516736 > PE(262007) で踏み潰す → -DHB_TOP_WORDS=524288u 必須
//   (PE=524151>516736、512MB geometry 分岐選択、deployed n.q 36a398c2 と整合)。yolov10n 16MB 既定は不変。
#endif
#define HB_RING_LEN     8                                     // ring entry 数 (2^n)
#define HB_RING_MASK    (HB_RING_LEN - 1)
// ★64語(4KB)整列: struct シュリンクで config layer が hb_ring_base を uint16 in(4KB単位)へ載せるため、
//   ring base を 64整列(下方切下げ)にして L_U/L_W が無損失になるよう保証する (旧 -8 = 非整列 262136 を回避)。
//   予約は 64語ページ (ring 実使用は上端 HB_RING_LEN=8 語)。build_id/pe は raw word 消費で self-consistent。
#define HB_RING_BASE    ((HB_TOP_WORDS - HB_RING_LEN) & ~63u)  // ring 予約先頭 (64語整列、BO 非連動)
#define BUILD_ID_OFFSET (HB_RING_BASE - 1)                    // build_id 1 word (ring 直下)
#define BUILD_ID_BYTES  24  // emit length (20 char string + 4 padding, fits in 1 GMEM_T)

// ── PSA Attention pe (depthwise3×3) weight の配送
#define ATTN_IO_ZOUT    3                             // qkv/attn 出力 data_shift zoomout 前提 (scale 2048)
// ── multi-PSA — PSA block が 2 つある構成
#define PE_MAX_REGIONS  2                             // PSA block 数上限 (yolo26=2 / yolov10=1)
#define PE_GMEM_WORDS   40                            // 1 region = 1280 int16 / 32 = 40 word (bias128+weight1152)
#define PE_RESERVE      128                           // ≥ PE_MAX_REGIONS*PE_GMEM_WORDS(80) + DBG/PROBE(19)、余裕
#define PE_GMEM_OFFSET  (BUILD_ID_OFFSET - PE_RESERVE) // pe 領域先頭 (= layer 出力域の直上、build_id 直下)
#define PE_REGION_OFF(r) (PE_GMEM_OFFSET + (uint32_t)(r)*PE_GMEM_WORDS) // region r の pe 先頭 word
#define PE_GMEM_SCALE   4096                          // pe weight/bias 量子化 scale (w_fp = w_i16 / 4096)
// ── mailbox / done slot (USE_MAILBOX 時)
#define DATA_MAILBOX_OFF  (PE_GMEM_OFFSET + 100)            // go_seq (host→kernel, lane0=go_seq monotonic)
#define RESULT_DONE_BASE  (PE_GMEM_OFFSET + 104)            // done ring [BASE .. +RESULT_DONE_LEN)
#define RESULT_DONE_LEN   16                                // done ring entry 数 (2^n)
#define RESULT_DONE_MASK  (RESULT_DONE_LEN - 1)
#define MAILBOX_MAGIC     0xC0FFEE00u                       // done token lane0 = MAILBOX_MAGIC | (img_idx & 0xFF)
// ★go word(host→kernel)は magic 付き: lane0 = (MAILBOX_GO_MAGIC<<16) | (seq & 0xFFFF)。
//   bootstrap(m_axi addr 未設定で reset addr 0x0=DDR base の garbage を読む)対策: magic 不一致は無視 →
//   host が addr 設定後に正しい go を書いた時のみ dispatch が seed = 誤起動防止。
#define MAILBOX_GO_MAGIC  0x600Du                           // go word high16 (host が seq と OR して書く)
#define DATA_TYPE int16_t   // 層出力の要素型(旧 tasks.h)
// ── gmem_param のレイアウト
#define GMEM_PARAM64_SIZE (64*1024*1024/sizeof(GMEM_T))  // x64bit
// ── GMEM_DATA64_SIZE_MB の既定 (256MB de-alias)
#ifndef GMEM_DATA64_SIZE_MB
#define GMEM_DATA64_SIZE_MB 128   // 2026-06-21 16MB DDR削減 (旧256)。kernel/host/n.q 同一値で合成・一致必須
#endif
#define GMEM_DATA64_SIZE  (GMEM_DATA64_SIZE_MB*1024*1024/sizeof(GMEM_T))
#if defined(VSCODE)
#define GMEM_PARAM_SIZE GMEM_PARAM64_SIZE // x64bit
#define GMEM_DATA_SIZE  GMEM_DATA64_SIZE  // x64bit static int64_t   GMEM_T outbuf[64*1024*1024/sizeof(GMEM_T)];
#else
#define GMEM_PARAM_SIZE (GMEM_PARAM64_SIZE*64/GMEM_BITWIDTH) // x512bit
#define GMEM_DATA_SIZE  (GMEM_DATA64_SIZE*64/GMEM_BITWIDTH)  // x512bit static int64_t
#endif // !defined(VSCODE)
// ★PE offset 固定定数化の不変条件: BO は固定上端領域 (ring/build_id/pe = HB_TOP_WORDS) を内包する事。
//   これが破れると pe/build_id/ring が BO 外 → kernel が範囲外 gather で PSA 破壊。GMEM_DATA64_SIZE を
//   HB_TOP_WORDS 相当 (256MB) 未満へ縮めると compile error 化して気付ける。
static_assert(GMEM_DATA_SIZE >= HB_TOP_WORDS, "GMEM_DATA_SIZE (BO words) must be >= HB_TOP_WORDS (fixed top region anchor); see tasks.h PE offset decoupling");

// ============================================================================
// §2 Layer / LayerType / n.q configure(旧 include/tasks.h)
// ============================================================================
enum class LayerType :short {
  Input     = 0,
  Concat    = 1,
  ReLU      = 2,
  Conv      = 3,  // Conv2d with SiLU
  // 4=MP, 5=SPPCSPC, 7=RepConv, 8=Detect : yolov7 専用 LayerType(削除済)。
  //   ordinal は n.q に焼かれているため欠番として予約(yolo26/yolov10 は未使用)。
  Upsample  = 6,
  Split     = 9,
  Add       = 10,
  Finish    = 11,
  Attention = 12,  // PSA (model.10): qkv[256] -> MatMul×2+scale+Softmax + pe(depthwise) + Add -> [128]
  Configure = 13   // n.q 先頭 metadata (network 層でない): i32[0]=STREAM_NIMG, 他=将来拡張用予約。
                   // host parse で読取・skip し param[] へは非搬入 (kernel 不可視=再合成不要)。
};
typedef struct {
}weight_header;       //+16

struct RecipeComv{    // trimed combo layer
  uint32_t weight;    // + 0  Weight size (Unit: Number of GMEM_T) 9/Block
  uint16_t bias;      // + 4  Bias size with padding (Unit: Number of GMEM_T)
  uint16_t k_size :3; // + 6  kernel_size
  uint16_t stride :2; // 
  uint16_t padding:2; //
  uint16_t SiLU   :1; // 
  uint16_t inlen  :8; //      length of input 2^e
  uint8_t  kernel :3; // + 8 size
  uint8_t  w_type :1; //     0:FP8 1:int16
  uint8_t  linear :1; //     1: identity passthrough (Split): no activation, o = sum >> (8+weight_m+zoomin)
  // depthwise marker は bitfield ではなく Layer.p[3] (Conv 未使用 short) で渡す
  // (2026-05-28 cosim ans=0 = bitfield read RTL で不安定)。
  int8_t   weight_m;  // + 9 weight magnitude  org*512*(1 << wh.weight_m)
  uint8_t  bias_m;    // +10 bias magnitude  org*512*(1 << wh.bias_m)
  uint16_t reserved2;     // +12 size of bias = os[1] 0:Header is Top of uint16_t load_param_to_compute[]
  int8_t   zoomin;    // +14 normal=org*(1<<(8+zoomin)),   data_shift[id][0]
  int8_t   zoomout;   // +15 normal=org*(1<<(8+zoomout)),  data_shift[id][1]
  float    weight_b;  // +16 normal=org/wh.weight_b
};                    // +20
#define L_W(u)     ((uint32_t)(u) << 6)                 // 4KB-unit → word index
#define L_U(w)     ((uint16_t)((uint32_t)(w) >> 6))     // word index → 4KB-unit (要 64-word 整列)
//   (値は従来 384×640 と byte 一致 = 下の static_assert が番人)。640×640 化は -DY26_NETH=640 だけで
//   n.q 生成器 / host / kernel の幾何が揃う。確定値は同 dir の geo640.env(shell)経由で各ビルドへ渡す。
#ifndef Y26_NETH
#define Y26_NETH 384u                                         // 入力 H(letterbox 後)。8 の倍数(P3 stride)
#endif
#ifndef Y26_NETW
#define Y26_NETW 640u                                         // 入力 W
#endif
#define Y26_IN_CH        4u                                   // 第1層入力は 4ch pack(RGB+pad)int16
#define Y26_IN_STRIDE    ((uint32_t)Y26_NETH*Y26_NETW*Y26_IN_CH*2u/64u)  // 第1層入力サイズ [word]: 384×640 → 30720
// head 3 尺度の画素数(P3=/8, P4=/16, P5=/32)
#define Y26_P3_POS       (((uint32_t)Y26_NETH/8u)*((uint32_t)Y26_NETW/8u))
#define Y26_P4_POS       (((uint32_t)Y26_NETH/16u)*((uint32_t)Y26_NETW/16u))
#define Y26_P5_POS       (((uint32_t)Y26_NETH/32u)*((uint32_t)Y26_NETW/32u))
// head 出力 [word]: box は 4ch → 16ch pack(int16 32B/pos)、cls は 80ch(int16 160B/pos)。64B word
#define Y26_HEAD_BOX_W(pos)  ((uint32_t)(pos)*16u*2u/64u)
#define Y26_HEAD_CLS_W(pos)  ((uint32_t)(pos)*80u*2u/64u)
#define Y26_PG_ALIGN(w)      ((((uint32_t)(w))+63u)&~63u)     // 4KB page(64 word)整列
// set 内 per-head offset(packed page-aligned、順序 131,136,140,145,149,154 固定)
#define Y26_HOFF_131     0u
#define Y26_HOFF_136     (Y26_HOFF_131 + Y26_PG_ALIGN(Y26_HEAD_BOX_W(Y26_P3_POS)))
#define Y26_HOFF_140     (Y26_HOFF_136 + Y26_PG_ALIGN(Y26_HEAD_CLS_W(Y26_P3_POS)))
#define Y26_HOFF_145     (Y26_HOFF_140 + Y26_PG_ALIGN(Y26_HEAD_BOX_W(Y26_P4_POS)))
#define Y26_HOFF_149     (Y26_HOFF_145 + Y26_PG_ALIGN(Y26_HEAD_CLS_W(Y26_P4_POS)))
#define Y26_HOFF_154     (Y26_HOFF_149 + Y26_PG_ALIGN(Y26_HEAD_BOX_W(Y26_P5_POS)))
#define Y26_HEAD_SET_END (Y26_HOFF_154 + Y26_HEAD_CLS_W(Y26_P5_POS))   // 384×640 → 15192
#define Y26_HEAD_BASE     (2u*Y26_IN_STRIDE)                  // = 61440。ring peak(30720)の上・IN base(135168)の下の gap 起点
                                                              // ★注意: 旧値は shrunk 誤読 map 由来(=2×IN_STRIDE と一致)。shrunk bringup 時に真 map で要再導出
#define Y26_HEAD_STRIDE   Y26_PG_ALIGN(Y26_HEAD_SET_END)      // 1 set(6head packed, 64整列): 384×640 → 15192→15232
#define Y26_FRONT_RESERVE (Y26_HEAD_BASE + 2u*Y26_HEAD_STRIDE)// = 91904 word。working ring はこの上へ wrap
#if Y26_NETH == 384u && Y26_NETW == 640u
// ★値不変の番人(geo640 step1): 導出式が従来リテラルと一致すること。ここが落ちたら n.q が別物になる
static_assert(Y26_IN_STRIDE   == 30720u, "geo640: Y26_IN_STRIDE must be 30720 for 384x640");
static_assert(Y26_HOFF_136    ==  1920u, "geo640: head off 136");
static_assert(Y26_HOFF_140    == 11520u, "geo640: head off 140");
static_assert(Y26_HOFF_145    == 12032u, "geo640: head off 145");
static_assert(Y26_HOFF_149    == 14464u, "geo640: head off 149");
static_assert(Y26_HOFF_154    == 14592u, "geo640: head off 154");
static_assert(Y26_HEAD_SET_END== 15192u, "geo640: head set end");
static_assert(Y26_HEAD_BASE   == 61440u, "geo640: Y26_HEAD_BASE");
static_assert(Y26_HEAD_STRIDE == 15232u, "geo640: Y26_HEAD_STRIDE");
static_assert(Y26_FRONT_RESERVE == 91904u, "geo640: Y26_FRONT_RESERVE");
#endif
#define L_ALIGN(w) (((uint32_t)(w) + 63u) & ~63u)       // word 数を 64-word(4KB) 境界へ切上げ
struct Layer { // *(Layer(*)[113])(gmem_in_buff+136/8)
  // ── ★struct Layer のシュリンク (id / in / out の幅)
  uint8_t  id;      // + 0 (旧 short)
  uint8_t  rsv0;    // + 1 reserved (旧 id 上位)
  LayerType type;   // + 2 Comb,ReLU               ★offset 不変
  uint16_t in;      // + 4 Index of GMEM_T data[] を 4KB(64語)単位で (旧 uint32)。実語=L_W(in)
  uint16_t out;     // + 6 Index of GMEM_T data[] を 4KB(64語)単位で (旧 uint32@8)。実語=L_W(out)
  uint16_t rsv1;    // + 8 reserved (旧 out 下位)
  uint16_t rsv2;    // +10 reserved (旧 out 上位/in 上位分)
  short f;          // +12 from layer              ★offset 不変
  short is[4];      // +14 in_shape                ★offset 不変
  short os[4];      // +22 out_shape               ★offset 不変
  short p[4];       // +30 param_shape             ★offset 不変
  uint16_t flags;   // +38 per-layer flags (旧 union 前 pad を命名。既存 n.q は 0=全 OFF)。★offset 不変
                    //     bit0=PROBE_LAYER_EN (per-layer probe gate)。
  union{            // +40                         ★offset 不変
    struct RecipeComv conv;
    short recipe[10];  // +22 [[kernel_size,stride.padding]  ★11→10 (実使用は recipe[0..3] のみ)
    int32_t i32[5];
  };                  // +40..+59 (20B)
  // ── rsv3 — 全 layer type 共通の 32bit 予約枠
  uint32_t rsv3;    // +60 reserved (旧 union 末尾 padding)
};                  // +64
static_assert(sizeof(Layer) == 64, "Layer stride は 1 GMEM_T(64B) 固定 (BOUNDLAYER/nn_kernel word 送信が依存)");
// ★id sentinel (manager→下流): 旧 id は short(signed) で -2=最終画像終端 / -1=画像境界。

// ── n.q 先頭の configure layer (runtime metadata)
struct NqConfig {                  // host 解析結果 (全 0 = configure 無し or 旧 n.q)
	int32_t  stream_nimg;
	int32_t  in_base,  in_stride;
	int32_t  out_base, out_stride;
	uint32_t hb_ring_base;
	uint32_t ring_reserve;
	uint32_t goaxis_en;            // i32[5]: GOAXIS doorbell runtime toggle (1=handshake / 0=free-run)。常時リンクの go_in/done を n.q で on/off。
	uint32_t pingpong_en;         // i32[6]: ping-pong 2-slot 入出力アドレッシング runtime toggle (1=ping-pong(i&1) / 0=linear(i))。kernel PP_SLOT を再ビルド無しで切替。
};
// n.q 先頭の configure layer を NqConfig へ全フィールド展開し、network 層の開始 byte offset
// (1 スロット分)を返す。configure 無し(旧 n.q)は 0 を返し *cfg を全 0 に。
static inline int nq_parse_configure(const void *buf, int nq_size, NqConfig *cfg){
	if(cfg) memset(cfg, 0, sizeof(*cfg));
	if(nq_size >= (int)sizeof(Layer)){
		const Layer *l0 = (const Layer *)buf;
		if(l0->type == LayerType::Configure){
			if(cfg){
				cfg->stream_nimg  = l0->i32[0];
				cfg->in_base      = l0->i32[1];  cfg->in_stride  = l0->i32[2];
				cfg->out_base     = l0->i32[3];  cfg->out_stride = l0->i32[4];
				cfg->goaxis_en    = (uint32_t)l0->i32[5];
				cfg->pingpong_en  = (uint32_t)l0->i32[6];
				cfg->hb_ring_base = L_W(l0->in);   // 4KB単位 → word 復元
				cfg->ring_reserve = L_W(l0->out);
			}
			return 8 * (int)sizeof(GMEM_T);   // 非 Conv 層 1 スロット = configure ブロック長
		}
	}
	return 0;
}
extern int g_nq_stream_nimg;   // 直近 parse した n.q の configure STREAM_NIMG (-1=configure 無し)
extern NqConfig g_nq_config;   // 直近 parse した n.q configure layer 全フィールド (全 0=無し)
extern const uint8_t data_shift[128][2];   // chw_load の入力量子化 shift(y26_live.cpp が実体を持つ)

// ============================================================================
// §3 streaming batch 定数(旧 include/tasks_kernel.h。kernel 宣言は host に不要なので持ち込まない)
// ============================================================================
   // 1 PL load (ap_ctrl_none free-run / csim 1-call) で処理する画像枚数。
   // P2 検証では 2 (同一画像×2 → 最終 result==golden で state-reset 合否判定)。
#  ifndef STREAM_NIMG
#    define STREAM_NIMG 2
#  endif
   // ── PERSIST_STREAM — 1 ap_start で無限枚
#  define GO_LAST_SHIFT 31
#  define GO_LAST_BIT   (1u << GO_LAST_SHIFT)
#  define GO_ADDR_MASK  0x7FFFFFFFu
#  ifdef PERSIST_STREAM
#    ifndef BATCH_GOAXIS
#      error "PERSIST_STREAM requires BATCH_GOAXIS (per-image go-token handshake)"
#    endif
#  endif
   // ── STREAM_NIMG_RUNTIME と per-image I/O アドレッシング
#  ifndef BATCH_IN_STRIDE
#    ifndef Y26_IN_STRIDE
#      error "tasks_kernel.h: include tasks.h first (BATCH_IN_STRIDE derives from Y26_IN_STRIDE / Y26_NETH,NETW)"
#    endif
#    define BATCH_IN_STRIDE  Y26_IN_STRIDE   // geo640: = full x.bin(384×640: 2,949,120 B / 64 = 30720 word)。kernel は先頭 IN_STRIDE を読む
#  endif
#  ifndef BATCH_OUT_STRIDE
#    define BATCH_OUT_STRIDE 8192u     // 最終層出力 1,920 word を収容 (2^n 丸め)
#  endif
#  if PE_GMEM_OFFSET < 400000u
   // --- 16MB+liveness 既定 (peak-live=128,768 < PE_GMEM_OFFSET=262,007) ---
#    ifndef BATCH_IN_BASE
#      define BATCH_IN_BASE   135168u  // peak-live(128768)+6400 headroom。IN K≤3 域 [135168,227328)
#    endif
#    ifndef BATCH_OUT_BASE
#      define BATCH_OUT_BASE  227328u  // = IN_BASE + 3*IN_STRIDE。OUT K≤3 域 [227328,251904) < PE_GMEM_OFFSET
#    endif
#  else
   // --- 512MB non-liveness 旧 (peak=516,736, linear K=13) ---
#    ifndef BATCH_IN_BASE
#      define BATCH_IN_BASE   530000u  // max extent(L0..128)=516736 の上、PE off(~1048439)未満。IN 13slot[530000,929360)
#    endif
#    ifndef BATCH_OUT_BASE
#      define BATCH_OUT_BASE  930000u  // IN 13slot端(929360)の上。OUT 13slot[930000,1036496)<PE
#    endif
#  endif
   // ── BATCH_FINAL_OUT_SLOT — 最終層 l.out の受け渡し
#  define BATCH_FINAL_OUT_SLOT (PE_GMEM_OFFSET + 120u)
#  define NQCFG_MAGIC          0x4E514347u   // "NQCG": BATCH_FINAL_OUT_SLOT lane3 validity marker

// ============================================================================
// §4 yolo26 tail decode + NV12 overlay(旧 include/y26_decode.h をそのまま)
// ============================================================================
#ifndef Y26_DECODE_H
#define Y26_DECODE_H
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

namespace y26 {

struct Det { float x1,y1,x2,y2,score; int cls; };

// head 層 id(P3,P4,P5)と幾何。end_layer=155 build の n.q layer id。
static const int   BOX_L[3]  = {131,140,149};
static const int   CLS_L[3]  = {136,145,154};
// ★geo640 H4(2026-08-28): 入力幾何は include/tasks.h の Y26_NETH / Y26_NETW(-D で上書き可)から導出する。
//   本 header は依存なし(単体 include 可)なので、tasks.h より先に include された場合は同じ既定値
//   (現行 384×640)で受ける。値の直書き(48/24/12 等)は禁止 = 全て NETH/NETW から導出。
#ifndef Y26_NETH
#define Y26_NETH 384u
#endif
#ifndef Y26_NETW
#define Y26_NETW 640u
#endif
static const int   NETH      = (int)(Y26_NETH);   // letterbox 入力(h)
static const int   NETW      = (int)(Y26_NETW);   //             (w)
static const int   Hh[3]     = {NETH/8, NETH/16, NETH/32};   // P3/P4/P5 grid(h)= NETH/stride
static const int   Ww[3]     = {NETW/8, NETW/16, NETW/32};   //                (w)= NETW/stride
static const float STRIDE[3] = {8.f,16.f,32.f};
static const int   NC        = 80;
static const int   MAXDET    = 300;
static_assert((Y26_NETH) % 32u == 0 && (Y26_NETW) % 32u == 0, "geo640: Y26_NETH/NETW must be multiples of 32 (P5 stride)");
#if (Y26_NETH) == 384u && (Y26_NETW) == 640u
static_assert(NETH/8 == 48 && NETH/16 == 24 && NETH/32 == 12, "geo640: y26 Hh for 384x640");
static_assert(NETW/8 == 80 && NETW/16 == 40 && NETW/32 == 20, "geo640: y26 Ww for 384x640");
#endif
// host 側 入力 scratch 語数(64B word)。tasks.cpp の chw_load 用 scratch(旧 46080 直書き ×6)の唯一の定義。
//   基準 = x.bin の float 数 3*H*W × 4B / 64B(384×640 → 46080、640×640 → 76800)。
//   chw_load が実際に書く HWC int16 [H][W][4] = H*W*8B は常にこれ以下(3*4 > 4*2)。
#define Y26_IN_SCRATCH_WORDS ((size_t)3u*(Y26_NETH)*(Y26_NETW)*4u/64u)
#if (Y26_NETH) == 384u && (Y26_NETW) == 640u
static_assert(Y26_IN_SCRATCH_WORDS == 46080u, "geo640: scratch words for 384x640 must stay 46080");
#endif
static_assert(Y26_IN_SCRATCH_WORDS*64u >= (size_t)(Y26_NETH)*(Y26_NETW)*4u*2u, "geo640: scratch must hold HWC int16 [H][W][4]");

// box[s]=int16 HWC [h,w,16](slot0-3=l,t,r,b), cls[s]=int16 HWC [h,w,80]。
// bsc/csc=pow2 dequant scale(build 定数)、conf=閾値。返り値は letterbox 座標の det(score 降順)。
inline std::vector<Det> decode(const int16_t* const box[3], const int16_t* const cls[3],
                               const float bsc[3], const float csc[3], float conf){
    int A=0; for(int s=0;s<3;s++) A+=Hh[s]*Ww[s];
    std::vector<float> bx((size_t)A*4), sc((size_t)A*NC), amax(A);
    int a=0;
    for(int s=0;s<3;s++){
        int h=Hh[s], w=Ww[s]; float st=STRIDE[s], ib=1.f/bsc[s], ic=1.f/csc[s];
        const int16_t* rb=box[s]; const int16_t* rc=cls[s];
        for(int y=0;y<h;y++) for(int x=0;x<w;x++,a++){
            long hw=(long)y*w+x;
            float l=rb[hw*16+0]*ib, t=rb[hw*16+1]*ib, r=rb[hw*16+2]*ib, b=rb[hw*16+3]*ib;
            float ax=x+0.5f, ay=y+0.5f;
            bx[(size_t)a*4+0]=(ax-l)*st; bx[(size_t)a*4+1]=(ay-t)*st;
            bx[(size_t)a*4+2]=(ax+r)*st; bx[(size_t)a*4+3]=(ay+b)*st;
            float mx=-1e30f;
            for(int c=0;c<NC;c++){ float v=1.f/(1.f+expf(-rc[hw*NC+c]*ic)); sc[(size_t)a*NC+c]=v; if(v>mx)mx=v; }
            amax[a]=mx;
        }
    }
    // 1段目 TopK-300: per-anchor max で anchor 選抜
    int K = A<MAXDET?A:MAXDET;
    std::vector<int> ai(A); for(int i=0;i<A;i++) ai[i]=i;
    std::partial_sort(ai.begin(), ai.begin()+K, ai.end(), [&](int p,int q){ return amax[p]>amax[q]; });
    ai.resize(K);
    // 2段目 TopK-300: 選抜 anchor の [K,80] flatten → TopK、class=idx%80
    long M=(long)K*NC; std::vector<long> ji(M); for(long j=0;j<M;j++) ji[j]=j;
    std::partial_sort(ji.begin(), ji.begin()+MAXDET, ji.end(), [&](long p,long q){
        return sc[(size_t)ai[p/NC]*NC + (p%NC)] > sc[(size_t)ai[q/NC]*NC + (q%NC)]; });
    std::vector<Det> out; out.reserve(MAXDET);
    for(int m=0;m<MAXDET;m++){
        long j=ji[m]; int aidx=ai[j/NC], cl=(int)(j%NC); float s=sc[(size_t)aidx*NC+cl];
        if(s<conf) break;                 // score 降順なので閾値未満で打切り
        out.push_back({ bx[(size_t)aidx*4+0], bx[(size_t)aidx*4+1],
                        bx[(size_t)aidx*4+2], bx[(size_t)aidx*4+3], s, cl });
    }
    return out;
}

// letterbox(NETW x NETH)座標 → 元フレーム(W0 x H0)座標
inline void unletterbox(const Det& d, int W0,int H0,
                        float& X1,float& Y1,float& X2,float& Y2){
    float r = std::min((float)NETH/H0, (float)NETW/W0);
    float padx = (NETW - r*W0)*0.5f, pady = (NETH - r*H0)*0.5f;
    X1=(d.x1-padx)/r; Y1=(d.y1-pady)/r; X2=(d.x2-padx)/r; Y2=(d.y2-pady)/r;
}

// NV12(Y[W*H] + interleaved UV[W*H/2])の Y/UV へ矩形枠を描く(RGB 指定 → YUV 変換)。
inline void nv12_rect(uint8_t* nv12,int W,int H,int x1,int y1,int x2,int y2,int thick,
                      uint8_t R,uint8_t G,uint8_t B){
    // BT.601 full-range 近似
    uint8_t Yv=(uint8_t)std::min(255.f,std::max(0.f, 0.299f*R+0.587f*G+0.114f*B));
    uint8_t Uv=(uint8_t)std::min(255.f,std::max(0.f,-0.169f*R-0.331f*G+0.5f*B+128));
    uint8_t Vv=(uint8_t)std::min(255.f,std::max(0.f, 0.5f*R-0.419f*G-0.081f*B+128));
    uint8_t* Y=nv12; uint8_t* UV=nv12+(size_t)W*H;
    auto clampi=[](int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); };
    x1=clampi(x1,0,W-1); x2=clampi(x2,0,W-1); y1=clampi(y1,0,H-1); y2=clampi(y2,0,H-1);
    if(x2<x1) std::swap(x1,x2); if(y2<y1) std::swap(y1,y2);
    auto put=[&](int x,int y){
        if(x<0||x>=W||y<0||y>=H) return;
        Y[(size_t)y*W+x]=Yv;
        size_t ci=((size_t)(y>>1)*(W>>1)+(x>>1))*2; UV[ci]=Uv; UV[ci+1]=Vv;
    };
    for(int t=0;t<thick;t++){
        for(int x=x1;x<=x2;x++){ put(x,y1+t); put(x,y2-t); }
        for(int y=y1;y<=y2;y++){ put(x1+t,y); put(x2-t,y); }
    }
}

// ---- COCO-80 クラス名(Det.cls index → 名前)----
static const char* const COCO80[80] = {
"person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light",
"fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow",
"elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee",
"skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle",
"wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange",
"broccoli","carrot","hot dog","pizza","donut","cake","chair","couch","potted plant","bed",
"dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven",
"toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"};

// ---- 8x8 bitmap font(printable ASCII 0x20..0x7F, 各 8byte=1行, bit0=左端)。public domain(font8x8_basic)。
static const unsigned char FONT8[96][8] = {
{0,0,0,0,0,0,0,0},{0x18,0x3C,0x3C,0x18,0x18,0,0x18,0},{0x36,0x36,0,0,0,0,0,0},{0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0},
{0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0},{0,0x63,0x33,0x18,0x0C,0x66,0x63,0},{0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0},{0x06,0x06,0x03,0,0,0,0,0},
{0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0},{0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0},{0,0x66,0x3C,0xFF,0x3C,0x66,0,0},{0,0x0C,0x0C,0x3F,0x0C,0x0C,0,0},
{0,0,0,0,0,0x0C,0x0C,0x06},{0,0,0,0x3F,0,0,0,0},{0,0,0,0,0,0x0C,0x0C,0},{0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0},
{0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0},{0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0},{0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0},{0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0},
{0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0},{0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0},{0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0},{0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0},
{0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0},{0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0},{0,0x0C,0x0C,0,0,0x0C,0x0C,0},{0,0x0C,0x0C,0,0,0x0C,0x0C,0x06},
{0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0},{0,0,0x3F,0,0,0x3F,0,0},{0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0},{0x1E,0x33,0x30,0x18,0x0C,0,0x0C,0},
{0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0},{0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0},{0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0},{0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0},
{0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0},{0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0},{0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0},{0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0},
{0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0},{0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0},{0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0},{0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0},
{0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0},{0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0},{0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0},{0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0},
{0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0},{0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0},{0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0},{0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0},
{0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0},{0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0},{0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0},{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0},
{0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0},{0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0},{0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0},{0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0},
{0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0},{0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0},{0x08,0x1C,0x36,0x63,0,0,0,0},{0,0,0,0,0,0,0,0xFF},
{0x0C,0x0C,0x18,0,0,0,0,0},{0,0,0x1E,0x30,0x3E,0x33,0x6E,0},{0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0},{0,0,0x1E,0x33,0x03,0x33,0x1E,0},
{0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0},{0,0,0x1E,0x33,0x3F,0x03,0x1E,0},{0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0},{0,0,0x6E,0x33,0x33,0x3E,0x30,0x1F},
{0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0},{0x0C,0,0x0E,0x0C,0x0C,0x0C,0x1E,0},{0x30,0,0x30,0x30,0x30,0x33,0x33,0x1E},{0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0},
{0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0},{0,0,0x33,0x7F,0x7F,0x6B,0x63,0},{0,0,0x1F,0x33,0x33,0x33,0x33,0},{0,0,0x1E,0x33,0x33,0x33,0x1E,0},
{0,0,0x3B,0x66,0x66,0x3E,0x06,0x0F},{0,0,0x6E,0x33,0x33,0x3E,0x30,0x78},{0,0,0x3B,0x6E,0x66,0x06,0x0F,0},{0,0,0x3E,0x03,0x1E,0x30,0x1F,0},
{0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0},{0,0,0x33,0x33,0x33,0x33,0x6E,0},{0,0,0x33,0x33,0x33,0x1E,0x0C,0},{0,0,0x63,0x6B,0x7F,0x7F,0x36,0},
{0,0,0x63,0x36,0x1C,0x36,0x63,0},{0,0,0x33,0x33,0x33,0x3E,0x30,0x1F},{0,0,0x3F,0x19,0x0C,0x26,0x3F,0},{0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0},
{0x18,0x18,0x18,0,0x18,0x18,0x18,0},{0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0},{0x6E,0x3B,0,0,0,0,0,0},{0,0,0,0,0,0,0,0}};

// NV12 の Y/UV へ 1 pixel を色付き書込(nv12_rect の put と同一規約)。
inline void nv12_putpix(uint8_t* Y,uint8_t* UV,int W,int H,int x,int y,uint8_t Yv,uint8_t Uv,uint8_t Vv){
    if((unsigned)x>=(unsigned)W||(unsigned)y>=(unsigned)H) return;
    Y[(size_t)y*W+x]=Yv;
    size_t ci=((size_t)(y>>1)*(W>>1)+(x>>1))*2; UV[ci]=Uv; UV[ci+1]=Vv;
}
// NV12 へ文字列を描画(px,py=左上, scale=整数倍率, bg=可読性用の暗い背景帯を先に塗る)。
inline void nv12_text(uint8_t* nv12,int W,int H,int px,int py,int scale,const char* s,
                      uint8_t R,uint8_t G,uint8_t B,bool bg){
    uint8_t Yv=(uint8_t)std::min(255.f,std::max(0.f, 0.299f*R+0.587f*G+0.114f*B));
    uint8_t Uv=(uint8_t)std::min(255.f,std::max(0.f,-0.169f*R-0.331f*G+0.5f*B+128));
    uint8_t Vv=(uint8_t)std::min(255.f,std::max(0.f, 0.5f*R-0.419f*G-0.081f*B+128));
    uint8_t* Y=nv12; uint8_t* UV=nv12+(size_t)W*H;
    int n=0; for(const char* p=s;*p;p++) n++;
    if(bg){ int tw=n*8*scale, th=8*scale;                  // 暗い半透明風背景(可読性)
        for(int yy=-scale;yy<th+scale;yy++) for(int xx=-scale;xx<tw+scale;xx++)
            nv12_putpix(Y,UV,W,H,px+xx,py+yy,16,128,128); }
    int cx=px;
    for(const char* p=s;*p;p++){
        unsigned c=(unsigned char)*p; if(c<0x20||c>0x7F){ cx+=8*scale; continue; }
        const unsigned char* g=FONT8[c-0x20];
        for(int row=0;row<8;row++) for(int col=0;col<8;col++)
            if(g[row]&(1u<<col))
                for(int sy=0;sy<scale;sy++) for(int sx=0;sx<scale;sx++)
                    nv12_putpix(Y,UV,W,H,cx+col*scale+sx,py+row*scale+sy,Yv,Uv,Vv);
        cx+=8*scale;
    }
}
// det ラベル文字列 "name conf" を生成(例 "person 0.91")。
inline void det_label(const Det& d,char* buf,size_t n){
    const char* nm=(d.cls>=0&&d.cls<80)?COCO80[d.cls]:"?";
    int pc=(int)(d.score*100.f+0.5f); if(pc>99)pc=99; if(pc<0)pc=0;
    std::snprintf(buf,n,"%s 0.%02d",nm,pc);
}

// KV260 IAS frmbuf の stride(bytesperline)quirk 対応: strided NV12(in, 行 stride バイト)を
// packed W×H NV12(out, stride=W)へ de-stride しつつ det の緑枠 + クラス名/確度ラベルを描く。
// stride<=0 は packed 入力扱い。out は呼び出し側が W*H*3/2 バイト確保。VCU がそのまま食う標準 NV12。
// env OVERLAY_NOLABEL=1 でラベル無効(枠のみ)。OVERLAY_LABEL_SCALE で倍率(既定 2)。
inline void overlay_destride(const uint8_t* in,int W,int H,int stride,
                             const std::vector<Det>& dets, uint8_t* out, int W0,int H0){
    if(stride<=0) stride=W;
    for(int y=0;y<H;y++)   std::memcpy(out+(size_t)y*W,        in+(size_t)y*stride,        W);       // Y
    const uint8_t* iuv=in+(size_t)stride*H; uint8_t* ouv=out+(size_t)W*H;
    for(int y=0;y<H/2;y++) std::memcpy(ouv+(size_t)y*W,        iuv+(size_t)y*stride,       W);       // UV
    bool label = !std::getenv("OVERLAY_NOLABEL");
    int  lsc   = std::getenv("OVERLAY_LABEL_SCALE")?std::atoi(std::getenv("OVERLAY_LABEL_SCALE")):2;
    if(lsc<1) lsc=1;
    for(size_t i=0;i<dets.size();i++){ float X1,Y1,X2,Y2; unletterbox(dets[i],W0,H0,X1,Y1,X2,Y2);
        nv12_rect(out,W,H,(int)X1,(int)Y1,(int)X2,(int)Y2,3,0,255,0);
        if(label){ char lb[64]; det_label(dets[i],lb,sizeof lb);
            int ty=(int)Y1-8*lsc-2; if(ty<0) ty=(int)Y1+2;            // 枠上に置く。上端なら枠内へ
            nv12_text(out,W,H,(int)X1,ty,lsc,lb,255,255,255,true); }  // 白字+暗背景
    }
}

// ── Web 配信用 letterbox 縮小 overlay(2026-08-12 新設)──
// strided NV12(in, W×H)を推論入力と同一の letterbox 幾何(NETW×NETH、既定 640x384)へ縮小し、
// det を letterbox 座標のまま直描き(unletterbox 不要 = 推論と 1:1 の座標系)。
// 縮小は整数比(720p→2:1 / 1080p→3:1)なら box 平均、それ以外は nearest。pad は gray114。
// out は呼び出し側が NETW*NETH*3/2 バイト確保。
inline void overlay_letterbox(const uint8_t* in,int W,int H,int stride,
                              const std::vector<Det>& dets, uint8_t* out){
    if(stride<=0) stride=W;
    const int OW=NETW, OH=NETH;
    float r=std::min((float)OW/W,(float)OH/H);
    int Ws=(int)(W*r+0.5f), Hs=(int)(H*r+0.5f);
    if(Ws>OW)Ws=OW; if(Hs>OH)Hs=OH;
    int px0=(OW-Ws)/2, py0=(OH-Hs)/2;
    uint8_t* oY=out; uint8_t* oUV=out+(size_t)OW*OH;
    // pad: gray114(BT.601: Y=114, UV=128。ultralytics letterbox と同色)
    std::memset(oY,114,(size_t)OW*OH); std::memset(oUV,128,(size_t)OW*OH/2);
    const uint8_t* iY=in; const uint8_t* iUV=in+(size_t)stride*H;
    int f=(Ws>0 && W==(W/Ws)*Ws && H==(H/Hs)*Hs && (W/Ws)==(H/Hs))?(W/Ws):0;   // 整数比なら f>0
    if(f>0){
        for(int y=0;y<Hs;y++){ uint8_t* orow=oY+(size_t)(py0+y)*OW+px0;
            for(int x=0;x<Ws;x++){ unsigned s=0;
                for(int dy=0;dy<f;dy++){ const uint8_t* ir=iY+(size_t)(y*f+dy)*stride+(size_t)x*f;
                    for(int dx=0;dx<f;dx++) s+=ir[dx]; }
                orow[x]=(uint8_t)(s/(unsigned)(f*f)); } }
        for(int y=0;y<Hs/2;y++){ uint8_t* orow=oUV+(size_t)((py0>>1)+y)*OW+((px0>>1)<<1);
            for(int x=0;x<Ws/2;x++){ unsigned su=0,sv=0;
                for(int dy=0;dy<f;dy++){ const uint8_t* ir=iUV+(size_t)(y*f+dy)*stride+(size_t)x*f*2;
                    for(int dx=0;dx<f;dx++){ su+=ir[dx*2]; sv+=ir[dx*2+1]; } }
                orow[x*2]=(uint8_t)(su/(unsigned)(f*f)); orow[x*2+1]=(uint8_t)(sv/(unsigned)(f*f)); } }
    } else {                                                    // 非整数比 fallback: nearest
        for(int y=0;y<Hs;y++){ int sy=(int)(y/r); if(sy>=H)sy=H-1;
            uint8_t* orow=oY+(size_t)(py0+y)*OW+px0;
            for(int x=0;x<Ws;x++){ int sx=(int)(x/r); if(sx>=W)sx=W-1; orow[x]=iY[(size_t)sy*stride+sx]; } }
        for(int y=0;y<Hs/2;y++){ int sy=(int)(y/r); if(sy>=H/2)sy=H/2-1;
            uint8_t* orow=oUV+(size_t)((py0>>1)+y)*OW+((px0>>1)<<1);
            for(int x=0;x<Ws/2;x++){ int sx=(int)(x/r); if(sx>=W/2)sx=W/2-1;
                orow[x*2]=iUV[(size_t)sy*stride+sx*2]; orow[x*2+1]=iUV[(size_t)sy*stride+sx*2+1]; } }
    }
    bool label = !std::getenv("OVERLAY_NOLABEL");
    for(size_t i=0;i<dets.size();i++){ const Det& d=dets[i];
        nv12_rect(out,OW,OH,(int)d.x1,(int)d.y1,(int)d.x2,(int)d.y2,2,0,255,0);
        if(label){ char lb[64]; det_label(d,lb,sizeof lb);
            int ty=(int)d.y1-8-2; if(ty<0) ty=(int)d.y1+2;
            nv12_text(out,OW,OH,(int)d.x1,ty,1,lb,255,255,255,true); } }
}

// ── 中央 NETW×NETH 等倍切り出し(デジタルズーム表示。2026-08-12 新設。既定 640x384)──
//   NETH==NETW(推論=配信=等倍、640×640 化)でも幾何は成立: W>=OW&&H>=OH なら cx0/cy0>=0、
//   足りなければ overlay_letterbox へ fallback(r=1, pad 0 の恒等)。UI 側では crop ボタンを撤去する。
// strided NV12(W×H)の中央 NETW×NETH 窓を等倍 memcpy し、det は letterbox 座標 →
// full-frame 座標(unletterbox)→ crop 窓座標へ変換して描く。窓外の det は clip/skip。
// W<NETW or H<NETH の入力は overlay_letterbox へ fallback。out は NETW*NETH*3/2 バイト。
inline void overlay_crop(const uint8_t* in,int W,int H,int stride,
                         const std::vector<Det>& dets, uint8_t* out){
    if(stride<=0) stride=W;
    const int OW=NETW, OH=NETH;
    if(W<OW||H<OH){ overlay_letterbox(in,W,H,stride,dets,out); return; }
    int cx0=((W-OW)/2)&~1, cy0=((H-OH)/2)&~1;         // NV12 の UV 整合のため偶数へ丸める
    uint8_t* oY=out; uint8_t* oUV=out+(size_t)OW*OH;
    const uint8_t* iY=in; const uint8_t* iUV=in+(size_t)stride*H;
    for(int y=0;y<OH;y++)   std::memcpy(oY +(size_t)y*OW, iY +(size_t)(cy0+y)*stride+cx0, OW);
    for(int y=0;y<OH/2;y++) std::memcpy(oUV+(size_t)y*OW, iUV+(size_t)(cy0/2+y)*stride+cx0, OW);
    float r=std::min((float)OW/W,(float)OH/H);
    float padx=(OW-r*W)*0.5f, pady=(OH-r*H)*0.5f;
    bool label=!std::getenv("OVERLAY_NOLABEL");
    for(size_t i=0;i<dets.size();i++){ const Det& d=dets[i];
        float X1=(d.x1-padx)/r-cx0, Y1=(d.y1-pady)/r-cy0,
              X2=(d.x2-padx)/r-cx0, Y2=(d.y2-pady)/r-cy0;
        if(X2<0||Y2<0||X1>=OW||Y1>=OH) continue;      // 完全に窓外
        nv12_rect(out,OW,OH,(int)X1,(int)Y1,(int)X2,(int)Y2,2,0,255,0);
        if(label){ char lb[64]; det_label(d,lb,sizeof lb);
            int tx=(int)X1<0?0:(int)X1;
            int ty=(int)Y1-8-2; if(ty<0) ty=((int)Y1<0?0:(int)Y1)+2;
            nv12_text(out,OW,OH,tx,ty,1,lb,255,255,255,true); } }
}

// FPS を右上に 3 桁("09.8"/"10.4" = 常に 3 数字+小数点)で描く。fps<=0 は描かない。
inline void draw_fps(uint8_t* nv12,int W,int H,float fps,int scale=2){
    if(fps<=0.f) return;
    if(fps>99.9f) fps=99.9f;
    char b[8]; std::snprintf(b,sizeof b,"%04.1f",fps);
    int tw=(int)std::strlen(b)*8*scale;
    nv12_text(nv12,W,H,W-tw-4,4,scale,b,255,255,0,true);   // 黄字+暗背景
}

} // namespace y26
#endif


// ============================================================================
// §5 ClHost — XRT ドライバ(旧 include/clHost.h の PHASE2_GOAXIS 版のみ)
//   旧版が持っていた OpenCL 実装(約 250 行)/ wait()(静的 batch 用)/ daemon_*(stdin daemon 用)/
//   extern "C" tasks_kernel 宣言(= tasks_fifo.h + tasks_kernel.h 1,195 行を引きずる原因。host からは
//   一度も呼んでいない)は live 配信で使わないので削除した。
// ============================================================================
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"
#include "xrt/xrt_bo.h"
#include "xrt/deprecated/xrt.h"        // XCL_BO_SYNC_BO_TO_DEVICE / FROM_DEVICE
#include "xrt/experimental/xrt_ip.h"   // feed_ctrl RTL ラッパを xrt::ip で直接 register アクセス
#include <thread>
#include <chrono>

class ClHost{
    xrt::device dev;
    xrt::uuid   uuid;
    xrt::kernel krnl;
    xrt::run    run;
    xrt::bo     bo_param;     // gmem0 (param)
    xrt::bo     bo_datares;   // gmem1(data)+gmem2(result) 同一 BO=alias
    int64_t*    ptr_i  = nullptr;
    int64_t*    ptr_io = nullptr;
    static constexpr size_t U32_PER_GMEM = sizeof(GMEM_T)/sizeof(uint32_t);  // 16
    // ★Phase 2 RTL ラッパ feed_ctrl (別カーネル, ap_ctrl_none free-run)。host が xrt::ip で
    //   AXI-Lite register を直接アクセス: go_seq に書込→go_out AXIS へ 1 token→tasks_kernel.go_in 解放。
    //   done_count を poll→tasks_kernel.done(M_AXIS)が feed_ctrl.done_in へ送った完了数。
    xrt::ip  ip_ctrl;
    static constexpr uint32_t FC_GO_SEQ = 0x10, FC_DONE_CNT = 0x18, FC_GO_CNT = 0x1C;
public:
    uint32_t param_words = 0;
    uint32_t stream_nimg_rt = 0;  // ★arg8 (STREAM_NIMG_RUNTIME): bit[6:0]=N / bit30=pingpong_en (ping-pong addressing runtime toggle) / bit31=goaxis_en (GOAXIS runtime toggle)。tasks.cpp が設定。
    ClHost(){}
    ~ClHost(){}
    void load(const char *xclbinFilename, size_t in_size, size_t out_size,
              int64_t* &inbuff, int64_t* &iobuff){
        std::cout << "[mailbox] xrt::device(0).load_xclbin " << xclbinFilename << std::endl;
        dev  = xrt::device(0);
        uuid = dev.load_xclbin(xclbinFilename);
        // ap_ctrl_chain CU: xrt::kernel が set_arg で offset、run.start() で ap_start を管理。
        krnl = xrt::kernel(dev, uuid, "tasks_kernel");
        run  = xrt::run(krnl);
        ip_ctrl = xrt::ip(dev, uuid, "feed_ctrl");   // RTL ラッパを IP として open (register 直アクセス)
        std::cout << "[phase2] xrt::ip feed_ctrl opened (go_seq=0x10 / done_count=0x18)" << std::endl;
        // KV260 single DDR。data/result は同一 BO(alias、proven 段A と同型)。group_id は arg 接続バンク。
        bo_param   = xrt::bo(dev, in_size,  xrt::bo::flags::normal, krnl.group_id(0));
        // ★mid-run コヒーレンシ実験: bo_datares のフラグを env MAILBOX_BO_FLAG で切替(normal/host_only/cacheable)。
        //   host_only = host メモリ(coherent path)で kernel が mid-run の host 書込を見えるか検証。
        auto dflag = xrt::bo::flags::normal; const char *fe = getenv("MAILBOX_BO_FLAG");
        std::string fs = fe ? fe : "normal";
        if(fs=="host_only") dflag = xrt::bo::flags::host_only;
        else if(fs=="cacheable") dflag = xrt::bo::flags::cacheable;
        bo_datares = xrt::bo(dev, out_size, dflag, krnl.group_id(1));
        ptr_i  = bo_param.map<int64_t*>();
        ptr_io = bo_datares.map<int64_t*>();
        inbuff = ptr_i; iobuff = ptr_io;
        std::cout << "[mailbox] kernel=tasks_kernel BOs mapped (datares flag=" << fs << ")" << std::endl;
    }
    // ★2026-07-03h param の dirty cache line を強制 evict(>PS cache size のダミー書込)。
    //   Cortex-A53 L2=1MB → 16MB を 64B step で touch し全 set を cycle=param 含む全 dirty line writeback。
    static void cache_flush_evict(){
        static volatile uint8_t evict[16 * 1024 * 1024];
        for(size_t i = 0; i < sizeof(evict); i += 64) evict[i]++;
    }
    void start(){
        uint32_t host_digest = 0; const uint32_t *p = (const uint32_t*)ptr_i;
        for(int i=0;i<4;i++) host_digest ^= p[i];
        // ★2026-07-03h HW cache-coherency fix(board 実機知見): CPU の param 書込(特に nq 末尾 layer の
        //   flags byte 等の小書込)が PS write-back cache に留まり、bo.sync(TO_DEVICE) だけでは DRAM へ
        //   確実に届かない事象がある(kernel が stale=flag 0 を読み設計C patch 未発火)。>cache-size の
        //   ダミー書込で param の dirty line を強制 evict → その後 sync で DRAM 反映を保証する。
        cache_flush_evict();
        bo_param.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        // go=(MAILBOX_GO_MAGIC<<16)|seq=1 を data BO へ書込 (ap_start 前に DRAM へ置く)。
        ((volatile uint32_t*)ptr_io)[(size_t)DATA_MAILBOX_OFF * U32_PER_GMEM]
            = ((uint32_t)MAILBOX_GO_MAGIC << 16) | 1u;
        bo_datares.sync(XCL_BO_SYNC_BO_TO_DEVICE);   // x.bin(data)+go を device へ
        // 引数: 0=param 1=data 2=result(alias) 3=probe(AXIS,skip) 4=host_digest 5=param_words。
        run.set_arg(0, bo_param);
        run.set_arg(1, bo_datares);
        run.set_arg(2, bo_datares);
        run.set_arg(4, host_digest);
        // ★引数順: param0 data1 result2 probe3 host_digest4 done5(AXIS) param_words6 go_in7(AXIS) stream_nimg_rt8。
        //   USE_DONE_AXIS で done が arg5 に挿入 → param_words は arg6 へシフト(proven build は arg5)。
        //   STREAM_NIMG_RUNTIME で stream_nimg_rt が arg8(go_in7 は AXIS で setArg skip)。
        //   stream_nimg_rt bit[6:0]=N / bit31=goaxis_en → 常時リンクの GOAXIS を n.q/env で on/off。
        run.set_arg(6, (uint32_t)param_words);
        run.set_arg(8, (uint32_t)stream_nimg_rt);
        std::cout << "[mailbox] host_digest=0x" << std::hex << host_digest << std::dec
                  << " param_words=" << param_words << " → run.start() (ap_ctrl_chain: offset書込後 ap_start)" << std::endl;
        run.start();   // XRT が offset register 書込 → ap_start。offset 捕捉が host 設定後 = 正。
        std::cout << "[mailbox] go_seq=1 written (DATA_MAILBOX_OFF=" << (unsigned)DATA_MAILBOX_OFF << ")" << std::endl;
    }
    // ★2026-07-03j param(gmem0)を device DRAM から読み戻す(DRAM-flag 検証用)。
    void sync_param_from(){ bo_param.sync(XCL_BO_SYNC_BO_FROM_DEVICE); }
    // ★2026-07-03k 設計C(l.out 直書き): param の sub-region(出力層 l.out)を device DRAM へ flush。
    //   小書込は cache 滞留しうるので呼出側で「l.out 直書き→入力画像(大書込)→本 sync」の順に(入力大書込が
    //   l.out の dirty line を evict、本 sync が保険)。
    void sync_param_region(size_t off_bytes, size_t nbytes){ bo_param.sync(XCL_BO_SYNC_BO_TO_DEVICE, nbytes, off_bytes); }
    // ★R-d coherency: mid-run に host が書いた sub-region を device(DDR)へ flush。
    //   走行中 kernel の後続 m_axi read が fresh data を見るか(壁2)の実証 primitive。
    void sync_region(size_t offset_bytes, size_t nbytes){
        bo_datares.sync(XCL_BO_SYNC_BO_TO_DEVICE, nbytes, offset_bytes);
        std::cout << "[rd] sync_region TO_DEVICE off=" << offset_bytes << " n=" << nbytes << std::endl;
    }
    // ★Phase 3: 完了画像の out-slot だけ device→host へ取り込む (per-image drain primitive)。
    //   ping-pong は out-slot が 2 つ=次の同 slot 画像が上書きする前に drain して保存する必要がある。
    void drain_region(size_t offset_bytes, size_t nbytes){
        bo_datares.sync(XCL_BO_SYNC_BO_FROM_DEVICE, nbytes, offset_bytes);
    }
    //   go_token: feed_ctrl.go_seq に seq を書込 → doorbell → go_out へ 1 beat → tasks_kernel.go_in 解放。
    void     go_token(uint32_t seq){ ip_ctrl.write_register(FC_GO_SEQ, seq); }
    //   done_cnt: tasks_kernel.done(画像完了 AXIS)を feed_ctrl が数えた累積 = 完了画像数。
    uint32_t done_cnt(){ return ip_ctrl.read_register(FC_DONE_CNT); }
    uint32_t go_cnt(){   return ip_ctrl.read_register(FC_GO_CNT); }
    // ap_ctrl_chain: run.start() 後 go token を入れてから ap_done を待つ。
    void run_wait(int ms){ try{ run.wait(std::chrono::milliseconds(ms)); }catch(...){ std::cout<<"[phase2] run.wait timeout/err\n"; }
                           bo_datares.sync(XCL_BO_SYNC_BO_FROM_DEVICE); }
};

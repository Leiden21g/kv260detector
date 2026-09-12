// SPDX-License-Identifier: AGPL-3.0-or-later
// nq_defs.h — n.q 生成器 (.pt → n.q) が必要とする型・定数だけを集めた公開用ヘッダ。
//
// 本ファイルは本プロジェクト内部の include/tasks.h (HLS kernel と host が共用する巨大ヘッダ)
// から、**生成器が参照する宣言だけ**を機械的に抜き出した trim 版である。
// 抜いていないもの = HLS kernel の実装、ap_int/ap_uint エミュレーション、hls::stream shim、
// OpenCL host ラッパ、推論用の swish/sigmoid LUT、デバッグ dump 群。
//
// ★GMEM_T は内部版では ap_uint<512> だが、生成器は sizeof() と 64B 境界計算にしか使わないので
//   ここでは POD (uint8_t[64]) に置き換えてある。値は n.q 上で一切変わらない。
#ifndef NQ_DEFS_H
#define NQ_DEFS_H

#define PRINT_DIFF_LIMIT  (1)   // 許容計算誤差(%)

// ===== [tasks.h 33-38] param/data ライン数と GMEM 幅 =====
#define PARAM_LINES 	  (34)  // L0 34line
#define DATA_LINES 	    (12288)
#define GMEM_BITWIDTH 	(512)
#define HEAD_BITWIDTH 	(512/8)
#define MAX_BURST		    (16)
#define PAGESIZE		    (GMEM_BITWIDTH*MAX_BURST/32)

// GMEM_T: 512bit = 64B の 1 word。生成器では「64B 単位」の型としてのみ使う。
struct GMEM_T_pod { unsigned char _b[64]; };
#define GMEM_T GMEM_T_pod

#define IF_SIZE       	(4096 *sizeof(int)*8/GMEM_BITWIDTH)  // Number of burst for int [4096]

// ===== [tasks.h 49-73] BUILD ID / heartbeat ring / PSA pe 領域の anchor =====
// ■ BUILD ID と heartbeat ring の予約
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

// ■ PSA Attention pe (depthwise3×3) weight の配送
#define ATTN_IO_ZOUT    3                             // qkv/attn 出力 data_shift zoomout 前提 (scale 2048)
// ■ multi-PSA — PSA block が 2 つある構成
#define PE_MAX_REGIONS  2                             // PSA block 数上限 (yolo26=2 / yolov10=1)
#define PE_GMEM_WORDS   40                            // 1 region = 1280 int16 / 32 = 40 word (bias128+weight1152)
#define PE_RESERVE      128                           // ≥ PE_MAX_REGIONS*PE_GMEM_WORDS(80) + DBG/PROBE(19)、余裕
#define PE_GMEM_OFFSET  (BUILD_ID_OFFSET - PE_RESERVE) // pe 領域先頭 (= layer 出力域の直上、build_id 直下)
#define PE_REGION_OFF(r) (PE_GMEM_OFFSET + (uint32_t)(r)*PE_GMEM_WORDS) // region r の pe 先頭 word
#define PE_GMEM_SCALE   4096                          // pe weight/bias 量子化 scale (w_fp = w_i16 / 4096)

// ===== [tasks.h 76-106] スイッチと probe/cyclestamp 領域 =====
#ifdef DISABLE_TAPFOLD
#define TAPFOLD_DISABLED 1
#else
#define TAPFOLD_DISABLED 0
#endif

// ■ dense32 (4×(8+1)) 1×1 conv 最適化スイッチ
#define DENSE32_WCOUNT(ic, oc) (9u * (uint32_t)(((ic) + 31) / 32) * ((uint32_t)(oc) >> 3))

// === L50 debug-store region (pe 予約 tail の未使用域、layer 出力域 maxw 外で安全) ===
//   pe は PE_GMEM_OFFSET..+39 (40 word) のみ使用、+40..+63 は PE_RESERVE 予約だが未使用。
//   そこへ manager header-read debug entry を linear 書込 (DBG_LEN=16 < 24 余裕)。
#define DBG_BASE        (PE_GMEM_OFFSET + PE_MAX_REGIONS*PE_GMEM_WORDS)   // 全 pe region の直後 (region 衝突回避)
#define DBG_LEN         16                                // entry 数 (linear, mod DBG_LEN)
#define DBG_MAGIC       0x4D475244u                       // 'MGRD' (Manager ReaD) entry lane0

// ■ swishs probe 領域 (PROBE63_*)
#define PROBE63_LAYER   124                               // 観測対象 kernel layer id (model.22.m.0.1.ffn.0)
#define PROBE63_OC      256                               // L124 の出力 channel 数
#define PROBE63_NPIX    2                                 // サンプルする pixel 数 (sum-only で 48word に収める)
#define PROBE63_SUMW    (PROBE63_OC/16)                   // sum word/pixel (16 int32/word) = 16 @ OC=256
#define PROBE63_BASE    DBG_BASE                          // (DBG 域再利用、DBG_L50 と排他)
#define PROBE63_WORDS   (1 + PROBE63_NPIX*PROBE63_SUMW)   // header(1)+NPIX×SUMW = 33 @ OC=256,NPIX=2
#define PROBE63_AVAIL   (PE_RESERVE - PE_MAX_REGIONS*PE_GMEM_WORDS) // PROBE/DBG 域語数 (= 48)

// ■ manager per-layer cyclestamp 領域 (MGR_CYC_*)
#ifndef MGR_CYC_BASE
#define MGR_CYC_BASE    180224u                           // 64整列 (=2816 unit)。> layer peak 163040、< PE_GMEM_OFFSET
#endif                                                    // ★geo640: 640² は peak 271616 / IN [278528,432128) / OUT [432128,456704) を避けて -DMGR_CYC_BASE=456704(geo640.env)
#define MGR_CYC_WORDS   512u                              // 3×156=468 < 512。1 record = 1 word (marker0xC7/id/phase/cyc)
#define MGR_CYC_MAGIC   0xC7u                             // record lane0 [7:0] marker (cyc stamp)

// ===== [tasks.h 116-131] host↔kernel mailbox (n.q configure が参照) =====
#define DATA_MAILBOX_OFF  (PE_GMEM_OFFSET + 100)            // go_seq (host→kernel, lane0=go_seq monotonic)
#define RESULT_DONE_BASE  (PE_GMEM_OFFSET + 104)            // done ring [BASE .. +RESULT_DONE_LEN)
#define RESULT_DONE_LEN   16                                // done ring entry 数 (2^n)
#define RESULT_DONE_MASK  (RESULT_DONE_LEN - 1)
#define MAILBOX_MAGIC     0xC0FFEE00u                       // done token lane0 = MAILBOX_MAGIC | (img_idx & 0xFF)
// ★go word(host→kernel)は magic 付き: lane0 = (MAILBOX_GO_MAGIC<<16) | (seq & 0xFFFF)。
//   bootstrap(m_axi addr 未設定で reset addr 0x0=DDR base の garbage を読む)対策: magic 不一致は無視 →
//   host が addr 設定後に正しい go を書いた時のみ dispatch が seed = 誤起動防止。
#define MAILBOX_GO_MAGIC  0x600Du                           // go word high16 (host が seq と OR して書く)

#define HB_MAGIC        0x48425453u   // 'HBTS' entry lane0。host が heartbeat 有効性を判定
// phase codes (entry lane2)
#define HB_PHASE_STARTED  1u          // store がこの index の burst に着手 (burst 未完)
#define HB_PHASE_DONE     2u          // store がこの index の burst を完了
#define HB_PHASE_FINISH   0xFFFFFFFFu // STORE_FINISH 到達 (graceful 終了 = store hang 無し)
#define HB_PHASE_INPUT_FREE 3u        // ★設計B(退役 2026-07-15): 旧=第1層 burst 着手 credit。HW未発火で production 非採用。phase 番号3は予約(未 emit)

// ===== [tasks.h 145-156] 標準ヘッダと外部テーブル =====
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <cstdint>   // GCC 13+ では uint8_t/uint16_t/... の暗黙 include が無いため明示
#include <cstring>   // memset (Layer/NqConfig の初期化)
#include <cstddef>
#include <cassert>
#include <vector>
#include <algorithm>
#include <functional>
extern  std::ofstream eval_st;
extern const uint8_t data_shift[128][2];
extern const float renge[128][2];

// ===== [tasks.h 963-980] ROUNDEDUP / 要素型 / 機能スイッチ =====
#define ROUNDEDUP(in, shift) \
  (((in)-1 + (1<<(shift))) - (((in)-1) & ((1<<(shift)) - 1)))

//#define __VITIS_HLS__
// #define START_LAYER 117  // index of Layer eg. 24 41 117
 #define LAST_NUMBER 1
#define DATA_TYPE int16_t // ex. half,int16_t
#define FIXED_POINT
// ■ 案A — liveness 区間管理 relayout
#define LIVENESS_ALLOC 1
// #define USE_THREAD
#define USE_FP8
#define USE_LONGFIFO
#define USE_WFIFO
#define USE_WRAM
#define SIZE_OF_WRAM (256 *1024/sizeof(uint64_t))
#define SIZE_OF_IBUFF (512)
#define USE_DETECT_FIFO

// ===== [tasks.h 1010-1024] GMEM word/byte 幅ヘルパ =====
#define ST_CAPACITY     (1024*2/sizeof(int64_t))
#define BURST_LENGTH    (8)     // Burst in class StreamContext
#define MAX_BURST       (16)    //
#define GMEM_BYTEWIDTH  (GMEM_BITWIDTH/8)
#define GMEM_SHORTWIDTH (GMEM_BITWIDTH/16)
#define GMEM_WORDWIDTH  (GMEM_BITWIDTH/64)
#define GMEM_PAGE_BYTE  (GMEM_BITWIDTH*MAX_BURST/8)  // Number of Bytes in page
#define GMEM_PAGE_SHORT (GMEM_BITWIDTH*MAX_BURST/16) // Number of Bytes in page
#define GMEM_PAGE_WORD  (GMEM_BITWIDTH*MAX_BURST/64) // Number of Words in page
#define GMEM_PAGE_TOP(INDEX)   (((GMEM_PAGE_SHORT-1) & INDEX) == 0) //
#define GMEM_LINE_COL(INDEX)   ((GMEM_SHORTWIDTH-1) & INDEX) //
#define GMEM_LINE_TOP(INDEX)   (((GMEM_SHORTWIDTH-1) & INDEX) == 0) //

#define GMEM_DATA_TOP  (0) // data_top index of data[]
#define GMEM_PARAM_TOP (0) // param_top index of param[]

// ===== [tasks.h 1063-1096] param レイアウトと Forward/Skip/Output =====
#define LAYER_TOP  (32)    // Index if int64_t param[]
#define WEIGHT_TOP (1024)  // Index if int64_t param[]
#define LOG_TOP    (63*1024*1024/8)  // Index if int64_t param[]

#define DATA_MAG  (1)  // magnification

#define IOU_THRES 0.45
class Skip{
public:
  int   dataSize;
  int   dataIndex;  // + 4
  short layerIndex; // + 8
};                  // +12
class Forward {
 public:
  int in;               // + 0 Index of in_buff[2];
  int in_buff[2];       // + 4 Index of data[]
  uint16_t reservrd1[2];// +12 Index of Start Layer
  Skip skip[8];     	  // +16 +12x8
  char skipSize;    	  //+112
  uint32_t size;    	  //+116 Size of Input Data [64bit unit]
  int reservrd[2];  	  //+120 +4x2  For Next layer info
  uint16_t output_buff[128]; // Top of layer output, (Index of data[])x256 output_buff[120]=detect is not use
};                  	  //+128

class Output {
 public:
  int index;        // Index of data
  short shape[2];   // Index of data
  short layerID;    // Index of layer
};

extern char* path;


// ===== [tasks.h 1098-1120] BO サイズと FAR/PIN =====
#define GMEM_PARAM64_SIZE (64*1024*1024/sizeof(GMEM_T))  // x64bit
// ■ GMEM_DATA64_SIZE_MB の既定 (256MB de-alias)
#ifndef GMEM_DATA64_SIZE_MB
#define GMEM_DATA64_SIZE_MB 128   // 2026-06-21 16MB DDR削減 (旧256)。kernel/host/n.q 同一値で合成・一致必須
#endif
#undef  GMEM_DATA64_SIZE   // nn.h の 128MB 既定を上書き
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
#define FAR0     ROUNDEDUP(sizeof(Forward)/sizeof(int64_t), 8)
#define FAR1     ROUNDEDUP(FAR0 + 2293760*sizeof(DATA_TYPE)/sizeof(int64_t) ,8)
#define FAR2     ROUNDEDUP(FAR1 + 1146880*sizeof(DATA_TYPE)/sizeof(int64_t), 8)
#define PIN      ROUNDEDUP(FAR2 +  143360*sizeof(DATA_TYPE)/sizeof(int64_t), 8)  // 2^8 = 256の倍数 切り上げ
#define PIN_SIZE ((128*1024*1024/sizeof(int64_t))  - PIN)

// ===== [tasks.h 28-32] PRINT_BIAS (既定で無効化されたデバッグ表示) =====
#define PRINT_BIAS    if(0)

// ===== [tasks.h 1128-1180] dump8 / dump16 (16 進ダンプ) =====
	inline void dump16(const void *d,int r,int c, int n){
	  char buf[256];

	  auto *p((uint16_t *)d);
	  for(int i=0; i<r; i++){
		buf[0] =0;
		sprintf(buf+strlen(buf), "%02X ", i&255);
		for(int j=0; j<c; j++){
		  for(int k=0; k<n; k++){
			  sprintf(buf+strlen(buf), "%04x",p[k] & 0x0FFFF);
		  }
		  p+=n;
		  sprintf(buf+strlen(buf), " ");
		}
		printf("%s\n", buf);
	  }
	}

	inline void dump8(const void *d,int r,int c, int n){
	  char buf[256];

	  auto *p((uint8_t *)d);
	  for(int i=0; i<r; i++){
		buf[0] =0;
		sprintf(buf+strlen(buf), "%02X ", i&255);
		for(int j=0; j<c; j++){
		  for(int k=0; k<n; k++){
			sprintf(buf+strlen(buf), "%02X",p[k]);
		  }
		  p+=n;
		  sprintf(buf+strlen(buf), " ");
		}
		printf("%s\n", buf);
	  }
	}

	inline void dump8(const void *d,int r,int c, int n, const char *header){
	  char buf[256];
	  strcpy(buf, header);
	  auto *p((uint8_t *)d);
	  for(int i=0; i<r; i++){
		sprintf(buf+strlen(buf), "%02X ", i&255);
		for(int j=0; j<c; j++){
		  for(int k=0; k<n; k++){
			sprintf(buf+strlen(buf), "%02X",p[k]);
		  }
		  p+=n;
		  sprintf(buf+strlen(buf), " ");
		}
		printf("%s\n", buf);
		buf[0] =0;
	  }
	}

// ===== [tasks.h 1762-1765] sigmoidf =====
template<typename T>
inline const T sigmoidf(const T value){
	return (T)(T(1)/(T(1) + (T)expf((T)-value)));
}

// ===== [tasks.h 1184-1247] class FP8 (重み 8bit 表現) =====
class FP8{
public:
  union FP8_U{
    int8_t bare; // 素のデータ
    struct {
      int8_t r:5; // 実数部 LSB first　下位ビット
      uint8_t e:3; // 指数部 
    };
  }data;

  union S16{
    int16_t in;
    struct{
      int16_t hiddn:16-5; // N/A
      int16_t r    :5; // 実数部 MSB
    };
  };
  
  operator int16_t() {
    return get();
  }


  int16_t get(){
    S16 t;
    t.in =1024; // ほぼ四捨五入
    t.r = data.r;
    t.in >>= data.e;
    return t.in; 
  }
  uint8_t set(int16_t in){ // こっちは遅くて良い
    S16 t={in};
    int i;
    for(i=0; i<7; i++){
      if((t.in & (int16_t)0x8000) && !(t.in & (int16_t)0x4000)) break;
      if(!(t.in & (int16_t)0x8000) && (t.in & (int16_t)0x4000)) break;
      t.in = t.in <<1;
    }
    data.e = i;
    data.r = t.r;
    return data.bare;
  }

#if defined(CHECK_UFP8)
        auto diff = abs(to8.get()-zoom);
        sum8 += diff;
        if(max8<diff){
            max8 =diff;
        }
        float eval((float)to8.get()*h.weight_b);
       printw(h.weight_a, h.weight_b, (float)to8.get(), eval, tmp, l, 0,0, j/(h.kernel*h.kernel*in)%out,
//         printw(h.weight_a, (float)h.weight_b, (float)to8.get(), (float)to8.data.e, to8.data.r, l, 0,0, j/(h.kernel*h.kernel*in)%out,
          j/(h.kernel*h.kernel)%in, j); //  s(h.out*h.in*h.kernel*h.kernel);
        diff = abs(eval-tmp);
        sum += diff;
        if(max<diff){
            max =diff;
        }
      }
      // printf("check ufp8 bias ave8=%d max8=%d ave=%d max=%d \n",
      // (int)(100*sum8/h.out/32767), 
      // (int)(100*max8/32767), (int)(sum/h.out), (int)max);
#endif
};

// ===== [tasks.h 1262-1305] LayerType / weight_header / RecipeComv =====
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

// LayerTypeName は (int)l.type で添字参照されるため要素を維持し、削除済 ordinal(4/5/7/8)は "-" placeholder。
// index 13 = Configure ("Cfg")。
static const char *const LayerTypeName[] = {
  "Inpt", "Concat", "ReLU", "Comv", "-", "-", "Upsample", "-", "-", "Split", "Add", "Finish", "Attn", "Cfg"
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
  // (2026-05-28 cosim ans=0 = bitfield read RTL で不安定。L3 reserved2 と同根)。
  int8_t   weight_m;  // + 9 weight magnitude  org*512*(1 << wh.weight_m)
  uint8_t  bias_m;    // +10 bias magnitude  org*512*(1 << wh.bias_m)
  uint16_t reserved2;     // +12 size of bias = os[1] 0:Header is Top of uint16_t load_param_to_compute[]
  int8_t   zoomin;    // +14 normal=org*(1<<(8+zoomin)),   data_shift[id][0]
  int8_t   zoomout;   // +15 normal=org*(1<<(8+zoomout)),  data_shift[id][1]
  float    weight_b;  // +16 normal=org/wh.weight_b
};                    // +20

// ===== [tasks.h 1307-1452] 4KB 単位変換 / 入力幾何 / head 予約 / struct Layer / flags =====
#define L_W(u)     ((uint32_t)(u) << 6)                 // 4KB-unit → word index
#define L_U(w)     ((uint16_t)((uint32_t)(w) >> 6))     // word index → 4KB-unit (要 64-word 整列)
// ■ HEAD_TEE_DUMP — 6 head を専用予約域へ redirect
#define HEAD_TEE_BASE 165888u

// ■ ★HEAD_RESERVE_FRONT — head 2 組を前方予約
// ★geo640 step1(2026-08-28): 入力幾何の唯一の源泉 = Y26_NETH / Y26_NETW。以下の定数は全てここから導出する
//   (値は従来 384×640 と byte 一致 = 下の static_assert が番人)。640×640 化は -DY26_NETH=640 だけで
//   n.q 生成器 / host / kernel の幾何が揃う。確定値は scripts/geo640.env(shell)経由で各ビルドへ渡す
//。
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
static inline uint32_t head_reserve_off(unsigned lid){
    switch(lid){
        case 131: return Y26_HOFF_131;   // box P3 (384×640: 1920w=30pg)
        case 136: return Y26_HOFF_136;   // cls P3 (9600w=150pg) [ends 11520]
        case 140: return Y26_HOFF_140;   // box P4 (480w→512=8pg)
        case 145: return Y26_HOFF_145;   // cls P4 (2400w→2432=38pg) [ends 14464]
        case 149: return Y26_HOFF_149;   // box P5 (120w→128=2pg)
        case 154: return Y26_HOFF_154;   // cls P5 (600w) [ends 15192]
        default:  return 0xFFFFFFFFu;   // not a head
    }
}
// slot(組)別 head アドレス。kernel(store)は out_slot_latch を、host は img slot を渡す。
static inline uint32_t head_reserve_addr(unsigned lid, unsigned slot){
    uint32_t off = head_reserve_off(lid);
    if(off == 0xFFFFFFFFu) return 0u;
    return Y26_HEAD_BASE + slot*Y26_HEAD_STRIDE + off;
}

static inline uint32_t head_tee_addr(unsigned lid){
#if defined(HEAD_RESERVE_FRONT)
    // 前方予約(slot0 既定)。kernel の slot-aware 経路は head_reserve_addr(lid, out_slot_latch) を直接使用。
    return head_reserve_addr(lid, 0u);
#else
    // ★v2 IN-region layout(5/6 head 実機動作実績、但し L145 は IN 域 aliasing race=cosim 確定)。
    switch(lid){
        case 131: return HEAD_TEE_BASE + 0u;       // box P3 (1920w) @165888
        case 136: return HEAD_TEE_BASE + 8192u;    // cls P3 (9600w) @174080 [ends 183680]
        case 140: return HEAD_TEE_BASE + 24576u;   // box P4 (480w)  @190464
        case 145: return HEAD_TEE_BASE + 32768u;   // cls P4 (2400w) @198656
        case 149: return HEAD_TEE_BASE + 40960u;   // box P5 (120w)  @206848
        case 154: return HEAD_TEE_BASE + 49152u;   // cls P5 (600w)  @215040
        default:  return 0u;                        // 0 = not a tee'd head
    }
#endif
}
#define L_ALIGN(w) (((uint32_t)(w) + 63u) & ~63u)       // word 数を 64-word(4KB) 境界へ切上げ
struct Layer { // *(Layer(*)[113])(gmem_in_buff+136/8)
  // ■ ★struct Layer のシュリンク (id / in / out の幅)
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
    short recipe[10];  // +22 kernel_size / stride / padding  ★11→10 (実使用は recipe[0..3] のみ)
    int32_t i32[5];
  };                  // +40..+59 (20B)
  // ■ rsv3 — 全 layer type 共通の 32bit 予約枠
  uint32_t rsv3;    // +60 reserved (旧 union 末尾 padding)
};                  // +64
static_assert(sizeof(Layer) == 64, "Layer stride は 1 GMEM_T(64B) 固定 (BOUNDLAYER/nn_kernel word 送信が依存)");
// ★id sentinel (manager→下流): 旧 id は short(signed) で -2=最終画像終端 / -1=画像境界。
//   uint8 化で -2→254 / -1→255 に wrap。実層 id は 0..~155 (<254) なので非衝突。
//   ★重要: 検出は `(int8_t)id<0` 不可 (id≥128 の実層と衝突) → `id>=0xFE` で判定すること。
#define LAYER_ID_FINAL      ((uint8_t)0xFEu)   // 最終画像 (旧 -2)
#define LAYER_ID_BOUNDARY   ((uint8_t)0xFFu)   // 画像境界 (旧 -1)
#define LAYER_ID_IS_SENTINEL(id) ((uint8_t)(id) >= 0xFEu)// Layer.flags bit 定義 (n.q ヘッダ byte+38)。host(scripts/nq_index.py --set-probe)で後付け可。
#define PROBE_LAYER_EN  ((uint16_t)0x0001)   // この層の probe(per-stage / phase)を emit 許可
#define OUTPUT_LAYER_EN ((uint16_t)0x0002)   // ★設計C: この層は出力層 → pad_weight が l.out を host 供給(go)アドレスで patch(HOST_OUTPUT_ADDR 時)
// ■ per-layer 述語の host 前計算 (tapfold / dense32)
#define TAPFOLD_LAYER_EN ((uint16_t)0x0004)   // bit2: 1x1 tap-fold conv (host 前計算, HOIST_PREDICATE_FLAGS で参照)
#define DENSE32_LAYER_EN ((uint16_t)0x0008)   // bit3: 1x1 dense32 conv (host 前計算, HOIST_PREDICATE_FLAGS で参照)
// ■ MaxPool / Resize 判別も host 前計算へ
#define MAXPOOL_LAYER_EN ((uint16_t)0x0010)   // bit4: SPPF MaxPool (n.q 上は Concat 型 1入力)
#define RESIZE_LAYER_EN  ((uint16_t)0x0020)   // bit5: nearest Upsample (n.q 上は Concat 型 1入力)
// ■ 述語ヘルパ — pad_weight / set / conv で同一に使う
#define IS_TAPFOLD_PRED(LREF, IC, OC)        ((((LREF).flags) & TAPFOLD_LAYER_EN) != 0)
#define IS_DENSE32_PRED(LREF, IC, OC, WCNT)  ((((LREF).flags) & DENSE32_LAYER_EN) != 0)
// ★MaxPool/Resize 述語 (2026-07-18)。既定は従来式へ 1:1 展開 = production ソース byte 不変。
//   -DHOIST_MPRS_FLAGS で flag 読取へ切替(**新 n.q 必須**。旧 n.q は flag=0 → 静かに誤値)。
//   非 hoist 展開は data_load の元式と完全同一(is_add / nin は呼出側の実行時値をそのまま渡す)。
#if defined(HOIST_MPRS_FLAGS)
#  define IS_MAXPOOL_PRED(LREF, IS_ADD, NIN)  ((((LREF).flags) & MAXPOOL_LAYER_EN) != 0)
#  define IS_RESIZE_PRED(LREF, IS_ADD, NIN)   ((((LREF).flags) & RESIZE_LAYER_EN) != 0)
#else
#  define IS_MAXPOOL_PRED(LREF, IS_ADD, NIN)  ((!(IS_ADD)) && ((NIN) == 1) && ((LREF).p[1] >= (short)2))
#  define IS_RESIZE_PRED(LREF, IS_ADD, NIN)   ((!(IS_ADD)) && ((NIN) == 1) && ((uint16_t)(LREF).os[2] > (uint16_t)(LREF).is[2]))
#endif
// ■ Tier4 — nword と count の三重積を前計算
#if defined(HOIST_NWORD) && !defined(NO_HDIV_NWORD)
#  define HDIV_NWORD(LREF, OC, OH, OW)   ((uint32_t)(LREF).rsv3)
#  define HDIV_COUNT(LREF, OC, OH, OW)   ((uint32_t)(LREF).rsv3)
#else
#  define HDIV_NWORD(LREF, OC, OH, OW)   ((uint32_t)((((uint32_t)(OC)) * (uint32_t)(OH) * (uint32_t)(OW)) >> 4))
#  define HDIV_COUNT(LREF, OC, OH, OW)   ((uint32_t)(((((OH)>>2) * ((OW)>>2) * ((OC)>>2))) << 2))
#endif

// ===== [tasks.h 1773-1776] 512bit 境界ヘルパ =====
#define BOUNDARY (512 / 8 / sizeof(DATA_TYPE))
#define BOUNDSIZE (512 / 8)  // Byte
#define BOUNDLAYER (512 / 8 / sizeof(Layer))
#define BOUND64 (512 / 8 / sizeof(int64_t))

// ===== [tasks.h 1484-1541] n.q 先頭 configure layer =====
struct NqConfig {                  // host 解析結果 (全 0 = configure 無し or 旧 n.q)
	int32_t  stream_nimg;
	int32_t  in_base,  in_stride;
	int32_t  out_base, out_stride;
	uint32_t hb_ring_base;
	uint32_t ring_reserve;
	uint32_t goaxis_en;            // i32[5]: GOAXIS doorbell runtime toggle (1=handshake / 0=free-run)。常時リンクの go_in/done を n.q で on/off。
	uint32_t pingpong_en;         // i32[6]: ping-pong 2-slot 入出力アドレッシング runtime toggle (1=ping-pong(i&1) / 0=linear(i))。kernel PP_SLOT を再ビルド無しで切替。
};
#if !defined(__SYNTHESIS__)
// 拡張 emit: 既定引数 0 のため `nq_make_configure(l, N)` 呼出は従来通り(他フィールド 0)。
static inline void nq_make_configure(Layer *l, int32_t stream_nimg,
                                     int32_t in_base = 0,  int32_t in_stride = 0,
                                     int32_t out_base = 0, int32_t out_stride = 0,
                                     uint32_t hb_ring_base = 0, uint32_t ring_reserve = 0,
                                     uint32_t goaxis_en = 0, uint32_t pingpong_en = 0){
	memset(l, 0, sizeof(Layer));
	l->type   = LayerType::Configure;
	l->i32[0] = stream_nimg;
	l->i32[1] = in_base;   l->i32[2] = in_stride;
	l->i32[3] = out_base;  l->i32[4] = out_stride;
	l->i32[5] = (int32_t)goaxis_en;   // GOAXIS doorbell runtime toggle (0=free-run 既定 → 旧 n.q 後方互換)
	l->i32[6] = (int32_t)pingpong_en; // ping-pong 2-slot addressing runtime toggle (0=linear 既定 → 旧 n.q 後方互換)
	// ★in/out は 4KB(64語)単位 uint16。hb_ring_base/ring_reserve は 64整列 word なので L_U で無損失格納。
	//   (production n.q は 0 → L_U(0)=0 で無影響。streaming n.q のみ非0、要 64整列)。
	l->in     = L_U(hb_ring_base);
	l->out    = L_U(ring_reserve);
}
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
// 後方互換 shim: STREAM_NIMG だけ欲しい既存 walker 用。nq_parse_configure へ委譲。
static inline int nq_skip_configure(const void *buf, int nq_size, int *stream_nimg_out){
	NqConfig c; int off = nq_parse_configure(buf, nq_size, &c);
	if(stream_nimg_out) *stream_nimg_out = off ? c.stream_nimg : 0;
	return off;
}
#endif  // !__SYNTHESIS__
extern int g_nq_stream_nimg;   // host: 直近 parse した n.q の configure STREAM_NIMG (-1=configure 無し)
extern uint32_t g_nq_param_words;  // ★整理③ native(3-arg kernel): exact param word 数。host が nq_filter 後に設定。

// ===== [tasks.h 1787-1799] roundedup =====
inline size_t roundedup(size_t in) {
  size_t out = (in - 1 + BOUNDARY) & ~(BOUNDARY - 1);
  //  PRINT("roundedup %8X -> %8X elements %4X BOUNDARYs\n", (int)in, (int)out,
  //  (int)(out/BOUNDARY));
  return out;
}

inline size_t roundedup(size_t in, size_t base) {
  size_t out = (in - 1 + base) - ((in - 1) & (base - 1));
  //  PRINT("roundedup%d %8X -> %8X elements %4X BOUNDARYs\n", (int)base,
  //  (int)in, (int)out, (int)((out-1)/BOUNDARY+1));
  return out;
}

// ===== [tasks.h 1813-1832] 出力バッファ割当の外部状態 =====
using namespace std;
using std::cout;

extern int transmission_index;
extern int transmission_fragment;
// env OUT_ALIGN4K: 層出力先頭を 4KB(64-word=64*64B) 境界に整列する (generator のみ、kernel 無関係)。
//   0=従来(word0 予約の +1 オフセット, byte 不変) / 非0=wrap/relayout base を 64-word 整列(=4KB)へ。
//   サイズは元々 ROUNDEDUP(.,8)=256-word(16KB) 整列なので、base を 64 整列にするだけで全出力が 4KB 整列。
extern int g_out_align4k;
#define OUT_ALIGN_BASE 64    // 4KB / 64B = 64 word。先頭 [0,64) を予約 (word0 sentinel を含む)。
// ■ FREERUN_RESERVE_LO — free-run の低位予約
#define FREERUN_RESERVE_LO  ((int)ROUNDEDUP(2u*30720u + 3u*8192u, 6))   // = 86016 word (yolov10 3-scale)
// ★yolo26 HEAD_RESERVE_FRONT の予約 floor は幾何が異なる: 入力 2 slot(61440) + head 2 組(2×15232=30464)
//   = Y26_FRONT_RESERVE=91904w。yolov10 3-scale 用の 86016 では head 2 組目末尾 [86016,91904)=5888w が
//   予約されず relayout/wrap に踏まれる。HEAD_RESERVE_FRONT 時は floor を 91904 へ切替える。
#if defined(HEAD_RESERVE_FRONT)
#define RING_RESERVE_LO ((int)Y26_FRONT_RESERVE)   // = 91904 word (yolo26: 入力2 + head 2組)
#else
#define RING_RESERVE_LO FREERUN_RESERVE_LO
#endif

// ===== [tasks.h 1833-1934] Transmission / NqBumpPlanner =====
template <typename T>
class Transmission{
public:
  int index;   // Top of data
  int layerID; // From layerID
  size_t s; // Size of Buffer

  Transmission()
  : index(0),
    layerID(0),
    s(0){}

  ~Transmission(){}
  void free(void){
    index = 0;
    s = 0;
  }

  void allocate(size_t size){   // 64 byte unit
    int&         i(transmission_index);
    int&         f(transmission_fragment);
    // 64 byte unit。終端 1 word = result[BUILD_ID_OFFSET] (=GMEM_DATA_SIZE-1) は build_id 専用に予約する。
    // wrap 上限を GMEM_DATA_SIZE → BUILD_ID_OFFSET に 1 下げ、i<=BUILD_ID_OFFSET の割当のみ許可 →
    // slot が占めるのは [index, i) = 最大 index BUILD_ID_OFFSET-1 (=GMEM_DATA_SIZE-2) まで。終端は層出力で踏まれない。
    auto         max(BUILD_ID_OFFSET);
    index =      i;
    
    // if(n<f){
    //   index = PIN + max-f;
    //   f-=n;
    // }else{
      i += ROUNDEDUP(size, 8);
      if(i > max){
        // ■ word 0 は result[0] 完了 sentinel 用に予約
#if defined(FREERUN_RING_RESERVE) || defined(HEAD_RESERVE_FRONT)
        // free-run 3-scale / yolo26 前方予約: 低位 [0, RING_RESERVE_LO) を入力(+head 2組/3-scale 出力)に
        //   予約し、その上で wrap。RING_RESERVE_LO は HEAD_RESERVE_FRONT 時 91904 / 他 86016。
        index = RING_RESERVE_LO;
        i = RING_RESERVE_LO + (int)ROUNDEDUP(size, 8);
#else
        // ★uint16 4KB(64語)単位化により層出力 word index は 64整列必須。wrap base も 64整列へ
        //   (旧 g_out_align4k=0 の index=1 非整列は L_U truncation を招くため撤去)。
        //   size は ROUNDEDUP(.,8)=256-word 整列なので i は 64-word 整列を保つ。
        index = OUT_ALIGN_BASE;
        i = OUT_ALIGN_BASE + (int)ROUNDEDUP(size, 8);
#endif
      }
    // }  
    s = size; 
  }
};

// ■ n.q 生成器専用の bump allocator
template <typename T>
struct NqBumpPlanner{
#define nSLOTS (8)
  Transmission<DATA_TYPE> slot[nSLOTS];
  int wp,rp;
  NqBumpPlanner()
  : wp(0),rp(0)
  {}

// ■ out() — 出力バッファの取得
int out(Layer &l){
  auto i = wp&(nSLOTS-1);
  // 本モデルは全層 os[0]=0 のため far-branch(旧 Conv && os[0]==1)は未到達 → 除去。通常 bump のみ。
  size_t s((size_t)l.os[1]*l.os[2]*l.os[3]*sizeof(int16_t)/sizeof(GMEM_T));
  slot[i].allocate(s);
  int o = slot[i].index;
  wp++;
  if(rp+ nSLOTS == wp){
    rp++;
  }
  // ★4KB(64語)単位で返す。全割当は 64整列 (init=0 / far=ROUNDEDUP(,8)=256整列 / size=ROUNDEDUP(,8))。
  //   NqBumpPlanner は generator(host) 専用 → 非整列は assert で offline 検出 (kernel は n.q 値を読むだけ)。
  assert((o & 63) == 0 && "layer out word index が 4KB(64語) 非整列 → L_U で truncation 破損");
  return L_U(o);
}

/// @brief 開始レイヤー以前の出力をbuffersに格納
/// @param param : Top of param gmem
/// @param c     : context of Forward
void set(Layer* param, Forward &c){
  Layer l = *param;   // read m_axi_gmem 
  size_t s((size_t)Y26_IN_STRIDE);   // geo640: = NETH*NETW*4*int16 / 64B(384×640 → 30720 word)
  slot[0].allocate(s);

SET_SKIP:
  for(int j=0; j < c.skipSize; j++){
    
    auto i = wp&(nSLOTS-1);

    slot[i].index = c.skip[j].dataIndex;
    wp++;
    if(rp+ nSLOTS == wp){
      rp++;
    }
  }
}

};
#undef nSLOTS

// ===== [tasks.h 1936-2276] relayout_liveness (liveness ベースの出力再配置) =====
#if !defined(__SYNTHESIS__) && LIVENESS_ALLOC
// ■ relayout_liveness — 旧 Buffers::relayout の free 関数化
inline void relayout_liveness(std::vector<Layer*>& L,
              const std::map<int, std::vector<int> >& extra,
              size_t inputWords){
  const int n = (int)L.size();
  if(n == 0) return;
  auto wordsOf = [](Layer* l)->size_t{
    return ROUNDEDUP((size_t)l->os[1]*l->os[2]*l->os[3]*sizeof(int16_t)/sizeof(GMEM_T), 8);
  };
  // ---- lastUse[i] (push-index 空間) と入力画像の lastUse ----
  std::vector<long> lastUse(n, -1);
  long inputLastUse = -1;
  auto note = [&](int prod, int cons){
    if(prod < 0){ if((long)cons > inputLastUse) inputLastUse = cons; }
    else if(prod < n){ if((long)cons > lastUse[prod]) lastUse[prod] = cons; }
  };
  for(int i=0;i<n;i++){
    Layer* l = L[i];
    note(l->f==i ? -1 : l->f, i);                  // f==i = 入力画像 (自己参照)
    auto it = extra.find(l->id);
    if(it != extra.end()) for(int pe : it->second) note(pe==i ? -1 : pe, i);
  }
  for(int i=0;i<n;i++) if(lastUse[i] < 0) lastUse[i] = n; // 未消費(最終出力)は末尾まで保持
  // ■ BIGSTORE_COOLDOWN — 大 store 層の live 域を延長
  {
    long bscd = 0; if(const char* e=getenv("MAP_BIGSTORE_COOLDOWN")) bscd = atol(e);
    size_t bsth = 1024; if(const char* e=getenv("MAP_BIGSTORE_THRESH")) bsth = (size_t)atol(e);
    if(bscd > 0){
      int cnt = 0;
      for(int i=0;i<n;i++){
        if(wordsOf(L[i]) >= bsth && lastUse[i] < (long)n){
          long nl = lastUse[i] + bscd; if(nl > (long)n) nl = (long)n;
          if(nl > lastUse[i]){ lastUse[i] = nl; cnt++; }
        }
      }
      fprintf(stderr,"[BIGSTORE_COOLDOWN] %d 層 (>=%zu word) の lastUse を +%ld 延長\n", cnt, bsth, bscd);
    }
  }
  // ★案B2 (16MB mAP dump): env MAP_PIN_HEADS の neck head を lastUse=n で末尾まで pin。
  //   内部で消費される neck 出力 (例 L96→L97/L129) を「最終まで保持」に格上げし、working DRAM 再利用で
  //   踏まれないようにする (consumer の in/i32 は newOut へ追従するので計算は不変)。push-index==id 前提。
  std::vector<int> pinnedHeads;   // ★案B2 fix: head 一意事前確保用(下記)
  const char* mp = getenv("MAP_PIN_HEADS");
#if defined(HEAD_RESERVE_FRONT)
  // ■ ★HEAD_PIN_DEFAULT — 既定で 6 head を pin (L140 上書きの恒久 fix)
  static const int kY26HostReadHeads[] = { 145, 140 };   // ★順序が l.out を決める: 145,140 = 実機検証済 4d10b24f (140,145 だと 76bef9c9 = 未検証)
  std::string autoPin;
  if(!mp){
    for(int h : kY26HostReadHeads) if(h < n){ if(!autoPin.empty()) autoPin += ","; autoPin += std::to_string(h); }
    if(!autoPin.empty()){ mp = autoPin.c_str();
      fprintf(stderr, "[MAP_PIN] 既定: host 読み head を自動 pin (%s) — L140 上書きの恒久 fix\n", mp); }
  }
#endif
  if(mp && mp[0]){
    std::string s(mp); size_t p=0;
    while(p<s.size()){ size_t c=s.find(',',p);
      int pid=atoi(s.substr(p, c==std::string::npos?std::string::npos:c-p).c_str());
      if(pid>=0 && pid<n){ lastUse[pid]=(long)n; pinnedHeads.push_back(pid); fprintf(stderr,"[MAP_PIN] L%d pinned (lastUse=n)\n", pid); }
      p=(c==std::string::npos)?s.size():c+1; }
  }
  // ■ bump 配置の捕捉 (over-read グループ用)
  std::vector<uint32_t> bumpOut(n);
  { uint32_t vb=1; for(int i=0;i<n;i++){ bumpOut[i]=vb; vb += (uint32_t)wordsOf(L[i]); } }
  // ■ over-read グループ検出 (union-find)
  std::vector<int> uf(n); for(int i=0;i<n;i++) uf[i]=i;
  auto find=[&](int x)->int{ while(uf[x]!=x){ uf[x]=uf[uf[x]]; x=uf[x]; } return x; };
  auto unite=[&](int a,int b){ int ra=find(a), rb=find(b); if(ra!=rb) uf[ra]=rb; };
  std::vector<uint32_t> readEnd(n); for(int i=0;i<n;i++) readEnd[i]=bumpOut[i]+(uint32_t)wordsOf(L[i]);
  for(int i=0;i<n;i++){ Layer* l=L[i];
    auto it=extra.find(l->id);
    bool realConcat = (l->type==LayerType::Concat) && (it!=extra.end()) && !it->second.empty();
    if(l->type!=LayerType::Add && !realConcat) continue;        // resize/maxpool(nin==1) 除外
    uint32_t npix=(uint32_t)l->os[2]*(uint32_t)l->os[3];
    auto handle=[&](int p, uint32_t wpp){ if(p<0||p>=n||p==i) return;
      uint32_t rw = (wpp==0)?((npix+1)/2):(npix*wpp);
      if(rw <= (uint32_t)wordsOf(L[p])) return;                 // over-read 無し
      uint32_t pend = bumpOut[p]+rw;                            // 食い込み端 (bump 座標)
      if(pend>readEnd[p]) readEnd[p]=pend;
      for(int q=0;q<n;q++){ if(q==p) continue;                  // [bumpOut[p],pend) に跨る全層を merge
        if(bumpOut[q] < pend && bumpOut[p] < bumpOut[q]+(uint32_t)wordsOf(L[q])) unite(p,q); } };
    handle(l->f, (uint32_t)l->is[0]);                           // in0
    if(l->type==LayerType::Add){ uint16_t ch=(uint16_t)l->os[1]; uint32_t wpp=(ch>=32)?(ch>>5):0; if(!it->second.empty()) handle(it->second[0], wpp); }
    else { int k=0; for(int p:it->second){ uint16_t vch=(uint16_t)l->p[k]; uint32_t wpp=(vch>=32)?(vch>>5):0; handle(p,wpp); k++; } }
  }
  // グループ集約: root ごとに lo(最小 bumpOut)/hi(最大 readEnd)/lastUse(最大)
  std::vector<uint32_t> grpLo(n,0xFFFFFFFFu), grpHi(n,0); std::vector<long> grpLast(n,-1);
  std::vector<uint32_t> grpBase(n,0); std::vector<char> grpPlaced(n,0); std::vector<int> grpCount(n,0);
  for(int i=0;i<n;i++){ int r=find(i);
    if(bumpOut[i]<grpLo[r]) grpLo[r]=bumpOut[i];
    if(readEnd[i]>grpHi[r]) grpHi[r]=readEnd[i];
    if(lastUse[i]>grpLast[r]) grpLast[r]=lastUse[i]; grpCount[r]++; }
  // ---- liveness-aware 配置 (bump + skip-live) ----
  struct IV{ uint32_t lo, hi; long last; };
  std::vector<IV> live;
  std::vector<uint32_t> newOut(n, 0);
  // ■ maxw — 実機 data buffer の上限
  const uint32_t maxw = (uint32_t)BUILD_ID_OFFSET - (uint32_t)PE_RESERVE;
  const uint32_t inHi = (uint32_t)ROUNDEDUP(inputWords, 8);
  const uint32_t inputIndex = 0;
  if(inputLastUse >= 0) live.push_back({0u, inHi, inputLastUse});
  // ■ word 0 の永久予約 (relayout 側)
#if defined(FREERUN_RING_RESERVE) || defined(HEAD_RESERVE_FRONT)
  const uint32_t reserveLo = (uint32_t)RING_RESERVE_LO;   // HEAD_RESERVE_FRONT=91904(入力2+head2組) / FREERUN=86016
#else
  const uint32_t reserveLo = (uint32_t)OUT_ALIGN_BASE;   // ★4KB単位化で 64整列必須 (旧 g_out_align4k=0 の 1u 非整列は撤去)
#endif
  live.push_back({0u, reserveLo, (long)n});
  // ■ ★first-fit 修正 — 解放済み下方領域を再利用
  auto place = [&](size_t sz)->uint32_t{
    // live を lo で昇順 (insertion sort)。HLS TB compiler は <algorithm> 非対応なので std::sort 不使用。
    for(size_t a=1; a<live.size(); a++){ IV key=live[a]; long b=(long)a-1;
      while(b>=0 && live[b].lo>key.lo){ live[b+1]=live[b]; b--; } live[b+1]=key; }
    uint32_t base = reserveLo;  // word0 予約 / 4KB整列時は先頭ページ / FREERUN 予約時は 86016
    for(size_t a=0; a<live.size(); a++){
      const IV& iv = live[a];
      if(iv.hi <= base) continue;                       // base より下の区間は無視
      if(base + (uint32_t)sz <= iv.lo) return base;     // iv の手前 gap に収まる (first-fit)
      base = iv.hi;                                     // live 区間を飛ばす
    }
    if(base + (uint32_t)sz <= maxw) return base;        // 全 live 区間の上、maxw 内に収まる
    fprintf(stderr,"[LIVENESS] OOM sz=%zu base=%u maxw=%u (peak-live が maxw 超過)\n", (size_t)sz, base, (unsigned)maxw);
    return base;                                        // 呼出側が maxw 超過を検知可
  };
  // ★reuse cooldown (env MAP_REUSE_COOLDOWN): dead buffer を K 層後まで live に残し再割当を遅らせる。
  //   kernel pipeline 先読み窓内で同一アドレスが produce/consume 競合して KPN deadlock するのを回避する試み。
  //   peak は増えるが空きが大きい(262007-peak)ので許容。0=従来(即再利用)。
  long cooldown = 0; if(const char* cd=getenv("MAP_REUSE_COOLDOWN")) cooldown = atol(cd);
  // ■ ★案B2 fix — MAP_PIN_HEADS を step0 から live に
  std::vector<char> prePlaced(n, 0);
  // ★#11(2026-08-31): MAP_PIN_HEADS_BASE=<word> で pin 先を固定 base からの連続確保にする(64-word 整列)。
  //   目的 = GO_EARLY 用に 6 head を working ring の外(IN slot 3 = [380928,432128)、pingpong 2 slot 運用で未使用)へ。
  //   place()(ring 最下位 gap)だと peak が IN_BASE を超える(384 での 6 head pin 棄却の理由)。既定(env 無し)は従来どおり。
  uint32_t pinCur = 0; { const char* e = getenv("MAP_PIN_HEADS_BASE"); if(e) pinCur = (uint32_t)atol(e); }
  const uint32_t pinBase = pinCur;
  if(pinBase) fprintf(stderr, "[MAP_PIN] BASE=%u (固定 base 連続確保)\n", pinBase);
  for(int pid : pinnedHeads){
    if(pid < 0 || pid >= n) continue;
    if(grpCount[find(pid)] > 1){ fprintf(stderr,"[MAP_PIN] L%d は over-read group 内 → 事前確保 skip(従来配置)\n", (int)L[pid]->id); continue; }
    size_t hsz = wordsOf(L[pid]);
    uint32_t hb;
    if(pinBase){
      pinCur = (uint32_t)ROUNDEDUP(pinCur, 6);             // 4KB(64word=2^6) 整列(ROUNDEDUP の第2引数は shift 量!)
      hb = pinCur; pinCur += (uint32_t)hsz;
      if(hb + (uint32_t)hsz > maxw){ fprintf(stderr,"[MAP_PIN] ★OOM: L%d base=%u sz=%zu > maxw=%u → place() へ fallback\n", (int)L[pid]->id, hb, hsz, (unsigned)maxw); hb = place(hsz); }
    }else
      hb = place(hsz);                                     // 従来: 現 live(reserve floor 等)を避けた最下位 gap
    newOut[pid] = hb; L[pid]->out = L_U(hb);   // ★4KB単位で格納 (LIVENESS 有効時は place() 64整列前提)
    live.push_back({hb, (uint32_t)(hb+hsz), (long)n});     // step0..n まで live = 他層から不可侵
    prePlaced[pid] = 1;
    fprintf(stderr,"[MAP_PIN] L%d 事前確保 out=%u (sz=%zu word, 一意)\n", (int)L[pid]->id, hb, hsz);
  }
  // ■ ★FINAL_OUT_UNIQUE — 最終層 l.out の一意化 (既定 ON)
  const bool finalUnique = (getenv("NO_FINAL_OUT_UNIQUE") == 0);
  for(int i=0;i<n;i++){
    // i 以降に不要 (lastUse < i-cooldown) な live 区間を解放 (manual compaction、std::remove_if 不使用)
    { size_t w=0; for(size_t r=0;r<live.size();r++){ if(live[r].last >= (long)i - cooldown) live[w++]=live[r]; } live.resize(w); }
    if(prePlaced[i]) continue;                             // ★事前確保済 head は再配置 skip
    int r = find(i);
    if(grpCount[r] > 1){
      // ★over-read グループ: 同 root 複数層を 1 連続ブロックで配置 (bump の相対 offset を保存)。
      //   初回 (最小 push 層) で grpHi-grpLo をまとめて place → 各層 newOut = base + (bumpOut - grpLo)。
      if(!grpPlaced[r]){ uint32_t gsz = grpHi[r]-grpLo[r]; grpBase[r]=place(gsz); grpPlaced[r]=1;
        live.push_back({grpBase[r], grpBase[r]+gsz, grpLast[r]}); }
      newOut[i] = grpBase[r] + (bumpOut[i]-grpLo[r]);
      L[i]->out = L_U(newOut[i]);   // ★4KB単位
      continue;
    }
    size_t sz = wordsOf(L[i]);
    uint32_t base;
    if(finalUnique && i == n-1){
      // ★最終層: 既配置の最上端(peak)の上へ。誰とも共有しない = kernel の address-match redirect が
      //   最終層だけに一致する。他層は既に配置済なので**この選択は他層に一切影響しない**。
      base = reserveLo;
      for(int j=0;j<n-1;j++){ uint32_t hi = newOut[j] + (uint32_t)wordsOf(L[j]); if(hi > base) base = hi; }
      base = (uint32_t)ROUNDEDUP(base, 8);
      if(base + (uint32_t)sz > maxw)
        fprintf(stderr,"[FINAL_OUT] ★OOM: 最終層 base=%u sz=%zu > maxw=%u\n", base, sz, (unsigned)maxw);
      fprintf(stderr,"[FINAL_OUT] L%d(最終層) を peak 上の一意アドレスへ out=%u (sz=%zu word) — redirect 誤爆を防止\n",
              (int)L[i]->id, base, sz);
    } else {
      base = place(sz);
    }
    newOut[i] = base;
    live.push_back({base, (uint32_t)(base+sz), lastUse[i]});
    L[i]->out = L_U(base);   // ★4KB単位
  }
  // ---- in / i32[] を新 out へ追従修正 (producer は push 順で先に確定済み) ----
  for(int i=0;i<n;i++){
    Layer* l = L[i];
    l->in = L_U((l->f == i) ? inputIndex : newOut[l->f]);   // ★4KB単位 (word→unit)
    auto it = extra.find(l->id);
    // extra 入力の buffer 追従。Concat の 3 番目 extra (k==2, =4 番目入力) は host と同じく i32[4]
    // (weight_b slot, recipe 非衝突) へ。i32[2]/i32[3] は Concat recipe(linear/weight_m/zoomin)と union
    // 衝突するため避ける。Add(extra 1)/Concat(2-3 extra) いずれも k<2 は i32[k] のまま。
    if(it != extra.end()){ int k=0; for(int pe : it->second){ int slot = (k==2) ? 4 : k; l->i32[slot] = L_U(pe==i ? inputIndex : newOut[pe]); k++; } }   // ★i32 二次入力アドレスも 4KB単位
  }
  // ■ ★FINAL_OUT 一意性の hard gate (常時実行)
  if(n > 0){
    int dup = 0;
    for(int i=0;i<n;i++) if(newOut[i] == newOut[n-1]) dup++;
    if(dup != 1)
      fprintf(stderr,"[FINAL_OUT] ★★不正な n.q: 最終層 L%d の out=%u を %d 層が共有 — kernel redirect が誤爆し\n"
                     "            その出力を読む層(head 等)が破損する。NO_FINAL_OUT_UNIQUE を外して再生成せよ。\n",
              (int)L[n-1]->id, newOut[n-1], dup);
    else
      fprintf(stderr,"[FINAL_OUT] 一意性 OK: 最終層 L%d out=%u を出力する層は 1 個\n", (int)L[n-1]->id, newOut[n-1]);
  }
  // ★案B2 自己診断 (env RELAYOUT_SELFCHECK): 全層ペアで「live 区間が重なる(=j<=lastUse[i]) かつ
  //   アドレス区間が重なる かつ 同 over-read グループでない」= WAR 破損候補を報告。0 なら liveness 健全。
  if(getenv("RELAYOUT_SELFCHECK")){
    int viol=0;
    for(int i=0;i<n;i++){
      uint32_t loi=newOut[i], hii=newOut[i]+(uint32_t)wordsOf(L[i]); long di=lastUse[i];
      for(int j=i+1;j<n;j++){
        if((long)j > di) break;                       // j 以降は i が既に dead (j は昇順)
        if(find(i)==find(j)) continue;                // 同 over-read グループは意図的隣接
        uint32_t loj=newOut[j], hij=newOut[j]+(uint32_t)wordsOf(L[j]);
        if(loi<hij && loj<hii){
          if(++viol<=12) fprintf(stderr,"[SELFCHECK] OVERLAP L%d[%u,%u) (die@%ld) ∩ L%d[%u,%u) born@%d\n",
            (int)L[i]->id,loi,hii,di,(int)L[j]->id,loj,hij,j);
        }
      }
    }
    fprintf(stderr,"[SELFCHECK] live×addr 重複違反 = %d %s\n", viol, viol?"<<BUG":"= liveness 健全");
    // ★ground-truth 整合チェック (lastUse 非依存): 各層 i が読む全 producer p(=f と extra)について、
    //   p が書かれた(=p)後 i が読むまでに、別層 j(p<j<i) が p の buffer を上書きしないか直接検査。
    //   lastUse 追跡漏れがあれば「self-check 健全」でもここが検出する(consumer 関係を直に使うため)。
    int gv=0;
    auto chk_prod=[&](int i,int p){ if(p<0||p>=n||p==i) return;
      uint32_t lop=newOut[p], hip=newOut[p]+(uint32_t)wordsOf(L[p]);
      for(int j=p+1;j<i;j++){ if(find(j)==find(p)) continue;
        uint32_t loj=newOut[j], hij=newOut[j]+(uint32_t)wordsOf(L[j]);
        if(lop<hij && loj<hip){
          if(++gv<=20) fprintf(stderr,"[GT] L%d の入力 producer L%d[%u,%u) が L%d[%u,%u)(@step%d) に上書きされる前に L%d が読む(@step%d, lastUse[L%d]=%ld)\n",
            (int)L[i]->id,(int)L[p]->id,lop,hip,(int)L[j]->id,loj,hij,j,(int)L[i]->id,i,(int)L[p]->id,lastUse[p]);
          break; } } };
    for(int i=0;i<n;i++){ Layer* l=L[i];
      chk_prod(i, l->f==i?-1:l->f);
      auto it=extra.find(l->id); if(it!=extra.end()) for(int pe:it->second) chk_prod(i, pe==i?-1:pe);
    }
    fprintf(stderr,"[GT] ground-truth 入力上書き違反 = %d %s\n", gv, gv?"<<真のバグ":"= 全consumer の入力健全");
    // ★予約 floor / peak / head-gap 占有チェック: relayout が floor 未満に置かない(place 保証)+
    //   HEAD_RESERVE_FRONT の head 域 [Y26_HEAD_BASE, Y26_FRONT_RESERVE) を通常層が踏まないか直接検査。
    {
      uint32_t mn=0xFFFFFFFFu, mx=0; int belowFloor=0, headHit=0;
      for(int i=0;i<n;i++){ uint32_t lo=newOut[i], hi=newOut[i]+(uint32_t)wordsOf(L[i]);
        if(lo<mn) mn=lo; if(hi>mx) mx=hi;
        if(lo < reserveLo) belowFloor++;
#if defined(HEAD_RESERVE_FRONT)
        if(lo < (uint32_t)Y26_FRONT_RESERVE && hi > (uint32_t)Y26_HEAD_BASE) headHit++;   // head 域と交差
#endif
      }
      fprintf(stderr,"[FLOOR] reserveLo=%u placed=[%u,%u) maxw=%u / floor未満=%d %s\n",
              (unsigned)reserveLo,(unsigned)mn,(unsigned)mx,(unsigned)maxw,belowFloor,belowFloor?"<<BUG":"OK");
#if defined(HEAD_RESERVE_FRONT)
      fprintf(stderr,"[FLOOR] head域[%u,%u) を踏む通常層 = %d %s\n",
              (unsigned)Y26_HEAD_BASE,(unsigned)Y26_FRONT_RESERVE,headHit,headHit?"<<衝突(修正失敗)":"= head 域クリア(修正成功)");
#endif
#if defined(BATCH_IN_BASE)
      // ■ ★CEIL 検査 — 層 peak が入力域を超えないこと
      if(mx > (uint32_t)BATCH_IN_BASE)
        fprintf(stderr,"[CEIL] ✗ 層 peak=%u > BATCH_IN_BASE=%u = working ring が入力スロットを踏む"
                       " (N>=3 の異画像で破損。canary N=2 では露見しない)\n",
                (unsigned)mx,(unsigned)BATCH_IN_BASE);
      else
        fprintf(stderr,"[CEIL] 層 peak=%u < BATCH_IN_BASE=%u OK (入力域を侵さない)\n",
                (unsigned)mx,(unsigned)BATCH_IN_BASE);
#endif
      // ■ ★uint16 in/out overflow ガード
      if(mx > (0xFFFFu << 6)){
        fprintf(stderr,"[CEIL] ✗ 層 peak=%u word (%.1f MiB) > uint16_t in/out 上限 %u word (256MiB)"
                       " = in/out overflow(truncate garbage)。GMEM/relayout を 256M 未満へ。\n",
                (unsigned)mx, (double)mx*sizeof(GMEM_T)/1048576.0, (unsigned)(0xFFFFu << 6));
        exit(1);
      }
    }
#if defined(HEAD_RESERVE_FRONT)
    // ■ ★HEADSAFE 検査 — host が読む head を後段が踏まないこと
    {
      static const int kHeads[] = { 131, 136, 140, 145, 149, 154 };
      int viol = 0;
      for(int h : kHeads){
        if(h >= n) continue;
        uint32_t hlo = newOut[h], hhi = newOut[h] + (uint32_t)wordsOf(L[h]);
        for(int j = h + 1; j < n; j++){                    // 後段層のみ(head を書いた後に踏む層)
          uint32_t lo = newOut[j], hi = newOut[j] + (uint32_t)wordsOf(L[j]);
          if(!wordsOf(L[j])) continue;
          if(lo < hhi && hlo < hi){
            fprintf(stderr,"[HEADSAFE] ✗ head L%d[%u,%u) を後段 L%d[%u,%u) が上書き\n",
                    h,(unsigned)hlo,(unsigned)hhi,j,(unsigned)lo,(unsigned)hi);
            viol++; break;                                  // head ごとに 1 件報告で十分
          }
        }
      }
      fprintf(stderr,"[HEADSAFE] host 読み head を踏む後段層のある head = %d %s\n",
              viol, viol ? "<<★実機で head が壊れる (MAP_PIN_HEADS に当該 head を追加せよ)"
                         : "= 全 head 生存 (host 読み安全)");
    }
#endif
  }
  // ■ over-read 整合チェック (OVERREAD_CHECK)
  if(getenv("OVERREAD_CHECK")){
    int orv=0, ortot=0;
    auto check_read=[&](int i,int p,uint32_t rw){ if(p<0||p>=n||p==i) return;
      uint32_t pw=(uint32_t)wordsOf(L[p]); if(rw<=pw) return;            // over-read 無し
      ortot++;
      uint32_t tlo=newOut[p]+pw, thi=newOut[p]+rw;                       // 食い込み tail [tlo,thi)
      // tail を占有する層を packed layout で列挙
      for(int q=0;q<n;q++){ if(q==p) continue;
        uint32_t qlo=newOut[q], qhi=newOut[q]+(uint32_t)wordsOf(L[q]);
        if(qlo<thi && tlo<qhi){                                         // tail と重なる層 q
          bool sameGrp=(find(q)==find(p));
          if(!sameGrp){
            if(++orv<=24) fprintf(stderr,
              "[OVERREAD] L%d(consumer@step%d) が producer L%d[%u,%u) を %u word(npix食込) 読む → tail[%u,%u) が別group L%d[%u,%u) に食込 (push %s)\n",
              (int)L[i]->id,i,(int)L[p]->id,newOut[p],newOut[p]+pw,rw,tlo,thi,(int)L[q]->id,qlo,qhi, (q==p+1?"連続":"NON連続=desync候補"));
          }
        }
      }
    };
    for(int i=0;i<n;i++){ Layer* l=L[i];
      auto it=extra.find(l->id);
      bool realConcat=(l->type==LayerType::Concat)&&(it!=extra.end())&&!it->second.empty();
      if(l->type!=LayerType::Add && !realConcat) continue;
      uint32_t npix=(uint32_t)l->os[2]*(uint32_t)l->os[3];
      check_read(i, l->f==i?-1:l->f, ((uint32_t)l->is[0]==0)?((npix+1)/2):(npix*(uint32_t)l->is[0]));
      if(l->type==LayerType::Add){ uint16_t ch=(uint16_t)l->os[1]; uint32_t wpp=(ch>=32)?(ch>>5):0; if(it!=extra.end()&&!it->second.empty()) check_read(i,it->second[0], wpp?npix*wpp:(npix+1)/2); }
      else if(it!=extra.end()){ int k=0; for(int pe:it->second){ uint16_t vch=(uint16_t)l->p[k]; uint32_t wpp=(vch>=32)?(vch>>5):0; check_read(i,pe, wpp?npix*wpp:(npix+1)/2); k++; } }
    }
    fprintf(stderr,"[OVERREAD] over-read 箇所=%d, 別group食込違反=%d %s\n", ortot, orv, orv?"<<corruption候補":"= 全over-read が同group連続(健全)");
    uint32_t pk=0; for(int i=0;i<n;i++){ uint32_t e=newOut[i]+(uint32_t)wordsOf(L[i]); if(e>pk)pk=e; }
    fprintf(stderr,"[PEAK] peak end=%u word (%.3f MB), maxw=%u, 余裕=%d word\n", pk, pk*64.0/1048576.0, (unsigned)maxw, (int)maxw-(int)pk);
  }
#ifdef RELAYOUT_DIAG
  { uint32_t pk=0; for(int i=0;i<n;i++){ uint32_t e=newOut[i]+(uint32_t)wordsOf(L[i]); if(e>pk)pk=e; } int grp=0; for(int i=0;i<n;i++) if(find(i)!=i) grp++;
    fprintf(stderr,"[PEAKLIVE] peak end=%u word (%.3f MB), grouped layers=%d\n", pk, pk*64.0/1048576.0, grp); }
#endif
  fprintf(stderr,"[LIVENESS] relayout done: %d layers (peak end watched)\n", n);
}
#endif

// 生成器が他 TU に期待する関数 (src/nq_support.cpp が実装)
int file_save(const char* fn, void* rp, int size);

#endif // NQ_DEFS_H

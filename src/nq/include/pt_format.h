// SPDX-License-Identifier: AGPL-3.0-or-later
/* pt_format.h  (format version 5)
 *
 * pt_tree がテキストで表示している情報 (メタ情報・モジュール階層・テンソル) に
 * 加え、重み (param) / const (buffer) テンソルの実データを 8bit 量子化して
 * パックしたバイナリ出力フォーマットの定義。
 *
 * このヘッダ単体で、出力された .ptb ファイルを読み取り・解釈できる。
 *
 * ==========================================================================
 * version 3 のレイアウト方針
 * ==========================================================================
 *   - pt_record は固定 64 バイト。
 *   - TENSOR レコードの直後に、その量子化データ (uint8 × numel) をインライン格納
 *     する。META / MODULE レコードの後ろには何も続かない。
 *     → TENSOR が可変長データを伴うため、レコードは 64B 等間隔ではない
 *        (roff + i*64 ではアクセスできない)。先頭から順次走査する。
 *   - 文字列はレコードに持たず、ファイル末尾の「文字列プール」にまとめて格納
 *     する (重複は共有)。レコードはプール内オフセットで参照する。
 *
 * --------------------------------------------------------------------------
 * 量子化 (8bit, per-tensor アフィン)
 * --------------------------------------------------------------------------
 *   各テンソルごとに最小値 min・最大値 max を求め、
 *       q = round((x - min) / (max - min) * 255)   (0..255, range=0 のとき 0)
 *   で uint8 に量子化する。min/max は pt_record.qmin / qmax (float32) に格納。
 *   復元 (脱量子化): x ~= min + q / 255 * (max - min)
 *
 * --------------------------------------------------------------------------
 * 活性化メモリアドレス (in[4] / out)  ※ トップレベル層 (MODULE, is_top=1) のみ
 * --------------------------------------------------------------------------
 *   推論時の活性化バッファを 512 バイト単位で連番割り当てした結果。
 *   固定入力 (既定 c=4, h=640, w=640) から各層の c*h*w を形状推論して算出する。
 *     out = in[0] + ceil(入力 c*h*w * 2 / 512)         (2 = float16 の 1 要素 2byte)
 *     最初の層 in[0] = 1 (入力は アドレス 1)
 *     次の層 in[0] = 前層の out
 *     out が 65535 を超えたら out = 1 に戻す
 *   in[0..3] は f (from) routing に対応した入力元レイヤの out アドレス
 *   (Concat は 2 入力、Detect は 3 入力など。-1 は直前層)。
 *   トップ層以外のレコードでは in[]/out は 0。
 *
 * --------------------------------------------------------------------------
 * ファイル全体の構造
 * --------------------------------------------------------------------------
 *   [pt_file_header]                 先頭 (52B)。以降 record_offset まで 0 埋め
 *   (padding)
 *   record_offset から順次:
 *       [pt_record] (48B)
 *       TENSOR なら [uint8 × int_value(numel)]   ← 量子化データ
 *   [string pool]                    string_pool_offset から。NUL 終端 UTF-8 連結
 *
 *   レコードはテキスト出力と同じ前順 (pre-order) 走査順。pt_record.depth
 *   (MODULE) で木構造を復元できる。多バイト数値はリトルエンディアン。
 */
#ifndef PT_FORMAT_H
#define PT_FORMAT_H

#include <stdint.h>

#define PT_FORMAT_VERSION 5
#define PT_MAGIC0 'P'
#define PT_MAGIC1 'T'
#define PT_MAGIC2 'T'
#define PT_MAGIC3 '5'        /* "PTT5" */

#define PT_RECORD_SIZE 56    /* pt_record の固定サイズ                   */
#define PT_ALIGN       64    /* レコード領域の開始境界                   */
#define PT_MAX_DIMS    4     /* レコード内にインラインできる次元数       */
#define PT_MAX_INPUTS  4     /* in[] (入力元アドレス) の数               */
#define PT_NO_STRING   0xFFFFFFFFu  /* 文字列なしを表すオフセット    */

/* テンソルの要素型 (pickle の storage クラスに対応) */
enum pt_dtype {
    PT_DT_UNKNOWN = 0,
    PT_F16,    /* HalfStorage     */
    PT_F32,    /* FloatStorage    */
    PT_F64,    /* DoubleStorage   */
    PT_BF16,   /* BFloat16Storage */
    PT_I64,    /* LongStorage     */
    PT_I32,    /* IntStorage      */
    PT_I16,    /* ShortStorage    */
    PT_I8,     /* CharStorage     */
    PT_U8,     /* ByteStorage     */
    PT_BOOL    /* BoolStorage     */
};

/* レコード種別 (pt_record.kind) */
enum pt_record_kind {
    PT_REC_META   = 0,   /* トップレベルのメタ情報 (version, date, ...) */
    PT_REC_MODULE = 1,   /* nn.Module ノード                           */
    PT_REC_TENSOR = 2    /* パラメータ または バッファ (量子化データ付) */
};

/* テンソル種別 (pt_tensor_rec.tensor_kind) */
enum pt_tensor_kind {
    PT_TENSOR_PARAM  = 0,  /* _parameters (weight, bias)             */
    PT_TENSOR_BUFFER = 1   /* _buffers (running_mean, running_var..)  */
};

#pragma pack(push, 1)

/* 固定長ファイルヘッダ (52 バイト)。以降 record_offset まで 0 埋め。 */
struct pt_file_header {
    char     magic[4];          /* {'P','T','T','5'}                  */
    uint16_t format_version;    /* PT_FORMAT_VERSION (=5)             */
    uint16_t endianness;        /* 1 = little endian                  */
    uint32_t record_count;      /* レコード総数                       */
    uint32_t module_count;      /* PT_REC_MODULE の数                 */
    uint32_t tensor_count;      /* PT_REC_TENSOR の数                 */
    uint64_t element_count;     /* 全テンソルの要素数合計             */
    uint32_t record_offset;     /* 先頭レコードのファイルオフセット (64 境界) */
    uint32_t record_stride;     /* = PT_RECORD_SIZE (48)。ただし TENSOR は直後に量子化データ */
    uint32_t string_pool_offset;/* 文字列プールのファイルオフセット   */
    uint32_t string_pool_size;  /* 文字列プールのバイト数             */
    uint64_t quant_data_bytes;  /* 量子化データの総バイト数           */
};

/* 1 レコード: 固定 64 バイト。フィールドの有効・無効は kind による。
 *
 *   PT_REC_META   : name_off=key, value_is_int? int_value : aux_off=value文字列
 *   PT_REC_MODULE : name_off=モジュール名, aux_off=クラス名, depth/is_top/layer_index
 *   PT_REC_TENSOR : name_off=名前, dtype/tensor_kind/ndim/dims,
 *                   int_value=numel(=直後の量子化データのバイト数),
 *                   qmin/qmax=脱量子化用の最小値・最大値
 */
struct pt_record {               /* offset                                       */
    uint8_t  kind;               /*  0  enum pt_record_kind                      */
    uint8_t  dtype;              /*  1  enum pt_dtype        (TENSOR; 元の型)     */
    uint8_t  tensor_kind;        /*  2  enum pt_tensor_kind  (TENSOR)            */
    uint8_t  ndim;               /*  3  次元数               (TENSOR)            */
    uint8_t  is_top;             /*  4  1=トップレベル層     (MODULE)            */
    uint8_t  value_is_int;       /*  5  1=数値が int_value   (META)              */
    uint16_t depth;              /*  6  階層の深さ           (MODULE)            */
    int16_t  layer_index;        /*  8  m.i, 非トップは -1   (MODULE)            */
    uint16_t out;                /* 10  出力アドレス(512B単位) (MODULE top のみ)  */
    uint32_t name_off;           /* 12  文字列プール内オフセット (name)          */
    uint32_t aux_off;            /* 16  MODULE:class / META:value ; 無=PT_NO_STRING */
    int32_t  int_value;          /* 20  META:int値 / TENSOR:numel(=量子化データ長) */
    float    qmin;               /* 24  TENSOR: 量子化前の最小値                 */
    float    qmax;               /* 28  TENSOR: 量子化前の最大値                 */
    int32_t  dims[PT_MAX_DIMS];  /* 32  TENSOR:各次元 (先頭 ndim 個が有効)        */
    uint16_t in[PT_MAX_INPUTS];  /* 48  入力アドレス(512B単位) (MODULE top のみ)  */
};                               /* 48 + 2*4 = 56 バイト                         */

#pragma pack(pop)

#endif /* PT_FORMAT_H */

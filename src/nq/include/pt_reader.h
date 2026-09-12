// SPDX-License-Identifier: AGPL-3.0-or-later
// pt_reader.h — .pt (PyTorch torch.save 形式) 取り込みの公開 API
//
// 参考実装 (pt_analyzer / pt_tree.cpp) を本プロジェクトへ
// 取り込んだもの。C++17 標準ライブラリのみで .pt の ZIP+pickle を解析する
// (LibTorch 非依存)。HW/cosim build には不要なので native 生成器 build (VSCODE) 限定。
//
#ifndef PT_READER_H
#define PT_READER_H
#if defined(VSCODE)

#include <map>
#include <string>
#include <vector>

// BN 融合後の Conv テンソル (float)。named_modules パス + ".weight"/".bias" をキーに保持し、
// レイアウトは ONNX initializer と同じ行優先 [oc,ic,kh,kw] (weight) / [oc] (bias)。
struct PtTensor {
    std::vector<int>   dims;   // weight: [oc,ic,kh,kw] / bias: [oc]
    std::vector<float> data;   // 行優先
};

// .pt の top module (model.0..N) 1 つ分の構造 (Phase C generator 用、ONNX 非依存)。
//   平坦 n.q への展開はこの top 粒度から module 型ごとに forward を再実装して行う。
struct PtTopModule {
    int              index;       // N in model.N
    std::string      cls;         // クラス名 ("Conv" / "C2f" / "SPPF" / "C2PSA" / ...)
    std::vector<int> f;           // 解決済み入力元 top index (-1 は直前 = index-1 に解決済)
    // Conv 系 (conv 子モジュールを持つ場合) の重み形状。無ければ全 0。
    int oc = 0, ic = 0, k = 0, stride = 0;
    std::string      conv_path;   // 例 "model.0.conv" (重み参照キー。weight = conv_path + ".weight")
    bool shortcut = false;        // C2f 系: bottleneck (m.0) の add 属性 (residual の有無)
    std::string      bneck_cls;   // C2f/C3k2 の m.0 クラス ("Bottleneck"=C2f互換 / "C3k"=要専用emitter / "Sequential"=model.22)
    bool add = false;             // top module 自身の add 属性 (yolo26 SPPF residual: cv2(cat)+x)
};

// .pt の Sequential("model") 直下の top module を順序通りに収集する (ONNX 非依存)。
//   f は planAddresses と同じ規約で解決 (-1 -> index-1)。Conv 系は oc/ic/k/stride を埋める。
//   module 内部分解 (C2f/PSA/Detect の forward) はここから generator 側で行う。
// 戻り値: true=成功。
bool pt_collect_modules(const char* pt_path, std::vector<PtTopModule>& out);

// Phase A: .pt の階層 + サマリを表示する。
//   ptb_out == nullptr : .ptb を出力しない (表示のみ)
//   ptb_out == ""      : 入力名から導出 (yolo26n.pt -> yolo26n.ptb, カレントへ)
//   それ以外           : 明示パスへ .ptb を書く
// 戻り値: 0=成功, 非0=失敗。
int pt_dump(const char* pt_path, const char* ptb_out);

// .pt の全 Conv の **BN 融合済み** weight/bias を収集する。
//   キー = conv モジュールの named_modules パス + ".weight" / ".bias"
//         (例 model.0.conv.weight) で ONNX initializer 名と一致する。
//   conv に bn 兄弟があれば融合、無ければ conv 自身の weight/bias をそのまま使う。
// 戻り値: true=成功。
bool pt_collect_conv_weights(const char* pt_path,
                             std::map<std::string, PtTensor>& weights,
                             std::map<std::string, PtTensor>& biases);

// Phase C step4: data_shift (zoomin/zoomout) sidecar をエクスポートする。
//   .pt/ONNX に無いキャリブレーション値を **layer id をキーにした sidecar** に切り出し、
//   .pt 完全生成器がこれを読んで encode_conv_block 等へ供給できるようにする (供給問題の解決)。
//   golden n.q (nq_path) を走査して使用 id 範囲を確定し、host data_shift[128][2] を出力。
//   同時に「golden n.q の Conv recipe」と「host data_shift[id]」の一致を検証する。
//   out_path == nullptr: nq_path と同じ dir の data_shift.txt。
//   (Layer 定義 / data_shift extern に依存するため定義は src/nq.cpp 側)
// 戻り値: 0=一致, 非0=不一致 / エラー。
int pt_dump_shift(const char* nq_path, const char* out_path);

// Phase C step4 (逆操作): data_shift sidecar を読み込んで shift[128][2] を再構成する。
//   .pt 完全生成器が呼び、ONNX に無い zoomin/zoomout を encode_conv_block 等へ供給する。
//   未記載 id は既定 {3,3} (host data_shift の支配値) で埋める。
// 戻り値: 読み込んだ最大 id (>=0)。エラー時は -1。
int pt_load_shift(const char* path, unsigned char shift[128][2]);

// Phase C step1/3: .pt から n.q Layer を生成し golden n.q と層ごとに突合する (forward 再実装の着手点)。
//   現状は pure-Conv prefix (1:1 module) のみ生成・検証 (最初の非 Conv module で打ち切り)。
//   header (id/type/f/is/os/p/recipe) + weight/bias byte を突合。in/out は planner 未移植で対象外。
//   (Layer 定義 / encode_conv_block に依存するため定義は src/nq.cpp 側)
// 戻り値: 0=header 全一致, 非0=不一致 / エラー。
int pt_gen_layers_verify(const char* pt_path, const char* nq_path, const char* shift_path, const char* gen_out);

// 診断: golden n.q の全 Layer ヘッダをダンプ (emitter 設計用)。
int pt_nq_dump(const char* nq_path);
int pt_nq_wdump(const char* nq_path, int target_id, const char* out_path);

#endif  // VSCODE
#endif  // PT_READER_H

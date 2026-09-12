// SPDX-License-Identifier: AGPL-3.0-or-later
// nq_main.cpp — n.q 生成器の CLI。
//   内部ツリーでは src/tasks.cpp の main() の .pt 取り込みブロック (VSCODE 分岐) に相当する。
//   推論/OpenCL 実行経路は公開物には含めず、.pt → n.q の生成と検査だけを提供する。
//
// 使い方:
//   nq --pt <model.pt> --gen <out/n.q>   .pt から n.q を生成
//   nq --pt <model.pt> --gen-layers      生成せず層構成だけを検査表示
//   nq --pt <model.pt> --nq-dump [n.q]   既存 n.q のヘッダを一覧表示
//   nq --pt <model.pt> --dump-shift [n.q] [out.txt]
//                                        既存 n.q から data_shift sidecar を書き出す
//   nq --pt <model.pt> --load-shift [in.txt]
//                                        sidecar を内蔵 data_shift と突合
//   nq --pt <model.pt>                   .pt の階層ダンプのみ
//
//   sidecar (data_shift_y26.txt) / n.q の既定 path は <model.pt> と同じディレクトリ。
#include "nq_defs.h"
#include "pt_reader.h"

#include <cstring>
#include <string>

// src/nq.cpp が提供する生成器エントリ
int pt_gen_layers_verify(const char* pt_path, const char* nq_path, const char* shift_path, const char* gen_out);
int pt_nq_dump(const char* nq_path);
int pt_nq_wdump(const char* nq_path, int target_id, const char* out_path);
int pt_dump_shift(const char* nq_path, const char* out_path);
int pt_load_shift(const char* path, unsigned char shift[128][2]);

static void usage(const char* argv0){
  fprintf(stderr,
    "使い方: %s --pt <model.pt> [--gen <out/n.q> | --gen-layers [n.q] | --nq-dump [n.q]\n"
    "                            | --dump-shift [n.q] [out.txt] | --load-shift [in.txt] | --ptb [prefix]]\n",
    argv0);
}

int main(int argc, char **argv){
  setvbuf(stdout, NULL, _IONBF, 0);
  // env OUT_ALIGN4K: 層出力先頭を 4KB(64-word) 境界へ整列 (NqBumpPlanner に効く)
  g_out_align4k = (getenv("OUT_ALIGN4K") && atoi(getenv("OUT_ALIGN4K"))) ? 1 : 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--pt")) continue;
    const char *pt = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (!pt) { fprintf(stderr, "--pt にはモデルパスが必要 (例: nq --pt data/yolo26n.pt)\n"); return 1; }
    const char *ptb = nullptr, *shiftOut = nullptr, *shiftIn = nullptr, *genNq = nullptr, *genOut = nullptr;
    bool dumpShift = false, loadShift = false, genLayers = false, nqDump = false, doGen = false;
    for (int j = i + 2; j < argc; j++) {
      if (!strcmp(argv[j], "--ptb"))             ptb = (j + 1 < argc && argv[j + 1][0] != '-') ? argv[++j] : "";
      else if (!strcmp(argv[j], "--dump-shift")) { dumpShift = true; if (j + 1 < argc && argv[j + 1][0] != '-') shiftOut = argv[++j]; }
      else if (!strcmp(argv[j], "--load-shift")) { loadShift = true; if (j + 1 < argc && argv[j + 1][0] != '-') shiftIn = argv[++j]; }
      else if (!strcmp(argv[j], "--gen-layers")) { genLayers = true; if (j + 1 < argc && argv[j + 1][0] != '-') genNq = argv[++j]; }
      else if (!strcmp(argv[j], "--gen"))        { genLayers = true; doGen = true; if (j + 1 < argc && argv[j + 1][0] != '-') genOut = argv[++j]; }
      else if (!strcmp(argv[j], "--nq-dump"))    { nqDump = true; if (j + 1 < argc && argv[j + 1][0] != '-') genNq = argv[++j]; }
    }
    // n.q / sidecar の既定 path: pt と同じ dir
    std::string dir;
    { std::string p(pt); size_t s = p.find_last_of("/\\"); dir = (s==std::string::npos? std::string() : p.substr(0,s+1)); }
    int rc = 0;
    if (!dumpShift && !loadShift && !genLayers && !nqDump) rc = pt_dump(pt, ptb);   // 階層ダンプのみ
    if (nqDump)    rc |= pt_nq_dump(((genNq && genNq[0]) ? std::string(genNq) : dir + "n.q").c_str());
    if (nqDump && getenv("NQ_WDUMP_ID")) {
      const char* nqp2 = (genNq && genNq[0]) ? genNq : nullptr;
      std::string nqs = nqp2 ? std::string(nqp2) : dir + "n.q";
      const char* wo = getenv("NQ_WDUMP_OUT"); std::string wos = wo ? std::string(wo) : std::string("nq_wdump.bin");
      rc |= pt_nq_wdump(nqs.c_str(), atoi(getenv("NQ_WDUMP_ID")), wos.c_str());
    }
    if (dumpShift) rc |= pt_dump_shift(((genNq && genNq[0]) ? std::string(genNq) : dir + "n.q").c_str(), shiftOut);
    if (genLayers) {
      std::string nqp = (genNq && genNq[0]) ? std::string(genNq) : dir + "n.q";
      std::string outp = doGen ? ((genOut && genOut[0]) ? std::string(genOut) : dir + "n_pt.q") : std::string();
      rc |= pt_gen_layers_verify(pt, nqp.c_str(), (dir + "data_shift.txt").c_str(), doGen ? outp.c_str() : nullptr);
    }
    if (loadShift) {
      std::string in = (shiftIn && shiftIn[0]) ? std::string(shiftIn) : dir + "data_shift.txt";
      unsigned char shift[128][2];
      int maxid = pt_load_shift(in.c_str(), shift);
      if (maxid < 0) rc |= 1;
      else {
        int mism = 0;
        for (int id = 0; id <= maxid; ++id)
          if (shift[id][0] != data_shift[id][0] || shift[id][1] != data_shift[id][1]) {
            printf("  [LOAD-MISMATCH] id=%d  sidecar{%d,%d} != 内蔵 data_shift{%d,%d}\n",
                   id, shift[id][0], shift[id][1], data_shift[id][0], data_shift[id][1]);
            ++mism;
          }
        printf("--load-shift: round-trip vs 内蔵 data_shift[0..%d] = %s (mismatch=%d)\n",
               maxid, mism ? "FAIL" : "OK", mism);
        rc |= mism ? 1 : 0;
      }
    }
    return rc;
  }
  usage(argv[0]);
  return 1;
}

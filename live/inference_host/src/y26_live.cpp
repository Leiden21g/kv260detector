// y26_live.cpp — KV260 Web 配信専用 推論 host(2026-09-10 に配信専用へ簡素化 + 改名)。
//   カメラ → (外部 ppdaemon が量子化) → PL 推論(YOLO26n 640×640)→ head decode →
//   NV12 overlay を /tmp/overlay_live.nv12 へ atomic 差替。以降は vcu_stream(別プロセス)が H.264 化する。
//
//   ★簡素化の内容(旧: src/host.cpp 869 行 + src/tasks.cpp 2,104 行 + ヘッダ 8 本 5,900 行)
//     - 旧 src/host.cpp + src/tasks.cpp を統合した実体なので、名前も **y26_live.cpp** にした
//       (ヘッダ include/y26_live.h / レシピ y26_live_recipe.env と対)。
//     - **src/tasks.cpp を廃止**し main を本ファイルへ統合。ヘッダは include/y26_live.h の **1 本**だけ。
//     - live 配信で通らない経路を全部落とした: 静的 batch 検証 / diff_tensor / self-consistency(Add,
//       Concat, MaxPool, Resize, Split)/ PSA golden 突合 / HB ring / PROBE63 / DBG_L50 / LAYER_MAP /
//       DUMP_{OUT,AUX,RAW,TABLE,HEADS,LAYERMAP,NQ} / SEED_RUN / GATE_A_TEST / STDIN_DAEMON /
//       PHASE3_PROF の全計測 / SERIAL 経路 / NQ_INPUT_ADDR(設計A')/ BUF_RING / in-host v4l2 capture /
//       in-host VCU encode(VCU_H264_FIFO)/ P5CLS_FLOAT / mid-chain seed(start_layer>0)。
//     - 本番 launcher が必ず立てる env は分岐ごと畳んだ(下の「★前提」)。
//
//   ★前提(run_live_rtsp_stream.sh が常に与えるもの。外れると起動時に abort するか既定で動く)
//     FILELIST=…            必須。無ければ abort(旧 in-host capture への fallback は削除)
//     start_layer=0         必須(mid-chain seed 削除)
//     OVERLAY_DEFER         常時 ON 扱い(overlay は go(k+1) の後 = PL と並走)。SNAP_MODE=skip は削除
//     VCU_KEEP_P4=1         常時 ON 扱い(cls P4 を殺す p4kill 経路は削除)
//     DUMP_HEADS_NOFILE=1   live では常時 ON(head の .bin 書き出しを止める)。★分岐は残してある —
//                           未設定時の <out>_L<id>.bin は canary_golden.sh が GOLD と md5 突合する唯一の出口
//     GO_FLUSH=cvac         cvac 固定(16MB evict / min は削除)
//   ★挙動が変わる点(board 検証項目): ①最終層出力の per-image dump(drain_dump → /tmp/lv/o)を廃止
//     = 毎フレームの device→host sync 1 回とファイル書込 1 回が消える(検出には未使用)。
//     ②GO_FLUSH 未指定時の既定が evict → cvac。③VCU_KEEP_P4 未指定でも P4 cls を殺さない。
#include "../include/y26_live.h"
#include <fstream>
#include <iomanip>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <thread>
#include <initializer_list>
using std::ifstream;

int g_nq_stream_nimg = -1;   // 直近 parse した n.q の configure STREAM_NIMG (-1=configure layer 無し=旧 n.q)
NqConfig g_nq_config = {};   // 直近 parse した n.q configure layer 全フィールド (全 0=configure 無し)

// 2026-05-28: L2/L7 OVERFLOW 解消 (fp_max>16 で int16 飽和) のサージカル修正。
// L2..L8 を chain consistent に書換 (L3=Split view of L2, L6=Add(L3,L5), L7=Concat(L2,L3,L6))。
// 値: L2=(0,2), L3=(2,2), L4=(2,3), L5=(3,2), L6=(2,2), L7=(2,2), L8=(2,3)。
// 詳細根拠は gen_l4l5_refs.py 出力の per-kernel-L fp_max(1bit margin)。L9 以降は据置。
const uint8_t data_shift[128][2]={
	6,2,2,0,0,2,2,2, 2,2,2,2,2,2,2,2, 2,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, // 0~15: L0-L15  ★L2-L8改 +真因①: id4(m.0/cv2)zout3→2, id5(Add)zin3→2 で残差Add operand scale一致 (calib_y10.py)
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, // 16~31
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, // 32
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, // 48
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, // 64
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, // 80
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, // 96
	3,3,3,3,4,1,3,1, 3,1,1,3,1,3,1,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, // 112~127
};
/*!
@brief CHWテンソルファイルから読み出す [3,NETH,NETW]float ->HWC[NETH,NETW,4]int16_t[0-255]
@param fn   ファイル名
@param wp_  出力先メモリ(呼出側は Y26_IN_SCRATCH_WORDS 語以上 = HWC int16 [NETH][NETW][4] を保持できること)
@return ファイルサイズ(Byts)。負値 = 失敗(-1 open 失敗 / -2 無し / -3 サイズ不一致)。呼出側は負値で abort すること
@details
  ★geo640 H4(2026-08-28): 幾何は include/tasks.h の Y26_NETH/Y26_NETW(-D で上書き可)から導出(旧 384/640 直書きを廃止)。
  ★入力ファイルサイズ検査: 期待 3*NETH*NETW*sizeof(float) と不一致なら -3(旧 384×640 の x.bin を 640×640 host に
    食わせると先頭 60% だけ埋まって黙って続行する事故の防止。input640_study §3-2)。
  ★EOF quirk(o[0][0][3] が最終 float 値になる)は proven golden 維持のため不変。
*/
int chw_load(const char* fn, void* wp_) {
	constexpr int NETH = (int)(Y26_NETH), NETW = (int)(Y26_NETW);
	typedef int16_t(*OUTPUT)[NETW][4];
	OUTPUT o = (OUTPUT)wp_;
	// auto &o = (int16_t(&)[NETH][NETW][4])wp_;

	struct stat sb {};
	if (!stat(fn, &sb)) {
		printf("%s %ldBytes %ldLines\n", fn, sb.st_size, (sb.st_size-1)/sizeof(GMEM_T)+1);
	} else {
		printf("%s Not found\n", fn);
		return (-2);
	}
	{
		const long expect = 3L * NETH * NETW * (long)sizeof(float);
		// ★req8(2026-08-29): 量子化済 HWC int16 [NETH][NETW][4](camera_preprocess --out-hwc16 の出力、
		//   = 本関数の float 経路が生成するものと byte 同一。o[0][0][3] quirk も ppdaemon 側で再現済)は
		//   **ファイルサイズで自動判別**し、変換無しで o へ fread 一発(4.9MB read + 1.2M float 変換 → 3.3MB read)。
		//   float(3*H*W*4B)と int16(H*W*4*2B)は常に 12:8 で衝突しない。どちらでもなければ従来どおり -3。
		const long expect16 = (long)NETH * NETW * 4 * (long)sizeof(int16_t);
		if ((long)sb.st_size == expect16 && expect16 != expect) {
			FILE *fp16 = fopen(fn, "rb");
			if (!fp16) { printf("%sを開けませんでした。", fn); return (-1); }
			size_t got16 = fread(o, 1, (size_t)expect16, fp16);
			fclose(fp16);
			if ((long)got16 != expect16) { printf("[chw_load] ✗ %s: hwc16 short read %zu/%ldB\n", fn, got16, expect16); return (-4); }
			printf("[chw_load] hwc16 direct: %ldB → [%d][%d][4] int16 (変換無し, o[0][0][3]=%d)\n", expect16, NETH, NETW, (int)o[0][0][3]);
			return sb.st_size;
		}
		if ((long)sb.st_size != expect) {
			printf("[chw_load] ✗ %s: サイズ不一致 %ldB != 期待 %ldB(3*%d*%d*4B, 幾何 NETH=%d NETW=%d)か %ldB(HWC int16 %d*%d*4*2B)。"
			       "旧幾何の x.bin か --size 違いの前処理出力 → 読み込まない\n",
			       fn, (long)sb.st_size, expect, NETH, NETW, NETH, NETW, expect16, NETH, NETW);
			return (-3);
		}
	}

	FILE *fp = fopen(fn, "rb");
	if (!fp) {
		printf("%sを開けませんでした。", fn);
		return (-1);
	}
	float min(0),max(0);
	int c(0),y(0),x(0),count(0);

	// ★一括読み: 旧実装は 1 float ずつ fread(737,280 回)し、live pipeline の refill を
	//   ~300ms/frame で律速していた(2026-07-15 PHASE3_PROF 実測)。一括 fread + 変換ループで
	//   ~10ms 台へ。旧 while(!feof) は EOF 後にもう 1 周し直前値 work のまま 1 要素余分に
	//   書く挙動があり(o[0][0][3] が最終 float 値になる)、proven golden(head byte-exact)
	//   維持のため i<=got の最終+1 周でそれも厳密に再現する。
	size_t nfl = (size_t)sb.st_size / sizeof(float);
	float* fb = (float*)malloc(nfl ? nfl*sizeof(float) : sizeof(float));
	size_t got = fb ? fread(fb, sizeof(float), nfl, fp) : 0;
	for (size_t i = 0; got && i <= got; i++) {
		float work = fb[i < got ? i : got-1];   // i==got: EOF 後の stale 値再現
		o[y][x][c] = work*(1<<(8+data_shift[0][0]));
		if(c == 2){
			o[y][x][c+1] = 0;
		}
		x++; count++;
		if(x==NETW){
			x=0;
			y++;
			if(y==NETH){
				y =0;
				c++;
			}
		}

		if(work < min){
			min=work;
		}
		if(max < work){
			max=work;
		}
	}
	free(fb);
 	std::cout <<std::setw(3+2)<<std::right<<std::fixed<<std::setprecision(2)
      <<"Min:" <<min <<" Max:" <<max <<" count:" <<count <<std::endl;
	fclose(fp);
	return sb.st_size;
}
/*!
@brief Layer 1 個の n.q 内サイズ(byte)を l.type と l.conv から算出
@details 各 layer は最低 8 GMEM_T (= 512 byte) を占有する:
         - 1 GMEM_T: Layer struct header (sizeof(Layer)==64)
         - 7 GMEM_T: reserved gap (nq.cpp::Conv で nn.pos += 8 として確保)
         Conv 系は header+gap の後に bias + weight (GMEM_T 単位) が続く。
         Split/Add/Concat/MaxPool/Finish 等は header+gap のみ (= 8 GMEM_T)。
*/
static int parse_layer_size(const Layer *l) {
	int size = 8 * (int)sizeof(GMEM_T);  // header (1) + reserved gap (7) = 8 GMEM_T = 512 byte
	if (l->type == LayerType::Conv) {
		size += ((int)l->conv.bias + (int)l->conv.weight) * (int)sizeof(GMEM_T);
	}
	return size;
}

/*!
@brief n.q (全 layer 含む完全版) から [start_layer..end_layer] を抽出し Finish[8] を末尾に
       付与して param OpenCL buffer に書き込む。argv[2]/argv[3] で範囲指定する用途。
@param nq_path  入力 n.q ファイル(build/nn が full 版を生成済の前提)
@param param    出力先 OpenCL buffer (int64_t array、GMEM_PARAM_SIZE * 8 byte 確保済)
@param start_layer  抽出開始 layer index (0 から)
@param end_layer    抽出終了 layer index (start_layer 以上)。範囲超過は clamp
@return 書き込んだ byte 数(成功時)、-1(エラー)
*/
int nq_filter_to_param(const char *nq_path, int64_t *param, int start_layer, int end_layer) {
	// 1. n.q 全体を temp buffer に読み込み
	struct stat sb {};
	if (stat(nq_path, &sb) != 0) {
		printf("%s Not found\n", nq_path);
		return -1;
	}
	const int nq_size = sb.st_size;
	std::vector<uint8_t> buf(nq_size);
	{
		ifstream is(nq_path, std::ifstream::binary);
		if (!is.is_open()) { printf("%sを開けませんでした\n", nq_path); return -1; }
		is.read((char*)buf.data(), nq_size);
		is.close();
	}

	// 2. Layer header を順に walk して offset[] table を構築 (Finish 直前まで)
	//    n.q 先頭の configure layer (network 層でない) は読取って skip → offsets[] は L0 から。
	std::vector<int> offsets;
	int off = nq_parse_configure(buf.data(), nq_size, &g_nq_config);
	g_nq_stream_nimg = off ? g_nq_config.stream_nimg : -1;
	if (off) printf("nq_config: STREAM_NIMG=%d in[base=%d,stride=%d] out[base=%d,stride=%d] hb=%u reserve=%u (%d byte skip)\n",
	                g_nq_config.stream_nimg, g_nq_config.in_base, g_nq_config.in_stride,
	                g_nq_config.out_base, g_nq_config.out_stride, g_nq_config.hb_ring_base, g_nq_config.ring_reserve, off);
	while (off + (int)sizeof(Layer) <= nq_size) {
		const Layer *l = (const Layer *)(buf.data() + off);
		if (l->type == LayerType::Finish) break;
		offsets.push_back(off);
		int sz = parse_layer_size(l);
		if (sz <= 0 || off + sz > nq_size) break;  // defensive
		off += sz;
	}
	int finish_off = off;  // first Finish の byte offset (or nq_size if no Finish)

	if (offsets.empty()) { printf("nq_filter: no layers parsed from %s\n", nq_path); return -1; }

	// ★旧 Tier1 dense32/n.q ミスマッチ validation は撤去 (2026-06-29)。kernel が per-layer に
	//   weight==DENSE32_WCOUNT で dense32/plain を自己判定 (Tier3) するため、dense32/plain どちらの
	//   n.q でも deadlock せず、host 側の build-flag 照合 abort は不要・有害 (plain n.q を誤 abort) に
	//   なった(旧 Tier1 の deadlock 事例に対する恒久策)。

	// 3. 範囲 clamp
	if (start_layer < 0) start_layer = 0;
	const int last = (int)offsets.size() - 1;
	if (end_layer > last) end_layer = last;
	if (end_layer < start_layer) end_layer = start_layer;

	// 4. byte range 計算
	int range_begin = offsets[start_layer];
	int range_end;
	if (end_layer + 1 < (int)offsets.size()) {
		range_end = offsets[end_layer + 1];
	} else {
		range_end = finish_off;
	}
	int range_bytes = range_end - range_begin;

	// 5. param へ memcpy
	memcpy(param, buf.data() + range_begin, range_bytes);

	// 6. Finish[8] を末尾に追加 (manager の rp+8<=wp 制約のため 8 個)
	const int FINISH_BLOCK = 8;
	for (int k = 0; k < FINISH_BLOCK; k++) {
		Layer *fl = (Layer*)((uint8_t*)param + range_bytes + k * sizeof(Layer));
		memset(fl, 0, sizeof(Layer));
		fl->type = LayerType::Finish;
		fl->id   = end_layer + 1 + k;
	}
	int filtered_bytes = range_bytes + FINISH_BLOCK * (int)sizeof(Layer);

	printf("nq_filter: %s [%d layers in src] → L%d..L%d (%d byte) + Finish[8] = %d byte total\n",
	       nq_path, (int)offsets.size(), start_layer, end_layer, range_bytes, filtered_bytes);
	return filtered_bytes;
}

// ============================================================================
//  live streaming 本体(旧 src/tasks.cpp の live 経路)
// ============================================================================
static inline uint32_t rt_out_base(){ const char* e = getenv("OUT_BASE_RT");
  return e ? (uint32_t)atol(e) : (g_nq_config.out_base   > 0 ? (uint32_t)g_nq_config.out_base   : (uint32_t)BATCH_OUT_BASE); }
static inline uint32_t rt_out_stride(){ const char* e = getenv("OUT_STRIDE_RT");
  return e ? (uint32_t)atol(e) : (g_nq_config.out_stride > 0 ? (uint32_t)g_nq_config.out_stride : (uint32_t)BATCH_OUT_STRIDE); }
// ── 入力側の runtime 化 (rt_in_base / rt_in_stride)
static inline uint32_t rt_in_base(){ const char* e = getenv("IN_BASE_RT");
  return e ? (uint32_t)atol(e) : (g_nq_config.in_base   > 0 ? (uint32_t)g_nq_config.in_base   : (uint32_t)BATCH_IN_BASE); }
static inline uint32_t rt_in_stride(){ const char* e = getenv("IN_STRIDE_RT");
  return e ? (uint32_t)atol(e) : (g_nq_config.in_stride > 0 ? (uint32_t)g_nq_config.in_stride : (uint32_t)BATCH_IN_STRIDE); }
// ── 実効 N の優先順 (rt_nimg_eff)
static inline uint32_t rt_nimg_eff(){
  if(const char* e = getenv("PERSIST_NIMG")){    int v = atoi(e); if(v > 0) return (uint32_t)v; }
  if(const char* e = getenv("STREAM_NIMG_RT")){  int v = atoi(e); if(v > 0) return (uint32_t)v; }
  if(g_nq_stream_nimg > 0) return (uint32_t)g_nq_stream_nimg;
  return (uint32_t)STREAM_NIMG;
}
// ★geo640 H4(2026-08-28): 入力ロードの scratch 語数は include/y26_decode.h の Y26_IN_SCRATCH_WORDS
//   (= 3*NETH*NETW*4B/64B、384×640 → 46080 = 旧直書き値)に一本化。chw_load の負値(open 失敗 / 無し /
//   サイズ不一致 = 旧幾何の x.bin)は入力破損なので **黙って続行せず abort**(旧: 戻り値未検査)。
//   ★abort は **サイズ不一致(-3)= 幾何の設定ミス** に限定。open 失敗/無し(-1/-2)は従来どおり警告のみで続行
//   (live refill で一時的にファイルが無い瞬間に yololoop を落とさない = 現行値での挙動不変)。
static int chw_load_or_die(const char* fn, void* dst){
  int rc = chw_load(fn, dst);
  if(rc == -3){ printf("[chw_load] ★★致命: 入力サイズ不一致 rc=%d (%s) = 幾何ミス → abort\n", rc, fn); fflush(stdout); exit(3); }
  if(rc < 0)  { printf("[chw_load] ⚠ 入力ロード失敗 rc=%d (%s) → 従来どおり続行\n", rc, fn); fflush(stdout); }
  return rc;
}
// ★req8(2026-08-29): 入力 scratch の memset 撤去(640 live で refill 175ms/img の一因 = 4.9MB memset/枚)。
//   根拠: chw_load(float 経路・hwc16 経路とも)は [NETH][NETW][4] int16 = Y26_HWC_BYTES を **全要素** 書く
//   (c=0..2 は値、c=3 は 0、最後に EOF quirk で o[0][0][3] を上書き)。slot へ memcpy するのは先頭 in_bytes
//   (= rt_in_stride()×64B = Y26_IN_STRIDE = NETH*NETW*4*2/64 語 → HWC_BYTES と同値)なので、memcpy が
//   chw_load の未書込領域を読むことは無い = 事前 memset は不要。従来と byte-exact を保つ保険として
//   (1) runtime in_stride が幾何より大きい(in_bytes > HWC_BYTES)ときは差分 [HWC_BYTES, in_bytes) だけ 0 埋め、
//   (2) ロード失敗(rc<0: open 失敗/無し)時は旧挙動(memset 済 scratch = 全 0 入力)を再現するため全 0 にする。
//   戻り = 0 埋めした bytes(PHASE3_PROF の _r1 内訳用)。
static constexpr size_t Y26_HWC_BYTES     = (size_t)(Y26_NETH)*(size_t)(Y26_NETW)*4u*sizeof(int16_t);
static constexpr size_t Y26_SCRATCH_BYTES = (size_t)Y26_IN_SCRATCH_WORDS*sizeof(GMEM_T);
static_assert(Y26_SCRATCH_BYTES >= Y26_HWC_BYTES, "req8: scratch must hold HWC int16");
static size_t scratch_zero_guard(void* scr, size_t in_bytes, int load_rc){
  if(load_rc < 0){ memset(scr, 0, Y26_SCRATCH_BYTES); return Y26_SCRATCH_BYTES; }
  if(in_bytes > Y26_HWC_BYTES){
    size_t end = in_bytes < Y26_SCRATCH_BYTES ? in_bytes : Y26_SCRATCH_BYTES;
    memset((char*)scr + Y26_HWC_BYTES, 0, end - Y26_HWC_BYTES); return end - Y26_HWC_BYTES;
  }
  return 0;
}


// → 旧 tasks.cpp §live-04 「FILELIST — 入出力パスの一括指定」(配信では入力 = ppdaemon が publish する live.bin)
static std::vector<std::pair<std::string,std::string>> g_filelist;
static bool g_filelist_loaded = false;
static void load_filelist_once(){
  if(g_filelist_loaded) return; g_filelist_loaded = true;
  const char* flp = getenv("FILELIST"); if(!flp || !flp[0]) return;
  FILE* flf = fopen(flp, "r");
  if(!flf){ printf("[FILELIST] ✗ 開けません: %s (固定名にフォールバック)\n", flp); return; }
  char lb[2048];
  while(fgets(lb, sizeof lb, flf)){
    std::string s(lb);
    size_t a = s.find_first_not_of(" \t\r\n"); if(a==std::string::npos) continue;
    size_t b = s.find_last_not_of(" \t\r\n"); s = s.substr(a, b-a+1);
    if(s.empty() || s[0]=='#') continue;
    size_t sp = s.find_first_of(" \t"); std::string in, out;
    if(sp==std::string::npos){ in = s; out = s + ".out"; }
    else { in = s.substr(0, sp); size_t c = s.find_first_not_of(" \t", sp);
           out = (c==std::string::npos) ? (in + ".out") : s.substr(c); }
    g_filelist.push_back(std::make_pair(in, out));
  }
  fclose(flf);
  printf("[FILELIST] %s: %zu 行を起動時メモリ格納\n", flp, g_filelist.size());
}
// 入力パス: k 行目があれば filelist、無ければ fallback(既存 x_k.bin 等)。
static std::string fl_in(uint32_t k, const std::string& fallback){
  return (k < g_filelist.size()) ? g_filelist[k].first : fallback;
}


int main(int argc, char **argv){
  // host watchdog で hang 時も部分ダンプを確実に拾うため stdout を unbuffered 化。
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("[host] y26 live 配信 host  build %s %s  geo=%ux%u  STREAM_NIMG=%d  DATA=%dMB\n",
         __DATE__, __TIME__, (unsigned)Y26_NETH, (unsigned)Y26_NETW, (int)STREAM_NIMG, (int)GMEM_DATA64_SIZE_MB);

  // → 旧 §args-02/03 「argv[1]=DATA_DIR / argv[2],[3]=start_layer,end_layer」
  std::string path(argc > 1 ? argv[1] : "data");
  int start_layer = (argc > 2) ? atoi(argv[2]) : 0;
  int end_layer   = (argc > 3) ? atoi(argv[3]) : 1;
  printf("layer range from argv: start_layer=%d end_layer=%d\n", start_layer, end_layer);
  if(start_layer != 0){
    printf("[host] ★★致命: 配信専用 host は start_layer=0 のみ(mid-chain seed 経路は削除済)→ abort\n"); return 2; }
  load_filelist_once();
  if(g_filelist.empty()){
    printf("[host] ★★致命: FILELIST が無い/空。配信 host の入力は外部 ppdaemon が publish する\n"
           "        live.bin(FILELIST の各行)だけ。in-host camera capture 経路は削除済 → abort\n"); return 2; }

  printf("load(binary_container_1)\n");
  ClHost ho;
  int64_t *param, *data;
  ho.load("/lib/firmware/xilinx/yolov7/binary_container_1.bin", GMEM_PARAM_SIZE * sizeof(GMEM_T),
          GMEM_DATA_SIZE * sizeof(GMEM_T), param, data);

  // runtime n.q filter: argv で指定された [start_layer..end_layer] を抽出 + Finish[8] 付与
  int nq_bytes = nq_filter_to_param((path + "/n.q").c_str(), param, start_layer, end_layer);
  if (nq_bytes < 0) {   // n.q 不在等 → kernel を走らせず clean exit (board hang 回避)
    printf("nq_filter_to_param 失敗 (nq_bytes=%d) → 中断。\n", nq_bytes);
    return -1;
  }
  ho.param_words = (uint32_t)(nq_bytes / (int)sizeof(GMEM_T));

  // ★HW 真 runtime N (STREAM_NIMG_RUNTIME): kernel arg6 へ渡す N。優先順 = env STREAM_NIMG_RT > n.q
  //   configure layer(g_nq_stream_nimg) > 0(=kernel が compile-time STREAM_NIMG へ fallback)。
  {
    const char *e = getenv("STREAM_NIMG_RT");
    int n = e ? atoi(e) : (g_nq_stream_nimg > 0 ? g_nq_stream_nimg : 0);
    // ★GOAXIS runtime toggle: arg6 bit31 へ goaxis_en を相乗り (bit[6:0]=N)。
    //   優先順 = env GOAXIS_EN > n.q configure layer(g_nq_config.goaxis_en)。0=free-run(既定)。
    const char *ge = getenv("GOAXIS_EN");
    uint32_t goaxis_en = ge ? (atoi(ge) ? 1u : 0u) : (g_nq_config.goaxis_en & 1u);
    // ★ping-pong runtime toggle: arg6 bit30 へ pingpong_en を相乗り。優先順 = env PINGPONG_EN >
    //   n.q configure layer(g_nq_config.pingpong_en)。0=linear(既定, 旧 n.q 後方互換)。
    const char *pe = getenv("PINGPONG_EN");
    uint32_t pingpong_en = pe ? (atoi(pe) ? 1u : 0u) : (g_nq_config.pingpong_en & 1u);
    uint32_t nbits = (n > 0) ? ((uint32_t)n & 0x7Fu) : 0u;
    // ── IN_BASE runtime を arg6 の空きビットへ pack
    uint32_t _ib = rt_in_base();
    if (_ib & 63u) { printf("[batch] ★★致命: IN_BASE は 64-word(4KB) 整列必須 (in_base=%u) → abort\n", _ib); exit(2); }
    if ((_ib >> 6) > 0x3FFFu) { printf("[batch] ★★致命: IN_BASE が 14bit(4KB単位=最大 1,048,512 word)を超過 (in_base=%u) → abort\n", _ib); exit(2); }
    uint32_t ib_u = (_ib >> 6) & 0x3FFFu;   // ★14b: Layer.f(short @+12)の空き bit98..111 のみ使う
    ho.stream_nimg_rt = nbits | (ib_u << 7) | (goaxis_en << 31) | (pingpong_en << 30);
    // ── csim(native) は g_nq_config を直読する
    g_nq_config.in_base     = (int32_t)_ib;   // ★csim 経路(top の !__SYNTHESIS__ 分岐)にも in_base を供給
    g_nq_config.goaxis_en   = goaxis_en;
    g_nq_config.pingpong_en = pingpong_en;
    if (n > 0) g_nq_stream_nimg = n;
    printf("stream_nimg_rt(arg6) = 0x%08x (N=%u goaxis_en=%u pingpong_en=%u, env_n=%s nq_nimg=%d nq_goaxis=%u nq_pingpong=%u)\n",
           ho.stream_nimg_rt, nbits, goaxis_en, pingpong_en, e ? e : "(none)", g_nq_stream_nimg, g_nq_config.goaxis_en, g_nq_config.pingpong_en);
  }

  // → 旧 §load-01 「入力データのロード」(start_layer==0 固定)
  chw_load_or_die((path + "/x.bin").c_str(), data);

  // ── PSA Attention pe weight の region 別ロード
  for (int r = 0; r < PE_MAX_REGIONS; r++) {
    int16_t *pe = (int16_t *)(data + (int64_t)PE_REGION_OFF(r) * (sizeof(GMEM_T)/sizeof(int64_t)));
    char wn[256], bn[256];
    if (r == 0) { snprintf(wn, sizeof wn, "%s/w_psa_pe_weight.bin", path.c_str());
                  snprintf(bn, sizeof bn, "%s/w_psa_pe_bias.bin",   path.c_str()); }
    else        { snprintf(wn, sizeof wn, "%s/w_psa_pe_weight_%d.bin", path.c_str(), r);
                  snprintf(bn, sizeof bn, "%s/w_psa_pe_bias_%d.bin",   path.c_str(), r); }
    FILE *fw = fopen(wn, "rb");
    FILE *fb = fopen(bn, "rb");
    if (fw && fb) {
      float wbuf[128*9], bbuf[128];
      size_t nw = fread(wbuf, sizeof(float), 128*9, fw);
      size_t nb = fread(bbuf, sizeof(float), 128, fb);
      if (nw == 128*9 && nb == 128) {
        // int16 へは飽和で詰める(単純キャストは範囲外で wrap = 符号反転。2 つ目の PSA の 2 tap が |w|>=8 で該当)。
        int pe_sat = 0;
        auto pe_q = [&](float v) -> int16_t {
          long q = lrintf(v * PE_GMEM_SCALE);
          if (q > 32767) { q = 32767; pe_sat++; } else if (q < -32768) { q = -32768; pe_sat++; }
          return (int16_t)q; };
        for (int i = 0; i < 128;   i++) pe[i]       = pe_q(bbuf[i]);       // bias[128]
        for (int i = 0; i < 128*9; i++) pe[128 + i] = pe_q(wbuf[i]);       // weight[128*9]
        if (pe_sat) printf("[pe] warning: region %d: %d values out of int16 range at scale %d -> saturated\n", r, pe_sat, PE_GMEM_SCALE);
        printf("pe weight region %d → data[GMEM_T#%u] (%d int16 @scale %d)\n", r, (unsigned)PE_REGION_OFF(r), 128 + 128*9, PE_GMEM_SCALE);
      } else printf("pe weight region %d read short (nw=%zu nb=%zu)\n", r, nw, nb);
    } else printf("[pe] warning: region %d not loaded (%s / %s missing) -> PSA positional term is zero\n", r, wn, bn);
    if (fw) fclose(fw); if (fb) fclose(fb);
  }

  // → 旧 §batch-02 「N-image batch の入力 preload」(ping-pong = 2 slot)
  const int i64pw = (int)(sizeof(GMEM_T)/sizeof(int64_t));   // GMEM_T 1 word = 8 int64
  {
    for(int im=0; im<STREAM_NIMG && im<2; im++){
      uint32_t slot = (uint32_t)(im & 1);                    // ping-pong: slot0/slot1
      std::string xf = fl_in((uint32_t)im, path + "/x.bin"); // 配信では FILELIST 行(= live.bin)
      { FILE *tf = fopen(xf.c_str(), "rb"); if(tf){ fclose(tf); } else { xf = path + "/x.bin"; } }
      int64_t *ibase = data + (int64_t)(rt_in_base() + slot*rt_in_stride()) * i64pw;
      // → 旧 §batch-04 「partial load — IN_STRIDE 境界を跨がせない」
      chw_load_or_die(xf.c_str(), data);   // scratch = data[0..](run 前は free)
      memcpy(ibase, data, (size_t)rt_in_stride() * sizeof(GMEM_T));
    }
    // (2) 最終層 (Finish 直前) の l.out を求め予約 slot へ書込 + 他層との非重複(uniqueness)検査。
    GMEM_T *gp=(GMEM_T*)param; Layer dl=(Layer&)*param; uint32_t final_lout=0;
    while(dl.type != LayerType::Finish){
      final_lout=(uint32_t)dl.out;
      uint32_t adv=8; if(dl.type==LayerType::Conv) adv+=(uint32_t)dl.conv.bias+(uint32_t)dl.conv.weight;
      gp+=adv; dl=(Layer&)*gp;
    }
    { GMEM_T *q=(GMEM_T*)param; Layer ql=(Layer&)*param; int cnt=0;
      while(ql.type != LayerType::Finish){ if((uint32_t)ql.out==final_lout) cnt++;
        uint32_t adv=8; if(ql.type==LayerType::Conv) adv+=(uint32_t)ql.conv.bias+(uint32_t)ql.conv.weight; q+=adv; ql=(Layer&)*q; }
      if(cnt!=1) printf("[batch] ★警告: final l.out=%u を出力する層が %d 個 — redirect 誤爆リスク\n", final_lout, cnt);
    }
    { uint32_t* fos = (uint32_t*)(data + (int64_t)BATCH_FINAL_OUT_SLOT * i64pw);
      fos[0] = L_W(final_lout);   // ★kernel store は index=L_W(l.out)(word)と比較するので word で供給
      // 整理①: 出力レイアウトを同 slot lane1/2 へ、lane3=magic で valid 表明 (kernel store が採用)。
      fos[1] = rt_out_base();   fos[2] = rt_out_stride();   fos[3] = NQCFG_MAGIC; }
    printf("[batch] N=%d 入力ロード完了, final l.out=%u → data[slot %u] (in_base=%u in_stride=%u out_base=%u stride=%u)\n",
           (int)STREAM_NIMG, final_lout, (unsigned)BATCH_FINAL_OUT_SLOT,
           rt_in_base(), rt_in_stride(), rt_out_base(), rt_out_stride());
    // === footprint マップ: HW BATCH base 配置決定用 (peak 中間層 extent / 入出力サイズ) ===
    { GMEM_T *q=(GMEM_T*)param; Layer ql=(Layer&)*param;
      uint32_t maxext=0, maxout=0; long in_words=0, out_words=0; bool first=true;
      // ★#33b(2026-08-31): head pin を I/O slot 窓の上([out_base+3*out_stride, PE_GMEM_OFFSET))へ置く運用を
      //   許すため、CEIL の対象を「I/O 窓と交差する層」に精密化する。窓の上は hiext として別枠で検査。
      const uint32_t _iolo = rt_in_base(), _iohi = rt_out_base() + 3u*rt_out_stride();
      uint32_t ioext=0, hiext=0;
      while(ql.type != LayerType::Finish){
        // 各層の出力語数 (HWC int16, 32/word)。Conv/その他とも os 積で近似。
        long ow = ((long)ql.os[1]*ql.os[2]*ql.os[3] + 31)/32;
        uint32_t outw = L_W(ql.out);   // ★unit→word (PE_GMEM_OFFSET と同 word 単位で比較)
        uint32_t ext = outw + (uint32_t)ow;
        if(ext>maxext) maxext=ext;
        if(outw>maxout) maxout=outw;
        if(outw >= _iohi){ if(ext>hiext) hiext=ext; }
        else if(ext > _iolo){ if(ext>ioext) ioext=ext; }
        if(first){ in_words=((long)ql.is[1]*ql.is[2]*ql.is[3]+31)/32; first=false; }
        out_words = ow;   // 最終 Finish 直前層の出力語数
        uint32_t adv=8; if(ql.type==LayerType::Conv) adv+=(uint32_t)ql.conv.bias+(uint32_t)ql.conv.weight; q+=adv; ql=(Layer&)*q;
      }
      printf("[batch][MAP] 中間層 peak extent=%u word (max l.out=%u), 第1層入力=%ld word, 最終層出力=%ld word\n",
             maxext, maxout, in_words, out_words);
      // ── [CEIL] 層 peak assert — 入力域の追い越し検出
      { uint32_t _ib = rt_in_base(), _is = rt_in_stride(), _n = rt_nimg_eff();
        if(hiext){
          printf("[batch][CEIL] 高位配置(head pin)あり: 最大 extent=%u word は I/O 窓 [%u,%u) の上 = CEIL 対象外\n",
                 hiext, _iolo, _iohi);
          if(hiext > (uint32_t)PE_GMEM_OFFSET){
            printf("[batch][CEIL] ★★致命: 高位配置 %u > PE_GMEM_OFFSET=%u(PE 予約を踏む)→ abort\n",
                   hiext, (unsigned)PE_GMEM_OFFSET); exit(2); }
        }
        if(ioext > _ib){
          unsigned hit = _is ? (unsigned)((ioext - _ib + _is - 1) / _is) : 0u;   // 踏まれる入力 slot 数
          printf("[batch][CEIL] ✗ 層 peak=%u > in_base=%u = working ring が入力 slot を踏む"
                 " (侵入=%u word / slot 0..%u が危険, in_stride=%u)\n",
                 ioext, _ib, ioext - _ib, hit ? hit-1 : 0u, _is);
          if(_n >= 3){
            printf("[batch][CEIL] ★★致命: N=%u (>=3) の異画像は silent に破損する → abort"
                   " (n.q を再生成するか IN_BASE_RT で入力域を peak の上へ)\n", _n);
            exit(2);
          }
          printf("[batch][CEIL] ★警告: N=%u は slot パリティ上たまたま無傷だが n.q は不正。N>=3 で壊れる\n", _n);
        } else
          printf("[batch][CEIL] 層 peak=%u < in_base=%u OK (入力域を侵さない, N=%u)\n", ioext?ioext:maxext, _ib, _n);
      }
      printf("[batch][MAP] PE_GMEM_OFFSET=%u → 予約域下の gap=[%u, %u) = %u word\n",
             (unsigned)PE_GMEM_OFFSET, maxext, (unsigned)PE_GMEM_OFFSET, (unsigned)PE_GMEM_OFFSET - maxext);
      long need_per_img = in_words + out_words;
      if(need_per_img>0) printf("[batch][MAP] N目安: gap/(in+out)=%u/(%ld)= 最大 %ld 枚\n",
             (unsigned)PE_GMEM_OFFSET - maxext, need_per_img, ((long)(PE_GMEM_OFFSET)-maxext)/need_per_img);
    }
  }

  ho.start();

  // → 旧 §addr-05 「Phase 3 — ping-pong refill + go/done handshake」(OVERLAP 固定)
  {
    GMEM_T *gp=(GMEM_T*)param; Layer dl=(Layer&)*param; long fn=0;
    GMEM_T *outlayer_gp=gp;   // 設計C(l.out 直書き): 最終実層の header word
    while(dl.type!=LayerType::Finish){ fn=(long)dl.os[1]*dl.os[2]*dl.os[3];
      outlayer_gp=gp;
      uint32_t adv=8; if(dl.type==LayerType::Conv) adv+=(uint32_t)dl.conv.bias+(uint32_t)dl.conv.weight; gp+=adv; dl=(Layer&)*gp; }
    size_t outlayer_hdr_off_b = (size_t)((char*)outlayer_gp - (char*)param);
    (void)fn;
    // → 旧 §addr-07 是正(#35): l.out は uint16 の 4KB(64語)単位 = **>>6 必須**。
    auto set_lout=[&](uint32_t out_addr){
      if(out_addr & 63u){ printf("[batch] ★★致命: l.out は 64-word 整列必須 (out=%u) → abort\n", out_addr); exit(2); }
      if((out_addr>>6) > 0xFFFFu){ printf("[batch] ★★致命: l.out は uint16(4KB単位)= 上限 word %u (out=%u) → abort\n", 0xFFFFu<<6, out_addr); exit(2); }
      ((Layer*)outlayer_gp)->out = (uint16_t)(out_addr >> 6); };
    // ★req10 GO_FLUSH=cvac 固定(本番 geo640.env の設定)。host が書いた l.out レコードの cache line
    //   だけ `dc cvac`(EL0 可、PoC=DRAM まで clean)+ `dsb sy` してから XRT の range sync に渡す。
    //   旧既定の evict(16MB ダミー touch)は go 13〜14ms/img の正体で、本番では使っていない。
    auto go_flush=[&](size_t rec_off){
#if defined(__aarch64__)
      const char* p=(const char*)param+rec_off;
      for(size_t b=0;b<sizeof(GMEM_T);b+=64) __asm__ volatile("dc cvac, %0" :: "r"(p+b) : "memory");
      __asm__ volatile("dsb sy" ::: "memory");
#endif
      ho.sync_param_region(rec_off, sizeof(GMEM_T)); };
    printf("[phase3] outlayer=L%d hdr_off=%zu baked l.out=%u → l.out 直書きモード (GO_FLUSH=cvac 固定)\n",
           (int)((Layer*)outlayer_gp)->id, outlayer_hdr_off_b, (unsigned)((Layer*)outlayer_gp)->out);
    int64_t *scr = new int64_t[Y26_IN_SCRATCH_WORDS*i64pw];   // refill scratch (data[0..] は不可触)
    struct timespec _ts; auto now_ms=[&]()->double{ clock_gettime(CLOCK_MONOTONIC,&_ts);
      return (double)_ts.tv_sec*1e3 + (double)_ts.tv_nsec/1e6; };

    // → 旧 §head-01 「DUMP_HEADS_PF — per-frame head drain」(検出の入力 = 6 head)
    int pf_hid[16]; long pf_off[16], pf_n[16]; int pf_cnt = 0; int pf_pos[16];
    if (getenv("DUMP_HEADS_PF")) {
      Layer *lp_end2 = (Layer*)param + GMEM_PARAM_SIZE;
      std::string ids = getenv("DUMP_HEADS_PF"); size_t p2 = 0;
      while (p2 < ids.size() && pf_cnt < 16) {
        size_t cm = ids.find(',', p2);
        std::string tok = ids.substr(p2, cm==std::string::npos?std::string::npos:cm-p2);
        p2 = (cm==std::string::npos)?ids.size():cm+1; if (tok.empty()) continue;
        int hid = atoi(tok.c_str()); Layer *lp=(Layer*)param; bool found=false;
        for (int g=0; g<2000 && lp && lp<lp_end2; g++){ if((int)lp->id==hid){found=true;break;}
          uint32_t adv=8; if(lp->type==LayerType::Conv) adv+=(uint32_t)lp->conv.bias+(uint32_t)lp->conv.weight; lp=lp+adv; }
        if(!found){ printf("[phase3] DUMP_HEADS_PF: L%d param chain 無(skip)\n",hid); continue; }
        pf_pos[pf_cnt]=0; { Layer *lq=(Layer*)param; for(int g2=0; g2<2000 && lq && lq<lp_end2 && lq!=lp; g2++){ uint32_t a2=8; if(lq->type==LayerType::Conv) a2+=(uint32_t)lq->conv.bias+(uint32_t)lq->conv.weight; lq=lq+a2; pf_pos[pf_cnt]=g2+1; } }
        pf_hid[pf_cnt]=hid; pf_off[pf_cnt]=(long)L_W(lp->out); pf_n[pf_cnt]=(long)lp->os[1]*lp->os[2]*lp->os[3]; pf_cnt++;
      }
      printf("[phase3] DUMP_HEADS_PF: %d head 層を per-frame drain\n", pf_cnt);
    }
    if(pf_cnt==0){ printf("[host] ★★致命: DUMP_HEADS_PF が無い = 検出 head が取れない → abort\n"); return 2; }
    std::vector<int16_t> pf_snap[16];
    // ★SNAP_THREADS: head snapshot の memcpy(非 cache BO 読み ~200MB/s)を n thread で分担(本番 2)。
    int snap_threads = getenv("SNAP_THREADS") ? atoi(getenv("SNAP_THREADS")) : 1;
    if(snap_threads<1) snap_threads=1; if(snap_threads>8) snap_threads=8;
    // ★GO_EARLY=1: go(k+1) を drain(k) 直後(snap/overlay の前)に発行 = snapshot を PL と並走させる。
    //   安全条件(自動検査): 各 head の l.out 範囲が chain 上で**先行する層**の l.out と交差しないこと。
    //   交差する n.q では image k+1 の先行層が go 直後に head(k) を上書きする → 自動で無効化する。
    bool go_early = getenv("GO_EARLY") && atoi(getenv("GO_EARLY"))!=0;
    double go_early_margin_ms = getenv("GO_EARLY_MARGIN_MS") ? atof(getenv("GO_EARLY_MARGIN_MS")) : 60.0;
    if(go_early){
      Layer *lp=(Layer*)param, *lpe=(Layer*)param+GMEM_PARAM_SIZE;
      for(int g=0; g<2000 && lp && lp<lpe && lp->type!=LayerType::Finish; g++){
        long n=(long)lp->os[1]*lp->os[2]*lp->os[3];
        if(n>0){ long a=(long)L_W(lp->out), b=a+(n*2+63)/64;
          for(int h=0; h<pf_cnt; h++){ if(g>=pf_pos[h]) continue; long ha=pf_off[h], hb=ha+(pf_n[h]*2+63)/64;
            if(a<hb && ha<b && go_early){
              printf("[go-early] ✗ head L%d out[%ld,%ld) は先行 L%d(chain#%d) out[%ld,%ld) と交差 → GO_EARLY 無効(既定順序)\n",
                     pf_hid[h], ha, hb, (int)lp->id, g, a, b);
              go_early=false; } } }
        uint32_t adv=8; if(lp->type==LayerType::Conv) adv+=(uint32_t)lp->conv.bias+(uint32_t)lp->conv.weight; lp=lp+adv; }
      if(go_early) printf("[go-early] ✓ %d head は先行層と非交差 → go(k+1) を drain(k) 直後に発行(margin 監視 %.0fms)\n", pf_cnt, go_early_margin_ms);
    }
    printf("[phase3] gates: GO_FLUSH=cvac(固定) GO_EARLY=%d SNAP_THREADS=%d OVERLAY_DEFER=常時\n",
           (int)go_early, snap_threads);

    // head snapshot(overlay が読む複製)。overlay は go(k+1) の後に走るので working-DRAM 直読は不可。
    auto snap_heads=[&](){
      struct Job { char* d; const char* s; size_t n; } job[16]; size_t tot=0;
      for(int h=0; h<pf_cnt; h++){
        if((size_t)pf_n[h] > pf_snap[h].size()) pf_snap[h].resize((size_t)pf_n[h]);
        job[h].d=(char*)pf_snap[h].data(); job[h].s=(const char*)(data+(int64_t)pf_off[h]*i64pw);
        job[h].n=(size_t)pf_n[h]*sizeof(int16_t); tot+=job[h].n; }
      if(snap_threads<=1 || tot<(size_t)(1u<<16)){
        for(int h=0; h<pf_cnt; h++) memcpy(job[h].d, job[h].s, job[h].n);
      } else {
        // byte 範囲 [tot*t/T, tot*(t+1)/T) を thread t が担当(job 境界跨ぎ。出力 byte は順序非依存)
        auto worker=[&](size_t b0, size_t b1){ size_t base=0;
          for(int h=0; h<pf_cnt; h++){ size_t e=base+job[h].n, lo=b0>base?b0:base, hi=b1<e?b1:e;
            if(lo<hi) memcpy(job[h].d+(lo-base), job[h].s+(lo-base), hi-lo); base=e; } };
        std::vector<std::thread> th; size_t T=(size_t)snap_threads;
        for(size_t t=1; t<T; t++) th.emplace_back(worker, tot*t/T, tot*(t+1)/T);
        worker(0, tot/T);
        for(auto& x: th) x.join();
      } };
    // head は shared working DRAM → image k+1 解放前に drain(done〜go の無走行窓で安全)。
    auto drain_heads=[&](uint32_t k){
      for (int h=0; h<pf_cnt; h++){
        size_t boff=(size_t)pf_off[h]*sizeof(GMEM_T);
        size_t nb=((size_t)pf_n[h]*sizeof(int16_t)+sizeof(GMEM_T)-1)/sizeof(GMEM_T)*sizeof(GMEM_T);
        ho.drain_region(boff, nb);   // ★HW→host coherency drain(overlay の head 読みに必須)
        // ★DUMP_HEADS_NOFILE: live 配信では overlay が working-DRAM/snapshot を直読するので
        //   pf_L*.bin は純粋な副産物 → launcher は常に 1 を立てて file IPC を止める。
        //   ⚠ この file 書き出しは **golden canary(6 head の md5 突合)の唯一の出口**なので残す。
        //   出力名 = FILELIST の out 列 + "_L<id>.bin"(canary_golden.sh がこの名前で拾う)。
        if(getenv("DUMP_HEADS_NOFILE")) continue;
        char fb[64]; snprintf(fb,sizeof fb,"_L%d.bin", pf_hid[h]);
        std::string fnp = g_filelist[k < g_filelist.size() ? k : g_filelist.size()-1].second + fb;
        FILE*fp=fopen(fnp.c_str(),"wb");
        if(fp){ fwrite((const int16_t*)(data+(int64_t)pf_off[h]*i64pw), sizeof(int16_t), pf_n[h], fp); fclose(fp); }
      } };


    // → 旧 §head-03 「per-frame overlay」= 検出枠つき NV12 を atomic 差替(vcu_stream が拾う)
    auto overlay_frame=[&](uint32_t k){
      const char* ovnv = getenv("OVERLAY_NV12"); if(!ovnv) return;
      // ★OVERLAY_BG_PAIR: 背景を「最新 frame」でなく image k の推論入力に対応する退避 NV12 に差替
      //   (refill が bg ring へ書く。k<2 は今 run の退避が無い=前 run の残骸を掴まないよう最新で描く)
      char ov_bgp[64];
      if(getenv("OVERLAY_BG_PAIR") && k>=2){
        snprintf(ov_bgp,sizeof ov_bgp,"/tmp/lv/bg_%u.nv12",k&3u);
        struct stat ov_bst; if(stat(ov_bgp,&ov_bst)==0 && ov_bst.st_size>0) ovnv=ov_bgp;
      }
      const int16_t *bx[3]={0,0,0}, *cl[3]={0,0,0};
      for(int h=0; h<pf_cnt; h++){
        const int16_t* hp = pf_snap[h].data();
        for(int s=0;s<3;s++){ if(pf_hid[h]==y26::BOX_L[s]) bx[s]=hp; if(pf_hid[h]==y26::CLS_L[s]) cl[s]=hp; }
      }
      if(!(bx[0]&&bx[1]&&bx[2]&&cl[0]&&cl[1]&&cl[2])) return;
      // box scale の正は P3=2048 (2026-07-14 に 50 枚校正で確定)。
      float bsc[3]={2048,2048,1024}, csc[3]={512,128,128}, conf=0.25f;
      if(const char*e=getenv("BOX_SCALES")) sscanf(e,"%f,%f,%f",&bsc[0],&bsc[1],&bsc[2]);
      if(const char*e=getenv("CLS_SCALES")) sscanf(e,"%f,%f,%f",&csc[0],&csc[1],&csc[2]);
      if(const char*e=getenv("CONF")) conf=(float)atof(e);
      int Wf=getenv("OVERLAY_W")?atoi(getenv("OVERLAY_W")):1920;
      int Hf=getenv("OVERLAY_H")?atoi(getenv("OVERLAY_H")):1080;
      // → 旧 §head-08 「OVERLAY_WEB — 推論幾何 letterbox 配信 + FPS 表示」
      const bool ov_web = getenv("OVERLAY_WEB")!=nullptr;
      const int Wo = ov_web ? y26::NETW : Wf, Ho = ov_web ? y26::NETH : Hf;
      std::vector<y26::Det> dets=y26::decode(bx,cl,bsc,csc,conf);
      FILE* nf=fopen(ovnv,"rb"); if(!nf) return;
      fseek(nf,0,SEEK_END); long flen=ftell(nf); fseek(nf,0,SEEK_SET);
      int stride=getenv("OVERLAY_STRIDE")?atoi(getenv("OVERLAY_STRIDE")):(int)((long)flen*2/(3L*Hf));
      if(stride<Wf) stride=Wf;
      size_t insz=(size_t)stride*Hf*3/2, outsz=(size_t)Wo*Ho*3/2;
      std::vector<uint8_t> inb(insz), fbuf(outsz);
      // 表示モード: OVERLAY_MODE_FILE("wide"/"crop")を毎フレーム読む(Web UI からの動的切替。
      // env はプロセス起動時固定なので flag ファイル経由。読めない時は wide)
      bool ov_crop=false;
      if(ov_web){ if(const char* mf=getenv("OVERLAY_MODE_FILE")){
        char mb[8]={0}; FILE* mfp=fopen(mf,"rb");
        if(mfp){ size_t mn=fread(mb,1,7,mfp); fclose(mfp); ov_crop=(mn>=4 && !strncmp(mb,"crop",4)); } } }
      if(fread(inb.data(),1,insz,nf)==insz){
        if(ov_web && ov_crop) y26::overlay_crop(inb.data(),Wf,Hf,stride,dets,fbuf.data());
        else if(ov_web)       y26::overlay_letterbox(inb.data(),Wf,Hf,stride,dets,fbuf.data());
        else                  y26::overlay_destride(inb.data(),Wf,Hf,stride,dets,fbuf.data(),Wf,Hf);
        // 推論 FPS を右上に描画(OVERLAY_NOFPS=1 で抑止)。直近 16 frame の個数÷経過時間(窓平均)。
        enum { FP_RING=16 };
        static struct timespec fp_ring[FP_RING]; static int fp_cnt=0;
        struct timespec fp_now; clock_gettime(CLOCK_MONOTONIC,&fp_now);
        fp_ring[fp_cnt%FP_RING]=fp_now; fp_cnt++;
        float fp_fps=0.f;
        if(fp_cnt>=2){
          int n = fp_cnt<FP_RING ? fp_cnt : FP_RING;
          const struct timespec& fp_old = fp_ring[(fp_cnt-n)%FP_RING];
          double el=(fp_now.tv_sec-fp_old.tv_sec)+(fp_now.tv_nsec-fp_old.tv_nsec)*1e-9;
          if(el>1e-3) fp_fps=(float)((n-1)/el);
        }
        if(!getenv("OVERLAY_NOFPS")) y26::draw_fps(fbuf.data(),Wo,Ho,fp_fps);
        // NV12 を tmp へ書いて rename = reader(vcu_stream)は常に完結した frame を見る。
        std::string outp=getenv("OVERLAY_OUT")?std::string(getenv("OVERLAY_OUT")):(std::string(ovnv)+".ov");
        std::string tmp=outp+".tmp";
        FILE* of=fopen(tmp.c_str(),"wb");
        if(of){ fwrite(fbuf.data(),1,outsz,of); fclose(of); rename(tmp.c_str(),outp.c_str());
          printf("[phase3] image%u OVERLAY: %zu det → %s\n", k, dets.size(), outp.c_str()); }
      }
      fclose(nf);
    };

    // in-slot((k+1)&1)へ image k+2 を refill(TO_DEVICE)。image k+2 が同 slot を読む前に完了させる。
    auto refill=[&](uint32_t k){
      if(k+2 >= rt_nimg_eff()) return;
      if(k+2 >= g_filelist.size()) return;      // FILELIST 末尾を越えたら refill 不要
      std::string xf = g_filelist[k+2].first;
      const size_t _in_nb = (size_t)rt_in_stride()*sizeof(GMEM_T);
      int _lrc = chw_load_or_die(xf.c_str(), scr);
      scratch_zero_guard(scr, _in_nb, _lrc);
      uint32_t in_off = rt_in_base() + (k&1u)*rt_in_stride();
      memcpy(data+(int64_t)in_off*i64pw, scr, _in_nb);
      ho.sync_region((size_t)in_off*sizeof(GMEM_T), _in_nb);
      // ★OVERLAY_BG_PAIR: 今 load した入力(live.bin)に対応する camera NV12(ppdaemon が対で publish
      //   する f.nv12)を image k+2 用の背景 ring へ退避 → BOX と背景の ~300ms スキューが消える。
      //   hardlink でゼロコピー(4MB copy は +~20ms/frame = fps −2 の主因だった)。
      if(getenv("OVERLAY_BG_PAIR")){
        const char* bs=getenv("OVERLAY_BG_SRC"); if(!bs) bs="/tmp/lv/f.nv12";
        char bt[64],bp[64]; snprintf(bp,sizeof bp,"/tmp/lv/bg_%u.nv12",(k+2)&3u); snprintf(bt,sizeof bt,"%s.tmp",bp);
        unlink(bt);
        if(link(bs,bt)==0){ rename(bt,bp); }
        else {                                  // 別 fs 等で link 不可なら従来コピーへ fallback
          FILE* bf=fopen(bs,"rb");
          if(bf){
            static std::vector<uint8_t> bgb; fseek(bf,0,SEEK_END); long bn=ftell(bf); fseek(bf,0,SEEK_SET);
            if(bn>0){ if((size_t)bn>bgb.size()) bgb.resize((size_t)bn);
              if(fread(bgb.data(),1,(size_t)bn,bf)==(size_t)bn){
                FILE* bo=fopen(bt,"wb"); if(bo){ fwrite(bgb.data(),1,(size_t)bn,bo); fclose(bo); rename(bt,bp); } } }
            fclose(bf);
          }
        }
      } };

    int wait_to_ms = 60000; if(const char*e=getenv("WAIT_MS")){ int v=atoi(e); if(v>0) wait_to_ms=v; }
    auto wait_done=[&](uint32_t db0, uint32_t need)->bool{
      for(int t=0;t<wait_to_ms;t++){ if(ho.done_cnt()-db0 >= need) return true; usleep(1000); } return false; };

    uint32_t db0 = ho.done_cnt();
    uint32_t gb0 = ho.go_cnt();
    // → 旧 §run-01 「実効 N の優先順(host 側)」
    const uint32_t nimg_eff = getenv("PERSIST_NIMG") ? (uint32_t)atoi(getenv("PERSIST_NIMG"))
                            : ((ho.stream_nimg_rt & 0x7Fu) ? (ho.stream_nimg_rt & 0x7Fu) : (uint32_t)STREAM_NIMG);
    printf("[phase3] === stream N=%u mode=OVERLAP (ping-pong 2-slot + handshake + per-image drain) baseline done=%u go=%u ===\n",
           nimg_eff, db0, gb0);
    double t0 = now_ms(), tprev = t0;
    uint32_t persist_n = getenv("PERSIST_NIMG") ? (uint32_t)atoi(getenv("PERSIST_NIMG")) : 0u;
    // slot 規則(ping-pong): PINGPONG_EN(env)> n.q configure。
    auto slot_of=[&](uint32_t img)->uint32_t{
      uint32_t _pp = getenv("PINGPONG_EN") ? (atoi(getenv("PINGPONG_EN"))?1u:0u) : (g_nq_config.pingpong_en & 1u);
      return _pp ? (img & 1u) : img; };
    // go doorbell(値は任意)。persist 時のみ最終画像に bit31(last)を立てて kernel を終端させる。
    auto go_img=[&](uint32_t img, bool last){
      uint32_t _last = (persist_n && last) ? GO_LAST_BIT : 0u;
      ho.go_token((img+1) | _last); };
    // 設計C: image の出力層 l.out を host 供給アドレスへ直書き → cvac flush。go の直前に呼ぶ。
    auto set_lout_flush=[&](uint32_t img){
      set_lout(rt_out_base() + slot_of(img)*rt_out_stride());
      go_flush(outlayer_hdr_off_b); };

    // → 旧 §run-03 「OVERLAP — PS と PL の並列化」。prologue: image0 を解放。
    uint32_t nrun = nimg_eff;
    set_lout_flush(0);
    go_img(0, nrun == 1);
    for(uint32_t k=0; k<nrun; k++){
      bool ok = wait_done(db0, k+1);                      // image k 完了待ち
      double td = now_ms();
      drain_heads(k);                                     // head は image k+1 解放前に drain
      double t_go_issue = 0;
      auto issue_go=[&](){ if(k+1 >= nrun) return;
        set_lout_flush(k+1);
        go_img(k+1, k+2 >= nrun);
        t_go_issue = now_ms(); };
      if(go_early) issue_go();
      snap_heads();                                       // 窓内は snapshot memcpy のみ(数 ms)
      double t_sn = now_ms();
      if(!go_early) issue_go();
      overlay_frame(k);                                   // decode+overlay は PL(k+1) と並走
      if(go_early && k+1 < nrun){
        double after = t_sn - t_go_issue;                 // head(k) の最終読出し完了 − go(k+1) 発行
        if(after > go_early_margin_ms)
          printf("[go-early] ★margin 超過 k=%u head(k) 読出し完了 = go 後 %.1fms > %.0fms → image k+1 が head を上書きした可能性\n",
                 k, after, go_early_margin_ms);
      }
      refill(k);
      printf("[phase3] image%u: %s done(rel)=%u Δ=%.1fms\n",
             k, ok?"done":"TIMEOUT", ho.done_cnt()-db0, td-tprev); tprev=td;
    }
    double t1 = now_ms();
    delete[] scr;
    ho.run_wait(30000);
    printf("[phase3] ★TIMING N=%d: 総 wall=%.1fms (%.2fms/img)\n", (int)nimg_eff, t1-t0, (t1-t0)/(double)nimg_eff);
  }

  // → 旧 §probe-04 「BUILD ID の読み出しと照合」(xclbin と host の版一致を 1 行で残す)
  {
    const char *kb = (const char *)(data + BUILD_ID_OFFSET *(sizeof(GMEM_T)/sizeof(int64_t)));
    char b[BUILD_ID_BYTES+1]; memcpy(b, kb, BUILD_ID_BYTES); b[BUILD_ID_BYTES]=0;
    for(int i=0;i<BUILD_ID_BYTES;i++) if(b[i]<32||b[i]>126) b[i]='.';
    printf("kernel build_id: \"%s\" / host build_id: \"%s %s\"\n", b, __DATE__, __TIME__);
  }
  return 0;
}


// SPDX-License-Identifier: AGPL-3.0-or-later

#include "nq_defs.h"
#include "pt_reader.h"        // Phase B: .pt(BN融合) 重みを ONNX initializer と突合 (pt_verify)

#include <fcntl.h>
#include <cassert>          // HOIST_LAYER_DIV: rsv 焼込み時の幅ガード
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <functional>   // pt_gen_layers_verify: 再帰 emitBlock (std::function)

using namespace std;

// ■ NQ_NO_DENSE32 — dense32 packing を切る診断ゲート
static const bool g_no_dense32 = (getenv("NQ_NO_DENSE32") && atoi(getenv("NQ_NO_DENSE32")));

typedef struct onnxid_{
	enum {
		INPUT,OUTPUT,NODE,INITIALIZER
	} FieldNumber; // onnx::GraphProto::kInitializerFieldNumber, onnx::GraphProto::kNodeFieldNumber
	int index;
	float min,max;
} RANGE;

struct NN_descriptor{
	uint16_t w,h;
	int pos;     // Index of Layer GMEM_T buff[]
	uint16_t id; // ID in prosseccing
	std::vector<Layer *> layer;
	std::vector<int16_t> node_id;
};

typedef struct onnx_net_{
	string sIn,sOut;				// Name of node
	int iIn,iOut;					// Index of Layer
	vector<int>dim;
	vector<int64_t>i;
	vector<float>f;
}ONNX_NET;

static NqBumpPlanner<GMEM_T> buffers;
#if LIVENESS_ALLOC
// 案A relayout 用: Add/Concat の「追加入力」producer id を l.id (=nn.id) key で記録。
// 第1入力は l.f に入るので記録不要。relayout が lastUse 算出と in/i32 追従に使う。
static std::map<int, std::vector<int> > g_extra;
#endif

// ■ encode_conv_block の引数と責務
// ■ WEIGHT_FP8_SIM — 8bit 化の精度だけを board 無しで測る
static int weight_fp8_sim(){
	static const int m = [](){
		const char* e = getenv("WEIGHT_FP8_SIM");
		int v = e ? atoi(e) : 0;
		if(v) printf("\n[WEIGHT_FP8_SIM] ★重み量子化シム 有効: 仮数 %d bit + 指数 3bit (M=5 が現 class FP8)。"
		             "n.q は int16 格納のまま値のみ量子化 = kernel 無改修で精度影響を測る。\n", v);
		return v;
	}();
	return m;
}
// ■ WEIGHT_FP8_SCOPE — 量子化を当てる層を絞る
enum WScope { WS_ALL = 0, WS_K3, WS_K1 };
static WScope weight_fp8_scope(){
	static const WScope s = [](){
		const char* e = getenv("WEIGHT_FP8_SCOPE");
		std::string v = e ? e : "all";
		WScope r = (v == "k3") ? WS_K3 : (v == "k1") ? WS_K1 : WS_ALL;
		if(e && e[0]) printf("[WEIGHT_FP8_SCOPE] 適用範囲 = %s\n",
			r == WS_K3 ? "k3 (3×3 conv のみ)" : r == WS_K1 ? "k1 (1×1 conv のみ)" : "all (全 conv)");
		return r;
	}();
	return s;
}
// 当該層 (kernel サイズ k) を量子化対象とするか。
static inline bool w_scope_hit(int kernel){
	switch(weight_fp8_scope()){
		case WS_K3: return kernel == 3;
		case WS_K1: return kernel == 1;
		default:    return true;
	}
}
// ■ w_quant_roundtrip — 仮数 M bit + 指数 3bit の往復
static inline int16_t w_quant_roundtrip(int16_t in, int M){
	int16_t t = in; int e;
	for(e = 0; e < 7; e++){                       // class FP8::set と同一の正規化ループ
		bool s15 = (t & (int16_t)0x8000) != 0, s14 = (t & (int16_t)0x4000) != 0;
		if(s15 != s14) break;
		t = (int16_t)(t << 1);
	}
	const int sh = 16 - M;                        // M=5 → sh=11 (= S16.r が bits[15:11] なのと一致)
	int16_t r   = (int16_t)(t >> sh);             // 上位 M bit を符号付仮数として抽出
	int16_t rec = (int16_t)((r << sh) + (1 << (sh - 1)));  // bucket 中心 (M=5 で +1024 = FP8 と同一)
	return (int16_t)(rec >> e);                   // class FP8::get と同一の再構成
}
// 平線形 N bit 量子化 (指数なし = 層スケール weight_b のみ)。M<0 のとき |M| bit として使う。
//   9tap×8bit = 72bit ちょうど = URAM 1word に収まる唯一の「指数なし」案 (= 素の int8) の評価用。
static inline int16_t w_lin_roundtrip(int16_t in, int N){
	const int sh = 16 - N;
	int32_t q = ((int32_t)in + (1 << (sh - 1))) >> sh;          // 最近傍丸め
	const int32_t hi = (1 << (N - 1)) - 1, lo = -(1 << (N - 1));
	if(q > hi) q = hi; if(q < lo) q = lo;                        // 飽和
	return (int16_t)(q << sh);
}
static inline int16_t w_fp8_roundtrip(int16_t zoom16, int kernel){
	const int M = weight_fp8_sim();
	if(!M) return zoom16;
	if(!w_scope_hit(kernel)) return zoom16;                     // 範囲外の層は素通し (int16 のまま)
	if(M < 0) return w_lin_roundtrip(zoom16, -M);               // 指数なし平線形 (素の int8 等)
	// ★自己検証: M=5 は class FP8 と bit 完全一致でなければならない (一般化の正しさの担保)。
	static bool checked = false;
	if(!checked){
		checked = true;
		for(int v = -32768; v <= 32767; v++){
			FP8 t8; t8.set((int16_t)v);
			if(t8.get() != w_quant_roundtrip((int16_t)v, 5)){
				printf("\n[WEIGHT_FP8_SIM] FATAL: M=5 一般化が class FP8 と不一致 (in=%d: FP8=%d gen=%d)\n",
				       v, (int)t8.get(), (int)w_quant_roundtrip((int16_t)v, 5));
				exit(1);
			}
		}
		printf("[WEIGHT_FP8_SIM] 自己検証 OK: M=5 は class FP8 と全 65536 値で bit 一致\n");
	}
	return w_quant_roundtrip(zoom16, M);
}

// ■ WEIGHT_K3_INT8 — 3×3 の重みを int8 で焼く (CONV_J8 の鍵)
static bool weight_k3_int8(){
	static const bool en = [](){
		const char* e = getenv("WEIGHT_K3_INT8");
		const bool v = e ? (atoi(e) != 0) : true;   // ★未設定 = ON(本番既定)
		if(v) printf("\n[WEIGHT_K3_INT8] ★3×3 conv を int8 で焼く (9tap=72bit=URAM 1word)。"
		             "kernel 側は -DCONV_J8_PROBE 相当の int8 読みが必須。\n");
		else  printf("\n[WEIGHT_K3_INT8] OFF (WEIGHT_K3_INT8=0) = 全層 int16。旧既定と n.q byte 完全一致。\n");
		return v;
	}();
	return en;
}
// ■ ★既定 SKIP は box 回帰 stem 6 層
#define K3I8_DEFAULT_SKIP "129,130,138,139,147,148"
// ■ parse_layer_id_set — int8 対象層の指定と既定の勝ち負け
static bool parse_layer_id_set(const char* s, std::vector<char>& mask, int& maxid){
	// mask[id]=1 を立てる。id 上限は動的に伸ばす。s が空/NULL なら false (=指定なし)。
	if(!s || !*s) return false;
	const char* p = s;
	while(*p){
		while(*p == ',' || *p == ' ' || *p == '\t') p++;
		if(!*p) break;
		char* e = nullptr;
		long a = strtol(p, &e, 10);
		if(e == p){ printf("\n[WEIGHT_K3_INT8] FATAL: 層リストが解釈できない: \"%s\"\n", s); exit(1); }
		long b = a;
		p = e;
		if(*p == '-'){
			p++;
			b = strtol(p, &e, 10);
			if(e == p){ printf("\n[WEIGHT_K3_INT8] FATAL: 範囲の終端が無い: \"%s\"\n", s); exit(1); }
			p = e;
		}
		if(a < 0 || b < a){ printf("\n[WEIGHT_K3_INT8] FATAL: 層範囲が不正: %ld-%ld\n", a, b); exit(1); }
		if((int)b > maxid) maxid = (int)b;
		if((size_t)b >= mask.size()) mask.resize((size_t)b + 1, 0);
		for(long i = a; i <= b; i++) mask[(size_t)i] = 1;
	}
	return true;
}
// ★Layer::id は uint8_t (tasks.h:1567)。実層 id は 0..~160 なので 255 を合成 Layer 用 sentinel に使う。
static const int K3I8_SYNTH_ID = 0xFF;
// 層選択ゲート。★親スイッチ ON かつ layout 適格な層に対してのみ問われる。
static bool k3_int8_id_selected(int id){
	struct Sel {
		bool has_inc = false, has_skip = false;
		std::vector<char> inc, skip;
		Sel(){
			int mx = -1;
			const char* sk = getenv("WEIGHT_K3_INT8_SKIP");
			const char* src;
			if(!sk)                     { sk = K3I8_DEFAULT_SKIP; src = "本番既定(box 回帰 stem)"; }
			else if(!strcmp(sk,"none")) { sk = "";                src = "明示指定: 除外なし = int8 全適用"; }
			else                        {                          src = "明示指定"; }
			has_inc  = parse_layer_id_set(getenv("WEIGHT_K3_INT8_LAYERS"), inc,  mx);
			has_skip = parse_layer_id_set(sk, skip, mx);
			printf("\n[WEIGHT_K3_INT8] ★部分 int8: LAYERS=\"%s\" / SKIP=\"%s\" [%s]\n",
			       getenv("WEIGHT_K3_INT8_LAYERS") ? getenv("WEIGHT_K3_INT8_LAYERS") : "(全適格層)",
			       *sk ? sk : "(なし)", src);
		}
	};
	static const Sel sel;
	if(id < 0 || id == K3I8_SYNTH_ID) return false;   // 合成 Layer (encode_one_conv) は常に int16
	if(sel.has_skip && (size_t)id < sel.skip.size() && sel.skip[(size_t)id]) return false;
	if(sel.has_inc)  return (size_t)id < sel.inc.size() && sel.inc[(size_t)id];
	return true;
}
// ■ ★int8 適格性の唯一の判定関数
static bool weight_dense32_int8(){
	static const bool en = [](){
		const char* e = getenv("WEIGHT_K3_INT8_DENSE32");
		const bool v = e && atoi(e);
		if(v) printf("\n[WEIGHT_K3_INT8] ★dense32 1×1 も int8 で焼く(実験)。"
		             "kernel は step_dense32 の j8 対応が必須 = 無いと deadlock。\n");
		return v;
	}();
	return en;
}
// ■ WEIGHT_DW_INT8 — depthwise も int8 (実験)
static bool weight_dw_int8(){
	static const bool en = [](){
		const char* e = getenv("WEIGHT_DW_INT8");
		const bool v = e && atoi(e);
		if(v) printf("\n[WEIGHT_K3_INT8] ★depthwise 3×3 も int8 で焼く(実験)。"
		             "kernel は -DCONV_J8_MUX -DCONV_DW_J8 が必須 = 無いと誤値。\n");
		return v;
	}();
	return en;
}
// → #28 C-1(2026-08-31): oc 別 2bit スケール。kernel は -DCONV_PC_SHIFT が対(無いと誤値)。
static bool weight_k3_int8_pc(){
	static const bool en = [](){
		const char* e = getenv("WEIGHT_K3_INT8_PC");
		const bool v = e && atoi(e);
		if(v) printf("\n[WEIGHT_K3_INT8] ★PC: oc 別 2bit スケール s_oc(bias int16 下位 2bit で運搬、案 B)。"
		             "kernel は -DCONV_PC_SHIFT 必須 = 無いと誤値。\n");
		return v;
	}();
	return en;
}
//   ★is_identity = Split/linear の合成 one-hot 恒等 1×1(reserved2=1)。out[o]=in[offset+o] を
//     **bit-exact** に運ぶのが仕事なので量子化してはいけない。dense32 判定に該当してしまうため明示除外。
static bool k3_int8_eligible(int id, int kernel, int ic, bool is_tapfold, bool is_dense32, bool is_dw,
                             bool is_identity){
	if(!weight_k3_int8())                    return false;
	if(is_tapfold || is_identity)            return false;
	if(is_dw){
		// ★実験 WEIGHT_DW_INT8: depthwise 3×3 を 8 レーン対角 layout で int8 化(CONV_DW_J8 kernel と対)。
		if(!weight_dw_int8())                return false;
		if(kernel != 3 || (ic % 8) != 0)     return false;
		return k3_int8_id_selected(id);
	}
	if(is_dense32){
		// dense32(1×1)は super-block 64ch 単位。ic が 8 の倍数でないと j*8+k の詰めが崩れる。
		if(!weight_dense32_int8())           return false;
		if((ic % 8) != 0)                    return false;
		return k3_int8_id_selected(id);
	}
	if(kernel != 3)                          return false;
	if((ic % 8) != 0)                        return false;  // L0 の ic=3→4 等は bb 崩れ
	return k3_int8_id_selected(id);
}
// encode 側: 焼かれた w_type を読むだけ。★整合検査つき (壊れた n.q を無警告で吐かせない)。
static inline bool k3_int8_layer(const Layer &l, int ic, bool is_tapfold, bool is_dense32, bool is_dw){
	const bool from_field = weight_k3_int8() && (l.conv.w_type == 0);
	const bool eligible   = k3_int8_eligible(l.id, l.conv.kernel, ic, is_tapfold, is_dense32, is_dw,
	                                        l.conv.reserved2 != 0);
	if(from_field != eligible){
		printf("\n[WEIGHT_K3_INT8] FATAL: layer id=%d で w_type(%d→int8=%d) と適格判定(%d) が不一致。"
		       "\n  呼出側の w_type 焼込みと k3_int8_eligible() が食い違っている = weight 語数と実書込が"
		       "\n  ズレて n.q が **無警告で壊れる** (walk desync → timeout/segfault)。生成を中止する。\n",
		       (int)l.id, (int)l.conv.w_type, (int)from_field, (int)eligible);
		exit(1);
	}
	return from_field;
}

// ■ k3_int8_tally — どの層が実際に int8 になったかの集計
static void k3_int8_tally(int id, int kernel, int ic, int oc, int oh, int ow,
                          bool is_tapfold, bool is_dense32, bool is_dw, bool k3i8){
	if(!weight_k3_int8()) return;
	if(id == K3I8_SYNTH_ID) return;                       // 合成 Layer は集計外
	struct Tally {
		std::vector<int> i8, i16_ineligible, i16_deselected;
		// ★非適格の内訳(2026-07-20d 追加)。「次に 4→8 レーン化できる余地はどこか」を
		//   検討するには**理由別**でないと判断できないため、種別ごとに分けて出す。
		std::vector<int> ng_dw, ng_tapfold, ng_dense32, ng_icalign, ng_other;
		long long m8 = 0, m_dw = 0, m_tf = 0, m_d32 = 0, m_ica = 0, m_skip = 0;
		long long b8 = 0, b16 = 0;
		~Tally(){
			auto dump = [](const char* tag, const std::vector<int>& v){
				printf("  %s (%zu 層):", tag, v.size());
				for(size_t i=0;i<v.size();i++) printf("%s%d", i?",":" ", v[i]);
				printf("\n");
			};
			printf("\n[WEIGHT_K3_INT8] ★部分 int8 の適用結果\n");
			dump("int8 化した層          ", i8);
			dump("int16 のまま(層選択で除外)", i16_deselected);
			dump("int16 のまま(layout 非適格)", i16_ineligible);
			const long long tot = b8 + b16;
			printf("  重み総量: int8 域 %lld B + int16 域 %lld B = %lld B"
			       " (全 int16 換算 %lld B に対し %.1f%% 削減)\n",
			       b8, b16, tot, b16 + b8*2,
			       (b16 + b8*2) ? 100.0*(double)b8/(double)(b16 + b8*2) : 0.0);
			printf("  ★この一覧を A/B の記録に残すこと(n.q md5 からは層構成が読めない)\n");

			// ★MAC レーン利用率の内訳(2026-07-20d)。「次に 4→8 化できる余地」を判断する材料。
			//   ★depthwise の MAC は oc*k*k*oh*ow(入力 1ch/出力ch)。ic を掛けると桁が狂うので注意。
			const long long mtot = m8 + m_skip + m_dw + m_tf + m_d32 + m_ica;
			auto row = [&](const char* tag, long long m, const std::vector<int>& v, const char* note){
				if(!mtot) return;
				printf("  %-22s %3zu 層  MAC %7.1fM (%5.1f%%)  %s\n",
				       tag, v.size(), (double)m/1e6, 100.0*(double)m/(double)mtot, note);
			};
			printf("\n[WEIGHT_K3_INT8] ★MAC レーン利用率の内訳(全 conv MAC = %.0fM)\n", (double)mtot/1e6);
			row("★8 レーン(int8)", m8,    i8,          "= 4→8 化済");
			row("4 レーン: box stem", m_skip, i16_deselected, "= 精度のため意図的に int16");
			row("4 レーン: depthwise", m_dw, ng_dw,      "= 入力 1ch/出力ch。8ch 供給の前提が無い");
			row("4 レーン: 1x1 tapfold", m_tf, ng_tapfold, "= 9tap slot を入力ブロックに転用済");
			row("4 レーン: 1x1 dense32", m_d32, ng_dense32, "= 同上(32ch/super)");
			row("4 レーン: ic%8!=0", m_ica, ng_icalign,  "= L0 の 3→4 padding 経路");
			if(!ng_other.empty()) row("4 レーン: その他", 0, ng_other, "");
		}
	};
	static Tally t;
	const long long bytes = (long long)ic * oc * kernel * kernel;   // tap 数 (int8=1B/tap, int16=2B/tap)
	// ★MAC は depthwise だけ式が違う(入力 1ch/出力ch)。ic を掛けると実態の ic 倍に膨らむ。
	const long long mac = (long long)(is_dw ? 1 : ic) * oc * kernel * kernel * oh * ow;
	if(k3i8){ t.i8.push_back(id); t.b8 += bytes; t.m8 += mac; }
	else{
		t.b16 += bytes * 2;
		// 「層選択で外した」= layout 上は適格だが LAYERS/SKIP で落ちた層。切分けが評価の肝。
		const bool layout_ok = (kernel == 3) && !is_tapfold && !is_dense32 && !is_dw && (ic % 8) == 0;
		if(layout_ok){ t.i16_deselected.push_back(id); t.m_skip += mac; }
		else{
			t.i16_ineligible.push_back(id);
			if(is_dw)            { t.ng_dw.push_back(id);      t.m_dw  += mac; }
			else if(is_tapfold)  { t.ng_tapfold.push_back(id); t.m_tf  += mac; }
			else if(is_dense32)  { t.ng_dense32.push_back(id); t.m_d32 += mac; }
			else if(ic % 8)      { t.ng_icalign.push_back(id); t.m_ica += mac; }
			else                   t.ng_other.push_back(id);
		}
	}
}

static uint8_t* encode_conv_block(Layer &l, uint8_t* bias_dst, uint8_t* weight_dst,
                                  const float* raw_w, const float* raw_b,
                                  int oc, int ic, bool is_depthwise, bool is_tapfold, bool is_dense32=false){
	// ■ per-layer 述語の host 前計算 (tapfold / dense32 フラグ)
	l.flags &= ~(uint16_t)(TAPFOLD_LAYER_EN | DENSE32_LAYER_EN);
	if(is_tapfold) l.flags |= TAPFOLD_LAYER_EN;
	if(is_dense32) l.flags |= DENSE32_LAYER_EN;
	// ★WEIGHT_K3_INT8 (既定 OFF): 本層を int8 3×3 layout で焼くか。weight 書込ループ・nblk(rsv0)・
	//   weight 語数の 3 箇所が同じ判定を共有する必要があるため、関数先頭で 1 度だけ決める。
	const bool k3i8 = k3_int8_layer(l, ic, is_tapfold, is_dense32, is_depthwise);
	k3_int8_tally(l.id, l.conv.kernel, ic, (int)l.os[1], (int)l.os[2], (int)l.os[3],
	              is_tapfold, is_dense32, is_depthwise, k3i8);
	// ★#28 C-1: oc 別 2bit スケール s_oc(既定 OFF = 全 0 → 従来 byte 不変)。
	//   plain 3×3(非 dw/tapfold/dense32)の k3i8 層のみ >0 を許す。dw/dense32 int8 は s=0 で
	//   bias 下位 2bit の清浄化だけ行う(kernel の PC_SPLIT が w8 全層で下位 2bit を読むため)。
	std::vector<uint8_t> pc_s((size_t)oc, 0);
	const bool pc_on = k3i8 && weight_k3_int8_pc();
	if(pc_on && !is_depthwise && !is_tapfold && !is_dense32 && l.conv.kernel == 3){
		const int kk = l.conv.kernel * l.conv.kernel;
		for(int o = 0; o < oc; o++){
			float mx = 0.f;
			for(int t = 0; t < ic*kk; t++){ float a = fabsf(raw_w[(size_t)o*ic*kk + t]); if(a > mx) mx = a; }
			float m16 = mx / l.conv.weight_b;                       // 現行スケールでの max|zoom16|
			int s = 0;
			while(s < 3 && m16 * (float)(1 << (s+1)) <= 32000.f) s++;   // int16 収まり(丸め余裕 767)
			// bias 制約: |bias·2^(2+zoomin+wm+s)| ≤ 32000(下の bias 書込と同式)
			float bmag = fabsf(raw_b[o]) * (float)(1 << (2 + l.conv.zoomin + l.conv.weight_m));
			while(s > 0 && bmag * (float)(1 << s) > 32000.f) s--;
			pc_s[(size_t)o] = (uint8_t)s;
		}
		int hist[4] = {0,0,0,0};
		for(int o = 0; o < oc; o++) hist[pc_s[(size_t)o]]++;
		printf("  [PC] L%d s_oc hist = [%d,%d,%d,%d]\n", (int)l.id, hist[0], hist[1], hist[2], hist[3]);
	}
	// === bias 書込 ===
	uint8_t* wp = bias_dst;
	FP8 to8;
	const float * b = raw_b;
	PRINT_BIAS{ printf("\n  bias: "); }
	for(int j =0; j < oc; j++) {
		float tmp(*b++);
		float zoom(tmp*512 *(1 << l.conv.bias_m));
		int16_t zoom16(tmp *(1<<(2 +l.conv.zoomin  +l.conv.weight_m)));
		if(pc_on){
			// ★#28 C-1: bias を 2^s_oc 細スケールで焼き、下位 2bit に s_oc を格納(案 B)。
			//   丸めは 4 の倍数へ(下位 2bit を情報に使うため)。s=0 の層/oc も清浄化される。
			const int sj = (int)pc_s[(size_t)j];
			int32_t q = (int32_t)lrintf(tmp * (float)(1 << (2 + l.conv.zoomin + l.conv.weight_m)) * (float)(1 << sj) / 4.f) * 4;
			if(q >  32764) q =  32764;
			if(q < -32768) q = -32768;
			zoom16 = (int16_t)(q | sj);
		}
		// ■ ★bias region は全層 int16 (2B/oc) で焼く
		if(l.conv.w_type || k3i8){
			*(int16_t*)wp = zoom16;
			PRINT_BIAS{ printf("%04X ", zoom16&0x0FFFF);}
			wp+=sizeof(int16_t);
		}else{
			to8.set(zoom);
			*(int8_t*)wp = to8.data.bare;
			wp+=sizeof(int8_t);
		}
	}
	PRINT_BIAS{ printf("\n");}
	l.os[0] =0;

	// === weight 書込 ===
	int row(0),col(0);
	cout <<"weight floats=" <<(long)oc*ic*l.conv.kernel*l.conv.kernel
	     <<(is_depthwise ? " (depthwise packed: ic_eff=4 in writer)" : "") <<endl;
	auto *w = (const float(*)[ic][l.conv.kernel*l.conv.kernel])(raw_w);  // normal Conv: [oc][ic][k*k]
	// depthwise の weight 実体は [oc][1][k*k]。下の loop は j=0 のみ raw を読み、
	// j=1..3 は 0 を書くので、上のキャスト式は使わず w_dw[o][k] = raw_w[o*k*k + k] で読む。
	auto *w_dw = (const float(*)[l.conv.kernel*l.conv.kernel])(raw_w);   // depthwise: [oc][k*k]
	auto waight_top = wp = weight_dst;
	uint8_t *header;
	uint8_t jc;
	if(ic < 4){
		jc = ic;
	}else{
		jc = 4;
	}
	// ■ tap-fold 1×1 重みライタ
	if(is_tapfold || is_dense32){
		// ■ tapfold 36ch/super と dense32 32ch/super
		const int jn      = k3i8 ? 8 : 4;                       // j の本数
		int super_ch = (is_dense32 ? 8 : 9) * jn;               // dense32: 8*jn / tapfold: 9*jn
		int nsuper   = (ic + super_ch - 1) / super_ch;
		const int bbmod_t = 9*jn;                               // param 1 語ぶんの slot 数
		const int bbhdr_t = 8*jn;                               // header 送りの下限
		for(int super=0; super<nsuper; super++)
		for(int o=0; o<oc; o++)
		for(int j=0; j<jn; j++)
		for(int k=0; k<9; k++){
			int c = is_dense32 ? (super*super_ch + j*8 + k) : (super*super_ch + j*9 + k);  // input channel
			// dense32 は各 j の k=8 がゼロ tap。tapfold は全 9 slot を channel に使う。
			float zoom = ((is_dense32 ? (k < 8) : true) && c < ic) ? (raw_w[o*ic + c] / l.conv.weight_b) : 0.0f;
			int16_t zoom16(w_fp8_roundtrip((int16_t)zoom, l.conv.kernel));  // WEIGHT_FP8_SIM: 既定 OFF=素通し
			if(row == 0 && col == 0){ header = wp; wp += sizeof(GMEM_T); }  // 8出力ブロック毎に header 行予約
			int bb = j*9 + k;
#if defined(WEIGHT_TEST_PATTERN)
			zoom16 = (int16_t)(o*64 + c);   // int16 収まる (255*64+511=16831<32767)。out[o]=64*o*S0+S1 (o 線形) で weight 読み検証
#endif
			if(k3i8){
				int32_t q = ((int32_t)zoom16 + (1 << 7)) >> 8;  // 最近傍丸め (int16 → int8)
				if(q >  127) q =  127;
				if(q < -128) q = -128;
				if(bbhdr_t <= bb && bb < bbmod_t){ *(int8_t*)header = (int8_t)q; header += sizeof(int8_t); }
				else                             { *(int8_t*)wp     = (int8_t)q; wp     += sizeof(int8_t); }
			}else{
				if(bbhdr_t <= bb && bb < bbmod_t){ *(int16_t*)header = zoom16; header += sizeof(int16_t); }
				else                             { *(int16_t*)wp     = zoom16; wp     += sizeof(int16_t); }
			}
			col++;
			if(col >= (k3i8 ? 9*8 : GMEM_SHORTWIDTH*9/8)){ col=0; row++; if(row>=8) row=0; }
		}
	} else {
	// ■ depthwise は 1 i_block 分だけ書く
	int ic_for_write = is_depthwise ? (k3i8 ? 8 : 4) : ic;
	// ★WEIGHT_K3_INT8: int8 3×3 は param 1 語 = 8j × 9B なので i を 8 刻み・j を 0..7 で回す。
	//   既定 (int16) は従来どおり i+=4 / j<4 = 展開結果が旧コードと同一 (byte 不変)。
	const int  jstep = k3i8 ? 8 : 4;                 // i の刻み
	const int  jcnt  = k3i8 ? 8 : (int)jc;           // j の本数
	const int  bbmod = k3i8 ? (9*8) : (9*4);         // bb の法 (param 1 語ぶんの tap 数)
	const int  bbhdr = k3i8 ? (8*8) : (8*4);         // header 送りの下限 (main = GMEM 1 語ぶん)
	for(int i =0; i < ic_for_write; i+=jstep)
	for(int o =0; o < oc; o++)
	for(int j =0; j < jcnt; j++)
	for(int k =0; k < l.conv.kernel*l.conv.kernel; k++) {
		float zoom;
		if (is_depthwise) {
			// ■ depthwise の対角配置
			const int _jm = k3i8 ? 7 : 3;
			zoom = ((j & _jm) == (o & _jm)) ? (w_dw[o][k] / l.conv.weight_b) : 0.0f;
		} else {
			zoom = w[o][i+j][k] / l.conv.weight_b;
		}
		if(pc_on) zoom *= (float)(1 << pc_s[(size_t)o]);   // ★#28 C-1: oc 別 2^s_oc(dw は s=0)
	  	int16_t zoom16(w_fp8_roundtrip((int16_t)zoom, l.conv.kernel));  // WEIGHT_FP8_SIM: 既定 OFF=素通し

	  	if(l.conv.kernel ==3 && row == 0 && col == 0){
			header = wp; 								// 9列目の格納場所
			wp += sizeof(GMEM_T);
	  	}

		// a/b は GMEM 内バイト位置の判定 (header 振り分け用)。depthwise では ic_for_write を採用する。
		auto a = (o*ic_for_write +(i+j))*9 +k;
		auto bb = a%bbmod;
	  	if(k3i8){
			// ■ int8 3×3 は 9tap×1B = URAM 1 列
			int32_t q = ((int32_t)zoom16 + (1 << 7)) >> 8;      // 最近傍丸め (int16 → int8)
			if(q >  127) q =  127;
			if(q < -128) q = -128;
			if(l.conv.kernel == 3 && bbhdr <= bb && bb < bbmod){
				*(int8_t*)header = (int8_t)q;
				header += sizeof(int8_t);
			}else{
				*(int8_t*)wp = (int8_t)q;
				wp += sizeof(int8_t);
			}
	  	}else if(l.conv.w_type){
#if defined(WEIGHT_TEST_PATTERN)
		*(int16_t*)wp = o*256 +(i+j)*16 +k; //テストパターン
#else
			if(l.conv.kernel == 3 && ic !=3 && bbhdr <= bb && bb < bbmod){
				*(int16_t*)header = zoom16;
				header+=sizeof(int16_t);
			}else{
				*(int16_t*)wp = zoom16;
				wp+=sizeof(int16_t);
			}
#endif
	  	}else{
			to8.set(zoom);
			*(int8_t*)wp = to8.data.bare;
			wp+=sizeof(int8_t);
		}
		col++;

		if(ic ==3 && ((a%(9*3))==(9*3-1))){ 	  // 3ch ->4ch padding
			for(int k=0;k<9;k++){
				if(col < GMEM_SHORTWIDTH){ // 次の行
					*(int16_t*)wp = 0;
					wp+=sizeof(int16_t);
				}else{
					*(int16_t*)header = 0;
					header+=sizeof(int16_t);
				}
				col++;
			}
		}
		// ■ param 1 語ぶんでの折返し
		if(col >= (k3i8 ? 9*8 : GMEM_SHORTWIDTH*9/8)){
			col=0;
			row++;
			if(row >= 8){
				row=0;
			}
		}
	}
	}  // end else (!is_tapfold)
	if(ic == 3){ 	  // 3ch ->4ch padding
		ic = l.is[1] = l.p[1] = 4;
	}
	// depthwise は ic_eff=4 分しか書いていないので l.conv.weight も縮減 (manager の next pointer 計算が依存)。
	if(is_tapfold || is_dense32){
		// ■ K3 バイト形式 (header 1 + main 8 = 9 行)
		const int jn_w  = k3i8 ? 8 : 4;
		int super_ch = (is_dense32 ? 8 : 9) * jn_w;
		int nsuper = (ic + super_ch - 1) / super_ch;
		l.conv.weight = 9 * nsuper * (l.os[1] >> 3);
	}else{
	// ★WEIGHT_DW_INT8: dw int8 は ic_eff=8×1B = int16 の 4×2B と同語数 (weight 式は *(1+w_type) で半減)。
	int ic_effective_for_weight = is_depthwise ? (k3i8 ? 8 : 4) : l.is[1];
	l.conv.weight = ic_effective_for_weight*l.os[1]*l.conv.k_size*l.conv.k_size*(1 +l.conv.w_type)/sizeof(GMEM_T);
	}
	// ■ Tier1 除算の host 前計算 (HOIST_LAYER_DIV)
	{
		int hic = (int)l.is[1];                                         // kernel の ic (=l.is[1], 3→4 pad 済)
		uint32_t w89 = (uint32_t)l.conv.weight * 8u / 9u;               // #1 manager uRam next chain (÷9)
		assert(w89 <= 0xFFFFu && "HOIST_LAYER_DIV: weight*8/9 が uint16 超過 → rsv1 拡幅要");
		l.rsv1 = (uint16_t)w89;
		// ■ WEIGHT_K3_INT8 時の nblk は ic/8
		const uint16_t d32_ch = k3i8 ? 64 : 32, tf_ch = k3i8 ? 72 : 36;
		uint16_t nblk = is_dense32 ? (uint16_t)((hic + d32_ch - 1) / d32_ch)
		              : (is_tapfold ? (uint16_t)((hic + tf_ch - 1) / tf_ch)
		                            : (k3i8 ? (uint16_t)(hic >> 3) : (uint16_t)(hic >> 2)));
		assert(nblk <= 0xFFu && "HOIST_LAYER_DIV: nblk が uint8 超過 → rsv0 拡幅要");
		l.rsv0 = (uint8_t)nblk;
		uint8_t sc = (hic >= 16) ? (uint8_t)1 : (uint8_t)(16 / hic);    // #3 sc (÷ic, ic<16 のみ非1)
		l.rsv2 = (uint16_t)((l.rsv2 & 0xFF00u) | sc);
	}
#if VALID_COMV == DUMPFORMAT_Q_WEIGHT
	if(l.id == 1){
		printf("row=%d\n", row); dump8(waight_top, 20, 4, 8*2);
	}
#endif // VALID_COMV == DUMPFORMAT_Q_WEIGHT
	return wp;
}
// ■ HOIST_LAYER_DIV #5 — Concat の P_max 前計算
static void hoist_set_concat_pmax(Layer &l, bool is_add){
	const int CCN_CP_ = 32;   // = tasks_kernel.cpp CCN_CP
	uint8_t max_wpp = 0;
	if(is_add){
		uint16_t ch = (uint16_t)l.os[1];
		max_wpp = (uint8_t)((ch >= 32) ? (ch >> 5) : 0);          // wpp[0]==wpp[1]
	}else{
		uint16_t och = 0;
		for(int k=0; k<4; k++){
			if(och >= (uint16_t)l.os[1]) break;                  // och 単調増 = kernel の continue と等価
			uint16_t vch = (k==0) ? (uint16_t)l.is[1] : (uint16_t)l.p[k-1];
			uint8_t  w   = (k==0) ? (uint8_t)l.is[0] : (uint8_t)((vch >= 32) ? (vch >> 5) : 0);
			if(w > max_wpp) max_wpp = w;
			och += vch;
		}
	}
	uint16_t P_max = (max_wpp == 0) ? (uint16_t)CCN_CP_ : (uint16_t)(64 / max_wpp);
	if(P_max > (uint16_t)CCN_CP_) P_max = (uint16_t)CCN_CP_;
	l.rsv0 = (uint8_t)P_max;   // ≤ CCN_CP=32 = uint8 に収まる
}





int load_transmission(char* path, Layer &l, void *data, int index) {
	int zoom(1<<(data_shift[index][1] +8));
	DATA_TYPE tolerance((renge[index][1] -renge[index][0])*zoom/100*PRINT_DIFF_LIMIT); // 許容量
	int hits=0;
	// (yolov7 専用 SPPCSPC/Detect の早期 return は削除。yolo26/yolov10 は当該層を持たない)
	char fname[128];
	strcpy(fname, path);
	if(l.id == 105){
		strcat(fname, "DetectConv0.bin");
	}else{
		sprintf(fname, "%sy%d.bin", path, l.id);
	}

	FILE *fp = fopen(fname, "rb");
	if(!fp) {
		printf("%sを開けませんでした。", fname);
		return(-1);
	}

	typedef DATA_TYPE(*D)[l.os[3]][l.os[1]];
	D a =(D)data;
	float ref_f;
	float diff(0),min(0),max(0);
	int count = 0;
	float diff_max = 0;

	int dc,dy,dx;
	for(int o=0;o<l.os[1];o++){
	for(int y=0;y<l.os[2];y++){
	for(int x=0;x<l.os[3];x++){
		float p(a[y][x][o]);

		fread((char*)&ref_f, sizeof(ref_f), 1, fp);
		if(l.type == LayerType::Conv && !l.conv.SiLU && !l.conv.linear){
			ref_f = sigmoidf(ref_f);
		}
		if(ref_f < min){
			min = ref_f;
		}
		if(max < ref_f){
			max = ref_f;
		}
		a[y][x][o] = ref_f *zoom;
	}}}

	fclose(fp);
	return l.os[1]*l.os[2]*l.os[3]*2;
}


#if defined(VSCODE)

// ===========================================================================
// Phase C scaffold: .pt → n.q 生成のための回帰土台 (pt_gen_verify)
// ===========================================================================
// onnx_save (src/nq.cpp:379 Conv) と完全に同一式で Conv 量子化 recipe スカラを再導出する。
//   weight_m = (int8_t)log2f(1/(maxabs/32767)/512)   ← 切り捨て (int8_t 代入) まで一致させる
//   weight_b = 1.0/512 / (1 << weight_m)
//   bias_m   = (uint8_t)log2f(1/(maxabs/32767)/512)
// maxabs は per-tensor の min/max から onnx_save と同じ分岐で選ぶ。入力 float が同じなら
// (Phase B が ~1e-7 で保証) 出力 recipe は一致するはず。境界 (log2f がちょうど整数) のみ要注意。
struct PtConvQuant { int weight_m; float weight_b; int bias_m; };
static PtConvQuant pt_conv_quant_scalars(const std::vector<float>& w, const std::vector<float>& b){
	auto maxabs_div = [](const std::vector<float>& v)->float{
		float mn=v.empty()?0.f:v[0], mx=v.empty()?0.f:v[0];
		for(float x : v){ if(x<mn)mn=x; if(x>mx)mx=x; }
		// onnx_save と同じ: |max|<|min| なら |min|/32767、そうでなければ |max|/32767
		return (std::fabs(mx) < std::fabs(mn)) ? std::fabs(mn)/32767.f : std::fabs(mx)/32767.f;
	};
	PtConvQuant q{};
	float wm = maxabs_div(w);
	q.weight_m = (int)(int8_t)log2f(1.f/wm/512.f);          // int8_t 代入の切り捨てを再現
	q.weight_b = 1.0f/512.f / (1 << q.weight_m);
	float bm = maxabs_div(b);
	q.bias_m   = (int)(uint8_t)log2f(1.f/bm/512.f);         // bias_m は uint8_t
	return q;
}

// golden n.q (raw Layer binary, 末尾 Finish[8]) の Layer 連鎖を走査する。
//   1 Layer = 1 GMEM_T (=sizeof(Layer)=64B) のヘッダ。Conv は直後に bias+weight を
//   GMEM_T 単位でインライン (tasks.cpp::DUMP_OUT の adv = 8 + bias + weight と同型)。
//   cb(layer) を各層について呼ぶ。Finish に達したら停止。
template<class CB>
static bool nq_walk(const char* nq_path, CB&& cb){
	std::ifstream f(nq_path, std::ios::binary);
	if(!f){ fprintf(stderr,"pt_gen_verify: n.q を開けません: %s\n", nq_path); return false; }
	std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), {});
	if(buf.size() < sizeof(Layer)){ fprintf(stderr,"pt_gen_verify: n.q が小さすぎ (%zuB)\n", buf.size()); return false; }
	size_t off = 0;  // GMEM_T(=64B) 単位ではなくバイト
	while(off + sizeof(Layer) <= buf.size()){
		const Layer* l = reinterpret_cast<const Layer*>(buf.data() + off);
		if(l->type == LayerType::Finish) break;
		cb(*l);
		uint32_t adv = 8;  // header(1) + reserved gap(7) = 8 GMEM_T
		if(l->type == LayerType::Conv) adv += (uint32_t)l->conv.bias + (uint32_t)l->conv.weight;
		off += (size_t)adv * sizeof(Layer);  // sizeof(Layer)==sizeof(GMEM_T)==64
	}
	return true;
}

// 1 つの Conv を共通 encoder (encode_conv_block) で量子化バイト列へ。recipe スカラは呼出側で算出済。
//   w = [oc][ic][k*k] 連続 (.pt fused / ONNX initializer 共に row-major で一致)、b = [oc] (空なら 0)。
//   ONNX/.pt の生 float を同一 recipe・同一フラグで通すことで、両者のバイト一致 = .pt 完全生成の weight
//   ブロックが ONNX パスと等価であることの検証になる (zoomin は両側同値なら bias バイトに影響しない)。
static std::vector<uint8_t> encode_one_conv(int oc,int ic,int k,bool w_type,int wm,float wb,int bm,
                                            bool is_dw,bool is_tf,
                                            const std::vector<float>& w,const std::vector<float>& b){
	Layer l{};
	// ★合成 Layer (ONNX↔.pt byte 突合ハーネス) の sentinel。k3_int8_id_selected() が常に false を
	//   返すので WEIGHT_K3_INT8 が ON でも本ハーネスは int16 固定 = 突合の意味が保たれる。
	l.id = K3I8_SYNTH_ID;
	l.conv.kernel = k; l.conv.k_size = k; l.conv.w_type = w_type?1:0;
	l.conv.weight_m = (int8_t)wm; l.conv.weight_b = wb; l.conv.bias_m = (uint8_t)bm;
	l.conv.zoomin = 0; l.os[1] = (short)oc; l.is[1] = (short)ic; l.p[1] = (short)ic;
	int bias_gmem = roundedup(oc, sizeof(GMEM_T)/sizeof(uint16_t)*8)*sizeof(uint16_t)/sizeof(GMEM_T);
	size_t cap = (size_t)bias_gmem*sizeof(GMEM_T) + (size_t)oc*ic*k*k*2 + (size_t)(oc+8)*sizeof(GMEM_T) + 4096;
	std::vector<uint8_t> buf(cap, 0);
	uint8_t* bias_dst   = buf.data();
	uint8_t* weight_dst = buf.data() + (size_t)bias_gmem*sizeof(GMEM_T);
	std::vector<float> bb = b.empty() ? std::vector<float>(oc, 0.f) : b;
	uint8_t* end = encode_conv_block(l, bias_dst, weight_dst, w.data(), bb.data(), oc, ic, is_dw, is_tf,
	                                 (k==1) && !is_dw && !is_tf && !g_no_dense32);  // is_dense32 (常時 encode, kernel は weight count で判定)
	buf.resize(end - buf.data());
	return buf;
}

int pt_dump_shift(const char* nq_path, const char* out_path){
	if(!nq_path || !nq_path[0]){ fprintf(stderr,"pt_dump_shift: n.q path 必須\n"); return 2; }
	std::string nq = nq_path, out;
	if(out_path && out_path[0]) out = out_path;
	else { size_t s=nq.find_last_of("/\\"); out = (s==std::string::npos? std::string() : nq.substr(0,s+1)) + "data_shift.txt"; }

	// golden n.q を走査: 使用 id 範囲 (maxid) と activation Conv recipe の zoomin/zoomout を収集。
	//   identity-linear Conv (Split/Concat/Add 合成、linear=1) は data_shift と無関係に {0,0} を持つので
	//   検証対象から除外する (data_shift を消費するのは activation Conv のみ)。
	int maxid = -1, nconv = 0;
	std::map<int,std::pair<int,int>> convShift;   // id -> (zoomin, zoomout) ← activation Conv recipe のみ
	bool walked = nq_walk(nq.c_str(), [&](const Layer& l){
		if((int)l.id > maxid) maxid = l.id;
		if(l.type==LayerType::Conv && !l.conv.linear){
			nconv++;
			convShift[l.id] = { (int)l.conv.zoomin, (int)l.conv.zoomout };
		}
	});
	if(!walked) return 2;

	// 検証: golden n.q の activation Conv recipe と host data_shift[id] が一致するか
	//   (一致 = この sidecar が ONNX 生成パスと同じ値を供給できる根拠)
	int mism = 0;
	for(auto& [id, zz] : convShift){
		if(id<0 || id>=128) continue;
		if(zz.first != (int)data_shift[id][0] || zz.second != (int)data_shift[id][1]){
			printf("  [SHIFT-MISMATCH] id=%d  n.q{%d,%d} != data_shift{%d,%d}\n",
			       id, zz.first, zz.second, (int)data_shift[id][0], (int)data_shift[id][1]);
			mism++;
		}
	}

	// sidecar 出力 (id 0..maxid)。.pt 完全生成器がこれを読んで data_shift[] を再構成する。
	std::ofstream f(out);
	if(!f){ fprintf(stderr,"pt_dump_shift: 出力を開けません: %s\n", out.c_str()); return 2; }
	f << "# data_shift sidecar : layer-id -> {zoomin, zoomout}\n";
	f << "# .pt/ONNX に無いキャリブレーション値。.pt 完全生成器が読み込んで encode_conv_block 等へ供給する。\n";
	f << "# source: host.cpp data_shift[128][2] (golden n.q " << nq << " の Conv recipe と一致を検証済)\n";
	f << "# cols: id zoomin zoomout [conv]   (conv = n.q に Conv layer が存在する id)\n";
	for(int id=0; id<=maxid && id<128; ++id){
		f << id << " " << (int)data_shift[id][0] << " " << (int)data_shift[id][1]
		  << (convShift.count(id) ? "  conv\n" : "\n");
	}
	f.close();

	printf("=== pt_dump_shift: %s -> %s ===\n", nq.c_str(), out.c_str());
	printf("  ids 0..%d を出力 (%d Conv)。golden n.q recipe vs host data_shift[id] = %s (mismatch=%d)\n",
	       maxid, nconv, mism?"FAIL":"OK", mism);
	return mism?1:0;
}

// data_shift sidecar (pt_dump_shift 出力) を shift[128][2] へ読み込む (.pt 完全生成器用)。
int pt_load_shift(const char* path, unsigned char shift[128][2]){
	for(int i=0;i<128;++i){ shift[i][0]=3; shift[i][1]=3; }   // 未記載 id は host 支配値 {3,3}
	std::ifstream f(path);
	if(!f){ printf("pt_load_shift: %s 無し → 全 id を既定 {3,3} で生成 (要キャリブレーション)\n", path); return 0; }
	std::string line; int maxid=-1, n=0;
	while(std::getline(f, line)){
		size_t h = line.find('#'); if(h!=std::string::npos) line.erase(h);  // 行コメント除去
		int id, zin, zout;
		if(std::sscanf(line.c_str(), "%d %d %d", &id, &zin, &zout) == 3){
			if(id<0 || id>=128){ fprintf(stderr,"pt_load_shift: id 範囲外: %d\n", id); return -1; }
			shift[id][0]=(unsigned char)zin; shift[id][1]=(unsigned char)zout;
			if(id>maxid) maxid=id; ++n;
		}
	}
	printf("pt_load_shift: %s から %d 行 (ids 0..%d) を読込\n", path, n, maxid);
	return maxid;
}

// golden n.q の全 Layer ヘッダをダンプする診断 (emitter 設計用)。id/type/linear/f/in/out/
// is/os/p + Conv recipe を表示。module 内部分解の dataflow (Split/Add/Concat の f/shape) を把握する。
// 指定 layer id の weight region を raw int16 で out_path へ書き出す (tapfold disk 形式そのまま)。
// + bias region と weight_b/weight_m/oc/ic/kernel を stderr に出力 (board 不要の weight 値検証用)。
int pt_nq_wdump(const char* nq_path, int target_id, const char* out_path){
	std::ifstream f(nq_path, std::ios::binary);
	if(!f){ fprintf(stderr,"pt_nq_wdump: open fail %s\n", nq_path); return 2; }
	std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), {});
	size_t off=0;
	while(off + sizeof(Layer) <= buf.size()){
		const Layer* l = reinterpret_cast<const Layer*>(buf.data()+off);
		if(l->type == LayerType::Finish) break;
		if((int)l->id == target_id && l->type==LayerType::Conv){
			size_t woff = off + (size_t)(8 + l->conv.bias)*sizeof(GMEM_T);
			size_t wbytes = (size_t)l->conv.weight*sizeof(GMEM_T);
			fprintf(stderr,"pt_nq_wdump L%d: off=%zu woff=%zu wbytes=%zu oc=%d ic=%d k=%d wm=%d weight_b=%.9g zoomin=%d\n",
			   target_id, off, woff, wbytes, (int)l->os[1],(int)l->is[1],(int)l->conv.kernel,
			   (int)l->conv.weight_m, (double)l->conv.weight_b, (int)l->conv.zoomin);
			FILE* o=fopen(out_path,"wb");
			if(!o){ fprintf(stderr,"pt_nq_wdump: out open fail\n"); return 2; }
			fwrite(buf.data()+woff, 1, wbytes, o); fclose(o);
			fprintf(stderr,"pt_nq_wdump: wrote %zu bytes -> %s\n", wbytes, out_path);
			return 0;
		}
		uint32_t adv = 8;
		if(l->type == LayerType::Conv) adv += (uint32_t)l->conv.bias + (uint32_t)l->conv.weight;
		off += (size_t)adv * sizeof(Layer);
	}
	fprintf(stderr,"pt_nq_wdump: L%d not found (Conv)\n", target_id);
	return 1;
}

int pt_nq_dump(const char* nq_path){
	printf("=== pt_nq_dump: %s ===\n", nq_path);
	printf("  %-4s %-7s %-3s %-4s %-6s %-6s  %-14s %-14s %-12s  %s\n",
	       "id","type","lin","f","in","out","is[C,H,W]","os[C,H,W]","p[oc,ic,k]","conv{k,s,p,wt,wm,bm,zin,zout,wsz,bsz}");
	bool w = nq_walk(nq_path, [&](const Layer& l){
		const char* tn = ((int)l.type>=0 && (int)l.type<13) ? LayerTypeName[(int)l.type] : "?";
		char cv[128]=""; int lin=0;
		if(l.type==LayerType::Conv){
			lin=l.conv.linear;
			snprintf(cv,sizeof(cv),"{k%d s%d p%d SiLU%d wt%d wm%d bm%d zi%d zo%d w%u b%u}",
			         (int)l.conv.k_size,(int)l.conv.stride,(int)l.conv.padding,(int)l.conv.SiLU,(int)l.conv.w_type,
			         (int)l.conv.weight_m,(int)l.conv.bias_m,(int)l.conv.zoomin,(int)l.conv.zoomout,
			         l.conv.weight,(unsigned)l.conv.bias);
		}
		printf("  %-4d %-7s %-3d %-4d %-6u %-6u  [%d,%d,%d]%*s [%d,%d,%d]%*s [%d,%d,%d]%*s  %s\n",
		       l.id, tn, lin, l.f, l.in, l.out,
		       l.is[1],l.is[2],l.is[3], 6-0,"", l.os[1],l.os[2],l.os[3], 6-0,"", l.p[0],l.p[1],l.p[2], 4-0,"", cv);
	});
	return w?0:2;
}

// ===========================================================================
// Phase C step1/3: .pt → n.q Layer 生成 (pure-Conv prefix) + golden 突合
// ===========================================================================
// .pt の top module を forward 順に走査し、pure Conv (1:1) の prefix について Layer header
// (id/type/f/is/os/p/recipe) と weight/bias byte を .pt から生成、golden n.q と層ごとに突合する。
// in/out (活性化 GMEM アドレス) は memory planner 未移植のため比較対象外 (別途)。
// 最初の非 Conv module (C2f 等、forward 未実装) で打ち切る。module 分解 emitter はここに追加していく。
struct GoldenLayer { Layer hdr; std::vector<uint8_t> bias, weight; bool isConv=false; };

int pt_gen_layers_verify(const char* pt_path, const char* nq_path, const char* shift_path, const char* gen_out){
	// 1) .pt top module 構造 + 融合 Conv 重み + data_shift sidecar
	std::vector<PtTopModule> mods;
	if(!pt_collect_modules(pt_path, mods)){ fprintf(stderr,"pt_gen_layers: module 収集失敗\n"); return 2; }
	std::map<std::string,PtTensor> ptw, ptb;
	if(!pt_collect_conv_weights(pt_path, ptw, ptb)){ fprintf(stderr,"pt_gen_layers: 重み収集失敗\n"); return 2; }
	unsigned char shift[128][2];
	if(pt_load_shift(shift_path, shift) < 0) return 2;

	// 2) golden n.q を id 別に取り込む (header + bias/weight byte をコピー)
	std::map<int,GoldenLayer> golden;
	bool walked = nq_walk(nq_path, [&](const Layer& l){
		GoldenLayer g; g.hdr = l;
		if(l.type==LayerType::Conv){
			g.isConv = true;
			const uint8_t* base = (const uint8_t*)&l;
			size_t b0 = (size_t)8*sizeof(Layer), w0 = (size_t)(8+l.conv.bias)*sizeof(Layer);
			size_t w1 = (size_t)(8+l.conv.bias+l.conv.weight)*sizeof(Layer);
			g.bias.assign(base+b0, base+w0);
			g.weight.assign(base+w0, base+w1);
		}
		golden[l.id] = std::move(g);
	});
	// golden が無い (yolo26n 等 ONNX 非対応) 場合は生成のみ (検証スキップ)。golden.empty() で判定。
	if(!walked) printf("  (golden n.q 無し: 生成のみ・byte 検証スキップ。data_shift も未キャリブレーション)\n");

	// yolo26 判定: C3k2 / C2PSA を含むモデルは ONNX golden 非対応 (onnx_save は yolov10 専用)。
	//   渡された golden (= yolov10 の data/n.q) は構造が別物なので破棄し「生成のみ」へ切替える。
	//   data_shift は yolo26_verify.py --dump-shift が出す path-keyed sidecar から conv 路で引く。
	bool yolo26 = false;
	for(const auto& m : mods) if(m.cls=="C3k2" || m.cls=="C2PSA"){ yolo26 = true; break; }
	std::map<std::string,std::pair<int,int>> zoomByPath;   // python conv 路 -> (zin, zout)
	if(yolo26){
		if(!golden.empty()){ printf("  (yolo26 検出: 渡された golden は別モデルなので破棄 → 生成のみ)\n"); golden.clear(); }
		std::string dir; { std::string p(pt_path); size_t s=p.find_last_of("/\\"); dir=(s==std::string::npos?std::string():p.substr(0,s+1)); }
		std::string sc = dir + "data_shift_y26.txt";
		std::ifstream sf(sc);
		if(sf){
			std::string line; int n=0;
			while(std::getline(sf,line)){
				size_t h=line.find('#'); if(h!=std::string::npos) line.erase(h);
				char path[256]; int zin,zout;
				if(std::sscanf(line.c_str(), "%255s %d %d", path, &zin, &zout)==3){ zoomByPath[path]={zin,zout}; ++n; }
			}
			printf("  yolo26 data_shift sidecar: %s から %d conv 路を読込 (path-keyed zin/zout)\n", sc.c_str(), n);
		} else {
			printf("  yolo26 data_shift sidecar %s 無し → 全 conv 既定 {3,3} (要 yolo26_verify.py --dump-shift)\n", sc.c_str());
		}
	}

	printf("=== pt_gen_layers_verify (Phase C step1/3): %s vs golden %s ===\n", pt_path, nq_path);
	printf("  module forward を .pt から再実装し golden 突合: Conv=header+weight byte / Split=byte-exact / 構造層=header。\n");
	printf("  in/out/f は NqBumpPlanner 再生で byte-exact。構造層(Split/Add/Concat/MaxPool/Resize/Attention)は full-header(64B) byte-exact。Conv weight のみ f16 丸めの ±LSB 許容。\n");

	// 3) generator 状態
	const std::array<int,3> INPUT = { 3, (int)Y26_NETH, (int)Y26_NETW };  // geo640: onnx_save nn={W,H}(既定 H=384,W=640),C=3
	int id = 0, nlayer = 0, allOk = 1;
	std::map<int, std::array<int,3>> layerOut;  // layer id -> 出力 CHW
	std::map<int, int>               topOut;    // top index -> その module の出力 layer id

	// === memory planner: onnx_save と同一の NqBumpPlanner (bump allocator) を生成層列で再生 ===
	//   LIVENESS_ALLOC=0 なので relayout 無し。in/out は set()(入力 30720 word 確保) + 層順の out() で確定。
	//   far 分岐 (Conv && os[0]==1) は本 model では未使用 (全層 os[0]=0)。l.in = producer(f) の out。
	transmission_index = 0; transmission_fragment = 0;
	// ★struct シュリンク: in/out は uint16 4KB(64語)単位。層出力の 4KB 整列は恒久必須
	//   (旧 env OUT_ALIGN4K 任意化は撤去。整列していないと L_U で truncation 破損)。out() が word 整列を assert。
	g_out_align4k = 1;
	printf("  [4KB-UNIT] 層出力を 4KB(64語=%d B)境界へ整列 + in/out を 4KB単位で n.q 格納\n", OUT_ALIGN_BASE*64);
	int align_viol = 0;
	NqBumpPlanner<GMEM_T> planner;
	{ Layer dummy{}; Forward c{}; planner.set(&dummy, c); }   // slot[0]=入力画像 (skipSize=0)
	std::map<int, uint32_t> outAddr;            // layer id -> out (4KB単位)。実語=L_W(out)
	auto assignAddr = [&](Layer& l)->void{      // l.os 確定後に呼ぶ。in/out を planner で確定し記録。
		l.in  = outAddr.count(l.f) ? outAddr[l.f] : 0;   // id0 は f=0 自己参照 → out 未確定で 0 (golden 一致)。単位=unit
		l.out = planner.out(l);                          // ★planner.out は 4KB単位を返す (word 整列は out() で assert)
		outAddr[l.id] = l.out;
		// uint16 単位オーバフロー監視 (実語 L_W(out) が 65535*4KB=256MB 超 → unit>65535 で uint16 溢れ)。
		if((uint32_t)l.out >= 0xFFFFu){
			printf("  [4KB-UNIT] ★L%d out=%u(unit) が uint16 上限逼迫 (実語 %u)\n",
			       (int)l.id, (unsigned)l.out, (unsigned)L_W(l.out));
			align_viol++;
		}
	};

	// === 実 n.q 累積 (--gen 用) ===
	//   1 層 = 8 GMEM_T ヘッダ領域 (先頭 64B=Layer 構造体, 残 448B=gap, onnx_save 同) + (Conv/Split のみ)
	//   bias+weight byte。type!=Conv (Add/Concat/MaxPool/Resize/Attention) は trailing 無し (adv=8)。
	std::vector<uint8_t> nqOut;
	auto appendBytes = [&](const Layer& l, const std::vector<uint8_t>& trailing){
		size_t base = nqOut.size();
		nqOut.resize(base + 8*sizeof(GMEM_T), 0);          // header 領域 8 GMEM_T (struct + gap), 0 埋め
		std::memcpy(nqOut.data()+base, &l, sizeof(Layer)); // 先頭 64B に Layer 構造体
		if(l.type==LayerType::Conv && !trailing.empty())   // Conv/Split は bias+weight を続けて配置
			nqOut.insert(nqOut.end(), trailing.begin(), trailing.end());
	};

	// 構造層 (Split-conv / Add / Concat) を emit: header(id/type/f/is/os) のみ golden 突合 (union byte は deferred)。
	auto emitStruct = [&](LayerType type, int fLayer, std::array<int,3> inS, std::array<int,3> outS, int linear)->int{
		int tid = id; Layer l{}; l.id=(short)tid; l.type=type; l.f=(short)fLayer;
		l.is[1]=(short)inS[0]; l.is[2]=(short)inS[1]; l.is[3]=(short)inS[2];
		l.os[1]=(short)outS[0]; l.os[2]=(short)outS[1]; l.os[3]=(short)outS[2];
		if(type==LayerType::Conv) l.conv.linear=(uint8_t)linear;
		assignAddr(l);
		const char* tn=((int)type>=0&&(int)type<13)?LayerTypeName[(int)type]:"?";
		auto git=golden.find(tid); int ok=1;
		if(git==golden.end()){ printf("  L%-3d %-6s: golden に id=%d 無し\n", tid, tn, tid); ok=0; }
		else { const Layer& G=git->second.hdr;
			auto chk=[&](const char* nm,long a,long b){ if(a!=b){ok=0;printf("    %-8s gen=%ld golden=%ld <<<\n",nm,a,b);} };
			chk("type",(long)l.type,(long)G.type); chk("f",l.f,G.f);
			chk("in",(long)l.in,(long)G.in); chk("out",(long)l.out,(long)G.out);
			chk("is1",l.is[1],G.is[1]); chk("is2",l.is[2],G.is[2]); chk("is3",l.is[3],G.is[3]);
			chk("os1",l.os[1],G.os[1]); chk("os2",l.os[2],G.os[2]); chk("os3",l.os[3],G.os[3]);
			if(type==LayerType::Conv) chk("linear",l.conv.linear,G.conv.linear);
			printf("  L%-3d %-6s%s f=%-3d in=%-6u out=%-6u is[%d,%d,%d]->os[%d,%d,%d]  header %s (union/byte deferred)\n",
			       tid, tn, linear?"(lin)":"     ", l.f, l.in, l.out, l.is[1],l.is[2],l.is[3], l.os[1],l.os[2],l.os[3], ok?"OK":"<<MISMATCH");
		}
		if(!ok) allOk=0;
		appendBytes(l, {});
		layerOut[tid]=outS; ++id; ++nlayer; return tid;
	};

	// conv の zoomin/zoomout を決める: yolo26 は path-keyed sidecar (zoomByPath) を優先、
	//   無ければ id-keyed shift[tid] (yolov10 既存経路 / 未キャリブレーション既定 {3,3})。
	//   cpath は conv 路 (例 "model.0.conv")。末尾 ".conv" を剥がして python step 路 (例 "model.0") に合わせる。
	auto zoomFor = [&](const std::string& cpath, int tid)->std::pair<int,int>{
		std::pair<int,int> z = { (int)shift[tid][0], (int)shift[tid][1] };
		std::string key = cpath; const std::string suf = ".conv";
		if(key.size()>=suf.size() && key.compare(key.size()-suf.size(), suf.size(), suf)==0)
			key.erase(key.size()-suf.size());
		if(!zoomByPath.empty() && !cpath.empty()){
			auto it = zoomByPath.find(key);
			if(it!=zoomByPath.end()) z = it->second;
		}
		// ★Attention HW kernel 制約 (ATTN_IO_ZOUT, tasks.h): attention kernel は qkv 入力 / 出力を
		//   scale 2^(8+ATTN_IO_ZOUT)=2048 固定で読む (ATTN_SCORE_SHIFT/pe shift が baked-in)。よって
		//   qkv 出力 zout と proj 入力 zin は calibration 値に依らず ATTN_IO_ZOUT へ pin する
		//   (不一致は全要素 2^Δ 倍の系統誤差 = 飽和クリップより致命的)。pin した値が calibration と
		//   異なる場合は kernel scale 一般化 (ATTN_SCORE_SHIFT 動的化) + xclbin 再ビルドで本来値を活かせる。
		auto ends=[&](const std::string& s){ return key.size()>=s.size() && key.compare(key.size()-s.size(),s.size(),s)==0; };
		if(ends(".attn.qkv") && z.second!=ATTN_IO_ZOUT){
			printf("  [ATTN-PIN] %s zout %d->%d 固定 (calibration 値は kernel 一般化時に有効化)\n", key.c_str(), z.second, ATTN_IO_ZOUT);
			z.second = ATTN_IO_ZOUT;
		}
		if(ends(".attn.proj") && z.first!=ATTN_IO_ZOUT){
			printf("  [ATTN-PIN] %s zin %d->%d 固定 (calibration 値は kernel 一般化時に有効化)\n", key.c_str(), z.first, ATTN_IO_ZOUT);
			z.first = ATTN_IO_ZOUT;
		}
		return z;
	};

	// Conv 層の core emit: 明示 weight/bias から header + byte を生成し golden 突合 (RepVGGDW 融合等で再利用)。
	//   W=[oc][ic_per_g][k*k] 連続 (is_dw なら ic_per_g=1)。is_dw=true で depthwise 経路。
	//   cpath: zoomByPath 参照用の conv 路 (空なら shift[tid] を使う)。
	auto emitConvCore = [&](const std::vector<float>& W, const std::vector<float>& B,
	                        int oc, int ic, int k, int fLayer, std::array<int,3> inS, int stride, bool silu, bool is_dw,
	                        const std::string& cpath = std::string())->int{
		int tid = id; int pad=k/2;
		// === sub-16 channel zero-pad: kernel は 16ch 粒度 (1 sum word=16ch, conv OUTPUT_SUM は (oc>>4) word emit)。
		//   oc<16 だと (oc>>4)==0 で conv が 1 word も emit せず、swishs/store は count=(oh*ow*oc)>>4 word を待って
		//   永久ブロック = HW/csim deadlock。yolo26 C3k2 の 8ch 最狭 (L4 16→8 /
		//   L5 8→16) と Detect head box (oc=4: L131/140/149) が該当。oc/ic を 16 倍数へ切上げ weight/bias を 0 拡張
		//   すると padded ch は weight=bias=0 で出力 0 = 実 ch は数値 bit-exact のまま全層 16 整合化し deadlock 解消。
		//   ic<=4 (L0 input 3→4) は既存 encode_conv_block の 3→4 path が処理済なので除外 (二重 pad 防止)。
		std::vector<float> Wpad, Bpad; const float *Wsrc=W.data(), *Bsrc=B.data();
		if(!is_dw && (oc < 16 || (ic > 4 && ic < 16))){
			int oc_p = (oc < 16) ? 16 : oc;
			int ic_p = (ic > 4 && ic < 16) ? 16 : ic;
			Bpad.assign(oc_p, 0.0f);
			for(int o=0;o<oc && o<(int)B.size();o++) Bpad[o]=B[o];
			Wpad.assign((size_t)oc_p*ic_p*k*k, 0.0f);
			for(int o=0;o<oc;o++) for(int c=0;c<ic;c++) for(int t=0;t<k*k;t++)
				Wpad[((size_t)o*ic_p+c)*k*k+t] = W[((size_t)o*ic+c)*k*k+t];
			printf("  [PAD16] L%d oc %d->%d ic %d->%d (zero-extend, sub-16 deadlock 回避)\n", tid, oc, oc_p, ic, ic_p);
			oc=oc_p; ic=ic_p; Wsrc=Wpad.data(); Bsrc=Bpad.data();
		}
		Layer l{}; l.id=(short)tid; l.type=LayerType::Conv; l.f=(short)fLayer;
		l.is[1]=(short)ic; l.is[2]=(short)inS[1]; l.is[3]=(short)inS[2];
		l.os[1]=(short)oc; l.os[2]=(short)((inS[1]+2*pad-k)/stride+1); l.os[3]=(short)((inS[2]+2*pad-k)/stride+1);
		l.p[0]=(short)oc; l.p[1]=(short)ic; l.p[2]=(short)k;
		PtConvQuant q = pt_conv_quant_scalars(W,B);
		l.conv.kernel=(uint8_t)k; l.conv.k_size=(uint16_t)k; l.conv.stride=(uint16_t)stride; l.conv.padding=(uint16_t)pad;
		l.conv.weight_m=(int8_t)q.weight_m; l.conv.weight_b=q.weight_b; l.conv.bias_m=(uint8_t)q.bias_m;
		// ★2026-07-17: is_tf は従来 2627 行で算出していたが、w_type 判定に要るので前倒しする
		//   (値は同式・同条件 = 従来と byte 不変)。
		bool is_tf_pre = !TAPFOLD_DISABLED && (k==1) && !is_dw && ((ic>>2)*(oc>>3) >= 512);
		// ★FP8 ガード + WEIGHT_K3_INT8 (2026-07-17): kernel は 8bit 重み非対応(int8 読みは
		//   -DCONV_J8_PROBE のみ)。旧ロジック `ic*oc*k*k <= 1024*1024/8*9` は大層を無警告で
		//   w_type=0 にし、kernel が int16 として読んで garbage になる → **int16 を強制**する。
		//   ★本経路(--pt)が production の n.q 生成経路。Conv()(ONNX)側だけ守っても意味がない。
		//   ★2026-07-20 (残作業17): 判定は k3_int8_eligible() **1 箇所**。is_dense32 も encode 呼出と
		//     同一式を共有する (下の encode_conv_block 引数で再利用)。
		const bool is_d32_pre = (k==1) && !is_dw && !is_tf_pre && !g_no_dense32;
		{
			bool k3i8_here = k3_int8_eligible(tid, k, ic, is_tf_pre, is_d32_pre, is_dw,
			                                  /*is_identity=*/false);
			if(!k3i8_here && !((long)ic*oc*k*k <= 1024*1024/8*9)){
				printf("\n[FP8-GUARD] FATAL: --pt layer %d ic=%d oc=%d k=%d: weight %ld > %d は旧ロジックで"
				       " w_type=0(8bit)になるが kernel は 8bit 非対応 = 無警告 garbage。\n",
				       tid, ic, oc, k, (long)ic*oc*k*k, 1024*1024/8*9);
				exit(1);
			}
			l.conv.w_type = k3i8_here ? 0 : 1;
		}
		l.conv.SiLU=silu?1:0;
		l.p[3] = is_dw ? 1 : 0;                                 // depthwise marker (onnx_save と同じ l.p[3])
		{ auto zo=zoomFor(cpath,tid); l.conv.zoomin=(int8_t)zo.first; l.conv.zoomout=(int8_t)zo.second; }
		{ int sLine=ic*(int)Y26_NETW; l.conv.inlen=(uint16_t)(log2f(sLine*2.5f*sizeof(DATA_TYPE)/(512*2/8))+stride); }  // onnx_save Conv() と同式 (nn.w=NETW)
		l.conv.bias=(uint16_t)(roundedup(oc,sizeof(GMEM_T)/sizeof(uint16_t)*8)*sizeof(uint16_t)/sizeof(GMEM_T));
		bool is_tf = is_tf_pre;   // ★上で前倒し算出済 (同式・同値)
		int biasGmem=l.conv.bias, icRef=ic;
		size_t cap=(size_t)biasGmem*sizeof(GMEM_T)+(size_t)oc*ic*k*k*2+(size_t)(oc+8)*sizeof(GMEM_T)+4096;
		std::vector<uint8_t> gen(cap,0);
		uint8_t* end=encode_conv_block(l, gen.data(), gen.data()+(size_t)biasGmem*sizeof(GMEM_T), Wsrc, Bsrc, oc, icRef, is_dw, is_tf,
		                               is_d32_pre);  // ★w_type 焼込みと同一式 (上で hoist 済)
		gen.resize(end-gen.data());      // encode は ic==3 のとき l.is[1]/l.p[1] を 4 へ更新
		assignAddr(l);
		std::array<int,3> outS = { oc, l.os[2], l.os[3] };
		auto git=golden.find(tid); int ok=1;
		if(golden.empty()){ printf("  L%-3d Conv  gen f=%-3d is[%d,%d,%d]->os[%d,%d,%d] k%d s%d wt%d wm%d (生成のみ)\n",
		                            tid,l.f,l.is[1],l.is[2],l.is[3],l.os[1],l.os[2],l.os[3],k,stride,(int)l.conv.w_type,(int)l.conv.weight_m); }
		else if(git==golden.end()||!git->second.isConv){ printf("  L%-3d Conv: golden に Conv id=%d 無し\n", tid, tid); ok=0; }
		else { const Layer& G=git->second.hdr;
			auto chk=[&](const char* nm,long a,long b){ if(a!=b){ok=0;printf("    %-8s gen=%ld golden=%ld <<<\n",nm,a,b);} };
			chk("type",(long)l.type,(long)G.type); chk("f",l.f,G.f);
			chk("in",(long)l.in,(long)G.in); chk("out",(long)l.out,(long)G.out);
			chk("is1",l.is[1],G.is[1]); chk("is2",l.is[2],G.is[2]); chk("is3",l.is[3],G.is[3]);
			chk("os1",l.os[1],G.os[1]); chk("os2",l.os[2],G.os[2]); chk("os3",l.os[3],G.os[3]);
			chk("p1",l.p[1],G.p[1]); chk("p2",l.p[2],G.p[2]);
			chk("k_size",l.conv.k_size,G.conv.k_size); chk("stride",l.conv.stride,G.conv.stride);
			chk("padding",l.conv.padding,G.conv.padding); chk("SiLU",l.conv.SiLU,G.conv.SiLU);
			chk("w_type",l.conv.w_type,G.conv.w_type); chk("weight_m",l.conv.weight_m,G.conv.weight_m);
			chk("bias_m",l.conv.bias_m,G.conv.bias_m); chk("zoomin",l.conv.zoomin,G.conv.zoomin);
			chk("zoomout",l.conv.zoomout,G.conv.zoomout); chk("bias_sz",l.conv.bias,G.conv.bias);
			chk("weight_sz",l.conv.weight,G.conv.weight); chk("inlen",l.conv.inlen,G.conv.inlen);
			if(l.conv.weight_b!=G.conv.weight_b){ok=0;printf("    weight_b gen=%.8g golden=%.8g <<<\n",l.conv.weight_b,G.conv.weight_b);}
			std::vector<uint8_t> gold(git->second.bias); gold.insert(gold.end(),git->second.weight.begin(),git->second.weight.end());
			long bdiff=(gen.size()!=gold.size())?(long)std::max(gen.size(),gold.size()):0;
			size_t nB=std::min(gen.size(),gold.size()); for(size_t z=0;z<nB;++z) if(gen[z]!=gold[z]) bdiff++;
			printf("  L%-3d Conv   f=%-3d is[%d,%d,%d]->os[%d,%d,%d] k%d s%d wt%d wm%d  byte diff=%ld/%zu  header %s\n",
			       tid, l.f, l.is[1],l.is[2],l.is[3], l.os[1],l.os[2],l.os[3], k, stride,
			       (int)l.conv.w_type,(int)l.conv.weight_m, bdiff, gold.size(), ok?"OK":"<<MISMATCH");
		}
		if(!ok) allOk=0;
		appendBytes(l, gen);
		layerOut[tid]=outS; ++id; ++nlayer; return tid;
	};

	// path 版: ptw/ptb から weight を引き depthwise を自動判定して emitConvCore へ。
	auto emitConv = [&](const std::string& cp, int fLayer, std::array<int,3> inS, int stride, bool silu)->int{
		auto wit = ptw.find(cp+".weight");
		if(wit==ptw.end()){ printf("  L%-3d Conv: .pt 重み %s 無し → 打ち切り\n", id, cp.c_str()); allOk=0; return -1; }
		const std::vector<int>& wd = wit->second.dims;          // [oc,ic_per_g,kh,kw]
		int oc=wd[0], k=wd[2];
		bool is_dw = (wd.size()>=3 && wd[1]==1 && oc>1 && k>1);  // depthwise (groups=oc)
		int ic = is_dw ? oc : wd[1];
		return emitConvCore(wit->second.data, ptb[cp+".bias"].data, oc, ic, k, fLayer, inS, stride, silu, is_dw, cp);
	};

	// RepVGGDW 融合: base.conv(7x7 dw, BN融合済) に base.conv1(3x3 dw) を中心 pad して加算 (推論時 fuse)。
	//   両者 depthwise [oc,1,k,k]。融合 weight=[oc][1][7][7] (連続 oc*49) と bias を Wf/Bf へ。
	auto fuseRepVGGDW = [&](const std::string& base, std::vector<float>& Wf, std::vector<float>& Bf, int& oc, int& k)->bool{
		auto w7=ptw.find(base+".conv.conv.weight"), w3=ptw.find(base+".conv1.conv.weight");
		if(w7==ptw.end()||w3==ptw.end()) return false;
		oc=w7->second.dims[0]; k=w7->second.dims[2];           // 7
		int k3=w3->second.dims[2];                             // 3
		Wf.assign(w7->second.data.begin(), w7->second.data.end());   // [oc][7*7]
		const std::vector<float>& W3=w3->second.data; int off=(k-k3)/2;
		for(int o=0;o<oc;o++) for(int r=0;r<k3;r++) for(int c=0;c<k3;c++)
			Wf[(size_t)o*k*k + (off+r)*k + (off+c)] += W3[(size_t)o*k3*k3 + r*k3 + c];
		Bf.assign(ptb[base+".conv.conv.bias"].data.begin(), ptb[base+".conv.conv.bias"].data.end());
		const std::vector<float>& B3=ptb[base+".conv1.conv.bias"].data;
		for(int o=0;o<oc && o<(int)B3.size();o++) Bf[o]+=B3[o];
		return true;
	};

	// Split out1 を byte-exact 生成: identity 1x1 conv (one-hot weight=256, zero bias, linear)。
	//   onnx_save Split() と同一 (合成のため f16 無し ⇒ byte diff=0 のはず)。ch[offset:offset+oc] を抽出。
	auto emitSplit = [&](int fLayer, int ic, int H, int Wd, int oc, int offset)->int{
		int tid = id;
		Layer l{}; l.id=(short)tid; l.type=LayerType::Conv; l.f=(short)fLayer;
		l.is[1]=(short)ic; l.is[2]=(short)H; l.is[3]=(short)Wd;
		l.os[1]=(short)oc; l.os[2]=(short)H; l.os[3]=(short)Wd;
		l.p[0]=(short)oc; l.p[1]=(short)ic; l.p[2]=1;
		l.conv.kernel=1; l.conv.k_size=1; l.conv.stride=1; l.conv.padding=0;
		l.conv.SiLU=0; l.conv.linear=1; l.conv.reserved2=1; l.conv.w_type=1;
		l.conv.weight_m=0; l.conv.bias_m=0; l.conv.zoomin=0; l.conv.zoomout=0; l.conv.weight_b=1.0f/512;
		l.conv.bias=(uint16_t)(roundedup(oc,sizeof(GMEM_T)/sizeof(uint16_t)*8)*sizeof(uint16_t)/sizeof(GMEM_T));
		int sLine = ic*(int)Y26_NETW;   // onnx_save Split(): nn.w=NETW (geo640)
		l.conv.inlen=(uint16_t)(log2f(sLine*2.5f*sizeof(DATA_TYPE)/(512*2/8)) + l.conv.stride);
		// one-hot weight (W[o][offset+o]=256*weight_b → zoom16=256) + zero bias を合成し共通 encoder で書く
		std::vector<float> Wf((size_t)oc*ic, 0.0f), Bf(oc, 0.0f);
		for(int o=0;o<oc;o++){ int c=offset+o; if(c<ic) Wf[(size_t)o*ic + c] = 256.0f*l.conv.weight_b; }
		int biasGmem=l.conv.bias, icRef=ic;
		size_t cap=(size_t)biasGmem*sizeof(GMEM_T)+(size_t)oc*ic*2+(size_t)(oc+8)*sizeof(GMEM_T)+4096;
		std::vector<uint8_t> gen(cap,0);
		// ★E-fix (2026-06-11): ONNX Split() と同様、kernel の is_tapfold(footprint=(ic/4)*(oc/8)>=512) と
		//   一致させる。pt 経路 (yolo26 C2PSA/C2f 等) の synth-split も footprint>=512 で tapfold 化しないと
		//   kernel が tapfold 期待 → weight count 不一致で stream deadlock する。
		bool sp_tapfold = !TAPFOLD_DISABLED && ((ic>>2)*(oc>>3) >= 512);
		uint8_t* end=encode_conv_block(l, gen.data(), gen.data()+(size_t)biasGmem*sizeof(GMEM_T), Wf.data(), Bf.data(), oc, icRef, false, sp_tapfold,
		                               !sp_tapfold && !g_no_dense32);  // is_dense32 (synth-split は k=1 非dw、常時 encode)
		gen.resize(end-gen.data());
		assignAddr(l);
		auto git=golden.find(tid); int ok=1;
		if(golden.empty()){ printf("  L%-3d Split gen f=%-3d is[%d,%d,%d]->os[%d,%d,%d] off=%d (生成のみ)\n", tid,l.f,l.is[1],l.is[2],l.is[3],l.os[1],l.os[2],l.os[3],offset); }
		else if(git==golden.end()||!git->second.isConv){ printf("  L%-3d Split: golden に Conv id=%d 無し\n", tid, tid); ok=0; }
		else { const Layer& G=git->second.hdr;
			auto chk=[&](const char* nm,long a,long b){ if(a!=b){ok=0;printf("    %-8s gen=%ld golden=%ld <<<\n",nm,a,b);} };
			chk("type",(long)l.type,(long)G.type); chk("f",l.f,G.f); chk("linear",l.conv.linear,G.conv.linear);
			chk("in",(long)l.in,(long)G.in); chk("out",(long)l.out,(long)G.out);
			chk("reserved2",l.conv.reserved2,G.conv.reserved2); chk("inlen",l.conv.inlen,G.conv.inlen);
			chk("is1",l.is[1],G.is[1]); chk("os1",l.os[1],G.os[1]);
			chk("k_size",l.conv.k_size,G.conv.k_size); chk("w_type",l.conv.w_type,G.conv.w_type);
			chk("weight_m",l.conv.weight_m,G.conv.weight_m); chk("bias_m",l.conv.bias_m,G.conv.bias_m);
			chk("zoomin",l.conv.zoomin,G.conv.zoomin); chk("zoomout",l.conv.zoomout,G.conv.zoomout);
			chk("bias_sz",l.conv.bias,G.conv.bias); chk("weight_sz",l.conv.weight,G.conv.weight);
			if(l.conv.weight_b!=G.conv.weight_b){ok=0;printf("    weight_b gen=%.8g golden=%.8g <<<\n",l.conv.weight_b,G.conv.weight_b);}
			std::vector<uint8_t> gold(git->second.bias); gold.insert(gold.end(),git->second.weight.begin(),git->second.weight.end());
			long bdiff=(gen.size()!=gold.size())?(long)std::max(gen.size(),gold.size()):0;
			size_t nB=std::min(gen.size(),gold.size()); for(size_t z=0;z<nB;++z) if(gen[z]!=gold[z]) bdiff++;
			printf("  L%-3d Split(lin) f=%-3d is[%d,%d,%d]->os[%d,%d,%d] off=%d  byte diff=%ld/%zu  %s\n",
			       tid, l.f, l.is[1],l.is[2],l.is[3], l.os[1],l.os[2],l.os[3], offset, bdiff, gold.size(),
			       (ok&&bdiff==0)?"BYTE-EXACT":(ok?"header OK, byte diff":"<<MISMATCH"));
		}
		if(!ok) allOk=0;
		appendBytes(l, gen);
		layerOut[tid]={oc,H,Wd}; ++id; ++nlayer; return tid;
	};

	// 構造層の full-header (64B) byte 突合ヘルパ (Add/Concat 用)。in/out は assignAddr で確定済。
	auto cmpFullHeader = [&](const Layer& l, int tid, const char* tn)->void{
		auto git=golden.find(tid); int ok=1; long hdiff=0;
		if(golden.empty()){ printf("  L%-3d %-6s gen f=%-3d os[%d,%d,%d] (生成のみ)\n", tid, tn, l.f, l.os[1],l.os[2],l.os[3]); }
		else if(git==golden.end()){ printf("  L%-3d %-6s: golden に id=%d 無し\n", tid, tn, tid); ok=0; }
		else { const uint8_t* A=(const uint8_t*)&l; const uint8_t* B=(const uint8_t*)&git->second.hdr;
			for(int z=0; z<(int)sizeof(Layer); ++z) if(A[z]!=B[z]) ++hdiff;
			ok=(hdiff==0);
			printf("  L%-3d %-6s f=%-3d in=%-6u out=%-6u os[%d,%d,%d]  header(64B) diff=%ld  %s\n",
			       tid, tn, l.f, l.in, l.out, l.os[1],l.os[2],l.os[3], hdiff, ok?"BYTE-EXACT":"<<MISMATCH");
			if(!ok && hdiff<=24){ printf("    diff bytes:"); for(int z=0;z<(int)sizeof(Layer);++z) if(A[z]!=B[z]) printf(" %d(%02x/%02x)",z,A[z],B[z]); printf("\n"); }
		}
		if(!ok) allOk=0;
	};

	// Add 層を byte-exact 生成 (onnx_save Add() 再現): is/os/p=in0->os、i32[0]=in1->out、linear/weight_m/zoomin/SiLU。
	auto emitAdd = [&](int firstInput, int secondInput)->int{
		int tid=id; std::array<int,3> s=layerOut[firstInput];
		Layer l{}; l.id=(short)tid; l.type=LayerType::Add; l.f=(short)firstInput;
		l.is[0]=0; l.is[1]=(short)s[0]; l.is[2]=(short)s[1]; l.is[3]=(short)s[2];
		l.os[0]=0; l.os[1]=(short)s[0]; l.os[2]=(short)s[1]; l.os[3]=(short)s[2];
		l.p[0]=0;  l.p[1]=(short)s[0];  l.p[2]=(short)s[1];  l.p[3]=(short)s[2];
		l.i32[0]=(int32_t)outAddr[secondInput];
		l.conv.linear=1; l.conv.weight_m=0; l.conv.zoomin=0; l.conv.SiLU=0;
		assignAddr(l);   // l.in=outAddr[f0], l.out=planner.out (= onnx_save の in->out / buffers.out と一致)
		g_extra[tid] = std::vector<int>{ secondInput };   // ★案B2: relayout liveness 用 (Add 第2入力 producer)
		hoist_set_concat_pmax(l, /*is_add=*/true);   // ★#5 P_max → rsv0 (.pt 経路。onnx Add():955 と同一。無しだと HOIST_LAYER_DIV で P_max=0→data_load Add passthrough deadlock @最初のAdd)
		cmpFullHeader(l, tid, "Add"); appendBytes(l, {});
		layerOut[tid]=s; ++id; ++nlayer; return tid;
	};

	// Concat 層を byte-exact 生成 (onnx_save Concat() 再現)。ins=[(layerId,nch)...]、ins[0]=primary(viewなら nch<full)。
	//   p[i-1]=入力i の nch、i32[slot]=入力i の out (slot: i=1→0,i=2→1,i=3→4)、os[1]=Σnch、linear/weight_m/zoomin/SiLU。
	auto emitConcat = [&](const std::vector<std::pair<int,int>>& ins)->int{
		int tid=id; int aId=ins[0].first, aNch=ins[0].second; std::array<int,3> ap=layerOut[aId];
		Layer l{}; l.id=(short)tid; l.type=LayerType::Concat; l.f=(short)aId;
		int ch=0; for(auto& p:ins) ch+=p.second;
		// ★2026-06-17 fix (.pt Phase C 経路, onnx Concat():1033 と同一バグ): producer が genuine <32ch
		//   (16ch) の場合は 2px/word 格納で wpp=0 が正 (ceil(16/32)=1 だと kernel が 1px/word 誤読 → 出力
		//   pixel pl が入力 2pl を読む 2x stretch + 領域超過ゼロ; board 実証 L93 corr 0.495→0.998)。
		//   kernel CCN は wpp=0 を 2px/word で正しく処理。Split view(producer≥32ch)は producer wpp 保持。
		l.is[0]=(short)(ap[0] >= 32 ? ((ap[0]+31)/32) : 0); l.is[1]=(short)aNch; l.is[2]=(short)ap[1]; l.is[3]=(short)ap[2];
		l.os[0]=0; l.os[1]=(short)ch; l.os[2]=(short)ap[1]; l.os[3]=(short)ap[2];
		for(size_t i=1;i<ins.size();++i){
			l.p[i-1]=(short)ins[i].second;
			int slot=(i==3)?4:(int)(i-1);
			l.i32[slot]=(int32_t)outAddr[ins[i].first];
		}
		l.conv.linear=1; l.conv.weight_m=0; l.conv.zoomin=0; l.conv.SiLU=0;
		assignAddr(l);
		{ std::vector<int> ex; for(size_t i=1;i<ins.size();++i) ex.push_back(ins[i].first); g_extra[tid]=ex; } // ★案B2: relayout liveness 用 (Concat 追加入力 producer)
		hoist_set_concat_pmax(l, /*is_add=*/false);   // ★#5 P_max → rsv0 (.pt 経路 real Concat。onnx Concat():1141 と同一。emitMaxPool/emitResize は P_max 未読ゆえ不要)
		cmpFullHeader(l, tid, "Concat"); appendBytes(l, {});
		layerOut[tid]={ch,ap[1],ap[2]}; ++id; ++nlayer; return tid;
	};

	// MaxPool 層 (n.q では Concat 型) を byte-exact 生成 (onnx_save MaxPool() 再現)。union アドレス無し。
	//   p=[ceil_mode=0, kernel, pads, strides]、is[0]=wpp、os HW/strides、linear/weight_m/zoomin/SiLU。
	auto emitMaxPool = [&](int fLayer, int kernel, int pads, int strides)->int{
		int tid=id; std::array<int,3> s=layerOut[fLayer];
		Layer l{}; l.id=(short)tid; l.type=LayerType::Concat; l.f=(short)fLayer;
		l.is[0]=(short)((s[0]+31)/32); l.is[1]=(short)s[0]; l.is[2]=(short)s[1]; l.is[3]=(short)s[2];
		l.os[0]=0; l.os[1]=(short)s[0]; l.os[2]=(short)(s[1]/strides); l.os[3]=(short)(s[2]/strides);
		l.p[0]=0; l.p[1]=(short)kernel; l.p[2]=(short)pads; l.p[3]=(short)strides;
		l.conv.linear=1; l.conv.weight_m=0; l.conv.zoomin=0; l.conv.SiLU=0;
		l.flags |= MAXPOOL_LAYER_EN;   // ★ONNX MaxPool() と対。片方漏れは cmpFullHeader が検出する
		assignAddr(l);
		cmpFullHeader(l, tid, "MaxPool"); appendBytes(l, {});
		layerOut[tid]={s[0], s[1]/strides, s[2]/strides}; ++id; ++nlayer; return tid;
	};

	// Resize/Upsample 層 (n.q では Concat 型) を byte-exact 生成 (onnx_save Resize() 再現)。union アドレス無し。
	//   p=[scale_h, 0, scale_w, 0]、is[0]=wpp、os HW×scale (ch 不変)、linear/weight_m/zoomin/SiLU。
	auto emitResize = [&](int fLayer, int sh, int sw)->int{
		int tid=id; std::array<int,3> s=layerOut[fLayer];
		Layer l{}; l.id=(short)tid; l.type=LayerType::Concat; l.f=(short)fLayer;
		l.is[0]=(short)((s[0]+31)/32); l.is[1]=(short)s[0]; l.is[2]=(short)s[1]; l.is[3]=(short)s[2];
		l.os[0]=0; l.os[1]=(short)s[0]; l.os[2]=(short)(s[1]*sh); l.os[3]=(short)(s[2]*sw);
		l.p[0]=(short)sh; l.p[1]=0; l.p[2]=(short)sw; l.p[3]=0;
		l.conv.linear=1; l.conv.weight_m=0; l.conv.zoomin=0; l.conv.SiLU=0;
		l.flags |= RESIZE_LAYER_EN;    // ★ONNX Resize() と対。片方漏れは cmpFullHeader が検出する
		assignAddr(l);
		cmpFullHeader(l, tid, "Resize"); appendBytes(l, {});
		layerOut[tid]={s[0], s[1]*sh, s[2]*sw}; ++id; ++nlayer; return tid;
	};

	// Attention 層 (PSA) を byte-exact 生成 (onnx_save Attention() milestone1=weightless 再現)。
	//   qkv(=in,256ch) を読み oc=pe out(128) を出力。union=recipe + p[oc,oc,peK,1(dwマーカ)]、weight/bias 無し。
	//   zoomout=data_shift[id] (sidecar)。pe weight 自体は milestone2 (raw 形式) 未実装で現状も bake しない。
	// ★multi-PSA: 各 Attention 層に出現順で region 0,1,... を採番し l.conv.bias に埋める。
	//   kernel data_load が PE_REGION_OFF(region) で per-block pe weight を引く (host が region 別に load)。
	int peRegion = 0;
	auto emitAttention = [&](int qkv, const std::string& peConv)->int{
		int tid=id; std::array<int,3> qs=layerOut[qkv];   // {256,H,W}
		auto wit = ptw.find(peConv+".weight");
		int oc  = (wit!=ptw.end() && wit->second.dims.size()>=3) ? wit->second.dims[0] : qs[0]/2;
		int peK = (wit!=ptw.end() && wit->second.dims.size()>=3) ? wit->second.dims[2] : 3;
		Layer l{}; l.id=(short)tid; l.type=LayerType::Attention; l.f=(short)qkv;
		l.is[0]=0; l.is[1]=(short)qs[0]; l.is[2]=(short)qs[1]; l.is[3]=(short)qs[2];
		l.os[0]=0; l.os[1]=(short)oc; l.os[2]=(short)qs[1]; l.os[3]=(short)qs[2];
		l.p[0]=(short)oc; l.p[1]=(short)oc; l.p[2]=(short)peK; l.p[3]=1;
		l.conv.kernel=(uint8_t)peK; l.conv.k_size=(uint16_t)peK; l.conv.stride=1; l.conv.padding=(uint16_t)(peK/2);
		l.conv.SiLU=0; l.conv.linear=1; l.conv.reserved2=1; l.conv.weight_m=0; l.conv.zoomin=0;
		// ★Attention 出力は kernel が scale 2^(8+ATTN_IO_ZOUT)=2048 固定で emit するため zoomout を明示固定。
		//   従来は id-keyed shift[tid] の既定 {3,3} に依存 (偶然 ATTN_IO_ZOUT=3 と一致) していたのを明示化。
		// l.conv.bias を pe region 番号として転用 (Attention は weightless で bias 未使用)。
		if(peRegion >= PE_MAX_REGIONS)
			printf("  [警告] PSA region %d >= PE_MAX_REGIONS(%d): pe weight 域不足 (tasks.h PE_RESERVE/PE_MAX_REGIONS 拡張要)\n", peRegion, PE_MAX_REGIONS);
		l.conv.zoomout=(int8_t)ATTN_IO_ZOUT; l.conv.bias=(uint16_t)peRegion; l.conv.weight=0;
		printf("  [ATTN-REGION] L%d (f=%d) -> pe region %d\n", tid, qkv, peRegion);
		++peRegion;
		assignAddr(l);
		cmpFullHeader(l, tid, "Attn"); appendBytes(l, {});
		layerOut[tid]={oc, qs[1], qs[2]}; ++id; ++nlayer; return tid;
	};

	// yolo26 入れ子 block を path から再帰 dispatch (yolo26_verify.py proc_block と同型)。
	//   bp = block の module 路 (例 "model.6.m.0")、prevId = 入力 layer、s = 入力 {C,H,W}。
	//   weight key の有無で型判定: PSABlock(.attn.qkv) / C3k(.cv3) / Sequential(.0.*) / Bottleneck(.cv1)。
	//   residual: yolo26 は全 Bottleneck/PSABlock add=True (venv 確認済) かつ channel 保存なので
	//   python の guard `mod.add and x.shape==o2.shape` は ic==oc で再現できる。
	std::function<int(const std::string&, int, std::array<int,3>)> emitBlock =
	    [&](const std::string& bp, int prevId, std::array<int,3> s)->int{
		int H=s[1], W=s[2];
		bool isPSA = ptw.count(bp+".attn.qkv.conv.weight")>0;
		bool isC3k = ptw.count(bp+".cv3.conv.weight")>0;
		bool isSeq = ptw.count(bp+".0.cv1.conv.weight")>0 || ptw.count(bp+".0.attn.qkv.conv.weight")>0;
		if(isPSA){
			// PSABlock: x1 = x + proj(Attn(qkv(x),pe)); out = x1 + ffn(x1)  (act: qkv/proj/pe/ffn.1=False, ffn.0=SiLU)
			int qkv  = emitConv(bp+".attn.qkv.conv", prevId, s, 1, false);
			int attn = emitAttention(qkv, bp+".attn.pe.conv");
			int proj = emitConv(bp+".attn.proj.conv", attn, layerOut[attn], 1, false);
			int c    = layerOut[proj][0];
			int x1   = (c==s[0]) ? emitAdd(prevId, proj) : proj;
			int f0   = emitConv(bp+".ffn.0.conv", x1, layerOut[x1], 1, true);
			int f1   = emitConv(bp+".ffn.1.conv", f0, layerOut[f0], 1, false);
			int out  = (layerOut[f1][0]==layerOut[x1][0]) ? emitAdd(x1, f1) : f1;
			return out;
		}
		if(isC3k){
			// C3k: t1=cv1(x); t1 を Bottleneck 列で更新; t2=cv2(x); cat[chain,t2]; cv3。split 無し (cv1/cv2 共に x)。
			int t1 = emitConv(bp+".cv1.conv", prevId, s, 1, true);
			int ch = layerOut[t1][0];
			int prev = t1, nb=0;
			while(ptw.count(bp+".m."+std::to_string(nb)+".cv1.conv.weight")) ++nb;
			for(int j=0;j<nb;++j) prev = emitBlock(bp+".m."+std::to_string(j), prev, {ch,H,W});
			int t2 = emitConv(bp+".cv2.conv", prevId, s, 1, true);
			int cat = emitConcat({{prev,ch},{t2,ch}});
			int cv3 = emitConv(bp+".cv3.conv", cat, {2*ch,H,W}, 1, true);
			return cv3;
		}
		if(isSeq){
			// Sequential[Bottleneck, PSABlock] (model.22 の m.0): 番号子を順に通す。
			int prev=prevId, nc=0;
			while(ptw.count(bp+"."+std::to_string(nc)+".cv1.conv.weight")>0 || ptw.count(bp+"."+std::to_string(nc)+".attn.qkv.conv.weight")>0){
				prev = emitBlock(bp+"."+std::to_string(nc), prev, layerOut[prev]); ++nc;
			}
			return prev;
		}
		// Bottleneck: cv1 -> cv2、channel 保存 (ic==oc) なら residual Add (yolo26 add=True)。
		int b1 = emitConv(bp+".cv1.conv", prevId, s, 1, true);
		int b2 = emitConv(bp+".cv2.conv", b1, layerOut[b1], 1, true);
		int oc = layerOut[b2][0];
		return (oc==s[0]) ? emitAdd(prevId, b2) : b2;
	};

	// 4) top module を forward 順に dispatch (実装済の型まで。未実装で打ち切り)
	for(auto& m : mods){
		int src = m.f.empty()? m.index-1 : m.f[0];
		int fLayer = (src==-1) ? 0 : (topOut.count(src)? topOut[src] : 0);   // 入力(src==-1)は net default f=0
		std::array<int,3> inShape = (src==-1) ? INPUT : (layerOut.count(fLayer)? layerOut[fLayer] : INPUT);
		std::string mp = "model." + std::to_string(m.index);

		if(m.cls=="Conv" && !m.conv_path.empty()){
			int cid = emitConv(m.conv_path, fLayer, inShape, m.stride, true);
			if(cid<0) break; topOut[m.index]=cid;
		}
		else if(m.cls=="SCDown"){
			// SCDown forward: cv2(cv1(x))。cv1=Conv(1x1 pointwise, SiLU)、cv2=DWConv(k3 s2, act=False)。
			int cv1id = emitConv(mp+".cv1.conv", fLayer, inShape, 1, true);
			if(cv1id<0) break;
			int cv2id = emitConv(mp+".cv2.conv", cv1id, layerOut[cv1id], 2, false);  // depthwise downsample, 活性なし
			if(cv2id<0) break; topOut[m.index]=cv2id;
		}
		else if(m.cls=="Upsample"){
			// Upsample(nearest scale=2) = ONNX Resize。n.q では Concat 型 (channel 不変, HW×2)。byte-exact。
			int cid = emitResize(fLayer, 2, 2);
			topOut[m.index]=cid;
		}
		else if(m.cls=="Concat"){
			// top-level Concat: f=入力リスト (各 top module 出力 layer)。primary=先頭、nch=各 full channel。
			std::vector<std::pair<int,int>> ins; bool okIn=true;
			for(int fk : m.f){
				int prod = topOut.count(fk)? topOut[fk] : -1;
				if(prod<0 || !layerOut.count(prod)){ printf("  L%-3d Concat: 入力 model.%d 未解決 → 打ち切り\n", id, fk); okIn=false; break; }
				ins.push_back({prod, layerOut[prod][0]});
			}
			if(!okIn){ allOk=0; break; }
			int cid = emitConcat(ins);
			topOut[m.index]=cid;
		}
		else if(m.cls=="PSA"){
			// PSA forward: a,b = cv1(x).split(c,c); b = b+attn(b); b = b+ffn(b); cv2(cat[a,b])
			//   attn(b) = proj(Attn(qkv(b)))  [qkv/proj act=False, Attn=LayerType::Attention(pe込)]
			//   ffn = Conv(c->2c, SiLU) + Conv(2c->c, act=False)
			int H=inShape[1], W=inShape[2];
			int cv1 = emitConv(mp+".cv1.conv", fLayer, inShape, 1, true);   // c1 -> 2c
			if(cv1<0) break;
			int twoc = layerOut[cv1][0], c = twoc/2;
			int b    = emitSplit(cv1, twoc, H, W, c, c);  // split out1 (b = ch[c:2c], byte-exact)
			int qkv  = emitConv(mp+".attn.qkv.conv", b, {c,H,W}, 1, false);       // c -> 2c+ (act=False)
			if(qkv<0) break;
			int attn = emitAttention(qkv, mp+".attn.pe.conv");  // qkv_oc -> c (pe depthwise 込, weightless)
			int proj = emitConv(mp+".attn.proj.conv", attn, {c,H,W}, 1, false);   // c -> c (act=False)
			if(proj<0) break;
			int add1 = emitAdd(b, proj);                                         // b + attn(=proj)
			int ffn0 = emitConv(mp+".ffn.0.conv", add1, {c,H,W}, 1, true);        // c -> 2c (SiLU)
			if(ffn0<0) break;
			int ffn0oc = layerOut[ffn0][0];
			int ffn1 = emitConv(mp+".ffn.1.conv", ffn0, {ffn0oc,H,W}, 1, false);  // 2c -> c (act=False)
			if(ffn1<0) break;
			int add2 = emitAdd(add1, ffn1);                                      // add1 + ffn
			int cat  = emitConcat({{cv1,c},{add2,c}});                           // cat[a(cv1 view), b_final]
			int cv2  = emitConv(mp+".cv2.conv", cat, {2*c,H,W}, 1, true);         // 2c -> c1
			if(cv2<0) break; topOut[m.index]=cv2;
		}
		else if(m.cls=="SPPF"){
			// SPPF forward: y=[cv1(x)]; y.extend(MaxPool(y[-1]) x3); cv2(cat(y)); (+x if add)
			//   MaxPool(k5 s1 p2) は n.q では Concat 型 (channel 不変) で表現。cat = 4*c。
			//   yolo26 は residual あり (m.add=True): cv2(cat)+x。channel 保存時のみ Add を出す。
			int H=inShape[1], W=inShape[2];
			// ★cv1 活性: yolov10 SPPF.cv1=SiLU だが yolo26 SPPF.cv1=Identity (act 無し、venv 実機確認)。
			//   旧コードは act=true 固定で yolo26 の cv1 に誤って SiLU を適用 → model.9 corr 0.84 / model.10 劣化。
			//   board 実測 + 固定小数突合で確定。emitConv は act 引数を直接使う。
			int cv1id = emitConv(mp+".cv1.conv", fLayer, inShape, 1, !yolo26);
			if(cv1id<0) break;
			int c = layerOut[cv1id][0];
			int prev = cv1id; std::vector<int> mps;
			for(int p=0;p<3;++p){ prev = emitMaxPool(prev, 5, 2, 1); mps.push_back(prev); }   // MaxPool(k5 s1 p2) x3 (byte-exact)
			std::vector<std::pair<int,int>> ins = {{cv1id,c}};
			for(int x : mps) ins.push_back({x, c});
			int catid = emitConcat(ins);                                               // cat[cv1,mp1,mp2,mp3]
			int cv2id = emitConv(mp+".cv2.conv", catid, {4*c,H,W}, 1, true);
			if(cv2id<0) break;
			if(m.add && layerOut[cv2id][0]==inShape[0]) topOut[m.index]=emitAdd(cv2id, fLayer);  // yolo26 residual
			else topOut[m.index]=cv2id;
		}
		else if((m.cls=="C2f" || m.cls=="C3k2") && (m.bneck_cls.empty() || m.bneck_cls=="Bottleneck")){
			// C2f / C3k2(c3k=False=Bottleneck) forward: y=cv1(x).chunk(2); y.extend(bottleneck); cv2(cat(y))
			//   cv1 → Split out1(線形conv, ch[c:2c]) → {bottleneck.cv1,cv2,Add(shortcut)}×n → Concat → cv2
			//   C3k2(c3k=True=C3k bottleneck) は m.0 が C3k で内部構造が違う (cv1/cv2/m を持つ) ため別 emitter 必須。
			//   ★ C2f path で扱うと C3k の cv1/cv2 を Bottleneck conv と誤認し silently 誤 emit するので bneck_cls で除外。
			int H=inShape[1], W=inShape[2];
			int cv1id = emitConv(mp+".cv1.conv", fLayer, inShape, 1, true);
			if(cv1id<0) break;
			int twoc = layerOut[cv1id][0], c = twoc/2;
			int splitid = emitSplit(cv1id, twoc, H, W, c, c);   // Split out1 = ch[c:2c] (byte-exact)
			int prev = splitid, n=0;
			while(ptw.count(mp+".m."+std::to_string(n)+".cv1.conv.weight")) ++n;
			std::vector<int> btlOut;
			for(int j=0;j<n;++j){
				int b1 = emitConv(mp+".m."+std::to_string(j)+".cv1.conv", prev, {c,H,W}, 1, true); if(b1<0){ allOk=0; break; }
				int b2 = emitConv(mp+".m."+std::to_string(j)+".cv2.conv", b1,   {c,H,W}, 1, true); if(b2<0){ allOk=0; break; }
				// shortcut あり (backbone) は Add(prev+b2)、無し (neck) は bottleneck 出力=b2 で Add 層を出さない。
				prev = m.shortcut ? emitAdd(prev, b2) : b2;
				btlOut.push_back(prev);
			}
			// Concat y=[cv1 out0 view(c), split-out1(c), 各 bottleneck 出力(c)]
			std::vector<std::pair<int,int>> ins = {{cv1id,c},{splitid,c}};
			for(int o : btlOut) ins.push_back({o, c});
			int catid = emitConcat(ins);
			int cv2id = emitConv(mp+".cv2.conv", catid, {(2+n)*c,H,W}, 1, true);
			if(cv2id<0) break; topOut[m.index]=cv2id;
		}
		else if(m.cls=="C3k2"){
			// C3k2(c3k=True): m.0 が C3k (model.6/8/13/16/19) または Sequential[Bottleneck,PSABlock] (model.22)。
			//   外形は C2f と同じ: cv1→chunk(2)=[a,b]; 各 m.j を emitBlock で b に適用し y へ; cat→cv2。
			//   (Bottleneck 列の C3k2 は上の専用 branch で処理済。ここは C3k / Sequential のみ。)
			int H=inShape[1], W=inShape[2];
			int cv1id = emitConv(mp+".cv1.conv", fLayer, inShape, 1, true);
			if(cv1id<0) break;
			int twoc = layerOut[cv1id][0], c = twoc/2;
			int splitid = emitSplit(cv1id, twoc, H, W, c, c);   // Split out1 = ch[c:2c] (byte-exact)
			// m.j の個数: m.j は C3k / Bottleneck / Sequential いずれも取り得るので block 存在で数える。
			auto blockExists = [&](const std::string& p)->bool{
				return ptw.count(p+".cv1.conv.weight")>0 || ptw.count(p+".attn.qkv.conv.weight")>0
				    || ptw.count(p+".0.cv1.conv.weight")>0 || ptw.count(p+".0.attn.qkv.conv.weight")>0;
			};
			int prev = splitid, n=0;
			while(blockExists(mp+".m."+std::to_string(n))) ++n;
			std::vector<int> blkOut;
			for(int j=0;j<n;++j){ prev = emitBlock(mp+".m."+std::to_string(j), prev, {c,H,W}); blkOut.push_back(prev); }
			std::vector<std::pair<int,int>> ins = {{cv1id,c},{splitid,c}};
			for(int o : blkOut) ins.push_back({o, layerOut[o][0]});
			int catc = 2*c; for(int o : blkOut) catc += layerOut[o][0];
			int catid = emitConcat(ins);
			int cv2id = emitConv(mp+".cv2.conv", catid, {catc,H,W}, 1, true);
			if(cv2id<0) break; topOut[m.index]=cv2id;
		}
		else if(m.cls=="C2PSA"){
			// C2PSA forward: a,b = cv1(x).split(c,c); b = PSABlock(m.0, b); cv2(cat[a,b])。
			//   PSABlock は emitBlock(model.X.m.0) が attn(qkv/pe/proj)+residual+ffn を展開。
			int H=inShape[1], W=inShape[2];
			int cv1id = emitConv(mp+".cv1.conv", fLayer, inShape, 1, true);
			if(cv1id<0) break;
			int twoc = layerOut[cv1id][0], c = twoc/2;
			int splitid = emitSplit(cv1id, twoc, H, W, c, c);   // b = ch[c:2c]
			int out = emitBlock(mp+".m.0", splitid, {c,H,W});   // PSABlock(b)
			int catid = emitConcat({{cv1id,c},{out, layerOut[out][0]}});  // cat[a(cv1 view), b_final]
			int cv2id = emitConv(mp+".cv2.conv", catid, {c+layerOut[out][0],H,W}, 1, true);
			if(cv2id<0) break; topOut[m.index]=cv2id;
		}
		else if(m.cls=="C2fCIB"){
			// C2fCIB = C2f だが bottleneck が CIB ブロック (dw3 → pw(c→2c) → dw7(RepVGGDW) → pw(2c→c) → dw3)。
			int H=inShape[1], W=inShape[2];
			int cv1 = emitConv(mp+".cv1.conv", fLayer, inShape, 1, true);
			if(cv1<0) break;
			int twoc=layerOut[cv1][0], c=twoc/2;
			int split = emitSplit(cv1, twoc, H, W, c, c);
			std::string b0=mp+".m.0.cv1";
			int b = emitConv(b0+".0.conv", split, {c,H,W}, 1, true);   if(b<0) break;   // dw3 c->c
			b = emitConv(b0+".1.conv", b, {c,H,W}, 1, true);           if(b<0) break;   // pw  c->2c
			int c2 = layerOut[b][0];                                                    // 2c
			{ std::vector<float> Wf,Bf; int roc,rk;                                      // dw7 (RepVGGDW 融合)
			  if(!fuseRepVGGDW(b0+".2", Wf, Bf, roc, rk)){ printf("  RepVGGDW 融合失敗 %s\n", (b0+".2").c_str()); allOk=0; break; }
			  b = emitConvCore(Wf, Bf, roc, roc, rk, b, {c2,H,W}, 1, true, true); }
			b = emitConv(b0+".3.conv", b, {c2,H,W}, 1, true);          if(b<0) break;   // pw  2c->c
			b = emitConv(b0+".4.conv", b, {c,H,W}, 1, true);           if(b<0) break;   // dw3 c->c
			if(m.shortcut) b = emitAdd(split, b);                                       // CIB residual
			int catid = emitConcat({{cv1,c},{split,c},{b,c}});
			int cv2 = emitConv(mp+".cv2.conv", catid, {3*c,H,W}, 1, true);
			if(cv2<0) break; topOut[m.index]=cv2;
		}
		else if(m.cls=="v10Detect" || m.cls=="Detect"){
			// v10Detect / yolo26 Detect (end2end one2one head): スケール毎に one2one_cv2(box: k3,k3,k1)
			//   + one2one_cv3(cls: [dw3,pw]×2, k1) + Concat。yolo26 は one2many cv2/cv3 も持つが
			//   推論は one2one (NMS-free) を使うので one2one のみ emit (yolo26_verify.py と一致)。
			//   最終 .2 は bare nn.Conv2d (BN 無し, act=False)。dfl/decode は golden 範囲外。
			bool fail=false;
			for(size_t s=0; s<m.f.size() && !fail; ++s){
				int feat = topOut.count(m.f[s])? topOut[m.f[s]] : -1;
				if(feat<0 || !layerOut.count(feat)){ printf("  Detect: scale %zu の feat model.%d 未解決\n", s, m.f[s]); fail=true; break; }
				std::array<int,3> fs = layerOut[feat];
				std::string c2=mp+".one2one_cv2."+std::to_string(s), c3=mp+".one2one_cv3."+std::to_string(s);
				int b2 = emitConv(c2+".0.conv", feat, fs, 1, true);        if(b2<0){fail=true;break;}  // k3
				b2 = emitConv(c2+".1.conv", b2, layerOut[b2], 1, true);    if(b2<0){fail=true;break;}  // k3
				b2 = emitConv(c2+".2",      b2, layerOut[b2], 1, false);   if(b2<0){fail=true;break;}  // k1 bare(act=False)
				int b3 = emitConv(c3+".0.0.conv", feat, fs, 1, true);      if(b3<0){fail=true;break;}  // dw3
				b3 = emitConv(c3+".0.1.conv", b3, layerOut[b3], 1, true);  if(b3<0){fail=true;break;}  // pw
				b3 = emitConv(c3+".1.0.conv", b3, layerOut[b3], 1, true);  if(b3<0){fail=true;break;}  // dw3
				b3 = emitConv(c3+".1.1.conv", b3, layerOut[b3], 1, true);  if(b3<0){fail=true;break;}  // pw
				b3 = emitConv(c3+".2",        b3, layerOut[b3], 1, false); if(b3<0){fail=true;break;}  // k1 bare(act=False)
				emitConcat({{b2, layerOut[b2][0]}, {b3, layerOut[b3][0]}});
			}
			if(fail) break;
			// dfl/decode (Reshape/Transpose/Softmax) は LayerType 化されず golden に無い → ここで終了。
			break;
		}
		else {
			printf("  -- model.%d (%s) で打ち切り: 生成 %d 層。次の emitter (SCDown/SPPF/C2PSA/neck) が必要。\n",
			       m.index, m.cls.c_str(), nlayer);
			break;
		}
	}
	printf("  generated layers=%d, 全突合フィールド一致(in/out/f 含む)=%s\n", nlayer, allOk?"YES":"NO");
	printf("  Conv=header byte-exact + weight(±LSB f16) / 構造層(Split/Add/Concat/MaxPool/Resize/Attention)=full byte-exact / in/out/f=planner byte-exact\n");

	// === liveness relayout (常時経由・全アドレス確定機構) ===
	//   ★2026-07-06 統一: pt 経路も常時 relayout_liveness() を通す(旧 HEAD_RESERVE_FRONT/MAP_PIN gate 撤去)。
	//   これで onnx_save 経路(LIVENESS_ALLOC 常時 ON)と揃い、bump 値は最終 n.q に出ず relayout が唯一の権威。
	//   floor は build config で自動選択(reserveLo): HEAD_RESERVE_FRONT=91904(前方域=入力2+head2組, decode+overlay
	//   head TEE 用/L145 恒久修正)/ FREERUN=86016 / それ以外=OUT_ALIGN_BASE(production yolo26 も liveness 配置)。
	//   env MAP_PIN_HEADS 指定時は relayout 内で neck head を lastUse=n 末尾保持(16MB mAP dump、pin は forward 摂動)。
	//   正当性は RELAYOUT_SELFCHECK / OVERREAD_CHECK で offline 検証(SELFCHECK=0/GT=0/OVERREAD=0)。
	//   PE/HB anchor 不変 = 再 csynth 不要、既存 firmware にそのまま deploy。
	if(gen_out && gen_out[0]){
		std::vector<Layer*> L;
		size_t off=0;
		for(int k=0; k<nlayer && off+sizeof(Layer)<=nqOut.size(); ++k){
			Layer* lp=(Layer*)(nqOut.data()+off); L.push_back(lp);
			uint32_t adv=8; if(lp->type==LayerType::Conv) adv+=(uint32_t)lp->conv.bias+(uint32_t)lp->conv.weight;
			off += (size_t)adv*sizeof(GMEM_T);
		}
		relayout_liveness(L, g_extra, (size_t)Y26_IN_STRIDE);   // geo640: 第1層入力 word 数(384×640 → 30720)
		const char* mp = getenv("MAP_PIN_HEADS");
		printf("  ★liveness relayout 適用 (%zu 層%s)\n", L.size(), mp?" + MAP_PIN 末尾保持":"");
	}

	// === 実 n.q 書き出し (--gen) ===
	//   生成層列 (nqOut) の末尾に Finish[8] (8 個の Finish Layer = 512B) を付けて file_save。
	//   golden と違い model.0..21 までの prefix だが、Finish 終端で有効な層連鎖として成立する。
	if(gen_out && gen_out[0]){
		for(int k=0;k<8;++k){
			Layer fin; std::memset(&fin,0,sizeof(Layer));
			fin.type = LayerType::Finish; fin.id = (short)(id+k);
			size_t b = nqOut.size(); nqOut.resize(b+sizeof(Layer));
			std::memcpy(nqOut.data()+b, &fin, sizeof(Layer));
		}
		file_save(gen_out, nqOut.data(), (int)nqOut.size());
		printf("  --gen: %s に書き出し (%zu bytes = %d 層 + Finish[8])\n", gen_out, nqOut.size(), nlayer);
	}
	return allOk?0:1;
}
#endif // #if defined(VSCODE)

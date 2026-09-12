// SPDX-License-Identifier: AGPL-3.0-or-later
// pt_reader.cpp — .pt (PyTorch torch.save 形式 = ZIP+pickle) 取り込み
//
// 参考実装 (pt_analyzer / pt_tree.cpp) を本プロジェクトへ取り込み、
// main() を外して Phase A(階層表示/.ptb 出力) と Phase B 用の重み収集 API を公開したもの。
// C++17 標準ライブラリのみ (ZIP は無圧縮 store のため zlib 不要)。LibTorch 非依存なので
// ultralytics.nn.tasks.* の Python クラス定義が無くても解析できる。
//
// 仕組み:
//   1. ZIP 中央ディレクトリを辿って best/data.pkl を取り出す
//   2. pickle protocol 2 のスタックマシンで、クラスは実体化せず汎用ノード(Value)の木を構築
//   3. model 以下の _modules / _parameters / _buffers を再帰的に辿る
//
// 本ファイルは ONNX に依存しない (yolo26n のように .onnx が無いモデルでも動く)。
// Phase B の ONNX 突合 (pt_verify) は ONNX protobuf を要するため src/nq.cpp 側に置く。
//
#if defined(VSCODE)

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pt_format.h"
#include "pt_reader.h"

namespace {  // 内部リンケージ (他 TU の同名シンボルと衝突させない)

// ---------------------------------------------------------------------------
// pickle の値を表す汎用ノード
// ---------------------------------------------------------------------------
struct Value;
using VP = std::shared_ptr<Value>;

enum class Kind {
    None, Bool, Int, Double, Str,
    Tuple, List, Dict,
    Global,   // クラス/関数への参照 (module + name)
    Object,   // NEWOBJ / REDUCE で生成 (callable + args + state)
    PersId,   // 永続ID (テンソルストレージ参照)
    Mark
};

struct Value {
    Kind kind = Kind::None;
    long long i = 0;
    double d = 0;
    bool b = false;
    std::string s;        // Str / Global の name
    std::string module;   // Global の module
    std::vector<VP> seq;                       // Tuple / List
    std::vector<std::pair<VP, VP>> dict;       // Dict (挿入順)
    VP callable, args, state;                  // Object
    VP pid;                                    // PersId のペイロード (Tuple)
};

VP mk(Kind k) { auto v = std::make_shared<Value>(); v->kind = k; return v; }

// ---------------------------------------------------------------------------
// ZIP 読み取り (無圧縮 store 専用、ZIP64 非対応 — 4GB 未満なら十分)
// ---------------------------------------------------------------------------
class Zip {
public:
    explicit Zip(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("ファイルを開けません: " + path);
        buf_.assign(std::istreambuf_iterator<char>(f), {});
        parseCentralDirectory();
    }
    std::vector<uint8_t> readEndsWith(const std::string& suffix) const {
        for (const auto& [name, e] : entries_)
            if (name.size() >= suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                return readEntry(e);
        throw std::runtime_error("エントリが見つかりません: *" + suffix);
    }
private:
    struct Entry { uint32_t localOffset, size; };
    std::vector<uint8_t> buf_;
    std::map<std::string, Entry> entries_;
    uint16_t u16(size_t p) const { return buf_[p] | (buf_[p + 1] << 8); }
    uint32_t u32(size_t p) const {
        return buf_[p] | (buf_[p + 1] << 8) | (buf_[p + 2] << 16) | (uint32_t(buf_[p + 3]) << 24);
    }
    void parseCentralDirectory() {
        const uint32_t EOCD = 0x06054b50;
        size_t eocd = std::string::npos;
        for (size_t p = buf_.size() >= 22 ? buf_.size() - 22 : 0; ; --p) {
            if (u32(p) == EOCD) { eocd = p; break; }
            if (p == 0) break;
        }
        if (eocd == std::string::npos) throw std::runtime_error("EOCD が見つかりません (ZIP ではない?)");
        uint32_t cdOffset = u32(eocd + 16);
        uint16_t nEntries = u16(eocd + 10);
        size_t p = cdOffset;
        for (uint16_t i = 0; i < nEntries; ++i) {
            if (u32(p) != 0x02014b50) throw std::runtime_error("中央ディレクトリ破損");
            uint16_t nameLen   = u16(p + 28);
            uint16_t extraLen  = u16(p + 30);
            uint16_t commLen   = u16(p + 32);
            uint32_t localOff  = u32(p + 42);
            uint32_t compSize  = u32(p + 20);
            std::string name(reinterpret_cast<const char*>(&buf_[p + 46]), nameLen);
            entries_[name] = {localOff, compSize};
            p += 46 + nameLen + extraLen + commLen;
        }
    }
    std::vector<uint8_t> readEntry(const Entry& e) const {
        size_t p = e.localOffset;
        if (u32(p) != 0x04034b50) throw std::runtime_error("ローカルヘッダ破損");
        uint16_t nameLen  = u16(p + 26);
        uint16_t extraLen = u16(p + 28);
        size_t dataStart = p + 30 + nameLen + extraLen;
        return std::vector<uint8_t>(buf_.begin() + dataStart, buf_.begin() + dataStart + e.size);
    }
};

// ---------------------------------------------------------------------------
// pickle スタックマシン (protocol 2、必要なオペコードのみ)
// ---------------------------------------------------------------------------
class Unpickler {
public:
    explicit Unpickler(const std::vector<uint8_t>& data) : d_(data) {}
    VP run() {
        while (p_ < d_.size()) {
            uint8_t op = d_[p_++];
            switch (op) {
            case 0x80: r1(); break;
            case '.': return pop();
            case '(': marks_.push_back(stack_.size()); break;
            case 'N': push(mk(Kind::None)); break;
            case 0x88: pushBool(true); break;
            case 0x89: pushBool(false); break;
            case 'K': pushInt(r1()); break;
            case 'M': pushInt(r2()); break;
            case 'J': pushInt(int32()); break;
            case 'G': pushDouble(binfloat()); break;
            case 'X': pushStr(binunicode()); break;
            case 'q': memo_[r1()] = top(); break;
            case 'r': memo_[r4()] = top(); break;
            case 'h': push(memo_.at(r1())); break;
            case 'j': push(memo_.at(r4())); break;
            case ')': push(mk(Kind::Tuple)); break;
            case 't': makeTuple(popToMark()); break;
            case 0x85: makeTupleN(1); break;
            case 0x86: makeTupleN(2); break;
            case 0x87: makeTupleN(3); break;
            case ']': push(mk(Kind::List)); break;
            case 'e': appends(popToMark()); break;
            case 'a': { VP v = pop(); top()->seq.push_back(v); } break;
            case '}': push(mk(Kind::Dict)); break;
            case 'u': setitems(popToMark()); break;
            case 's': { VP v = pop(), k = pop(); top()->dict.emplace_back(k, v); } break;
            case 'c': global(); break;
            case 'R': reduce(); break;
            case 0x81: reduce(); break;
            case 'b': { VP st = pop(); top()->state = st; } break;
            case 'Q': { VP t = pop(); VP v = mk(Kind::PersId); v->pid = t; push(v); } break;
            default:
                throw std::runtime_error("未対応のオペコード 0x" + toHex(op) +
                    " (offset " + std::to_string(p_ - 1) + ")");
            }
        }
        throw std::runtime_error("STOP に到達せず終了");
    }
private:
    const std::vector<uint8_t>& d_;
    size_t p_ = 0;
    std::vector<VP> stack_;
    std::vector<size_t> marks_;
    std::unordered_map<uint32_t, VP> memo_;
    uint8_t  r1() { return d_[p_++]; }
    uint16_t r2() { uint16_t v = d_[p_] | (d_[p_ + 1] << 8); p_ += 2; return v; }
    uint32_t r4() { uint32_t v = d_[p_] | (d_[p_+1]<<8) | (d_[p_+2]<<16) | (uint32_t(d_[p_+3])<<24); p_ += 4; return v; }
    int32_t  int32() { return static_cast<int32_t>(r4()); }
    double binfloat() { uint8_t b[8]; for (int i = 0; i < 8; ++i) b[7 - i] = d_[p_++]; double v; std::memcpy(&v, b, 8); return v; }
    std::string binunicode() { uint32_t n = r4(); std::string s(reinterpret_cast<const char*>(&d_[p_]), n); p_ += n; return s; }
    void push(VP v) { stack_.push_back(std::move(v)); }
    VP   top()      { return stack_.back(); }
    VP   pop()      { VP v = stack_.back(); stack_.pop_back(); return v; }
    std::vector<VP> popToMark() {
        size_t m = marks_.back(); marks_.pop_back();
        std::vector<VP> out(stack_.begin() + m, stack_.end());
        stack_.resize(m); return out;
    }
    void pushBool(bool b)    { auto v = mk(Kind::Bool);   v->b = b; push(v); }
    void pushInt(long long i){ auto v = mk(Kind::Int);    v->i = i; push(v); }
    void pushDouble(double x){ auto v = mk(Kind::Double); v->d = x; push(v); }
    void pushStr(std::string s){ auto v = mk(Kind::Str);  v->s = std::move(s); push(v); }
    void makeTuple(std::vector<VP> items) { auto v = mk(Kind::Tuple); v->seq = std::move(items); push(v); }
    void makeTupleN(int n) { auto v = mk(Kind::Tuple); v->seq.resize(n); for (int i = n - 1; i >= 0; --i) v->seq[i] = pop(); push(v); }
    void appends(const std::vector<VP>& items) { VP lst = top(); for (auto& it : items) lst->seq.push_back(it); }
    void setitems(const std::vector<VP>& kv) { VP dct = top(); for (size_t i = 0; i + 1 < kv.size(); i += 2) dct->dict.emplace_back(kv[i], kv[i + 1]); }
    void global() { auto v = mk(Kind::Global); v->module = readline(); v->s = readline(); push(v); }
    std::string readline() { std::string s; while (d_[p_] != '\n') s += char(d_[p_++]); ++p_; return s; }
    void reduce() {
        VP args = pop(), callable = pop();
        auto v = mk(Kind::Object); v->callable = callable; v->args = args;
        // collections.OrderedDict は dict として扱う (古い checkpoint 例 yolov10n.pt は
        // _modules/_parameters/_buffers が OrderedDict)。中身は後続 SETITEM(S) で v->dict に積まれる。
        if (callable && callable->kind == Kind::Global && callable->s == "OrderedDict") v->kind = Kind::Dict;
        push(v);
    }
    static std::string toHex(uint8_t b) { const char* h = "0123456789abcdef"; return std::string{h[b >> 4], h[b & 0xf]}; }
};

// ---------------------------------------------------------------------------
// 共通ヘルパ
// ---------------------------------------------------------------------------
std::string dtypeName(const std::string& storage) {
    static const std::map<std::string, std::string> m = {
        {"HalfStorage","f16"},{"FloatStorage","f32"},{"DoubleStorage","f64"},{"BFloat16Storage","bf16"},
        {"LongStorage","i64"},{"IntStorage","i32"},{"ShortStorage","i16"},
        {"CharStorage","i8"},{"ByteStorage","u8"},{"BoolStorage","bool"},
    };
    auto it = m.find(storage); return it != m.end() ? it->second : storage;
}
pt_dtype dtypeEnum(const std::string& storage) {
    static const std::map<std::string, pt_dtype> m = {
        {"HalfStorage",PT_F16},{"FloatStorage",PT_F32},{"DoubleStorage",PT_F64},{"BFloat16Storage",PT_BF16},
        {"LongStorage",PT_I64},{"IntStorage",PT_I32},{"ShortStorage",PT_I16},
        {"CharStorage",PT_I8},{"ByteStorage",PT_U8},{"BoolStorage",PT_BOOL},
    };
    auto it = m.find(storage); return it != m.end() ? it->second : PT_DT_UNKNOWN;
}

struct TensorInfo {
    bool ok = false;
    std::string dtype, storage, storageKey;
    long long storageOffset = 0;
    std::vector<long long> shape;
};

TensorInfo tensorOf(const VP& v) {
    if (!v || v->kind != Kind::Object || !v->callable) return {};
    const std::string& fn = v->callable->s;
    if (fn == "_rebuild_parameter") {
        if (v->args && !v->args->seq.empty()) return tensorOf(v->args->seq[0]);
        return {};
    }
    if (fn == "_rebuild_tensor_v2" || fn == "_rebuild_tensor") {
        TensorInfo t; t.ok = true;
        const auto& a = v->args->seq;
        if (a.size() > 1 && a[1]->kind == Kind::Int) t.storageOffset = a[1]->i;
        if (a.size() > 2 && a[2]->kind == Kind::Tuple)
            for (auto& e : a[2]->seq) t.shape.push_back(e->i);
        if (!a.empty() && a[0]->kind == Kind::PersId) {
            const VP& pid = a[0]->pid;
            if (pid && pid->seq.size() >= 3) {
                if (pid->seq[1]->kind == Kind::Global) { t.storage = pid->seq[1]->s; t.dtype = dtypeName(t.storage); }
                if (pid->seq[2]->kind == Kind::Str) t.storageKey = pid->seq[2]->s;
            }
        }
        return t;
    }
    return {};
}

// dict ペアを kind 非依存で走査する。理由: 古い checkpoint (例 yolov10n.pt v8.2.30) は
// _modules を OrderedDict として格納し、pickle 上 REDUCE→Object になる。その場合 SETITEMS で
// 入ったペアは Value::dict に積まれるが kind は Object のため、Kind::Dict 限定だと辿れない。
// 通常 dict (EMPTY_DICT) も OrderedDict(Object) も dict ベクタを見れば一様に扱える。
VP dictGet(const VP& dict, const std::string& key) {
    if (!dict) return nullptr;
    for (auto& [k, val] : dict->dict)
        if (k && k->kind == Kind::Str && k->s == key) return val;
    return nullptr;
}

std::string shapeStr(const TensorInfo& t) {
    std::string s = "(";
    for (size_t i = 0; i < t.shape.size(); ++i) { if (i) s += ", "; s += std::to_string(t.shape[i]); }
    if (t.shape.size() == 1) s += ",";
    s += ")"; return s;
}

float halfToFloat(uint16_t h) {
    uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu, f;
    if (exp == 0) {
        if (mant == 0) f = sign;
        else { exp = 127 - 15 + 1; while (!(mant & 0x400u)) { mant <<= 1; --exp; } mant &= 0x3FFu; f = sign | (exp << 23) | (mant << 13); }
    } else if (exp == 0x1F) { f = sign | 0x7F800000u | (mant << 13); }
    else { f = sign | ((exp - 15 + 127) << 23) | (mant << 13); }
    float out; std::memcpy(&out, &f, 4); return out;
}
int dtypeBytes(pt_dtype dt) {
    switch (dt) {
        case PT_F16: case PT_BF16: case PT_I16: return 2;
        case PT_F32: case PT_I32:               return 4;
        case PT_F64: case PT_I64:               return 8;
        case PT_I8: case PT_U8: case PT_BOOL:   return 1;
        default: return 0;
    }
}
std::vector<double> decodeStorage(const std::vector<uint8_t>& raw, pt_dtype dt, long long offset, long long numel) {
    std::vector<double> out; int esz = dtypeBytes(dt);
    if (esz == 0) return out;
    size_t byteOff = static_cast<size_t>(offset) * esz; out.reserve(numel);
    for (long long i = 0; i < numel; ++i) {
        const uint8_t* p = &raw[byteOff + i * esz];
        switch (dt) {
            case PT_F16:  { uint16_t v; std::memcpy(&v,p,2); out.push_back(halfToFloat(v)); } break;
            case PT_BF16: { uint16_t v; std::memcpy(&v,p,2); uint32_t f=uint32_t(v)<<16; float ff; std::memcpy(&ff,&f,4); out.push_back(ff); } break;
            case PT_F32:  { float v;    std::memcpy(&v,p,4); out.push_back(v); } break;
            case PT_F64:  { double v;   std::memcpy(&v,p,8); out.push_back(v); } break;
            case PT_I64:  { int64_t v;  std::memcpy(&v,p,8); out.push_back(double(v)); } break;
            case PT_I32:  { int32_t v;  std::memcpy(&v,p,4); out.push_back(double(v)); } break;
            case PT_I16:  { int16_t v;  std::memcpy(&v,p,2); out.push_back(double(v)); } break;
            case PT_I8:   { int8_t  v = (int8_t)*p; out.push_back(double(v)); } break;
            case PT_U8: case PT_BOOL: out.push_back(double(*p)); break;
            default: break;
        }
    }
    return out;
}
std::vector<uint8_t> quantize(const std::vector<double>& vals, float& qmin, float& qmax) {
    double mn = 1e300, mx = -1e300;
    for (double v : vals) { if (v < mn) mn = v; if (v > mx) mx = v; }
    if (vals.empty()) { mn = mx = 0; }
    qmin = static_cast<float>(mn); qmax = static_cast<float>(mx);
    double range = mx - mn; std::vector<uint8_t> q(vals.size());
    for (size_t i = 0; i < vals.size(); ++i) {
        double s = range > 0 ? (vals[i] - mn) / range * 255.0 : 0.0;
        long r = std::lround(s);
        q[i] = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
    }
    return q;
}

VP childModule(const VP& state, const std::string& name) { return dictGet(dictGet(state, "_modules"), name); }
long long convOutChannels(const VP& wrapState) {
    VP conv = childModule(wrapState, "conv"); if (!conv) return 0;
    TensorInfo t = tensorOf(dictGet(dictGet(conv->state, "_parameters"), "weight"));
    return (t.ok && !t.shape.empty()) ? t.shape[0] : 0;
}
int convStride(const VP& wrapState) {
    VP conv = childModule(wrapState, "conv"); if (!conv) return 1;
    VP s = dictGet(conv->state, "stride");
    if (s && s->kind == Kind::Tuple && !s->seq.empty()) return static_cast<int>(s->seq[0]->i);
    return 1;
}

// ---------------------------------------------------------------------------
// 階層表示 (Phase A)
// ---------------------------------------------------------------------------
struct Stats {
    long long tensors = 0, elements = 0;
    std::unordered_set<const Value*> uniqueModules;
    long long modules() const { return static_cast<long long>(uniqueModules.size()); }
};
void printTensors(const VP& container, const char* tag, int depth, Stats& st) {
    if (!container || container->kind != Kind::Dict) return;
    std::string pad(depth * 2, ' ');
    for (auto& [k, val] : container->dict) {
        if (!val || val->kind == Kind::None) continue;
        TensorInfo t = tensorOf(val); if (!t.ok) continue;
        long long n = 1; for (auto s : t.shape) n *= s;
        st.tensors++; st.elements += n;
        std::cout << pad << "    [" << tag << "] " << k->s << "  "
                  << (t.dtype.empty() ? "?" : t.dtype) << " " << shapeStr(t) << "\n";
    }
}
void printModule(const VP& mod, const std::string& name, int depth, Stats& st) {
    std::string pad(depth * 2, ' ');
    std::string cls = (mod && mod->callable) ? mod->callable->s : "?";
    std::cout << pad << name << ": " << cls << "\n";
    if (mod) st.uniqueModules.insert(mod.get());
    const VP& state = mod ? mod->state : nullptr;
    printTensors(dictGet(state, "_parameters"), "param", depth, st);
    printTensors(dictGet(state, "_buffers"),    "buf",   depth, st);
    VP mods = dictGet(state, "_modules");
    if (mods && mods->kind == Kind::Dict)
        for (auto& [k, child] : mods->dict) printModule(child, k->s, depth + 1, st);
}

// ---------------------------------------------------------------------------
// 活性化メモリアドレス計画 + バイナリ出力 (.ptb, pt_format.h v5)
// ---------------------------------------------------------------------------
struct LayerAddr { uint16_t in[PT_MAX_INPUTS] = {0,0,0,0}; uint16_t out = 0; };
struct Shape { long long c = 0, h = 0, w = 0; bool set = false; };

std::map<int, LayerAddr> planAddresses(const VP& model, long long inC, long long inH, long long inW) {
    std::map<int, LayerAddr> addr; std::map<int, Shape> shp; std::map<int, long long> outAddr;
    shp[-1] = {inC, inH, inW, true}; outAddr[-1] = 1;
    VP seqWrap = childModule(model->state, "model");
    VP mods = seqWrap ? dictGet(seqWrap->state, "_modules") : nullptr;
    if (!mods || mods->kind != Kind::Dict) return addr;
    for (auto& [k, layer] : mods->dict) {
        int i = std::atoi(k->s.c_str());
        std::string cls = layer->callable ? layer->callable->s : "";
        const VP& st = layer->state;
        VP fv = dictGet(st, "f"); std::vector<int> flist;
        if (fv) {
            if (fv->kind == Kind::Int) flist.push_back(static_cast<int>(fv->i));
            else if (fv->kind == Kind::List || fv->kind == Kind::Tuple)
                for (auto& e : fv->seq) flist.push_back(static_cast<int>(e->i));
        }
        if (flist.empty()) flist.push_back(-1);
        std::vector<int> src; for (int s : flist) src.push_back(s == -1 ? i - 1 : s);
        long long inVol = 0; for (int s : src) { Shape& ss = shp[s]; inVol += ss.c * ss.h * ss.w; }
        Shape primary = shp[src[0]]; Shape o; o.set = true;
        if (cls == "Conv" || cls == "Conv2" || cls == "DWConv") {
            int st2 = convStride(st); o.c = convOutChannels(st); o.h = primary.h / st2; o.w = primary.w / st2;
        } else if (cls == "Upsample") {
            long long sc = 2; VP scf = dictGet(st, "scale_factor");
            if (scf && scf->kind == Kind::Double) sc = static_cast<long long>(scf->d);
            else if (scf && scf->kind == Kind::Int) sc = scf->i;
            o.c = primary.c; o.h = primary.h * sc; o.w = primary.w * sc;
        } else if (cls == "Concat") {
            long long cc = 0; for (int s : src) cc += shp[s].c; o.c = cc; o.h = primary.h; o.w = primary.w;
        } else if (cls == "C3k2" || cls == "C3k" || cls == "SPPF" || cls == "C2PSA" || cls == "C2f") {
            VP cv2 = childModule(st, "cv2"); o.c = cv2 ? convOutChannels(cv2->state) : 0; o.h = primary.h; o.w = primary.w;
        } else if (cls == "Detect") { o.set = false; }
        else { o.c = primary.c; o.h = primary.h; o.w = primary.w; }
        if (o.set) shp[i] = o;
        LayerAddr la;
        for (size_t kk = 0; kk < src.size() && kk < PT_MAX_INPUTS; ++kk) la.in[kk] = static_cast<uint16_t>(outAddr[src[kk]]);
        long long blocks = (inVol * 2 + 511) / 512;
        long long out = static_cast<long long>(la.in[0]) + blocks;
        if (out > 65535) out = 1;
        la.out = static_cast<uint16_t>(out); outAddr[i] = out; addr[i] = la;
    }
    return addr;
}

struct BinWriter {
    std::vector<uint8_t> body; std::string pool;
    std::map<std::string, uint32_t> interned;
    uint32_t records = 0, modules = 0, tensors = 0;
    uint64_t elements = 0, quantBytes = 0;
    static_assert(sizeof(pt_record) == PT_RECORD_SIZE, "pt_record は PT_RECORD_SIZE バイトであること");
    uint32_t intern(const std::string& s) {
        auto it = interned.find(s); if (it != interned.end()) return it->second;
        uint32_t off = static_cast<uint32_t>(pool.size()); pool.append(s); pool.push_back('\0');
        interned.emplace(s, off); return off;
    }
    void emit(const pt_record& r) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&r); body.insert(body.end(), p, p + sizeof(r)); ++records; }
    void meta(const std::string& key, const std::string& val, bool isInt, int32_t iv) {
        pt_record r{}; r.kind = PT_REC_META; r.value_is_int = isInt ? 1 : 0; r.int_value = iv;
        r.name_off = intern(key); r.aux_off = isInt ? PT_NO_STRING : intern(val); emit(r);
    }
    void module(int depth, bool top, int32_t idx, const std::string& name, const std::string& cls, const LayerAddr& a = LayerAddr{}) {
        pt_record r{}; r.kind = PT_REC_MODULE; r.depth = static_cast<uint16_t>(depth);
        r.is_top = top ? 1 : 0; r.layer_index = static_cast<int16_t>(idx);
        r.name_off = intern(name); r.aux_off = intern(cls); r.out = a.out;
        for (int j = 0; j < PT_MAX_INPUTS; ++j) r.in[j] = a.in[j];
        emit(r); ++modules;
    }
    void tensor(uint8_t kind, pt_dtype dt, const std::string& name, const std::vector<long long>& shape,
                float qmin, float qmax, const std::vector<uint8_t>& quant) {
        if (shape.size() > PT_MAX_DIMS) throw std::runtime_error("次元数が PT_MAX_DIMS を超過: " + name);
        pt_record r{}; r.kind = PT_REC_TENSOR; r.tensor_kind = kind; r.dtype = static_cast<uint8_t>(dt);
        r.ndim = static_cast<uint8_t>(shape.size());
        int64_t n = 1; for (size_t i = 0; i < shape.size(); ++i) { r.dims[i] = static_cast<int32_t>(shape[i]); n *= shape[i]; }
        r.int_value = static_cast<int32_t>(n); r.qmin = qmin; r.qmax = qmax;
        r.name_off = intern(name); r.aux_off = PT_NO_STRING; emit(r);
        body.insert(body.end(), quant.begin(), quant.end());
        ++tensors; elements += static_cast<uint64_t>(n); quantBytes += quant.size();
    }
};
void binTensors(BinWriter& w, const Zip& zip, const VP& container, uint8_t kind) {
    if (!container || container->kind != Kind::Dict) return;
    for (auto& [k, val] : container->dict) {
        if (!val || val->kind == Kind::None) continue;
        TensorInfo t = tensorOf(val); if (!t.ok) continue;
        pt_dtype dt = dtypeEnum(t.storage);
        long long numel = 1; for (auto s : t.shape) numel *= s;
        float qmin = 0, qmax = 0; std::vector<uint8_t> quant;
        if (!t.storageKey.empty() && dt != PT_DT_UNKNOWN) {
            std::vector<uint8_t> raw = zip.readEndsWith("/data/" + t.storageKey);
            std::vector<double> vals = decodeStorage(raw, dt, t.storageOffset, numel);
            quant = quantize(vals, qmin, qmax);
        }
        w.tensor(kind, dt, k->s, t.shape, qmin, qmax, quant);
    }
}
void binModule(BinWriter& w, const Zip& zip, const VP& mod, const std::string& name, int depth, const std::map<int, LayerAddr>& plan) {
    std::string cls = (mod && mod->callable) ? mod->callable->s : "?";
    const VP& state = mod ? mod->state : nullptr;
    VP iv = dictGet(state, "i"); bool top = iv && iv->kind == Kind::Int;
    int idx = top ? static_cast<int32_t>(iv->i) : -1; LayerAddr a;
    if (top) { auto it = plan.find(idx); if (it != plan.end()) a = it->second; }
    w.module(depth, top, idx, name, cls, a);
    binTensors(w, zip, dictGet(state, "_parameters"), PT_TENSOR_PARAM);
    binTensors(w, zip, dictGet(state, "_buffers"),    PT_TENSOR_BUFFER);
    VP mods = dictGet(state, "_modules");
    if (mods && mods->kind == Kind::Dict) for (auto& [k, child] : mods->dict) binModule(w, zip, child, k->s, depth + 1, plan);
}
void writeBinary(const std::string& outPath, const Zip& zip, const VP& root, const VP& model) {
    BinWriter w;
    for (const char* key : {"version", "date", "epoch", "license"}) {
        VP v = dictGet(root, key); if (!v) continue;
        if (v->kind == Kind::Int) w.meta(key, "", true, v->i);
        else if (v->kind == Kind::Str) w.meta(key, v->s, false, 0);
    }
    std::map<int, LayerAddr> plan = planAddresses(model, 4, 640, 640);
    binModule(w, zip, model, "model", 0, plan);
    const uint32_t recordOffset = (static_cast<uint32_t>(sizeof(pt_file_header)) + (PT_ALIGN - 1)) / PT_ALIGN * PT_ALIGN;
    const uint32_t poolOffset = recordOffset + static_cast<uint32_t>(w.body.size());
    pt_file_header h{};
    h.magic[0] = PT_MAGIC0; h.magic[1] = PT_MAGIC1; h.magic[2] = PT_MAGIC2; h.magic[3] = PT_MAGIC3;
    h.format_version = PT_FORMAT_VERSION; h.endianness = 1;
    h.record_count = w.records; h.module_count = w.modules; h.tensor_count = w.tensors;
    h.element_count = w.elements; h.record_offset = recordOffset; h.record_stride = PT_RECORD_SIZE;
    h.string_pool_offset = poolOffset; h.string_pool_size = static_cast<uint32_t>(w.pool.size());
    h.quant_data_bytes = w.quantBytes;
    std::ofstream f(outPath, std::ios::binary);
    if (!f) throw std::runtime_error("バイナリ出力を開けません: " + outPath);
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    std::vector<char> pad(recordOffset - sizeof(pt_file_header), 0);
    if (!pad.empty()) f.write(pad.data(), static_cast<std::streamsize>(pad.size()));
    f.write(reinterpret_cast<const char*>(w.body.data()), static_cast<std::streamsize>(w.body.size()));
    f.write(w.pool.data(), static_cast<std::streamsize>(w.pool.size()));
    std::cout << "\n--- binary output (format v5) ---\n"
              << "file        : " << outPath << "\n"
              << "bytes       : " << (poolOffset + w.pool.size()) << "\n"
              << "records     : " << w.records << " x " << PT_RECORD_SIZE << "B"
              << " (modules " << w.modules << ", tensors " << w.tensors << ")\n"
              << "quant data  : " << w.quantBytes << " bytes (8bit, inline after each TENSOR)\n"
              << "string pool : " << w.pool.size() << " bytes @ off " << poolOffset
              << " (" << w.interned.size() << " unique strings)\n";
}

std::string deriveBinPath(const std::string& input) {
    size_t slash = input.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? input : input.substr(slash + 1);
    size_t dot = base.find_last_of('.'); if (dot != std::string::npos && dot != 0) base.erase(dot);
    return base + ".ptb";
}

// ---------------------------------------------------------------------------
// Phase B 用: BN 融合済み Conv 重みの収集
// ---------------------------------------------------------------------------
// パラメータ/バッファのノードを float に復号する (要素オフセット対応)。
bool readTensorFloat(const Zip& zip, const VP& node, std::vector<int>& dims, std::vector<float>& out) {
    TensorInfo t = tensorOf(node); if (!t.ok) return false;
    pt_dtype dt = dtypeEnum(t.storage); if (dt == PT_DT_UNKNOWN || t.storageKey.empty()) return false;
    long long numel = 1; for (auto s : t.shape) numel *= s;
    std::vector<uint8_t> raw = zip.readEndsWith("/data/" + t.storageKey);
    std::vector<double> v = decodeStorage(raw, dt, t.storageOffset, numel);
    if ((long long)v.size() != numel) return false;
    dims.clear(); for (auto s : t.shape) dims.push_back(static_cast<int>(s));
    out.assign(v.begin(), v.end());
    return true;
}
VP param(const VP& node, const char* name) { return dictGet(dictGet(node ? node->state : nullptr, "_parameters"), name); }
VP buffer(const VP& node, const char* name) { return dictGet(dictGet(node ? node->state : nullptr, "_buffers"), name); }

// model 以下の全モジュールを named_modules パスをキーに収集する。
void collectNodes(const VP& node, const std::string& path, std::map<std::string, VP>& out) {
    out[path] = node;
    VP mods = dictGet(node ? node->state : nullptr, "_modules");
    if (mods && mods->kind == Kind::Dict)
        for (auto& [k, child] : mods->dict)
            collectNodes(child, path.empty() ? k->s : path + "." + k->s, out);
}

}  // namespace

// ===========================================================================
// 公開 API
// ===========================================================================

// Phase A
int pt_dump(const char* pt_path, const char* ptb_out) {
    try {
        std::string path = pt_path;
        Zip zip(path);
        std::vector<uint8_t> pkl = zip.readEndsWith("data.pkl");
        Unpickler up(pkl); VP root = up.run();
        std::cout << "=== " << path << " ===\n";
        for (const char* key : {"version", "date", "epoch", "license"}) {
            VP v = dictGet(root, key); if (!v) continue;
            std::cout << key << ": ";
            if (v->kind == Kind::Str) std::cout << v->s;
            else if (v->kind == Kind::Int) std::cout << v->i;
            else std::cout << "(...)";
            std::cout << "\n";
        }
        std::cout << "\n--- model hierarchy ---\n";
        VP model = dictGet(root, "model");
        if (!model) { std::cerr << "'model' キーがありません\n"; return 1; }
        Stats st; printModule(model, "model", 0, st);
        std::cout << "\n--- summary ---\nmodules : " << st.modules()
                  << "\ntensors : " << st.tensors << "\nelements: " << st.elements << "\n";
        if (ptb_out) {
            std::string outPath = ptb_out[0] ? std::string(ptb_out) : deriveBinPath(path);
            writeBinary(outPath, zip, root, model);
        }
    } catch (const std::exception& e) {
        std::cerr << "pt_dump エラー: " << e.what() << "\n"; return 1;
    }
    return 0;
}

// Phase B 用: BN 融合済み Conv weight/bias を収集 (キー = conv パス + ".weight"/".bias")。
bool pt_collect_conv_weights(const char* pt_path,
                             std::map<std::string, PtTensor>& weights,
                             std::map<std::string, PtTensor>& biases) {
    try {
        std::string sp(pt_path);
        Zip zip(sp);
        std::vector<uint8_t> pkl = zip.readEndsWith("data.pkl");
        Unpickler up(pkl); VP root = up.run();
        VP model = dictGet(root, "model");
        if (!model) { std::cerr << "pt_collect: 'model' なし\n"; return false; }
        // named_modules 規約: DetectionModel ルート="" → 子 Sequential="model" → "model.0.conv"...
        // (ONNX initializer 名 model.0.conv.weight と一致させるため root は空文字列)
        std::map<std::string, VP> nodes; collectNodes(model, "", nodes);

        for (auto& [path, node] : nodes) {
            std::string cls = (node && node->callable) ? node->callable->s : "";
            if (cls != "Conv2d") continue;
            VP wNode = param(node, "weight"); if (!wNode) continue;
            std::vector<int> wdims; std::vector<float> W;
            if (!readTensorFloat(zip, wNode, wdims, W)) continue;
            if (wdims.empty()) continue;
            int oc = wdims[0];
            long long perOc = 1; for (size_t i = 1; i < wdims.size(); ++i) perOc *= wdims[i];

            // conv の親 (= ラッパ) 直下の bn 兄弟を探す
            std::string parent = (path.find_last_of('.') == std::string::npos)
                                 ? std::string() : path.substr(0, path.find_last_of('.'));
            auto bnIt = nodes.find(parent + ".bn");
            VP bn = (bnIt != nodes.end()) ? bnIt->second : nullptr;
            bool bnOk = bn && bn->callable && bn->callable->s == "BatchNorm2d";

            std::vector<float> bias(oc, 0.0f);
            // conv 自身の bias (detect head の生 Conv2d など。ラッパ Conv は通常 bias 無し)
            if (VP bNode = param(node, "bias")) {
                std::vector<int> bd; std::vector<float> bv;
                if (readTensorFloat(zip, bNode, bd, bv) && (int)bv.size() == oc) bias = bv;
            }

            if (bnOk) {
                // W'[o] = W[o]*gamma[o]/sqrt(var[o]+eps), b'[o] = beta[o] - gamma[o]*mean[o]/sqrt(var[o]+eps)
                std::vector<int> gd; std::vector<float> gamma, beta, mean, var;
                std::vector<int> dd;
                readTensorFloat(zip, param(bn, "weight"), gd, gamma);
                readTensorFloat(zip, param(bn, "bias"),   dd, beta);
                readTensorFloat(zip, buffer(bn, "running_mean"), dd, mean);
                readTensorFloat(zip, buffer(bn, "running_var"),  dd, var);
                double eps = 1e-5;
                if (VP ev = dictGet(bn->state, "eps")) {
                    if (ev->kind == Kind::Double) eps = ev->d; else if (ev->kind == Kind::Int) eps = (double)ev->i;
                }
                if ((int)gamma.size() == oc && (int)beta.size() == oc &&
                    (int)mean.size() == oc && (int)var.size() == oc) {
                    // 標準 BN 融合: W'[o]=W[o]*gamma[o]/sqrt(var[o]+eps),
                    //               b'[o]=(b_conv[o]-mean[o])*gamma[o]/sqrt(var[o]+eps)+beta[o]
                    //   (ラッパ Conv は bias 無し=0 なので b'=beta-gamma*mean/sqrt(var+eps) に帰着)
                    for (int o = 0; o < oc; ++o) {
                        double inv = 1.0 / std::sqrt((double)var[o] + eps);
                        double sc  = (double)gamma[o] * inv;
                        for (long long j = 0; j < perOc; ++j) W[(long long)o * perOc + j] = (float)(W[(long long)o * perOc + j] * sc);
                        bias[o] = (float)(((double)bias[o] - (double)mean[o]) * sc + (double)beta[o]);
                    }
                }
            }
            PtTensor wt; wt.dims = wdims; wt.data = std::move(W);
            PtTensor bt; bt.dims = {oc}; bt.data = std::move(bias);
            weights[path + ".weight"] = std::move(wt);
            biases [path + ".bias"]   = std::move(bt);
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "pt_collect_conv_weights エラー: " << e.what() << "\n"; return false;
    }
}

// Phase C generator 用: top module (model.0..N) を順序通りに収集 (cls / f / Conv 形状)。
bool pt_collect_modules(const char* pt_path, std::vector<PtTopModule>& out) {
    try {
        Zip zip(pt_path);
        std::vector<uint8_t> pkl = zip.readEndsWith("data.pkl");
        Unpickler up(pkl); VP root = up.run();
        VP model = dictGet(root, "model");
        if (!model) { std::cerr << "pt_collect_modules: 'model' なし\n"; return false; }
        VP seqWrap = childModule(model->state, "model");
        VP mods = seqWrap ? dictGet(seqWrap->state, "_modules") : nullptr;
        if (!mods || mods->kind != Kind::Dict) { std::cerr << "pt_collect_modules: Sequential 'model' なし\n"; return false; }
        for (auto& [k, layer] : mods->dict) {
            int i = std::atoi(k->s.c_str());
            PtTopModule m;
            m.index = i;
            m.cls   = (layer && layer->callable) ? layer->callable->s : "";
            const VP& st = layer ? layer->state : nullptr;
            // f 解決 (planAddresses と同規約: -1 -> index-1)
            VP fv = dictGet(st, "f"); std::vector<int> flist;
            if (fv) {
                if (fv->kind == Kind::Int) flist.push_back(static_cast<int>(fv->i));
                else if (fv->kind == Kind::List || fv->kind == Kind::Tuple)
                    for (auto& e : fv->seq) flist.push_back(static_cast<int>(e->i));
            }
            if (flist.empty()) flist.push_back(-1);
            for (int s : flist) m.f.push_back(s == -1 ? i - 1 : s);
            // top module 自身の add 属性 (yolo26 SPPF は residual を持つ: cv2(cat)+x)
            if (VP av = dictGet(st, "add")) m.add = (av->kind == Kind::Bool && av->b);
            // Conv 系ラッパ (conv 子モジュール) の重み形状 + stride
            VP conv = childModule(st, "conv");
            if (conv) {
                TensorInfo wt = tensorOf(param(conv, "weight"));
                if (wt.ok && wt.shape.size() >= 3) {
                    m.oc = static_cast<int>(wt.shape[0]);
                    m.ic = static_cast<int>(wt.shape[1]);
                    m.k  = static_cast<int>(wt.shape[2]);
                    m.stride = convStride(st);
                    m.conv_path = "model." + std::to_string(i) + ".conv";
                }
            }
            // C2f 系: bottleneck (m.0) の add 属性 = residual の有無 (backbone=true / neck=false)
            VP mlist = childModule(st, "m");
            if (mlist) {
                VP m0 = dictGet(dictGet(mlist->state, "_modules"), "0");
                if (m0) {
                    VP addv = dictGet(m0->state, "add");
                    m.shortcut = (addv && addv->kind == Kind::Bool && addv->b);
                    m.bneck_cls = (m0->callable) ? m0->callable->s : "";   // "Bottleneck" / "C3k" 等
                }
            }
            out.push_back(std::move(m));
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "pt_collect_modules エラー: " << e.what() << "\n"; return false;
    }
}

#endif  // VSCODE

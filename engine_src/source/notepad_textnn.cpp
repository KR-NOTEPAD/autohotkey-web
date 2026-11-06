// ============================================================================
//  NOTEPAD.dll  -  notepad_textnn.cpp
//  [NOTEPAD] 세트 AG-t: 한국어 문장 분류 학습기 (임베딩 백본 + C++ MLP, DLL 내부 학습).
//
//  이미지 학습(세트 AG, notepad_nn.cpp)의 텍스트판. 구조가 완전히 대칭이다:
//    이미지:  파일 → mobilenetv2_embed.onnx → 1280-d 임베딩 → MLP(softmax) → .nn
//    텍스트:  문장 → kosimcse_embed.onnx     → 768-d  임베딩 → MLP(softmax) → .ntc
//
//  백본: BM-K/KoSimCSE-bert (BERT, WordPiece vocab.txt). export_kosimcse.py 가
//        models\kor_embed\ 에 kosimcse_embed.onnx + vocab.txt 를 만든다(문장→임베딩).
//        토크나이저는 notepad_textcls.cpp 와 동일한 BertTokenizer 재현(외부 의존 0).
//        추론은 ORT C API 다중입력(input_ids/attention_mask/token_type_ids) →
//        last_hidden_state 를 mean-pool(attention_mask 가중) 또는 [CLS] 로 풀링.
//
//  분류기: notepad_nn.cpp 의 MLP 와 동일한 입력→은닉(ReLU)→출력(softmax) + SGD/모멘텀.
//          입력 차원만 768(임베딩)로. 학습 루프(에포크/학습률 감쇠/클래스 불균형 보정)도 동형.
//
//  모델파일 .ntc (텍스트):
//     NPTC1
//     backbone=<abs path or filename>
//     dim=768
//     pool=mean            (mean | cls)
//     maxlen=64
//     hidden=64
//     labels=question,statement,command,...
//     W1 ...   (in*hid, 공백구분)
//     b1 ...
//     W2 ...   (hid*out)
//     b2 ...
//
//  export 규약은 exports2.cpp / textcls.cpp 와 동일: UTF-16, 2단계 버퍼(필요문자수/음수=버퍼부족),
//  NOTEPAD.def 에 이름 등록. NOTEPAD_ENABLE_ONNX 미정의 시 학습/추론은 사유 남기고 실패.
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <random>

#ifdef NOTEPAD_ENABLE_ONNX
#include <onnxruntime_c_api.h>
namespace notepad { const OrtApi* ortApiHandle(); } // notepad_onnx.cpp
#endif

#ifndef NOTEPAD_API
#define NOTEPAD_API extern "C" __declspec(dllexport)
#endif

namespace notepad { namespace textnn {

// ---- 진단 -------------------------------------------------------------------
static std::wstring g_lastErr;
static void setErr(const std::wstring& e) { g_lastErr = e; }

// ---- 문자열/파일 유틸 (textcls.cpp 와 동일 규약) ----------------------------
static std::wstring utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string wToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring ansiToW(const char* s) {
    if (!s) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}
static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool readFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0; bool ok = true;
    if (sz.QuadPart > 0) ok = ReadFile(h, &out[0], (DWORD)sz.QuadPart, &rd, nullptr) && rd == (DWORD)sz.QuadPart;
    CloseHandle(h);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return ok;
}
static bool writeFileUtf8(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(h); return ok;
}
static void splitLines(const std::string& s, std::vector<std::string>& lines) {
    size_t st = 0;
    while (st <= s.size()) {
        size_t nl = s.find('\n', st);
        std::string ln = s.substr(st, nl == std::string::npos ? std::string::npos : nl - st);
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        lines.push_back(ln);
        if (nl == std::string::npos) break;
        st = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
}
static std::wstring joinDir(const std::wstring& dir, const wchar_t* file) {
    if (dir.empty()) return file;
    wchar_t last = dir.back();
    return (last == L'\\' || last == L'/') ? dir + file : dir + L"\\" + file;
}
static std::wstring dirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : path.substr(0, p);
}

// ============================================================================
//  BertTokenizer 재현 (notepad_textcls.cpp 와 바이트 동일 규약)
//    do_lower_case=false, tokenize_chinese_chars=true, NFC, WordPiece(##).
// ============================================================================
struct Vocab {
    std::unordered_map<std::wstring, int> tok2id;
    int unk = -1, cls = -1, sep = -1, pad = 0;
};
static bool loadVocab(const std::wstring& dir, Vocab& v) {
    std::string raw;
    if (!readFileUtf8(joinDir(dir, L"vocab.txt"), raw)) { setErr(L"vocab.txt 읽기 실패: " + dir); return false; }
    std::vector<std::string> lines; splitLines(raw, lines);
    v.tok2id.clear(); v.tok2id.reserve(lines.size() * 2);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::wstring w = utf8ToW(lines[i]);
        if (v.tok2id.find(w) == v.tok2id.end()) v.tok2id[w] = (int)i;
    }
    auto get = [&](const wchar_t* t) { auto it = v.tok2id.find(t); return it == v.tok2id.end() ? -1 : it->second; };
    v.unk = get(L"[UNK]"); v.cls = get(L"[CLS]"); v.sep = get(L"[SEP]");
    int pad = get(L"[PAD]"); v.pad = pad < 0 ? 0 : pad;
    if (v.unk < 0 || v.cls < 0 || v.sep < 0) { setErr(L"vocab.txt 에 [UNK]/[CLS]/[SEP] 없음"); return false; }
    return true;
}
static uint32_t cpAt(const std::wstring& s, size_t i, int& units) {
    wchar_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) {
        wchar_t d = s[i + 1];
        if (d >= 0xDC00 && d <= 0xDFFF) { units = 2; return 0x10000 + (((uint32_t)c - 0xD800) << 10) + ((uint32_t)d - 0xDC00); }
    }
    units = 1; return (uint32_t)c;
}
static WORD ctype1(const wchar_t* p, int n) { WORD t[2] = { 0, 0 }; GetStringTypeW(CT_CTYPE1, p, n, t); return t[0]; }
static bool isWs(uint32_t cp, const wchar_t* p, int n) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return true;
    if (cp < 0x80) return false;
    return (ctype1(p, n) & C1_SPACE) != 0 && cp != 0x85;
}
static bool isCtrl(uint32_t cp, const wchar_t* p, int n) {
    if (cp == '\t' || cp == '\n' || cp == '\r') return false;
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return true;
    if (cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
        (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB) ||
        cp == 0x061C || (cp >= 0x180B && cp <= 0x180E) || cp == 0xE0001 || (cp >= 0xE0020 && cp <= 0xE007F))
        return true;
    return cp >= 0x80 && (ctype1(p, n) & C1_CNTRL) != 0;
}
static bool isPunct(uint32_t cp, const wchar_t* p, int n) {
    if ((cp >= 33 && cp <= 47) || (cp >= 58 && cp <= 64) || (cp >= 91 && cp <= 96) || (cp >= 123 && cp <= 126)) return true;
    if (cp < 0x80) return false;
    return (ctype1(p, n) & C1_PUNCT) != 0;
}
static bool isCjk(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x20000 && cp <= 0x2A6DF) || (cp >= 0x2A700 && cp <= 0x2B73F) ||
           (cp >= 0x2B740 && cp <= 0x2B81F) || (cp >= 0x2B820 && cp <= 0x2CEAF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x2F800 && cp <= 0x2FA1F);
}
static void basicTokenize(const std::wstring& text, std::vector<std::wstring>& out) {
    std::wstring cleaned; cleaned.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size();) {
        int u = 1; uint32_t cp = cpAt(text, i, u);
        const wchar_t* p = &text[i];
        if (cp == 0 || cp == 0xFFFD || isCtrl(cp, p, u)) { i += u; continue; }
        if (isWs(cp, p, u)) { cleaned += L' '; i += u; continue; }
        if (isCjk(cp)) { cleaned += L' '; cleaned.append(p, u); cleaned += L' '; i += u; continue; }
        cleaned.append(p, u); i += u;
    }
    if (!cleaned.empty()) {
        int need = NormalizeString(NormalizationC, cleaned.c_str(), (int)cleaned.size(), nullptr, 0);
        if (need > 0) {
            std::wstring nfc((size_t)need, L'\0');
            int got = NormalizeString(NormalizationC, cleaned.c_str(), (int)cleaned.size(), &nfc[0], need);
            if (got > 0) { nfc.resize((size_t)got); cleaned.swap(nfc); }
        }
    }
    std::wstring cur;
    auto flush = [&]() { if (!cur.empty()) { out.push_back(cur); cur.clear(); } };
    for (size_t i = 0; i < cleaned.size();) {
        int u = 1; uint32_t cp = cpAt(cleaned, i, u);
        const wchar_t* p = &cleaned[i];
        if (cp == ' ') { flush(); i += u; continue; }
        if (isPunct(cp, p, u)) { flush(); out.push_back(std::wstring(p, u)); i += u; continue; }
        cur.append(p, u); i += u;
    }
    flush();
}
static void wordpiece(const Vocab& v, const std::wstring& word, std::vector<int>& ids) {
    if (word.size() > 100) { ids.push_back(v.unk); return; }
    std::vector<int> sub; size_t start = 0; bool bad = false;
    while (start < word.size()) {
        size_t end = word.size(); int cur = -1;
        while (start < end) {
            std::wstring piece = word.substr(start, end - start);
            if (start > 0) piece = L"##" + piece;
            auto it = v.tok2id.find(piece);
            if (it != v.tok2id.end()) { cur = it->second; break; }
            --end;
        }
        if (cur < 0) { bad = true; break; }
        sub.push_back(cur); start = end;
    }
    if (bad) ids.push_back(v.unk); else ids.insert(ids.end(), sub.begin(), sub.end());
}
// [CLS] + 토큰(최대 maxLen-2) + [SEP]. mask=1, type=0.
static void encode(const Vocab& v, const std::wstring& text, int maxLen,
                   std::vector<int64_t>& ids, std::vector<int64_t>& mask, std::vector<int64_t>& types) {
    if (maxLen < 8) maxLen = 8;
    if (maxLen > 256) maxLen = 256;
    std::vector<std::wstring> words; basicTokenize(text, words);
    std::vector<int> toks;
    for (auto& w : words) { wordpiece(v, w, toks); if ((int)toks.size() >= maxLen - 2) break; }
    if ((int)toks.size() > maxLen - 2) toks.resize(maxLen - 2);
    ids.clear(); ids.push_back(v.cls);
    for (int t : toks) ids.push_back(t);
    ids.push_back(v.sep);
    mask.assign(ids.size(), 1); types.assign(ids.size(), 0);
}

// ============================================================================
//  임베딩 백본 (KoSimCSE ONNX) — ORT C API 다중입력 → 풀링된 768-d
//    출력: last_hidden_state [1, seq, dim] 가정. pool: mean(mask 가중) 또는 cls([0]).
//    풀링 임베딩을 만든 뒤 L2 정규화(문장임베딩 관례).
// ============================================================================
struct Backbone {
    std::wstring path;
    Vocab vocab;
    int dim = 0;      // 임베딩 차원(첫 추론에서 확정)
    bool loaded = false;
#ifdef NOTEPAD_ENABLE_ONNX
    OrtEnv* env = nullptr; OrtSession* sess = nullptr; OrtSessionOptions* so = nullptr; OrtMemoryInfo* mem = nullptr;
    std::vector<std::string> inNames; std::string outName;
    int idxIds = -1, idxMask = -1, idxType = -1;
#endif
};
static Backbone g_bb;

static void bbUnload() {
#ifdef NOTEPAD_ENABLE_ONNX
    const OrtApi* ort = ortApiHandle();
    if (ort) {
        if (g_bb.sess) ort->ReleaseSession(g_bb.sess);
        if (g_bb.so) ort->ReleaseSessionOptions(g_bb.so);
        if (g_bb.mem) ort->ReleaseMemoryInfo(g_bb.mem);
        if (g_bb.env) ort->ReleaseEnv(g_bb.env);
    }
    g_bb.sess = nullptr; g_bb.so = nullptr; g_bb.mem = nullptr; g_bb.env = nullptr;
    g_bb.inNames.clear(); g_bb.outName.clear(); g_bb.idxIds = g_bb.idxMask = g_bb.idxType = -1;
#endif
    g_bb.loaded = false; g_bb.path.clear(); g_bb.dim = 0; g_bb.vocab = Vocab();
}

#ifdef NOTEPAD_ENABLE_ONNX
static std::wstring ortMsg(const OrtApi* ort, OrtStatus* st, const wchar_t* where) {
    std::wstring m = where; m += L": ";
    if (st) { m += ansiToW(ort->GetErrorMessage(st)); ort->ReleaseStatus(st); }
    return m;
}
#endif

// 백본 로드: backbonePath(=kosimcse_embed.onnx). vocab.txt 는 같은 폴더.
static std::wstring canonPath(const std::wstring& p) {
    if (p.empty()) return p;
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring r(n, L'\0');
    DWORD g = GetFullPathNameW(p.c_str(), n, &r[0], nullptr);
    if (!g || g >= n) return p;
    r.resize(g); return r;
}
// 경로 표기가 달라도(..\ 포함 등) 같은 파일이면 세션 재생성 안 함(440MB 백본 재로드 방지)
static bool bbLoad(const std::wstring& backbonePathIn) {
    std::wstring backbonePath = canonPath(backbonePathIn);
    if (g_bb.loaded && g_bb.path == backbonePath) return true;
    bbUnload();
    if (!fileExists(backbonePath)) { setErr(L"백본 없음: " + backbonePath + L" (export_kosimcse.py 실행)"); return false; }
    std::wstring dir = dirOf(backbonePath);
    if (!loadVocab(dir, g_bb.vocab)) return false;
#ifndef NOTEPAD_ENABLE_ONNX
    setErr(L"ONNX 비활성 빌드(/DNOTEPAD_ENABLE_ONNX + third_party\\onnxruntime 필요)");
    return false;
#else
    const OrtApi* ort = ortApiHandle();
    if (!ort) { setErr(L"onnxruntime.dll 로드 실패(ORT API 없음)"); return false; }
    OrtStatus* st = nullptr;
    if ((st = ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "notepad_textnn", &g_bb.env))) { setErr(ortMsg(ort, st, L"CreateEnv")); bbUnload(); return false; }
    if ((st = ort->CreateSessionOptions(&g_bb.so))) { setErr(ortMsg(ort, st, L"CreateSessionOptions")); bbUnload(); return false; }
    ort->SetIntraOpNumThreads(g_bb.so, 0);
    ort->SetSessionGraphOptimizationLevel(g_bb.so, ORT_ENABLE_ALL);
    if ((st = ort->CreateSession(g_bb.env, backbonePath.c_str(), g_bb.so, &g_bb.sess))) { setErr(ortMsg(ort, st, L"CreateSession(backbone)")); bbUnload(); return false; }
    if ((st = ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_bb.mem))) { setErr(ortMsg(ort, st, L"CreateCpuMemoryInfo")); bbUnload(); return false; }
    OrtAllocator* alloc = nullptr; ort->GetAllocatorWithDefaultOptions(&alloc);
    size_t nIn = 0; ort->SessionGetInputCount(g_bb.sess, &nIn);
    for (size_t i = 0; i < nIn; ++i) {
        char* nm = nullptr; ort->SessionGetInputName(g_bb.sess, i, alloc, &nm);
        std::string s = nm ? nm : ""; if (nm) alloc->Free(alloc, nm);
        g_bb.inNames.push_back(s);
        if (s.find("input_ids") != std::string::npos) g_bb.idxIds = (int)i;
        else if (s.find("attention") != std::string::npos) g_bb.idxMask = (int)i;
        else if (s.find("token_type") != std::string::npos) g_bb.idxType = (int)i;
    }
    { char* nm = nullptr; ort->SessionGetOutputName(g_bb.sess, 0, alloc, &nm); g_bb.outName = nm ? nm : ""; if (nm) alloc->Free(alloc, nm); }
    if (g_bb.idxIds < 0) { setErr(L"백본 입력에 input_ids 없음"); bbUnload(); return false; }
    g_bb.path = backbonePath; g_bb.loaded = true;
    return true;
#endif
}

// 문장 → 풀링 임베딩(L2 정규화). pool: 0=mean(mask 가중), 1=cls.
static bool bbEmbed(const std::wstring& text, int maxLen, int pool, std::vector<float>& emb) {
    emb.clear();
    if (!g_bb.loaded) { setErr(L"백본 미로드"); return false; }
#ifndef NOTEPAD_ENABLE_ONNX
    setErr(L"ONNX 비활성 빌드"); return false;
#else
    const OrtApi* ort = ortApiHandle(); if (!ort) { setErr(L"ORT API 없음"); return false; }
    std::vector<int64_t> ids, mask, types; encode(g_bb.vocab, text, maxLen, ids, mask, types);
    int seq = (int)ids.size();
    int64_t shape[2] = { 1, (int64_t)seq };
    size_t nIn = g_bb.inNames.size();
    std::vector<OrtValue*> vals(nIn, nullptr);
    std::vector<const char*> names(nIn);
    for (size_t i = 0; i < nIn; ++i) names[i] = g_bb.inNames[i].c_str();
    auto mk = [&](int idx, std::vector<int64_t>& buf) -> bool {
        if (idx < 0) return true;
        OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(g_bb.mem, buf.data(), buf.size() * sizeof(int64_t),
                            shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &vals[idx]);
        if (st) { setErr(ortMsg(ort, st, L"CreateTensor")); return false; }
        return true;
    };
    bool ok = mk(g_bb.idxIds, ids) && mk(g_bb.idxMask, mask) && mk(g_bb.idxType, types);
    std::vector<int64_t> zeros(seq, 0);
    for (size_t i = 0; ok && i < nIn; ++i) if (!vals[i]) {
        OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(g_bb.mem, zeros.data(), zeros.size() * sizeof(int64_t),
                            shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &vals[i]);
        if (st) { setErr(ortMsg(ort, st, L"CreateTensor(extra)")); ok = false; }
    }
    if (ok) {
        const char* outNames[] = { g_bb.outName.c_str() };
        OrtValue* outVal = nullptr;
        OrtStatus* st = ort->Run(g_bb.sess, nullptr, names.data(), (const OrtValue* const*)vals.data(), nIn, outNames, 1, &outVal);
        if (st) { setErr(ortMsg(ort, st, L"Run")); ok = false; }
        else if (outVal) {
            float* data = nullptr; OrtTensorTypeAndShapeInfo* info = nullptr;
            size_t dims = 0; std::vector<int64_t> d;
            if (!ort->GetTensorMutableData(outVal, (void**)&data) && !ort->GetTensorTypeAndShape(outVal, &info)) {
                ort->GetDimensionsCount(info, &dims);
                d.resize(dims); if (dims) ort->GetDimensions(info, d.data(), dims);
                ort->ReleaseTensorTypeAndShapeInfo(info);
                // 두 가지 출력 형태 지원:
                //  (A) last_hidden_state [1, seq, H]  → 풀링 필요
                //  (B) sentence_embedding [1, H]      → 그대로 사용(export 시 풀링 포함)
                if (dims == 3) {
                    int S = (int)d[1], H = (int)d[2];
                    emb.assign(H, 0.0f);
                    if (pool == 1) { // CLS
                        for (int j = 0; j < H; ++j) emb[j] = data[j];
                    } else {         // mean (mask 가중)
                        float cnt = 0;
                        for (int t = 0; t < S; ++t) {
                            float m = (t < (int)mask.size()) ? (float)mask[t] : 1.0f;
                            if (m <= 0) continue; cnt += m;
                            const float* row = data + (size_t)t * H;
                            for (int j = 0; j < H; ++j) emb[j] += m * row[j];
                        }
                        if (cnt > 0) for (int j = 0; j < H; ++j) emb[j] /= cnt;
                    }
                } else if (dims == 2) {
                    int H = (int)d[1]; emb.assign(data, data + H);
                } else {
                    setErr(L"백본 출력 차원이 2/3 이 아님"); ok = false;
                }
                // L2 정규화
                if (ok) {
                    double s2 = 0; for (float v : emb) s2 += (double)v * v;
                    s2 = std::sqrt(s2); if (s2 > 1e-9) for (auto& v : emb) v = (float)(v / s2);
                    g_bb.dim = (int)emb.size();
                }
            } else { setErr(L"백본 출력 텐서 읽기 실패"); ok = false; }
            ort->ReleaseValue(outVal);
        }
    }
    for (auto* v : vals) if (v) ort->ReleaseValue(v);
    return ok && !emb.empty();
#endif
}

// ============================================================================
//  MLP (notepad_nn.cpp 와 동일: 입력→은닉ReLU→출력softmax, SGD+모멘텀+L2)
// ============================================================================
struct MLP {
    int in = 0, hid = 64, out = 0;
    std::vector<float> W1, b1, W2, b2;
    std::vector<float> vW1, vb1, vW2, vb2;
    void init(int classes, int hidden, int inDim) {
        out = classes; hid = hidden; in = inDim;
        std::mt19937 rng(12345);
        auto he = [&](int fan) { std::normal_distribution<float> d(0.0f, std::sqrt(2.0f / fan)); return d(rng); };
        W1.resize((size_t)in * hid); for (auto& w : W1) w = he(in);
        b1.assign(hid, 0.0f);
        W2.resize((size_t)hid * out); for (auto& w : W2) w = he(hid);
        b2.assign(out, 0.0f);
        vW1.assign(W1.size(), 0.0f); vb1.assign(hid, 0.0f);
        vW2.assign(W2.size(), 0.0f); vb2.assign(out, 0.0f);
    }
    void forward(const std::vector<float>& x, std::vector<float>& h, std::vector<float>& y) const {
        h.assign(hid, 0.0f);
        const int n = std::min(in, (int)x.size());
        for (int j = 0; j < hid; ++j) {
            float s = b1[j]; const float* w = &W1[(size_t)j * in];
            for (int i = 0; i < n; ++i) s += w[i] * x[i];
            h[j] = s > 0 ? s : 0;
        }
        y.assign(out, 0.0f);
        float mx = -1e30f;
        for (int k = 0; k < out; ++k) {
            float s = b2[k]; const float* w = &W2[(size_t)k * hid];
            for (int j = 0; j < hid; ++j) s += w[j] * h[j];
            y[k] = s; if (s > mx) mx = s;
        }
        float sum = 0; for (int k = 0; k < out; ++k) { y[k] = std::exp(y[k] - mx); sum += y[k]; }
        for (int k = 0; k < out; ++k) y[k] /= (sum > 0 ? sum : 1);
    }
    float trainStep(const std::vector<float>& x, int label, float lr, float mom, float l2) {
        std::vector<float> h, y; forward(x, h, y);
        float loss = -std::log(std::max(1e-7f, y[label]));
        std::vector<float> dy(out);
        for (int k = 0; k < out; ++k) dy[k] = y[k] - (k == label ? 1.0f : 0.0f);
        std::vector<float> dh(hid, 0.0f);
        for (int k = 0; k < out; ++k) {
            float g = dy[k]; float* w = &W2[(size_t)k * hid]; float* vw = &vW2[(size_t)k * hid];
            for (int j = 0; j < hid; ++j) { float grad = g * h[j] + l2 * w[j]; vw[j] = mom * vw[j] - lr * grad; dh[j] += g * w[j]; w[j] += vw[j]; }
            vb2[k] = mom * vb2[k] - lr * g; b2[k] += vb2[k];
        }
        for (int j = 0; j < hid; ++j) if (h[j] <= 0) dh[j] = 0;
        for (int j = 0; j < hid; ++j) {
            float g = dh[j]; float* w = &W1[(size_t)j * in]; float* vw = &vW1[(size_t)j * in];
            const int n = std::min(in, (int)x.size());
            for (int i = 0; i < n; ++i) { float grad = g * x[i] + l2 * w[i]; vw[i] = mom * vw[i] - lr * grad; w[i] += vw[i]; }
            vb1[j] = mom * vb1[j] - lr * g; b1[j] += vb1[j];
        }
        return loss;
    }
    int predict(const std::vector<float>& x, std::vector<float>& probOut) const {
        std::vector<float> h; forward(x, h, probOut);
        int best = 0; for (int k = 1; k < out; ++k) if (probOut[k] > probOut[best]) best = k;
        return best;
    }
};

// ============================================================================
//  .ntc 저장/로드
// ============================================================================
struct TextModel {
    std::wstring backbone;   // 백본 경로(절대 or 파일명)
    int dim = 0, hid = 64, maxlen = 64, pool = 0; // pool 0=mean 1=cls
    std::vector<std::wstring> labels;
    MLP net;
    bool loaded = false;
    std::wstring path;
};
static TextModel g_tm;

static std::string fnum(float f) { char b[32]; snprintf(b, sizeof(b), "%.6g", f); return b; }

static bool ntcSave(const std::wstring& path, const TextModel& m) {
    std::string o = "NPTC1\n";
    o += "backbone=" + wToUtf8(m.backbone) + "\n";
    o += "dim=" + std::to_string(m.dim) + "\n";
    o += "hidden=" + std::to_string(m.hid) + "\n";
    o += "maxlen=" + std::to_string(m.maxlen) + "\n";
    o += "pool=" + std::string(m.pool == 1 ? "cls" : "mean") + "\n";
    std::string lab; for (size_t i = 0; i < m.labels.size(); ++i) { if (i) lab += ","; lab += wToUtf8(m.labels[i]); }
    o += "labels=" + lab + "\n";
    auto row = [&](const char* key, const std::vector<float>& v) {
        o += key; o += "=";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ' '; o += fnum(v[i]); }
        o += "\n";
    };
    row("W1", m.net.W1); row("b1", m.net.b1); row("W2", m.net.W2); row("b2", m.net.b2);
    return writeFileUtf8(path, o);
}
static void parseFloatsInto(const std::string& s, std::vector<float>& v) {
    v.clear(); const char* p = s.c_str(); char* e = nullptr;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p; if (!*p) break;
        float f = strtof(p, &e); if (e == p) break; v.push_back(f); p = e;
    }
}
static bool ntcLoad(const std::wstring& path, TextModel& m) {
    std::string raw; if (!readFileUtf8(path, raw)) { setErr(L".ntc 읽기 실패: " + path); return false; }
    std::vector<std::string> lines; splitLines(raw, lines);
    if (lines.empty() || lines[0].rfind("NPTC1", 0) != 0) { setErr(L".ntc 형식 아님(NPTC1 헤더 없음)"); return false; }
    m = TextModel();
    std::vector<float> W1, b1, W2, b2;
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string& ln = lines[i]; size_t eq = ln.find('=');
        if (eq == std::string::npos) continue;
        std::string k = ln.substr(0, eq), val = ln.substr(eq + 1);
        if (k == "backbone") m.backbone = utf8ToW(val);
        else if (k == "dim") m.dim = atoi(val.c_str());
        else if (k == "hidden") m.hid = atoi(val.c_str());
        else if (k == "maxlen") m.maxlen = atoi(val.c_str());
        else if (k == "pool") m.pool = (val == "cls") ? 1 : 0;
        else if (k == "labels") { size_t st = 0; while (st <= val.size()) { size_t c = val.find(',', st); std::string t = val.substr(st, c == std::string::npos ? std::string::npos : c - st); if (!t.empty()) m.labels.push_back(utf8ToW(t)); if (c == std::string::npos) break; st = c + 1; } }
        else if (k == "W1") parseFloatsInto(val, W1);
        else if (k == "b1") parseFloatsInto(val, b1);
        else if (k == "W2") parseFloatsInto(val, W2);
        else if (k == "b2") parseFloatsInto(val, b2);
    }
    int out = (int)m.labels.size();
    if (out < 2 || m.dim <= 0 || (int)b1.size() != m.hid || (int)W1.size() != m.dim * m.hid ||
        (int)W2.size() != m.hid * out || (int)b2.size() != out) {
        setErr(L".ntc 가중치 크기 불일치"); return false;
    }
    m.net.in = m.dim; m.net.hid = m.hid; m.net.out = out;
    m.net.W1 = W1; m.net.b1 = b1; m.net.W2 = W2; m.net.b2 = b2;
    m.loaded = true; m.path = path;
    return true;
}

// ============================================================================
//  데이터셋 읽기: "라벨<TAB>문장" 또는 "라벨,문장". # 주석/빈줄 무시.
// ============================================================================
static bool readDataset(const std::wstring& path, std::vector<std::wstring>& texts, std::vector<std::wstring>& labs) {
    std::string raw; if (!readFileUtf8(path, raw)) { setErr(L"데이터셋 읽기 실패: " + path); return false; }
    std::vector<std::string> lines; splitLines(raw, lines);
    for (auto& ln : lines) {
        if (ln.empty() || ln[0] == '#') continue;
        size_t sep = ln.find('\t'); if (sep == std::string::npos) sep = ln.find(',');
        if (sep == std::string::npos) continue;
        std::string lab = ln.substr(0, sep), txt = ln.substr(sep + 1);
        // 앞뒤 공백 trim
        auto trim = [](std::string& s) { size_t a = s.find_first_not_of(" \t"); size_t b = s.find_last_not_of(" \t"); if (a == std::string::npos) s.clear(); else s = s.substr(a, b - a + 1); };
        trim(lab); trim(txt);
        if (lab.empty() || txt.empty()) continue;
        labs.push_back(utf8ToW(lab)); texts.push_back(utf8ToW(txt));
    }
    return !texts.empty();
}

// ============================================================================
//  학습: 데이터셋 → 임베딩 → MLP 학습 → .ntc 저장. 반환 샘플 수(실패 0).
// ============================================================================
static int train(const std::wstring& backbone, const std::wstring& datasetPath, const std::wstring& modelPath,
                 int epochs, int hidden, int maxlen, int pool) {
    g_lastErr.clear();
    if (epochs <= 0) epochs = 300;
    if (hidden <= 0) hidden = 64;
    if (maxlen <= 0) maxlen = 64;
    if (!bbLoad(backbone)) return 0;
    std::vector<std::wstring> texts, labs;
    if (!readDataset(datasetPath, texts, labs)) return 0;
    // 라벨 인덱스화
    std::vector<std::wstring> labels;
    auto labelId = [&](const std::wstring& l) {
        for (size_t i = 0; i < labels.size(); ++i) if (labels[i] == l) return (int)i;
        labels.push_back(l); return (int)labels.size() - 1;
    };
    std::vector<std::vector<float>> X; std::vector<int> Y;
    for (size_t i = 0; i < texts.size(); ++i) {
        std::vector<float> e;
        if (!bbEmbed(texts[i], maxlen, pool, e)) { setErr(L"임베딩 실패(" + texts[i].substr(0, 20) + L"): " + g_lastErr); return 0; }
        X.push_back(std::move(e)); Y.push_back(labelId(labs[i]));
    }
    int dim = (int)X[0].size(), out = (int)labels.size();
    if (out < 2) { setErr(L"라벨이 2개 미만(데이터셋에 최소 2개 라벨 필요)"); return 0; }
    // 클래스 불균형 보정용 카운트
    std::vector<int> cnt(out, 0); for (int y : Y) cnt[y]++;
    std::vector<float> cw(out, 1.0f);
    int mx = 0; for (int c : cnt) mx = std::max(mx, c);
    for (int k = 0; k < out; ++k) if (cnt[k] > 0) cw[k] = std::min(3.0f, (float)mx / cnt[k]);

    MLP net; net.init(out, hidden, dim);
    std::vector<int> idx(X.size()); for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
    std::mt19937 rng(777);
    float lr0 = 0.05f, mom = 0.9f, l2 = 1e-4f;
    for (int e = 0; e < epochs; ++e) {
        std::shuffle(idx.begin(), idx.end(), rng);
        float lr = lr0 * (1.0f - 0.9f * e / epochs);
        for (int id : idx) net.trainStep(X[id], Y[id], lr * cw[Y[id]], mom, l2);
    }
    g_tm = TextModel();
    g_tm.backbone = backbone; g_tm.dim = dim; g_tm.hid = hidden; g_tm.maxlen = maxlen; g_tm.pool = pool;
    g_tm.labels = labels; g_tm.net = net; g_tm.loaded = true; g_tm.path = modelPath;
    if (!ntcSave(modelPath, g_tm)) { setErr(L".ntc 저장 실패: " + modelPath); return 0; }
    return (int)X.size();
}

// 추론: 로드된(또는 지정) 모델로 문장 분류 → "label1,p1;label2,p2;..."
static bool classify(const std::wstring& modelPath, const std::wstring& text, int topK, std::wstring& outStr) {
    g_lastErr.clear();
    if (!modelPath.empty() && (!g_tm.loaded || g_tm.path != modelPath)) {
        if (!ntcLoad(modelPath, g_tm)) return false;
    }
    if (!g_tm.loaded) { setErr(L"모델 미로드"); return false; }
    if (!bbLoad(g_tm.backbone)) return false;
    std::vector<float> emb;
    if (!bbEmbed(text, g_tm.maxlen, g_tm.pool, emb)) return false;
    std::vector<float> prob; g_tm.net.predict(emb, prob);
    // topK 정렬
    std::vector<int> ord(prob.size()); for (size_t i = 0; i < ord.size(); ++i) ord[i] = (int)i;
    std::sort(ord.begin(), ord.end(), [&](int a, int b) { return prob[a] > prob[b]; });
    if (topK <= 0) topK = (int)prob.size();
    std::wstring s; wchar_t buf[32];
    for (int i = 0; i < (int)ord.size() && i < topK; ++i) {
        int k = ord[i];
        swprintf(buf, 32, L",%.4f", prob[k]);
        if (!s.empty()) s += L';';
        s += (k < (int)g_tm.labels.size() ? g_tm.labels[k] : L"?"); s += buf;
    }
    outStr = s;
    return true;
}

// ---- 세트 AP(notepad_textft.cpp) 브리지: 백본 임베딩 / 오류 (export 아님) --------------------
bool nnEmbed(const std::wstring& backbone, const std::wstring& text, int maxLen, int pool, std::vector<float>& emb) {
    if (!bbLoad(backbone)) return false;
    return bbEmbed(text, maxLen, pool, emb);
}
const std::wstring& nnLastError() { return g_lastErr; }

}} // namespace notepad::textnn

// ============================================================================
//  exports (AHK v1 64비트 DllCall). 출력 규약 = exports2.cpp / textcls.cpp 와 동일.
// ============================================================================
static int tnWriteOut(const std::wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}
static std::wstring tnArg(const wchar_t* p) { return p ? std::wstring(p) : std::wstring(); }

using namespace notepad::textnn;

// 학습. 반환: 학습 샘플 수(실패 0, 사유는 LastError). pool: 0=mean 1=cls.
NOTEPAD_API int Notepad_TextNNTrain(const wchar_t* backbone, const wchar_t* datasetPath, const wchar_t* modelPath,
                                    int epochs, int hidden, int maxlen, int pool) {
    return train(tnArg(backbone), tnArg(datasetPath), tnArg(modelPath), epochs, hidden, maxlen, pool);
}

// 분류. "label,prob;label,prob;..." (확률 내림차순). modelPath "" = 로드된 모델.
NOTEPAD_API int Notepad_TextNNClassify(const wchar_t* modelPath, const wchar_t* text, int topK,
                                       wchar_t* out, int outCch) {
    std::wstring s;
    if (!classify(tnArg(modelPath), tnArg(text), topK, s)) return 0;
    return tnWriteOut(s, out, outCch);
}

// 모델 로드(선로드). 실패 0.
NOTEPAD_API int Notepad_TextNNLoad(const wchar_t* modelPath) {
    std::wstring mp = tnArg(modelPath);
    if (g_tm.loaded && g_tm.path == mp) return 1;
    if (!ntcLoad(mp, g_tm)) return 0;
    return bbLoad(g_tm.backbone) ? 1 : 0;
}
NOTEPAD_API int Notepad_TextNNUnload() { g_tm = TextModel(); bbUnload(); return 1; }

// 모델 정보: "backbone|dim|pool|labels(콤마)"
NOTEPAD_API int Notepad_TextNNInfo(const wchar_t* modelPath, wchar_t* out, int outCch) {
    std::wstring mp = tnArg(modelPath);
    if (!mp.empty() && (!g_tm.loaded || g_tm.path != mp)) { if (!ntcLoad(mp, g_tm)) return 0; }
    if (!g_tm.loaded) { setErr(L"모델 미로드"); return 0; }
    std::wstring s = g_tm.backbone + L"|" + std::to_wstring(g_tm.dim) + L"|" + (g_tm.pool == 1 ? L"cls" : L"mean") + L"|";
    for (size_t i = 0; i < g_tm.labels.size(); ++i) { if (i) s += L','; s += g_tm.labels[i]; }
    return tnWriteOut(s, out, outCch);
}

// 토큰 id 열(콤마). export 시 tokens_check.txt 와 대조용. modelPath 의 백본 vocab 사용.
NOTEPAD_API int Notepad_TextNNTokens(const wchar_t* backbone, const wchar_t* text, int maxLen,
                                     wchar_t* out, int outCch) {
    std::wstring bb = tnArg(backbone);
    if (!bbLoad(bb)) return 0;
    std::vector<int64_t> ids, mask, types; encode(g_bb.vocab, tnArg(text), maxLen <= 0 ? 64 : maxLen, ids, mask, types);
    std::wstring s; wchar_t b[24];
    for (size_t i = 0; i < ids.size(); ++i) { if (i) s += L','; swprintf(b, 24, L"%lld", (long long)ids[i]); s += b; }
    return tnWriteOut(s, out, outCch);
}

NOTEPAD_API int Notepad_TextNNLastError(wchar_t* out, int outCch) {
    return tnWriteOut(g_lastErr, out, outCch);
}

// 세트 존재 확인용(AHK 에서 Note_TextNNAvailable). 항상 1(빌드되어 있으면 존재).
NOTEPAD_API int Notepad_TextNNAvailable() {
#ifdef NOTEPAD_ENABLE_ONNX
    return 1;
#else
    return 0;
#endif
}

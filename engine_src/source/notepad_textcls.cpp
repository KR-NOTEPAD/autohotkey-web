// ============================================================================
//  NOTEPAD.dll  -  notepad_textcls.cpp
//  [NOTEPAD] 세트 AO: 한국어 텍스트 분류 (kor_unsmile) → 선/악 판정.
//
//  모델: smilegate-ai/kor_unsmile (KcBERT-base 파인튜닝, BertForSequenceClassification)
//        문제유형 multi_label_classification → 라벨별 시그모이드 확률(서로 독립).
//        라벨 10개: 여성/가족, 남성, 성소수자, 인종/국적, 연령, 지역, 종교, 기타 혐오,
//                   악플/욕설, clean   (labels.txt 순서 = 모델 출력 인덱스)
//        max_position_embeddings=300 → 시퀀스 길이 상한 300.
//
//  모델 폴더 구성(models\kor_unsmile\, export_kor_unsmile.py 가 생성):
//        model.onnx        fp32 (약 430MB)
//        model_int8.onnx   동적 int8 양자화 (약 110MB, CPU 2~3배 빠름) ← 기본 우선
//        vocab.txt         WordPiece 사전 (30000)
//        labels.txt        라벨명(한 줄에 하나, 인덱스 순)
//
//  토크나이저: BertTokenizer 규약을 C++ 로 재현 (외부 의존 0).
//        do_lower_case=false, strip_accents=null, tokenize_chinese_chars=true
//        → 소문자화/악센트제거 없음(한글 보존), CJK 한자만 글자단위 분리,
//          제어문자 제거, 공백 정규화, 구두점 분리, WordPiece(##) 최장일치,
//          단어 100자 초과 또는 매칭 실패 → [UNK].
//        유니코드 분류(구두점/공백/제어)는 Win32 GetStringTypeW 로 판정.
//        검증: Notepad_TextClsTokens 로 id 열을 뽑아 export 시 기록한
//              tokens_check.txt(HF 토크나이저 결과)와 대조하면 된다.
//
//  추론: ORT C API. 세션은 static 캐시(모델 폴더가 바뀌면 재로드). 배치 1.
//        입력 int64 3개(input_ids/attention_mask/token_type_ids, 이름으로 매핑),
//        출력 logits[1,10] → sigmoid.
//
//  export 규약은 exports2.cpp 와 동일: UTF-16, 2단계 버퍼(필요문자수/0=실패),
//  bool 계열 1/0. NOTEPAD.def 에 이름 등록. NOTEPAD_ENABLE_ONNX 미정의 시 스텁.
// ============================================================================
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <cstdio>

#ifdef NOTEPAD_ENABLE_ONNX
#include <onnxruntime_c_api.h>
namespace notepad { const OrtApi* ortApiHandle(); } // notepad_onnx.cpp
#endif

#ifndef NOTEPAD_API
#define NOTEPAD_API extern "C" __declspec(dllexport)
#endif

namespace notepad { namespace textcls {

// ---- 진단 ---------------------------------------------------------------
static std::wstring g_lastErr;
static void setErr(const std::wstring& e) { g_lastErr = e; }

// ---- 문자열/파일 유틸 --------------------------------------------------------
static std::wstring utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
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
        out.erase(0, 3); // BOM
    return ok;
}
// 줄 단위 분해(\r 제거). 빈 줄도 보존(vocab 인덱스 유지용).
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

// ---- 사전 / 라벨 ----------------------------------------------------------
struct Vocab {
    std::unordered_map<std::wstring, int> tok2id;
    int unk = -1, cls = -1, sep = -1, pad = 0;
    std::vector<std::wstring> labels;
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
    v.labels.clear();
    std::string lraw;
    if (readFileUtf8(joinDir(dir, L"labels.txt"), lraw)) {
        std::vector<std::string> ll; splitLines(lraw, ll);
        for (auto& s : ll) if (!s.empty()) v.labels.push_back(utf8ToW(s));
    }
    return true;
}

// ---- 유니코드 분류 (BertTokenizer 규약 재현) ---------------------------------
// 서로게이트 쌍이면 2단위, 아니면 1단위 → 코드포인트/단위수.
static uint32_t cpAt(const std::wstring& s, size_t i, int& units) {
    wchar_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) {
        wchar_t d = s[i + 1];
        if (d >= 0xDC00 && d <= 0xDFFF) { units = 2; return 0x10000 + (((uint32_t)c - 0xD800) << 10) + ((uint32_t)d - 0xDC00); }
    }
    units = 1; return (uint32_t)c;
}
// n=2(서로게이트 쌍)이면 결과 WORD 도 2개 채워지므로 배열로 받는다. 쌍은 첫 단위 결과를 사용.
static WORD ctype1(const wchar_t* p, int n) { WORD t[2] = { 0, 0 }; GetStringTypeW(CT_CTYPE1, p, n, t); return t[0]; }

// _is_whitespace: " \t\n\r" 또는 카테고리 Zs
static bool isWs(uint32_t cp, const wchar_t* p, int n) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return true;
    if (cp < 0x80) return false;
    return (ctype1(p, n) & C1_SPACE) != 0 && cp != 0x85; // NEL 은 control 취급
}
// _is_control: "\t\n\r" 제외한 Cc/Cf
static bool isCtrl(uint32_t cp, const wchar_t* p, int n) {
    if (cp == '\t' || cp == '\n' || cp == '\r') return false;
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return true;
    // Cf(서식): ZW*, BOM, 방향제어 등 — GetStringTypeW 가 C1_CNTRL 로 잡지 못하는 대표값 보강
    if (cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
        (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB) ||
        cp == 0x061C || (cp >= 0x180B && cp <= 0x180E) || cp == 0xE0001 || (cp >= 0xE0020 && cp <= 0xE007F))
        return true;
    return cp >= 0x80 && (ctype1(p, n) & C1_CNTRL) != 0;
}
// _is_punctuation: ASCII 특수문자 4구간 또는 카테고리 P*
static bool isPunct(uint32_t cp, const wchar_t* p, int n) {
    if ((cp >= 33 && cp <= 47) || (cp >= 58 && cp <= 64) || (cp >= 91 && cp <= 96) || (cp >= 123 && cp <= 126)) return true;
    if (cp < 0x80) return false;
    return (ctype1(p, n) & C1_PUNCT) != 0;
}
// _is_chinese_char: CJK 통합한자 블록(한글은 해당 없음 → 어절 유지)
static bool isCjk(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x20000 && cp <= 0x2A6DF) || (cp >= 0x2A700 && cp <= 0x2B73F) ||
           (cp >= 0x2B740 && cp <= 0x2B81F) || (cp >= 0x2B820 && cp <= 0x2CEAF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x2F800 && cp <= 0x2FA1F);
}

// BasicTokenizer: clean → 한자 분리 → 공백 분할 → 구두점 분리
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
    // HF BasicTokenizer 는 공백분할 전에 NFC 정규화(맥/일부 앱에서 온 자소분리 한글 대응). normaliz.lib
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

// WordPiece 최장일치. 실패 시 단어 전체 [UNK].
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

// [CLS] + 토큰(최대 maxLen-2) + [SEP]. mask=1, type=0. (배치 1 → 패딩 없음)
static void encode(const Vocab& v, const std::wstring& text, int maxLen,
                   std::vector<int64_t>& ids, std::vector<int64_t>& mask, std::vector<int64_t>& types) {
    if (maxLen < 8) maxLen = 8;
    if (maxLen > 300) maxLen = 300;
    std::vector<std::wstring> words; basicTokenize(text, words);
    std::vector<int> toks;
    for (auto& w : words) { wordpiece(v, w, toks); if ((int)toks.size() >= maxLen - 2) break; }
    if ((int)toks.size() > maxLen - 2) toks.resize(maxLen - 2);
    ids.clear(); ids.push_back(v.cls);
    for (int t : toks) ids.push_back(t);
    ids.push_back(v.sep);
    mask.assign(ids.size(), 1); types.assign(ids.size(), 0);
}

// ---- 모델 세션(캐시) ---------------------------------------------------------
struct Model {
    std::wstring dir, modelPath;
    Vocab vocab;
    bool loaded = false;
#ifdef NOTEPAD_ENABLE_ONNX
    OrtEnv* env = nullptr; OrtSession* sess = nullptr; OrtSessionOptions* so = nullptr;
    OrtMemoryInfo* mem = nullptr;
    // 입력 인덱스(이름으로 매핑, 순서 무관). -1 = 모델에 없음(token_type 없는 export 대응)
    int idxIds = -1, idxMask = -1, idxType = -1;
    std::vector<std::string> inNames; std::string outName;
    std::string outPooled;   // 2026-09-23 export 부터: "pooled"[1,768] (세트 AP 특징). 없으면 ""
#endif
};
static Model g_m;

static void unload() {
#ifdef NOTEPAD_ENABLE_ONNX
    const OrtApi* ort = ortApiHandle();
    if (ort) {
        if (g_m.sess) ort->ReleaseSession(g_m.sess);
        if (g_m.so) ort->ReleaseSessionOptions(g_m.so);
        if (g_m.mem) ort->ReleaseMemoryInfo(g_m.mem);
        if (g_m.env) ort->ReleaseEnv(g_m.env);
    }
    g_m.sess = nullptr; g_m.so = nullptr; g_m.mem = nullptr; g_m.env = nullptr;
    g_m.inNames.clear(); g_m.outName.clear(); g_m.outPooled.clear(); g_m.idxIds = g_m.idxMask = g_m.idxType = -1;
#endif
    g_m.loaded = false; g_m.dir.clear(); g_m.modelPath.clear();
    g_m.vocab = Vocab();
}

// useInt8: 1 이면 model_int8.onnx 우선(없으면 model.onnx), 0 이면 model.onnx 고정.
static std::wstring pickModel(const std::wstring& dir, int useInt8) {
    std::wstring q = joinDir(dir, L"model_int8.onnx"), f = joinDir(dir, L"model.onnx");
    if (useInt8 && fileExists(q)) return q;
    if (fileExists(f)) return f;
    return useInt8 && fileExists(q) ? q : L"";
}

#ifdef NOTEPAD_ENABLE_ONNX
static std::wstring ortMsg(const OrtApi* ort, OrtStatus* st, const wchar_t* where) {
    std::wstring m = where; m += L": ";
    if (st) { m += ansiToW(ort->GetErrorMessage(st)); ort->ReleaseStatus(st); }
    return m;
}
#endif

// 경로 정규화("a\..\b" 와 "b" 를 같은 모델로 인식 → 표기만 다른 호출이 섞여도 110MB 모델을 매번 재로드하지 않게)
static std::wstring canonPath(const std::wstring& p) {
    if (p.empty()) return p;
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring r(n, L'\0');
    DWORD g = GetFullPathNameW(p.c_str(), n, &r[0], nullptr);
    if (!g || g >= n) return p;
    r.resize(g);
    while (r.size() > 3 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    return r;
}

static bool ensureLoaded(const std::wstring& dirIn, int useInt8) {
    std::wstring dir = canonPath(dirIn);
    if (dir.empty()) { if (g_m.loaded) return true; setErr(L"모델 폴더가 지정되지 않았고 로드된 모델도 없음 (Note_TextClsLoad 먼저)"); return false; }
    std::wstring mp = pickModel(dir, useInt8);
    if (mp.empty()) { setErr(L"model.onnx / model_int8.onnx 없음: " + dir + L" (models\\export_kor_unsmile.py 실행)"); return false; }
    if (g_m.loaded && g_m.modelPath == mp) return true;
    unload();
    if (!loadVocab(dir, g_m.vocab)) return false;
#ifndef NOTEPAD_ENABLE_ONNX
    setErr(L"ONNX 비활성 빌드(/DNOTEPAD_ENABLE_ONNX + third_party\\onnxruntime 필요)");
    return false;
#else
    const OrtApi* ort = ortApiHandle();
    if (!ort) { setErr(L"onnxruntime.dll 로드 실패(ORT API 없음)"); return false; }
    OrtStatus* st = nullptr;
    if ((st = ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "notepad_textcls", &g_m.env))) { setErr(ortMsg(ort, st, L"CreateEnv")); unload(); return false; }
    if ((st = ort->CreateSessionOptions(&g_m.so))) { setErr(ortMsg(ort, st, L"CreateSessionOptions")); unload(); return false; }
    ort->SetIntraOpNumThreads(g_m.so, 0);
    ort->SetSessionGraphOptimizationLevel(g_m.so, ORT_ENABLE_ALL);
    if ((st = ort->CreateSession(g_m.env, mp.c_str(), g_m.so, &g_m.sess))) { setErr(ortMsg(ort, st, (L"CreateSession(" + mp + L")").c_str())); unload(); return false; }
    if ((st = ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_m.mem))) { setErr(ortMsg(ort, st, L"CreateCpuMemoryInfo")); unload(); return false; }
    // 입력/출력 이름 조회 → 이름으로 슬롯 매핑
    OrtAllocator* alloc = nullptr; ort->GetAllocatorWithDefaultOptions(&alloc);
    size_t nIn = 0; ort->SessionGetInputCount(g_m.sess, &nIn);
    for (size_t i = 0; i < nIn; ++i) {
        char* nm = nullptr; ort->SessionGetInputName(g_m.sess, i, alloc, &nm);
        std::string s = nm ? nm : ""; if (nm) alloc->Free(alloc, nm);
        g_m.inNames.push_back(s);
        if (s.find("input_ids") != std::string::npos) g_m.idxIds = (int)i;
        else if (s.find("attention") != std::string::npos) g_m.idxMask = (int)i;
        else if (s.find("token_type") != std::string::npos) g_m.idxType = (int)i;
    }
    { char* nm = nullptr; ort->SessionGetOutputName(g_m.sess, 0, alloc, &nm); g_m.outName = nm ? nm : ""; if (nm) alloc->Free(alloc, nm); }
    {   // 선택 출력 "pooled" (logits 는 항상 0번. pooled 는 세트 AP 만 요청하므로 기존 경로 성능 영향 없음)
        size_t nOut = 0; ort->SessionGetOutputCount(g_m.sess, &nOut);
        for (size_t i = 1; i < nOut; ++i) {
            char* nm = nullptr; ort->SessionGetOutputName(g_m.sess, i, alloc, &nm);
            std::string s = nm ? nm : ""; if (nm) alloc->Free(alloc, nm);
            if (s == "pooled") { g_m.outPooled = s; break; }
        }
    }
    if (g_m.idxIds < 0) { setErr(L"모델 입력에 input_ids 없음"); unload(); return false; }
    g_m.dir = dir; g_m.modelPath = mp; g_m.loaded = true;
    return true;
#endif
}

// 텍스트 → 라벨별 확률(sigmoid). 실패 false(+lastErr). applySigmoid=false 면 raw logits(세트 AP 파인튜닝용).
static bool infer(const std::wstring& text, int maxLen, std::vector<float>& probs, bool applySigmoid = true,
                  std::vector<float>* pooled = nullptr) {
    probs.clear();
    if (pooled) pooled->clear();
    if (!g_m.loaded) { setErr(L"모델 미로드"); return false; }
#ifndef NOTEPAD_ENABLE_ONNX
    setErr(L"ONNX 비활성 빌드"); return false;
#else
    const OrtApi* ort = ortApiHandle(); if (!ort) { setErr(L"ORT API 없음"); return false; }
    std::vector<int64_t> ids, mask, types; encode(g_m.vocab, text, maxLen, ids, mask, types);
    int64_t shape[2] = { 1, (int64_t)ids.size() };
    size_t nIn = g_m.inNames.size();
    std::vector<OrtValue*> vals(nIn, nullptr);
    std::vector<const char*> names(nIn);
    for (size_t i = 0; i < nIn; ++i) names[i] = g_m.inNames[i].c_str();
    auto mk = [&](int idx, std::vector<int64_t>& buf) -> bool {
        if (idx < 0) return true;
        OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(g_m.mem, buf.data(), buf.size() * sizeof(int64_t),
                            shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &vals[idx]);
        if (st) { setErr(ortMsg(ort, st, L"CreateTensor")); return false; }
        return true;
    };
    bool ok = mk(g_m.idxIds, ids) && mk(g_m.idxMask, mask) && mk(g_m.idxType, types);
    // 이름 매핑에 안 걸린 여분 입력이 있으면 그 자리도 int64 0 텐서로 채움(안전장치)
    std::vector<int64_t> zeros(ids.size(), 0);
    for (size_t i = 0; ok && i < nIn; ++i) if (!vals[i]) {
        OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(g_m.mem, zeros.data(), zeros.size() * sizeof(int64_t),
                            shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &vals[i]);
        if (st) { setErr(ortMsg(ort, st, L"CreateTensor(extra)")); ok = false; }
    }
    if (ok) {
        bool wantPooled = pooled && !g_m.outPooled.empty();
        if (pooled && !wantPooled) { setErr(L"베이스 모델에 pooled 출력 없음 (python export_kor_unsmile.py 로 재export 필요)"); ok = false; }
    }
    if (ok) {
        bool wantPooled = pooled != nullptr;
        const char* outNames[] = { g_m.outName.c_str(), g_m.outPooled.c_str() };
        OrtValue* outVals[2] = { nullptr, nullptr };
        OrtStatus* st = ort->Run(g_m.sess, nullptr, names.data(), (const OrtValue* const*)vals.data(), nIn, outNames, wantPooled ? 2 : 1, outVals);
        OrtValue* outVal = outVals[0];
        if (!st && wantPooled && outVals[1]) {
            float* pd = nullptr; OrtTensorTypeAndShapeInfo* pi = nullptr; size_t pc = 0;
            if (!ort->GetTensorMutableData(outVals[1], (void**)&pd) && !ort->GetTensorTypeAndShape(outVals[1], &pi)) {
                ort->GetTensorShapeElementCount(pi, &pc); ort->ReleaseTensorTypeAndShapeInfo(pi);
                pooled->assign(pd, pd + pc);
            }
            ort->ReleaseValue(outVals[1]);
            if (pooled->empty()) { setErr(L"pooled 출력 읽기 실패"); ok = false; }
        }
        if (st) { setErr(ortMsg(ort, st, L"Run")); ok = false; }
        else if (outVal) {
            float* data = nullptr; OrtTensorTypeAndShapeInfo* info = nullptr; size_t count = 0;
            if (!ort->GetTensorMutableData(outVal, (void**)&data) && !ort->GetTensorTypeAndShape(outVal, &info)) {
                ort->GetTensorShapeElementCount(info, &count);
                ort->ReleaseTensorTypeAndShapeInfo(info);
                probs.resize(count);
                for (size_t i = 0; i < count; ++i) probs[i] = applySigmoid ? 1.0f / (1.0f + std::exp(-data[i])) : data[i];
            } else { setErr(L"출력 텐서 읽기 실패"); ok = false; }
            ort->ReleaseValue(outVal);
        }
    }
    for (auto* v : vals) if (v) ort->ReleaseValue(v);
    return ok && !probs.empty();
#endif
}

static std::wstring labelOf(size_t i) {
    if (i < g_m.vocab.labels.size()) return g_m.vocab.labels[i];
    wchar_t b[16]; swprintf(b, 16, L"label%u", (unsigned)i); return b;
}
static std::wstring fmt4(float p) { wchar_t b[32]; swprintf(b, 32, L"%.4f", p); return b; }

// ---- 세트 AP(notepad_textft.cpp) 브리지: 베이스 모델의 raw logits / 라벨 / 오류 ------------
// 다른 TU 에서 static 내부를 못 쓰므로 여기서만 non-static 으로 노출한다(export 아님).
bool clsLogits(const std::wstring& dir, const std::wstring& text, int maxLen, std::vector<float>& logits) {
    if (!ensureLoaded(dir, 1)) return false;
    return infer(text, maxLen, logits, false);
}
// logits + pooled(마지막 은닉 mean-pool) 를 한 번의 추론으로. 모델에 pooled 출력이 없으면 false.
bool clsLogitsPooled(const std::wstring& dir, const std::wstring& text, int maxLen,
                     std::vector<float>& logits, std::vector<float>& pooled) {
    if (!ensureLoaded(dir, 1)) return false;
    return infer(text, maxLen, logits, false, &pooled);
}
bool clsHasPooled(const std::wstring& dir) {
#ifdef NOTEPAD_ENABLE_ONNX
    return ensureLoaded(dir, 1) && !g_m.outPooled.empty();
#else
    (void)dir; return false;
#endif
}
bool clsLabels(const std::wstring& dir, std::vector<std::wstring>& labels) {
    if (!ensureLoaded(dir, 1)) return false;
    labels = g_m.vocab.labels; return true;
}
const std::wstring& clsLastError() { return g_lastErr; }

}} // namespace notepad::textcls

// ============================================================================
//  exports (AHK v1 64비트 DllCall). 출력 규약 = exports2.cpp 의 writeOut2 와 동일.
// ============================================================================
static int tcWriteOut(const std::wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}
static std::wstring tcArg(const wchar_t* p) { return p ? std::wstring(p) : std::wstring(); }

using namespace notepad::textcls;

// 모델 로드(선로드). modelDir: model*.onnx + vocab.txt + labels.txt 폴더. useInt8: int8 우선 여부.
NOTEPAD_API int Notepad_TextClsLoad(const wchar_t* modelDir, int useInt8) {
    return ensureLoaded(tcArg(modelDir), useInt8) ? 1 : 0;
}
NOTEPAD_API int Notepad_TextClsUnload() { unload(); return 1; }

// 라벨별 확률. 한 줄에 "라벨,확률(0~1)" (인덱스 순, 개행 구분). modelDir "" = 로드된 모델 사용.
NOTEPAD_API int Notepad_TextCls(const wchar_t* modelDir, const wchar_t* text, int maxLen,
                                wchar_t* out, int outCch) {
    if (!ensureLoaded(tcArg(modelDir), 1)) return 0;
    std::vector<float> p;
    if (!infer(tcArg(text), maxLen, p)) return 0;
    std::wstring s;
    for (size_t i = 0; i < p.size(); ++i) { if (i) s += L'\n'; s += labelOf(i) + L"," + fmt4(p[i]); }
    return tcWriteOut(s, out, outCch);
}

// 선/악 판정. thrPct: 혐오/욕설 라벨 중 하나라도 확률 ≥ thrPct% 이면 "악".
//   반환 "선|악,최고혐오라벨,그확률,clean확률"  예) "악,악플/욕설,0.9731,0.0122"
NOTEPAD_API int Notepad_TextClsVerdict(const wchar_t* modelDir, const wchar_t* text, int thrPct, int maxLen,
                                       wchar_t* out, int outCch) {
    if (!ensureLoaded(tcArg(modelDir), 1)) return 0;
    std::vector<float> p;
    if (!infer(tcArg(text), maxLen, p)) return 0;
    if (thrPct < 1) thrPct = 1;
    if (thrPct > 99) thrPct = 99;
    // clean 인덱스: labels.txt 에서 "clean" 을 찾고, 없으면 마지막 인덱스로 간주
    size_t cleanIdx = p.size() - 1;
    for (size_t i = 0; i < g_m.vocab.labels.size() && i < p.size(); ++i)
        if (g_m.vocab.labels[i] == L"clean") { cleanIdx = i; break; }
    size_t best = (cleanIdx == 0 && p.size() > 1) ? 1 : 0; float bv = -1.f;
    for (size_t i = 0; i < p.size(); ++i) { if (i == cleanIdx) continue; if (p[i] > bv) { bv = p[i]; best = i; } }
    bool evil = bv >= (float)thrPct / 100.0f;
    std::wstring s = (evil ? L"악" : L"선");
    s += L"," + labelOf(best) + L"," + fmt4(bv < 0 ? 0.f : bv) + L"," + fmt4(p[cleanIdx]);
    return tcWriteOut(s, out, outCch);
}

// 라벨 목록(개행 구분, 인덱스 순)
NOTEPAD_API int Notepad_TextClsLabels(const wchar_t* modelDir, wchar_t* out, int outCch) {
    if (!ensureLoaded(tcArg(modelDir), 1)) return 0;
    std::wstring s;
    for (size_t i = 0; i < g_m.vocab.labels.size(); ++i) { if (i) s += L'\n'; s += g_m.vocab.labels[i]; }
    return tcWriteOut(s, out, outCch);
}

// 토큰 id 열(콤마). HF 토크나이저와 대조용(tokens_check.txt). 모델 없이 vocab.txt 만 있어도 동작.
NOTEPAD_API int Notepad_TextClsTokens(const wchar_t* modelDir, const wchar_t* text, int maxLen,
                                      wchar_t* out, int outCch) {
    std::wstring dir = tcArg(modelDir);
    Vocab tmp; const Vocab* v = nullptr;
    if (g_m.loaded && (dir.empty() || dir == g_m.dir)) v = &g_m.vocab;
    else { if (!loadVocab(dir, tmp)) return 0; v = &tmp; }
    std::vector<int64_t> ids, mask, types; encode(*v, tcArg(text), maxLen, ids, mask, types);
    std::wstring s; wchar_t b[24];
    for (size_t i = 0; i < ids.size(); ++i) { if (i) s += L','; swprintf(b, 24, L"%lld", (long long)ids[i]); s += b; }
    return tcWriteOut(s, out, outCch);
}

NOTEPAD_API int Notepad_TextClsLastError(wchar_t* out, int outCch) {
    return tcWriteOut(g_lastErr, out, outCch);
}

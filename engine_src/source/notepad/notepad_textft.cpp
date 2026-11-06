// ============================================================================
//  NOTEPAD.dll  -  notepad_textft.cpp
//  [NOTEPAD] 세트 AP: 폴더형 분류 모델(kor_unsmile 등) "파인튜닝" — 파이썬 없이 DLL 안에서.
//
//  파이썬 train_kor_unsmile.py 가 하던 일(내 TSV 로 kor_unsmile 을 재학습)을 AHK 한 줄로 한다.
//  BERT 전체를 C++ 로 역전파하는 대신 "잔차(residual) 파인튜닝" 을 쓴다:
//
//     문장 ──► 베이스(kor_unsmile model.onnx) ──► 원본 logits (nlab)  ─────────┐
//         └─► 백본(kor_embed kosimcse_embed.onnx) ──► 768-d 임베딩 ──┐        (+)──► sigmoid ──► 라벨 확률
//                                 x = [임베딩 ; logits*lscale] ──► MLP Δ ────┘
//
//   최종 logit = 원본 logit + Δ(x).  Δ 의 출력층을 0 근처로 초기화하므로 학습 전엔 베이스와 동일하고,
//   학습 데이터가 "틀렸다"고 말하는 곳(예: ㄹㅇ 을 악플로 보는 과탐)만 고쳐진다.
//   → 수백 문장으로 BERT 전체를 재학습할 때 생기는 망각(catastrophic forgetting)이 구조적으로 없다.
//   베이스/백본은 동결(추론만). 학습되는 것은 MLP(in→hid ReLU→nlab) 뿐 → CPU 수 초.
//
//  손실: BCEWithLogits(멀티라벨, 파이썬과 동일 규약). SGD+모멘텀+L2. 임베딩은 L2 정규화된 값.
//
//  입력 데이터셋: "라벨목록<TAB>문장" (train_data.tsv 와 동일. 라벨 여러 개면 콤마, 혐오 없으면 clean)
//                '#' 주석/빈줄 무시, 모르는 라벨은 건너뜀(리포트에 수 기록).
//  산출 .ftc(텍스트):
//     NPFT1
//     base=<베이스 모델 폴더>      backbone=<백본 onnx 경로>
//     labels=...(콤마)  dim=768  nlab=10  hidden=64  maxlen=128  pool=mean  lscale=0.2  samples=N
//     W1 / b1 / W2 / b2 (공백구분 float)
//
//  추론 규약은 세트 AO(TextCls) 와 동일 문자열 → 봇 코드에서 Note_TextClsVerdict 를 Note_TextFTVerdict 로
//  바꾸기만 하면 된다. 베이스/백본 세션은 각 모듈(textcls/textnn)이 캐시.
//  export 규약: UTF-16, 2단계 버퍼(필요문자수/음수=버퍼부족). NOTEPAD.def 등록. ONNX 미정의 시 실패.
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <random>

#ifndef NOTEPAD_API
#define NOTEPAD_API extern "C" __declspec(dllexport)
#endif

// ---- 브리지: 다른 TU 의 non-static 함수 (notepad_textcls.cpp / notepad_textnn.cpp) ----
namespace notepad { namespace textcls {
    bool clsLogits(const std::wstring& dir, const std::wstring& text, int maxLen, std::vector<float>& logits);
    bool clsLabels(const std::wstring& dir, std::vector<std::wstring>& labels);
    bool clsLogitsPooled(const std::wstring& dir, const std::wstring& text, int maxLen,
                         std::vector<float>& logits, std::vector<float>& pooled);
    bool clsHasPooled(const std::wstring& dir);
    const std::wstring& clsLastError();
}}
namespace notepad { namespace textnn {
    bool nnEmbed(const std::wstring& backbone, const std::wstring& text, int maxLen, int pool, std::vector<float>& emb);
    const std::wstring& nnLastError();
}}

namespace notepad { namespace textft {

// ---- 진단 -------------------------------------------------------------------
static std::wstring g_lastErr, g_report;
static void setErr(const std::wstring& e) { g_lastErr = e; }

// ---- 문자열/파일 유틸 (textcls/textnn 과 동일 규약) ----------------------------
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
static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
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
static void trimW(std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"); size_t b = s.find_last_not_of(L" \t");
    if (a == std::wstring::npos) s.clear(); else s = s.substr(a, b - a + 1);
}
static std::string fnum(float f) { char b[32]; snprintf(b, sizeof(b), "%.6g", f); return b; }
static std::wstring fmt4(float p) { wchar_t b[32]; swprintf(b, 32, L"%.4f", p); return b; }
static void parseFloatsInto(const std::string& s, std::vector<float>& v) {
    v.clear(); const char* p = s.c_str(); char* e = nullptr;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p; if (!*p) break;
        float f = strtof(p, &e); if (e == p) break; v.push_back(f); p = e;
    }
}
static inline float sigm(float z) { return 1.0f / (1.0f + std::exp(-z)); }

// ---- 경로 유틸: .ftc 에 절대경로 + (.ftc 기준)상대경로를 같이 저장 → 다른 PC/드라이브 문자에서도 열림 ----
static std::wstring fullPath(const std::wstring& p) {
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
static std::wstring joinDir(const std::wstring& dir, const std::wstring& file) {
    if (dir.empty()) return file;
    wchar_t last = dir.back();
    return (last == L'\\' || last == L'/') ? dir + file : dir + L"\\" + file;
}
static std::wstring dirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : path.substr(0, p);
}
// fromDir → to 상대경로(둘 다 fullPath). 루트(드라이브)가 다르면 "".
static std::wstring relPath(const std::wstring& fromDir, const std::wstring& to) {
    auto split = [](const std::wstring& s) {
        std::vector<std::wstring> v; std::wstring cur;
        for (wchar_t c : s) { if (c == L'\\' || c == L'/') { if (!cur.empty()) v.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) v.push_back(cur);
        return v;
    };
    auto a = split(fromDir), b = split(to);
    if (a.empty() || b.empty() || _wcsicmp(a[0].c_str(), b[0].c_str()) != 0) return L"";
    size_t i = 0;
    while (i < a.size() && i < b.size() && _wcsicmp(a[i].c_str(), b[i].c_str()) == 0) ++i;
    std::wstring r;
    for (size_t k = i; k < a.size(); ++k) r += L"..\\";
    for (size_t k = i; k < b.size(); ++k) { r += b[k]; if (k + 1 < b.size()) r += L'\\'; }
    return r.empty() ? L"." : r;
}
static long long fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}
// 베이스 모델 지문: 크기 + 앞/중간/끝 256KB FNV-1a. 같은 구조로 재학습하면 크기가 똑같으므로 크기만으론 못 잡는다.
static std::wstring fingerprint(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz; if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return L""; }
    const LONGLONG CH = 256 * 1024;
    LONGLONG offs[3] = { 0, sz.QuadPart / 2, sz.QuadPart > CH ? sz.QuadPart - CH : 0 };
    unsigned long long hs = 1469598103934665603ULL;
    std::vector<unsigned char> buf((size_t)CH);
    for (LONGLONG o : offs) {
        LARGE_INTEGER li; li.QuadPart = o; SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
        DWORD rd = 0; ReadFile(h, buf.data(), (DWORD)CH, &rd, nullptr);
        for (DWORD i = 0; i < rd; ++i) { hs ^= buf[i]; hs *= 1099511628211ULL; }
    }
    CloseHandle(h);
    wchar_t b[64]; swprintf(b, 64, L"%lld-%016llx", (long long)sz.QuadPart, hs);
    return b;
}
// 세트 AO(textcls) 의 pickModel(useInt8=1) 과 동일 규칙: int8 우선, 없으면 fp32.
static std::wstring pickBaseName(const std::wstring& dir) {
    if (fileExists(joinDir(dir, L"model_int8.onnx"))) return L"model_int8.onnx";
    if (fileExists(joinDir(dir, L"model.onnx"))) return L"model.onnx";
    return L"";
}

// ============================================================================
//  잔차 헤드 MLP: x(in) → 은닉(ReLU, hid) → Δ(out).  최종 logit z = base + Δ.
//  손실 BCEWithLogits(멀티라벨). SGD+모멘텀+L2. (notepad_textnn.cpp 의 MLP 와 동형, 출력층만 다름)
// ============================================================================
struct Head {
    int in = 0, hid = 64, out = 0;
    std::vector<float> W1, b1, W2, b2;
    std::vector<float> vW1, vb1, vW2, vb2;
    void init(int inDim, int hidden, int outDim, unsigned seed = 12345) {
        in = inDim; hid = hidden; out = outDim;
        std::mt19937 rng(seed);
        auto he = [&](int fan, float scale) { std::normal_distribution<float> d(0.0f, scale * std::sqrt(2.0f / fan)); return d(rng); };
        W1.resize((size_t)in * hid); for (auto& w : W1) w = he(in, 1.0f);
        b1.assign(hid, 0.0f);
        // 출력층은 아주 작게(0 은 W1 이 영원히 학습 안 됨) → 학습 전 Δ≈0 = 베이스 그대로
        W2.resize((size_t)hid * out); for (auto& w : W2) w = he(hid, 0.01f);
        b2.assign(out, 0.0f);
        resetMomentum();
    }
    void resetMomentum() {
        vW1.assign(W1.size(), 0.0f); vb1.assign(b1.size(), 0.0f);
        vW2.assign(W2.size(), 0.0f); vb2.assign(b2.size(), 0.0f);
    }
    void forward(const std::vector<float>& x, std::vector<float>& h, std::vector<float>& d) const {
        h.assign(hid, 0.0f);
        const int n = std::min(in, (int)x.size());
        for (int j = 0; j < hid; ++j) {
            float s = b1[j]; const float* w = &W1[(size_t)j * in];
            for (int i = 0; i < n; ++i) s += w[i] * x[i];
            h[j] = s > 0 ? s : 0;
        }
        d.assign(out, 0.0f);
        for (int k = 0; k < out; ++k) {
            float s = b2[k]; const float* w = &W2[(size_t)k * hid];
            for (int j = 0; j < hid; ++j) s += w[j] * h[j];
            d[k] = s;
        }
    }
    // 한 샘플 SGD. base: 원본 logits, y: multihot. 반환 BCE 손실(라벨 평균).
    //   dpen: Δ 크기 벌점(dpen·Δ²). 베이스가 이미 맞는 곳에서 확률을 0/1 로 끝까지 밀어붙이는 것(과신·과적합)과
    //         학습 문장 주변의 불필요한 드리프트를 억제한다. 베이스가 틀린 곳은 BCE 이득이 커서 그대로 고쳐진다.
    float trainStep(const std::vector<float>& x, const std::vector<float>& base, const std::vector<float>& y,
                    float lr, float mom, float l2, float dpen) {
        std::vector<float> h, d; forward(x, h, d);
        std::vector<float> dz(out); float loss = 0.0f;
        for (int k = 0; k < out; ++k) {
            float z = base[k] + d[k], p = sigm(z), t = y[k];
            // 수치 안정 BCE: max(z,0) - z*t + log(1+exp(-|z|))
            loss += std::max(z, 0.0f) - z * t + std::log1p(std::exp(-std::fabs(z))) + dpen * d[k] * d[k];
            dz[k] = p - t + 2.0f * dpen * d[k];
        }
        std::vector<float> dh(hid, 0.0f);
        for (int k = 0; k < out; ++k) {
            float g = dz[k]; float* w = &W2[(size_t)k * hid]; float* vw = &vW2[(size_t)k * hid];
            for (int j = 0; j < hid; ++j) { float grad = g * h[j] + l2 * w[j]; vw[j] = mom * vw[j] - lr * grad; dh[j] += g * w[j]; w[j] += vw[j]; }
            vb2[k] = mom * vb2[k] - lr * g; b2[k] += vb2[k];
        }
        for (int j = 0; j < hid; ++j) if (h[j] <= 0) dh[j] = 0;
        const int n = std::min(in, (int)x.size());
        for (int j = 0; j < hid; ++j) {
            float g = dh[j]; if (g == 0.0f) continue;
            float* w = &W1[(size_t)j * in]; float* vw = &vW1[(size_t)j * in];
            for (int i = 0; i < n; ++i) { float grad = g * x[i] + l2 * w[i]; vw[i] = mom * vw[i] - lr * grad; w[i] += vw[i]; }
            vb1[j] = mom * vb1[j] - lr * g; b1[j] += vb1[j];
        }
        return loss / (out > 0 ? out : 1);
    }
};

// ============================================================================
//  모델 상태 / .ftc 저장·로드
// ============================================================================
struct FTModel {
    std::wstring base, backbone, path;
    std::wstring baseRel, backboneRel;   // .ftc 폴더 기준 상대경로(다른 PC 폴백)
    std::wstring baseModel;              // 학습 시 사용한 베이스 파일(model_int8.onnx / model.onnx)
    long long baseSize = -1;             // 그 파일 크기 — 베이스가 바뀌면 Δ 가 무의미해지므로 로드 시 대조
    std::wstring baseFp;                 // 지문(크기+샘플 해시). 있으면 이것으로 대조
    int bestEpoch = 0;
    int dim = 0, nlab = 0, hid = 64, maxlen = 128, pool = 0, samples = 0;
    float lscale = 0.2f;   // 원본 logits(±5~10) 을 임베딩(성분 ~0.04) 스케일에 맞춰 입력에 섞는 계수
    std::vector<std::wstring> labels;
    // 임베딩 표준화(z-score, 학습셋 기준). 베이스 pooled 는 분류용으로 학습된 BERT 라 벡터들이 한 방향에 몰려 있어
    // (비등방성) 그대로 쓰면 헤드가 큰 가중치로 미세한 차이를 잡으려다 일반화가 나빠진다 → 평균 빼고 차원별 표준편차로 나눔.
    std::vector<float> mu, sd;
    Head net;
    bool loaded = false;
};
static FTModel g_ft;
static const float kZScale = 0.036f;   // 표준화 후 스케일(≈1/sqrt(768)) — L2 정규화 임베딩과 같은 크기대로 맞춤
// z 는 ±6 으로 자름: 학습셋에서 분산이 아주 작던 차원이 낯선 문장에서 튀어 헤드를 폭주시키지 않게.
static inline float zstd(float v, float mu, float sd) {
    float z = (v - mu) / sd;
    if (z > 6.0f) z = 6.0f; else if (z < -6.0f) z = -6.0f;
    return z * kZScale;
}

static bool ftcSave(const std::wstring& path, const FTModel& m) {
    std::string o = "NPFT1\n";
    std::wstring fdir = dirOf(fullPath(path));
    o += "base=" + wToUtf8(m.base) + "\n";
    o += "backbone=" + wToUtf8(m.backbone) + "\n";
    o += "base_rel=" + wToUtf8(relPath(fdir, m.base)) + "\n";
    o += "backbone_rel=" + wToUtf8(relPath(fdir, m.backbone)) + "\n";
    o += "base_model=" + wToUtf8(m.baseModel) + "\n";
    o += "base_size=" + std::to_string(m.baseSize) + "\n";
    o += "base_fp=" + wToUtf8(m.baseFp) + "\n";
    o += "best_epoch=" + std::to_string(m.bestEpoch) + "\n";
    std::string lab; for (size_t i = 0; i < m.labels.size(); ++i) { if (i) lab += ","; lab += wToUtf8(m.labels[i]); }
    o += "labels=" + lab + "\n";
    o += "dim=" + std::to_string(m.dim) + "\n";
    o += "nlab=" + std::to_string(m.nlab) + "\n";
    o += "hidden=" + std::to_string(m.hid) + "\n";
    o += "maxlen=" + std::to_string(m.maxlen) + "\n";
    o += "pool=" + std::string(m.pool == 1 ? "cls" : "mean") + "\n";
    o += "lscale=" + fnum(m.lscale) + "\n";
    o += "samples=" + std::to_string(m.samples) + "\n";
    auto row = [&](const char* key, const std::vector<float>& v) {
        o += key; o += "=";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ' '; o += fnum(v[i]); }
        o += "\n";
    };
    if (!m.mu.empty()) { row("mu", m.mu); row("sd", m.sd); }
    row("W1", m.net.W1); row("b1", m.net.b1); row("W2", m.net.W2); row("b2", m.net.b2);
    return writeFileUtf8(path, o);
}
static bool ftcLoad(const std::wstring& path, FTModel& m) {
    std::string raw; if (!readFileUtf8(path, raw)) { setErr(L".ftc 읽기 실패: " + path); return false; }
    std::vector<std::string> lines; splitLines(raw, lines);
    if (lines.empty() || lines[0].rfind("NPFT1", 0) != 0) { setErr(L".ftc 형식 아님(NPFT1 헤더 없음): " + path); return false; }
    m = FTModel();
    std::vector<float> W1, b1, W2, b2;
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string& ln = lines[i]; size_t eq = ln.find('=');
        if (eq == std::string::npos) continue;
        std::string k = ln.substr(0, eq), val = ln.substr(eq + 1);
        if (k == "base") m.base = utf8ToW(val);
        else if (k == "backbone") m.backbone = utf8ToW(val);
        else if (k == "base_rel") m.baseRel = utf8ToW(val);
        else if (k == "backbone_rel") m.backboneRel = utf8ToW(val);
        else if (k == "base_model") m.baseModel = utf8ToW(val);
        else if (k == "base_size") m.baseSize = _atoi64(val.c_str());
        else if (k == "base_fp") m.baseFp = utf8ToW(val);
        else if (k == "best_epoch") m.bestEpoch = atoi(val.c_str());
        else if (k == "dim") m.dim = atoi(val.c_str());
        else if (k == "nlab") m.nlab = atoi(val.c_str());
        else if (k == "hidden") m.hid = atoi(val.c_str());
        else if (k == "maxlen") m.maxlen = atoi(val.c_str());
        else if (k == "pool") m.pool = (val == "cls") ? 1 : 0;
        else if (k == "lscale") m.lscale = strtof(val.c_str(), nullptr);
        else if (k == "samples") m.samples = atoi(val.c_str());
        else if (k == "labels") { size_t st = 0; while (st <= val.size()) { size_t c = val.find(',', st); std::string t = val.substr(st, c == std::string::npos ? std::string::npos : c - st); if (!t.empty()) m.labels.push_back(utf8ToW(t)); if (c == std::string::npos) break; st = c + 1; } }
        else if (k == "mu") parseFloatsInto(val, m.mu);
        else if (k == "sd") parseFloatsInto(val, m.sd);
        else if (k == "W1") parseFloatsInto(val, W1);
        else if (k == "b1") parseFloatsInto(val, b1);
        else if (k == "W2") parseFloatsInto(val, W2);
        else if (k == "b2") parseFloatsInto(val, b2);
    }
    int in = m.dim + m.nlab, out = m.nlab;
    if (out < 2 || m.dim <= 0 || (int)m.labels.size() != out || (int)b1.size() != m.hid ||
        (int)W1.size() != in * m.hid || (int)W2.size() != m.hid * out || (int)b2.size() != out ||
        (!m.mu.empty() && ((int)m.mu.size() != m.dim || (int)m.sd.size() != m.dim))) {
        setErr(L".ftc 가중치 크기 불일치: " + path); return false;
    }
    m.net.in = in; m.net.hid = m.hid; m.net.out = out;
    m.net.W1 = W1; m.net.b1 = b1; m.net.W2 = W2; m.net.b2 = b2; m.net.resetMomentum();
    m.loaded = true; m.path = path;
    return true;
}

// ============================================================================
//  특징 벡터 / 데이터셋 / 지표
// ============================================================================
// 문장 → x=[임베딩 ; logits*lscale], logits=베이스 원본 logits. 실패 false(+lastErr).
//   backbone "" (기본, 2026-09-23~) = 베이스 자신의 pooled 출력(마지막 은닉 mean-pool)을 특징으로 → 모델 1개만 추론.
//   backbone 경로 지정 = 예전 방식(별도 kor_embed 백본, 메모리 +440MB, 추론 2회).
static bool features(const FTModel& m, const std::wstring& text, std::vector<float>& x, std::vector<float>& logits) {
    std::vector<float> emb;
    if (m.backbone.empty()) {
        if (!textcls::clsLogitsPooled(m.base, text, m.maxlen, logits, emb)) { setErr(L"베이스 추론 실패: " + textcls::clsLastError()); return false; }
        double s2 = 0; for (float v : emb) s2 += (double)v * v;          // L2 정규화(백본 방식과 동일 스케일)
        s2 = std::sqrt(s2); if (s2 > 1e-9) for (auto& v : emb) v = (float)(v / s2);
    } else {
        if (!textcls::clsLogits(m.base, text, m.maxlen, logits)) { setErr(L"베이스 추론 실패: " + textcls::clsLastError()); return false; }
        if (!textnn::nnEmbed(m.backbone, text, m.maxlen, m.pool, emb)) { setErr(L"백본 임베딩 실패: " + textnn::nnLastError()); return false; }
    }
    if (m.nlab > 0 && (int)logits.size() != m.nlab) { setErr(L"베이스 출력 수(" + std::to_wstring(logits.size()) + L") != 라벨 수(" + std::to_wstring(m.nlab) + L")"); return false; }
    if (m.dim > 0 && (int)emb.size() != m.dim) { setErr(L"백본 차원(" + std::to_wstring(emb.size()) + L") != 모델 dim(" + std::to_wstring(m.dim) + L")"); return false; }
    if (!m.mu.empty() && m.mu.size() == emb.size())
        for (size_t j = 0; j < emb.size(); ++j) emb[j] = zstd(emb[j], m.mu[j], m.sd[j]);
    x.clear(); x.reserve(emb.size() + logits.size());
    x.insert(x.end(), emb.begin(), emb.end());
    for (float l : logits) x.push_back(l * m.lscale);
    return true;
}

// 최종 확률: sigmoid(base + Δ). 실패 false.
static bool predict(const FTModel& m, const std::wstring& text, std::vector<float>& probs) {
    std::vector<float> x, base, h, d;
    if (!features(m, text, x, base)) return false;
    m.net.forward(x, h, d);
    probs.resize(base.size());
    for (size_t k = 0; k < base.size(); ++k) probs[k] = sigm(base[k] + (k < d.size() ? d[k] : 0.0f));
    return true;
}

// "라벨목록<TAB>문장" 읽기. labels 로 multihot. 모르는 라벨/TAB 없는 줄은 건너뛰고 카운트.
struct Sample { std::vector<float> y; std::wstring text; std::wstring rawLab; };
static bool readDataset(const std::wstring& path, const std::vector<std::wstring>& labels,
                        std::vector<Sample>& out, int& skippedUnknown, int& skippedFormat) {
    skippedUnknown = skippedFormat = 0;
    std::string raw; if (!readFileUtf8(path, raw)) { setErr(L"데이터셋 읽기 실패: " + path); return false; }
    std::vector<std::string> lines; splitLines(raw, lines);
    for (auto& ln8 : lines) {
        std::wstring ln = utf8ToW(ln8); trimW(ln);
        if (ln.empty() || ln[0] == L'#') continue;
        size_t tab = ln.find(L'\t');
        if (tab == std::wstring::npos) { skippedFormat++; continue; }
        std::wstring labpart = ln.substr(0, tab), txt = ln.substr(tab + 1); trimW(txt);
        if (txt.empty()) { skippedFormat++; continue; }
        Sample s; s.y.assign(labels.size(), 0.0f); s.text = txt; s.rawLab = labpart;
        bool hit = false, unknown = false; size_t st = 0;
        while (st <= labpart.size()) {
            size_t c = labpart.find(L',', st);
            std::wstring lab = labpart.substr(st, c == std::wstring::npos ? std::wstring::npos : c - st); trimW(lab);
            if (!lab.empty()) {
                bool found = false;
                for (size_t i = 0; i < labels.size(); ++i) if (labels[i] == lab) { s.y[i] = 1.0f; found = true; hit = true; break; }
                if (!found) unknown = true;
            }
            if (c == std::wstring::npos) break; st = c + 1;
        }
        if (!hit) { skippedUnknown++; continue; }
        if (unknown) skippedUnknown++;   // 일부 라벨만 모름 → 아는 라벨로 학습하되 카운트
        out.push_back(std::move(s));
    }
    if (out.empty()) { setErr(L"학습 가능한 문장이 없음(라벨목록<TAB>문장 형식, 라벨은 labels.txt 와 동일): " + path); return false; }
    return true;
}

static size_t cleanIndex(const std::vector<std::wstring>& labels) {
    for (size_t i = 0; i < labels.size(); ++i) if (labels[i] == L"clean") return i;
    return labels.empty() ? 0 : labels.size() - 1;
}
// 선/악: clean 제외 라벨 중 최대 확률 ≥ thr 이면 악. 반환 best 인덱스, bv 에 그 확률.
static bool isEvil(const std::vector<float>& p, size_t cleanIdx, float thr, size_t& best, float& bv) {
    best = (cleanIdx == 0 && p.size() > 1) ? 1 : 0; bv = -1.f;
    for (size_t i = 0; i < p.size(); ++i) { if (i == cleanIdx) continue; if (p[i] > bv) { bv = p[i]; best = i; } }
    return bv >= thr;
}
struct Metrics { int n = 0, exact = 0, verdictOk = 0; long tp = 0, fp = 0, fn = 0; double lossSum = 0; };
static void accumulate(Metrics& mt, const std::vector<float>& p, const std::vector<float>& y, size_t cleanIdx, float thr) {
    mt.n++; bool ex = true; double ls = 0;
    for (size_t k = 0; k < p.size() && k < y.size(); ++k) {
        bool pr = p[k] >= 0.5f, gd = y[k] >= 0.5f;
        if (pr && gd) mt.tp++; else if (pr) mt.fp++; else if (gd) mt.fn++;
        if (pr != gd) ex = false;
        double pk = std::min(1.0 - 1e-7, std::max(1e-7, (double)p[k]));
        ls += -(y[k] * std::log(pk) + (1.0 - y[k]) * std::log(1.0 - pk));   // BCE(벌점 제외)
    }
    if (!p.empty()) mt.lossSum += ls / p.size();
    if (ex) mt.exact++;
    size_t b; float bv; bool pe = isEvil(p, cleanIdx, thr, b, bv);
    bool ge = false; for (size_t k = 0; k < y.size(); ++k) if (k != cleanIdx && y[k] >= 0.5f) ge = true;
    if (pe == ge) mt.verdictOk++;
}
static std::wstring metricsStr(const Metrics& mt) {
    float f1 = (2 * mt.tp + mt.fp + mt.fn) > 0 ? 2.0f * mt.tp / (2.0f * mt.tp + mt.fp + mt.fn) : 0.f;
    wchar_t b[256];
    swprintf(b, 256, L"n=%d|exact=%.4f|f1_micro=%.4f|verdict_acc=%.4f|loss=%.4f",
             mt.n, mt.n ? (float)mt.exact / mt.n : 0.f, f1, mt.n ? (float)mt.verdictOk / mt.n : 0.f,
             mt.n ? (float)(mt.lossSum / mt.n) : 0.f);
    return b;
}

// ============================================================================
//  학습: 데이터셋 → (임베딩+베이스logits 1회 캐시) → 잔차 헤드 SGD → .ftc 저장. 반환 샘플 수(실패 0).
//    epochs(기본 60) hidden(64) maxlen(128) lrMilli(학습률×1000, 기본 20=0.02)
//    initFrom: 기존 .ftc 로 헤드를 초기화(이어서 학습). "" 이면 새로.
// ============================================================================
//    valPct: 홀드아웃 비율(기본 15, 0=끔). 문장 40개 이상일 때 홀드아웃으로 일반화 지표를 리포트.
//    earlyStop: 1 이면 홀드아웃 검증손실 최소 epoch 까지만 전체 데이터로 재학습. 헤드 학습은 수 초라 2회 해도 부담 없음.
//    Δ 벌점(kDPen): 확률 과신(0/1 포화)과 학습 문장 주변 드리프트를 억제.
// ============================================================================
static const float kMom = 0.9f, kL2 = 1e-4f, kDPen = 0.02f;

static int train(const std::wstring& baseIn, const std::wstring& backboneIn, const std::wstring& dataset,
                 const std::wstring& outIn, int epochs, int hidden, int maxlen, int lrMilli,
                 const std::wstring& initFrom, int valPct, int earlyStop) {
    g_lastErr.clear(); g_report.clear();
    std::wstring base = fullPath(baseIn), backbone = fullPath(backboneIn), outPath = fullPath(outIn);
    if (epochs <= 0) epochs = 60;
    if (hidden <= 0) hidden = 64;
    if (maxlen <= 0) maxlen = 128;
    if (valPct < 0) valPct = 15;
    if (valPct > 50) valPct = 50;
    float lr0 = (lrMilli > 0 ? lrMilli : 20) / 1000.0f;
    if (!dirExists(base)) { setErr(L"베이스 모델 폴더 없음: " + base); return 0; }
    if (!backbone.empty() && !fileExists(backbone)) { setErr(L"백본 없음: " + backbone + L" (models\\kor_embed\\kosimcse_embed.onnx, 또는 \"\" = 베이스 pooled 사용)"); return 0; }
    if (backbone.empty() && !textcls::clsHasPooled(base)) {
        std::wstring e = textcls::clsLastError();
        setErr(L"베이스 모델에 pooled 출력 없음 → python export_kor_unsmile.py 로 재export 하거나 backbone 에 kosimcse_embed.onnx 경로 지정" + (e.empty() ? L"" : L" (" + e + L")"));
        return 0;
    }
    DWORD t0 = GetTickCount();

    FTModel m;
    m.base = base; m.backbone = backbone; m.hid = hidden; m.maxlen = maxlen; m.pool = 0;
    if (!textcls::clsLabels(base, m.labels)) { setErr(L"베이스 로드 실패: " + textcls::clsLastError()); return 0; }
    m.nlab = (int)m.labels.size();
    if (m.nlab < 2) { setErr(L"labels.txt 라벨이 2개 미만: " + base); return 0; }
    m.baseModel = pickBaseName(base);
    m.baseSize = fileSize(joinDir(base, m.baseModel));
    m.baseFp = fingerprint(joinDir(base, m.baseModel));

    // 이어서 학습할 헤드(있으면). lscale 은 특징 계산 전에 맞춰야 한다.
    FTModel prev; bool havePrev = false;
    if (!initFrom.empty()) {
        if (ftcLoad(fullPath(initFrom), prev) && prev.nlab == m.nlab && prev.hid == hidden) { havePrev = true; m.lscale = prev.lscale; }
        else g_lastErr.clear();
    }

    std::vector<Sample> ds; int skU = 0, skF = 0;
    if (!readDataset(dataset, m.labels, ds, skU, skF)) return 0;

    // 특징 캐시(문장당 베이스 1회 + 백본 1회) — 전체 시간의 대부분
    std::vector<std::vector<float>> X(ds.size()), B(ds.size());
    for (size_t i = 0; i < ds.size(); ++i) {
        if (!features(m, ds[i].text, X[i], B[i])) { setErr(L"[" + std::to_wstring(i + 1) + L"] " + ds[i].text.substr(0, 20) + L" … " + g_lastErr); return 0; }
        if (m.dim == 0) m.dim = (int)X[i].size() - m.nlab;
    }
    if (havePrev && prev.dim != m.dim) havePrev = false;
    // 베이스 pooled 모드: 임베딩 부분을 z-score 표준화(비등방성 제거). 이어서 학습이면 이전 mu/sd 를 그대로 써야 헤드와 맞다.
    if (backbone.empty() && m.dim > 0) {
        if (havePrev && (int)prev.mu.size() == m.dim) { m.mu = prev.mu; m.sd = prev.sd; }
        else {
            std::vector<double> s1(m.dim, 0.0), s2(m.dim, 0.0);
            for (auto& x : X) for (int j = 0; j < m.dim; ++j) { s1[j] += x[j]; s2[j] += (double)x[j] * x[j]; }
            m.mu.resize(m.dim); m.sd.resize(m.dim);
            double n = (double)X.size();
            for (int j = 0; j < m.dim; ++j) {
                double mean = s1[j] / n, var = s2[j] / n - mean * mean;
                m.mu[j] = (float)mean; m.sd[j] = (float)std::sqrt(std::max(var, 0.0) + 1e-8);
            }
        }
        for (auto& x : X) for (int j = 0; j < m.dim; ++j) x[j] = zstd(x[j], m.mu[j], m.sd[j]);
    }
    DWORD t1 = GetTickCount();

    size_t ci = cleanIndex(m.labels);
    auto initNet = [&](Head& h) { if (havePrev) { h = prev.net; h.resetMomentum(); } else h.init(m.dim + m.nlab, hidden, m.nlab); };
    auto evalSet = [&](const Head* h, const std::vector<int>& ids, Metrics& mt) {
        for (int i : ids) {
            std::vector<float> hh, d, p(B[i].size());
            if (h) h->forward(X[i], hh, d);
            for (size_t k = 0; k < p.size(); ++k) p[k] = sigm(B[i][k] + (h ? d[k] : 0.0f));
            accumulate(mt, p, ds[i].y, ci, 0.5f);
        }
    };
    auto runEpoch = [&](Head& h, std::vector<int>& ids, int e, std::mt19937& rng) {
        std::shuffle(ids.begin(), ids.end(), rng);
        float lr = lr0 * (1.0f - 0.9f * e / epochs);
        for (int id : ids) h.trainStep(X[id], B[id], ds[id].y, lr, kMom, kL2, kDPen);
    };
    std::vector<int> all(ds.size()); for (size_t i = 0; i < all.size(); ++i) all[i] = (int)i;

    // 1) 홀드아웃 검증(일반화 확인용 리포트). 학습셋 지표는 외운 결과라 1.0 에 가깝게 나오므로 이 값을 봐야 한다.
    //    earlyStop=1 이면 검증손실이 최소인 epoch 까지만 최종 학습(데이터가 수천 줄 이상일 때 권장).
    //    기본(0)은 전체 epochs — 소량 데이터에선 홀드아웃이 작아 조기종료가 너무 일찍 멈춰 내 교정 문장을 반영 못 한다.
    int bestEpoch = epochs, finalEpochs = epochs; std::wstring valLine;
    int nVal = (valPct > 0 && ds.size() >= 40) ? (int)(ds.size() * valPct / 100) : 0;
    if (nVal > 0) {
        std::vector<int> perm = all; std::mt19937 sr(4242); std::shuffle(perm.begin(), perm.end(), sr);
        std::vector<int> vaIdx(perm.begin(), perm.begin() + nVal), trIdx(perm.begin() + nVal, perm.end());
        Head h; initNet(h); Head bestNet = h; std::mt19937 rng(777);
        double bestLoss = 1e30; bestEpoch = 0;
        for (int e = 0; e < epochs; ++e) {
            runEpoch(h, trIdx, e, rng);
            Metrics mv; evalSet(&h, vaIdx, mv);
            double vl = mv.lossSum / (mv.n ? mv.n : 1);
            if (vl < bestLoss - 1e-6) { bestLoss = vl; bestEpoch = e + 1; bestNet = h; }
        }
        if (bestEpoch < 1) bestEpoch = 1;
        Metrics vb, vbest, vlast; evalSet(nullptr, vaIdx, vb); evalSet(&bestNet, vaIdx, vbest); evalSet(&h, vaIdx, vlast);
        valLine = L"\nvalid(holdout " + std::to_wstring(nVal) + L" — 학습에 안 쓴 문장, best_epoch=" + std::to_wstring(bestEpoch) + L")\n"
                  L"  base:        " + metricsStr(vb) + L"\n"
                  L"  ft@best:     " + metricsStr(vbest) + L"\n"
                  L"  ft@" + std::to_wstring(epochs) + L":      " + metricsStr(vlast);
        if (earlyStop) finalEpochs = bestEpoch;
    }
    // 2) 전체 데이터로 최종 학습(같은 lr 스케줄로 finalEpochs 까지)
    Head net; initNet(net);
    { std::mt19937 rng(777); std::vector<int> ids = all; for (int e = 0; e < finalEpochs; ++e) runEpoch(net, ids, e, rng); }
    m.net = net; m.bestEpoch = finalEpochs;
    Metrics before, after; evalSet(nullptr, all, before); evalSet(&m.net, all, after);
    DWORD t2 = GetTickCount();

    m.samples = (int)ds.size(); m.path = outPath; m.loaded = true;
    if (!ftcSave(outPath, m)) { setErr(L".ftc 저장 실패: " + outPath); return 0; }
    g_ft = m;
    wchar_t b[320];
    swprintf(b, 320, L"samples=%d|skipped_unknown_label=%d|skipped_format=%d|epochs=%d|trained_epochs=%d|hidden=%d|dim=%d|lr=%.4f|resumed=%d|base_model=%ls|feature_ms=%u|train_ms=%u\n",
             (int)ds.size(), skU, skF, epochs, finalEpochs, hidden, m.dim, lr0, havePrev ? 1 : 0, m.baseModel.c_str(),
             (unsigned)(t1 - t0), (unsigned)(t2 - t1));
    g_report = b;
    g_report += L"before(base): " + metricsStr(before) + L"\n";
    g_report += L"after(train): " + metricsStr(after);
    g_report += valLine;
    return (int)ds.size();
}

// .ftc 에 저장된 절대경로가 없으면(.ftc 기준 상대경로로) 재탐색.
static bool resolvePath(const std::wstring& ftcPath, const std::wstring& absP, const std::wstring& rel, bool isDir, std::wstring& out) {
    auto ok = [&](const std::wstring& p) { return isDir ? dirExists(p) : fileExists(p); };
    if (!absP.empty() && ok(absP)) { out = absP; return true; }
    if (!rel.empty()) {
        std::wstring p = fullPath(joinDir(dirOf(ftcPath), rel));
        if (ok(p)) { out = p; return true; }
    }
    out = absP; return false;
}

// 로드(캐시). path "" 이면 이미 로드된 모델.
//   실패도 30초 캐시: 봇이 매 메시지마다 깨진 .ftc 를 다시 파싱·해시(수십 ms)하지 않게. force=1(Note_TextFTLoad)이면 즉시 재시도.
static std::wstring g_failPath, g_failErr; static DWORD g_failTick = 0;
static bool ensureLoadedImpl(const std::wstring& path);
static bool ensureLoaded(const std::wstring& pathIn, bool force = false) {
    if (pathIn.empty()) { if (g_ft.loaded) return true; setErr(L".ftc 경로가 없고 로드된 모델도 없음 (Note_TextFTLoad 먼저)"); return false; }
    std::wstring path = fullPath(pathIn);
    if (g_ft.loaded && g_ft.path == path) return true;
    if (!force && path == g_failPath && GetTickCount() - g_failTick < 30000) { setErr(g_failErr); return false; }
    if (ensureLoadedImpl(path)) { g_failPath.clear(); return true; }
    g_failPath = path; g_failErr = g_lastErr; g_failTick = GetTickCount();
    return false;
}
static bool ensureLoadedImpl(const std::wstring& path) {
    FTModel m; if (!ftcLoad(path, m)) return false;
    m.path = path;
    if (!resolvePath(path, m.base, m.baseRel, true, m.base)) { setErr(L".ftc 의 베이스 폴더 없음: " + m.base); return false; }
    if (!m.backbone.empty() && !resolvePath(path, m.backbone, m.backboneRel, false, m.backbone)) { setErr(L".ftc 의 백본 없음: " + m.backbone); return false; }
    // 베이스가 학습 이후 바뀌었으면(재export/재학습/int8 삭제 등) Δ 가 엉뚱하게 더해지므로 거부.
    if (!m.baseModel.empty()) {
        std::wstring cur = pickBaseName(m.base);
        std::wstring fp = cur.empty() ? L"" : fingerprint(joinDir(m.base, cur));
        long long sz = cur.empty() ? -1 : fileSize(joinDir(m.base, cur));
        bool changed = (cur != m.baseModel) || (!m.baseFp.empty() ? fp != m.baseFp : sz != m.baseSize);
        if (changed) {
            setErr(L"베이스 모델이 학습 이후 바뀜 (" + m.baseModel + L" " + m.baseFp + L" → " +
                   (cur.empty() ? L"없음" : cur) + L" " + fp + L"). 01_학습 을 다시 실행하세요.");
            return false;
        }
    }
    // 라벨 순서/이름이 베이스 labels.txt 와 같은지(순서가 바뀌면 Δ 가 엉뚱한 라벨에 더해짐)
    {
        std::vector<std::wstring> bl;
        if (!textcls::clsLabels(m.base, bl)) { setErr(L"베이스 로드 실패: " + textcls::clsLastError()); return false; }
        if (bl != m.labels) { setErr(L"베이스 labels.txt 가 .ftc 학습 당시와 다름(순서/이름). 01_학습 을 다시 실행하세요."); return false; }
    }
    g_ft = m; return true;
}

// 데이터셋 평가: 지표 + 틀린 줄(최대 maxWrong). 반환 문자열.
static bool evalDataset(const std::wstring& path, const std::wstring& dataset, int thrPct, std::wstring& out) {
    g_lastErr.clear();
    if (!ensureLoaded(path)) return false;
    std::vector<Sample> ds; int skU = 0, skF = 0;
    if (!readDataset(dataset, g_ft.labels, ds, skU, skF)) return false;
    float thr = std::min(99, std::max(1, thrPct)) / 100.0f;
    size_t ci = cleanIndex(g_ft.labels);
    Metrics mt, mb; std::wstring wrong; int nWrong = 0;
    for (auto& s : ds) {
        std::vector<float> x, b, h, d;
        if (!features(g_ft, s.text, x, b)) return false;
        g_ft.net.forward(x, h, d);
        std::vector<float> p(b.size()), pb(b.size());
        for (size_t k = 0; k < b.size(); ++k) { p[k] = sigm(b[k] + d[k]); pb[k] = sigm(b[k]); }
        accumulate(mt, p, s.y, ci, thr); accumulate(mb, pb, s.y, ci, thr);
        size_t best; float bv; bool pe = isEvil(p, ci, thr, best, bv);
        bool ge = false; for (size_t k = 0; k < s.y.size(); ++k) if (k != ci && s.y[k] >= 0.5f) ge = true;
        if (pe != ge) {
            nWrong++;
            if (nWrong <= 50) wrong += L"\n" + std::wstring(ge ? L"[악→선] " : L"[선→악] ") + s.rawLab + L"\t" + s.text + L"  (" + g_ft.labels[best] + L" " + fmt4(bv) + L", clean " + fmt4(p[ci]) + L")";
        }
    }
    out = L"finetuned: " + metricsStr(mt) + L"\nbase:      " + metricsStr(mb) + L"\nverdict_wrong=" + std::to_wstring(nWrong) + wrong;
    return true;
}

}} // namespace notepad::textft

// ============================================================================
//  exports (AHK v1 64비트 DllCall). 출력 규약 = textcls.cpp 와 동일(2단계 버퍼).
// ============================================================================
static int tfWriteOut(const std::wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}
static std::wstring tfArg(const wchar_t* p) { return p ? std::wstring(p) : std::wstring(); }

using namespace notepad::textft;

// 세트 존재 확인. ONNX 빌드면 1.
NOTEPAD_API int Notepad_TextFTAvailable() {
#ifdef NOTEPAD_ENABLE_ONNX
    return 1;
#else
    return 0;
#endif
}

// 파인튜닝. 반환: 학습 샘플 수(0=실패, 사유 LastError). 결과 요약은 Notepad_TextFTReport.
//   baseDir=kor_unsmile 폴더, backbone=kosimcse_embed.onnx, dataset=라벨목록<TAB>문장 TSV, outPath=산출 .ftc
NOTEPAD_API int Notepad_TextFTTrain(const wchar_t* baseDir, const wchar_t* backbone, const wchar_t* dataset,
                                    const wchar_t* outPath, int epochs, int hidden, int maxlen, int lrMilli,
                                    const wchar_t* initFrom, int valPct, int earlyStop) {
#ifndef NOTEPAD_ENABLE_ONNX
    setErr(L"ONNX 비활성 빌드(/DNOTEPAD_ENABLE_ONNX + third_party\\onnxruntime 필요)"); return 0;
#else
    return train(tfArg(baseDir), tfArg(backbone), tfArg(dataset), tfArg(outPath), epochs, hidden, maxlen, lrMilli, tfArg(initFrom), valPct, earlyStop);
#endif
}

NOTEPAD_API int Notepad_TextFTLoad(const wchar_t* ftPath) { g_lastErr.clear(); return ensureLoaded(tfArg(ftPath), true) ? 1 : 0; }
NOTEPAD_API int Notepad_TextFTUnload() { g_ft = FTModel(); return 1; }

// 라벨별 확률 "라벨,확률\n..." (TextCls 와 동일 규약). ftPath "" = 로드된 모델.
NOTEPAD_API int Notepad_TextFT(const wchar_t* ftPath, const wchar_t* text, wchar_t* out, int outCch) {
    g_lastErr.clear();
    if (!ensureLoaded(tfArg(ftPath))) return 0;
    std::vector<float> p; if (!predict(g_ft, tfArg(text), p)) return 0;
    std::wstring s;
    for (size_t i = 0; i < p.size(); ++i) { if (i) s += L'\n'; s += (i < g_ft.labels.size() ? g_ft.labels[i] : L"?") + L"," + fmt4(p[i]); }
    return tfWriteOut(s, out, outCch);
}

// 선/악 판정 "선|악,최고혐오라벨,그확률,clean확률" (TextClsVerdict 와 동일 규약)
NOTEPAD_API int Notepad_TextFTVerdict(const wchar_t* ftPath, const wchar_t* text, int thrPct, wchar_t* out, int outCch) {
    g_lastErr.clear();
    if (!ensureLoaded(tfArg(ftPath))) return 0;
    std::vector<float> p; if (!predict(g_ft, tfArg(text), p)) return 0;
    float thr = std::min(99, std::max(1, thrPct)) / 100.0f;
    size_t ci = cleanIndex(g_ft.labels), best; float bv;
    bool evil = isEvil(p, ci, thr, best, bv);
    std::wstring s = (evil ? L"악" : L"선");
    s += L"," + (best < g_ft.labels.size() ? g_ft.labels[best] : L"?") + L"," + fmt4(bv < 0 ? 0.f : bv) + L"," + fmt4(p[ci]);
    return tfWriteOut(s, out, outCch);
}

// 모델 정보 "base|backbone|dim|hidden|maxlen|samples|labels(콤마)"
NOTEPAD_API int Notepad_TextFTInfo(const wchar_t* ftPath, wchar_t* out, int outCch) {
    g_lastErr.clear();
    if (!ensureLoaded(tfArg(ftPath))) return 0;
    std::wstring s = g_ft.base + L"|" + g_ft.backbone + L"|" + std::to_wstring(g_ft.dim) + L"|" + std::to_wstring(g_ft.hid)
                   + L"|" + std::to_wstring(g_ft.maxlen) + L"|" + std::to_wstring(g_ft.samples) + L"|";
    for (size_t i = 0; i < g_ft.labels.size(); ++i) { if (i) s += L','; s += g_ft.labels[i]; }
    return tfWriteOut(s, out, outCch);
}

// 마지막 학습 리포트(학습 전/후 지표). 학습 직후에만 유효.
NOTEPAD_API int Notepad_TextFTReport(wchar_t* out, int outCch) { return tfWriteOut(g_report, out, outCch); }

// 데이터셋 평가: "finetuned: n=..|exact=..|f1_micro=..|verdict_acc=..\nbase: ...\nverdict_wrong=N\n[선→악] 라벨\t문장 (…)" 
NOTEPAD_API int Notepad_TextFTEval(const wchar_t* ftPath, const wchar_t* dataset, int thrPct, wchar_t* out, int outCch) {
    std::wstring s;
    if (!evalDataset(tfArg(ftPath), tfArg(dataset), thrPct, s)) return 0;
    return tfWriteOut(s, out, outCch);
}

NOTEPAD_API int Notepad_TextFTLastError(wchar_t* out, int outCch) { return tfWriteOut(g_lastErr, out, outCch); }

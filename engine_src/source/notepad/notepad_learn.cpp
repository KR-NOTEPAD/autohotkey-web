// ============================================================================
//  NOTEPAD.dll  -  notepad_learn.cpp
//  [NOTEPAD] 세트 AG-b: 특징 기반 분류기 (의존성 0, onnxruntime 불필요).
//  링크: gdiplus.lib (이미 포함). 순수 C++.
//
//  세트 AG(신경망, notepad_nn.cpp)와 상호보완:
//   - 신경망(Note_NN*): 정확도 높음. 단 mobilenetv2.onnx + onnxruntime.dll 필요.
//   - 특징기반(Note_*): onnxruntime 없이 어디서나 즉시 동작, 매우 빠름, 슬라이딩
//     위치탐지 지원. 가벼운 배포·빠른 반응이 필요하거나 ONNX 를 못 켜는 환경용.
//
//  다중 특징 융합: HSV 히스토그램 + 그레이 16x16 구조 + 에지방향 + 종횡비.
//  분류는 라벨별 최소거리 k-NN(코사인). 학습파일 .dat(텍스트).
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

#pragma comment(lib, "gdiplus.lib")

namespace notepad {

bool captureRegionToPng(int x, int y, int w, int h, const std::string& outPngUtf8);

static ULONG_PTR g_lnGpToken = 0; static bool g_lnGpUp = false;
static void lnEnsureGdiplus() {
    if (g_lnGpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_lnGpToken, &in, nullptr) == Gdiplus::Ok) g_lnGpUp = true;
}
static std::wstring lnU8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}
static std::string lnWToU8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}

struct LImg { int w = 0, h = 0; std::vector<unsigned char> px; };
static bool lnLoad(const std::wstring& wpath, LImg& img) {
    lnEnsureGdiplus();
    Gdiplus::Bitmap bmp(wpath.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;
    Gdiplus::Rect rc(0, 0, w, h); Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    img.w = w; img.h = h; img.px.resize((size_t)w * h * 4);
    for (int r = 0; r < h; ++r) memcpy(img.px.data() + (size_t)r * w * 4, (unsigned char*)bd.Scan0 + (size_t)r * bd.Stride, (size_t)w * 4);
    bmp.UnlockBits(&bd);
    return true;
}

static const int LFEAT_HSV = 256, LFEAT_GRAY = 256, LFEAT_EDGE = 8, LFEAT_ASPECT = 1;
static const int LFEAT_DIM = LFEAT_HSV + LFEAT_GRAY + LFEAT_EDGE + LFEAT_ASPECT;

static void lrgb2hsv(int R, int G, int B, int& H, int& S, int& V) {
    double r = R / 255.0, g = G / 255.0, b = B / 255.0;
    double mx = std::max({ r,g,b }), mn = std::min({ r,g,b }), d = mx - mn;
    double hh = 0;
    if (d > 1e-9) {
        if (mx == r) hh = 60 * fmod((g - b) / d, 6);
        else if (mx == g) hh = 60 * ((b - r) / d + 2);
        else hh = 60 * ((r - g) / d + 4);
    }
    if (hh < 0) hh += 360;
    H = (int)hh; S = (int)(mx > 0 ? d / mx * 255 : 0); V = (int)(mx * 255);
}

static bool lnExtract(const LImg& im, std::vector<float>& feat) {
    if (im.w <= 0 || im.h <= 0) return false;
    feat.assign(LFEAT_DIM, 0.0f);
    for (int i = 0; i < im.w * im.h; ++i) {
        const unsigned char* p = im.px.data() + (size_t)i * 4;
        int H, S, V; lrgb2hsv(p[2], p[1], p[0], H, S, V);
        int hb = std::min(15, H * 16 / 360), sb = std::min(3, S * 4 / 256), vb = std::min(3, V * 4 / 256);
        feat[hb * 16 + sb * 4 + vb] += 1.0f;
    }
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            int sx0 = x * im.w / 16, sx1 = (x + 1) * im.w / 16, sy0 = y * im.h / 16, sy1 = (y + 1) * im.h / 16;
            if (sx1 <= sx0) sx1 = sx0 + 1; if (sy1 <= sy0) sy1 = sy0 + 1;
            long sum = 0, cnt = 0;
            for (int yy = sy0; yy < sy1 && yy < im.h; ++yy)
                for (int xx = sx0; xx < sx1 && xx < im.w; ++xx) {
                    const unsigned char* p = im.px.data() + ((size_t)yy * im.w + xx) * 4;
                    sum += (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000; cnt++;
                }
            feat[LFEAT_HSV + y * 16 + x] = cnt ? (float)(sum / cnt) : 0.0f;
        }
    {
        int gw = 32, gh = 32; std::vector<float> g(gw * gh);
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                int sx = std::min(im.w - 1, x * im.w / gw), sy = std::min(im.h - 1, y * im.h / gh);
                const unsigned char* p = im.px.data() + ((size_t)sy * im.w + sx) * 4;
                g[y * gw + x] = (float)((p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000);
            }
        for (int y = 1; y < gh - 1; ++y)
            for (int x = 1; x < gw - 1; ++x) {
                float gxv = g[y * gw + x + 1] - g[y * gw + x - 1];
                float gyv = g[(y + 1) * gw + x] - g[(y - 1) * gw + x];
                float mag = std::sqrt(gxv * gxv + gyv * gyv);
                if (mag < 20) continue;
                float ang = (float)(atan2(gyv, gxv) + 3.14159265);
                int bin = std::min(7, (int)(ang / (2 * 3.14159265) * 8));
                feat[LFEAT_HSV + LFEAT_GRAY + bin] += mag;
            }
    }
    feat[LFEAT_HSV + LFEAT_GRAY + LFEAT_EDGE] = (float)im.w / (float)im.h;
    // ★ 오탐 수정(핵심): 각 특징 블록을 "평균 중심화(mean-center) 후 L2 정규화" 한다.
    //   기존엔 L2 정규화만 해서 특징이 전부 음수 없는 값(히스토그램/밝기)이었고,
    //   그러면 코사인 유사도가 무관한 이미지끼리도 0.8~0.9 로 높게 나온다
    //   (모든 성분이 양수라 방향이 비슷). → 빈 배경도 어떤 클래스와 90% 매치되어 오탐.
    //   평균을 빼면 성분에 음수가 생겨, 무관한 이미지는 코사인이 0 근처로 떨어지고
    //   진짜 매치만 높게 남는다. (self-match 는 그대로 ~100%)
    auto centerL2 = [&](int st, int len) {
        double m = 0; for (int i = 0; i < len; ++i) m += feat[st + i]; m /= (len > 0 ? len : 1);
        for (int i = 0; i < len; ++i) feat[st + i] = (float)(feat[st + i] - m);
        double s = 0; for (int i = 0; i < len; ++i) s += (double)feat[st + i] * feat[st + i];
        s = std::sqrt(s); if (s < 1e-9) return;
        for (int i = 0; i < len; ++i) feat[st + i] = (float)(feat[st + i] / s);
    };
    centerL2(0, LFEAT_HSV); centerL2(LFEAT_HSV, LFEAT_GRAY); centerL2(LFEAT_HSV + LFEAT_GRAY, LFEAT_EDGE);
    feat[LFEAT_HSV + LFEAT_GRAY + LFEAT_EDGE] = (float)tanh(feat[LFEAT_HSV + LFEAT_GRAY + LFEAT_EDGE] - 1.0);
    return true;
}
static bool lnExtractFile(const std::wstring& path, std::vector<float>& feat) {
    LImg im; if (!lnLoad(path, im)) return false;
    return lnExtract(im, feat);
}

struct LSample { std::wstring label; std::vector<float> feat; };

static bool lnSave(const std::wstring& path, const std::vector<LSample>& samples) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string out = "NPLEARN1\ndim=" + std::to_string(LFEAT_DIM) + "\n"; char buf[32];
    for (auto& s : samples) {
        out += lnWToU8(s.label); out += '\t';
        for (size_t i = 0; i < s.feat.size(); ++i) { if (i) out += ' '; snprintf(buf, sizeof(buf), "%.5g", s.feat[i]); out += buf; }
        out += '\n';
    }
    DWORD w; WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr); CloseHandle(h);
    return true;
}
static bool lnLoadModel(const std::wstring& path, std::vector<LSample>& samples) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string data; std::vector<char> buf(65536); DWORD rd;
    while (ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd) data.append(buf.data(), rd);
    CloseHandle(h);
    size_t pos = 0; std::vector<std::string> lines;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) { lines.push_back(data.substr(pos)); break; }
        std::string ln = data.substr(pos, nl - pos);
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        lines.push_back(ln); pos = nl + 1;
    }
    if (lines.empty() || lines[0].rfind("NPLEARN1", 0) != 0) return false;
    for (size_t li = 2; li < lines.size(); ++li) {
        std::string& ln = lines[li]; if (ln.empty()) continue;
        size_t tab = ln.find('\t'); if (tab == std::string::npos) continue;
        LSample s; s.label = lnU8ToW(ln.substr(0, tab));
        std::string rest = ln.substr(tab + 1), cur;
        for (char c : rest) { if (c == ' ') { if (!cur.empty()) { s.feat.push_back((float)atof(cur.c_str())); cur.clear(); } } else cur.push_back(c); }
        if (!cur.empty()) s.feat.push_back((float)atof(cur.c_str()));
        if (!s.feat.empty()) samples.push_back(std::move(s));
    }
    return !samples.empty();
}

static bool isImg(const std::wstring& n) {
    size_t d = n.find_last_of(L'.'); if (d == std::wstring::npos) return false;
    std::wstring e = n.substr(d + 1); for (auto& c : e) c = (wchar_t)towlower(c);
    return e == L"png" || e == L"jpg" || e == L"jpeg" || e == L"bmp" || e == L"gif";
}

// 증강: 이미지 1장 -> 크기/밝기 변형 사본. (notepad_nn.cpp 와 동일 개념)
//   0=원본, 1=축소0.75+패딩, 2=확대1.3+크롭, 3=밝게, 4=어둡게.
static void lnMakeVariant(const LImg& src, int v, LImg& dst) {
    dst.w = src.w; dst.h = src.h; dst.px.resize((size_t)src.w * src.h * 4);
    if (v == 0) { dst.px = src.px; return; }
    auto sample = [&](int sx, int sy, unsigned char* out4) {
        sx = std::max(0, std::min(src.w - 1, sx)); sy = std::max(0, std::min(src.h - 1, sy));
        memcpy(out4, src.px.data() + ((size_t)sy * src.w + sx) * 4, 4);
    };
    if (v == 1) {
        long b = 0, g = 0, r = 0, a = 0; int N = src.w * src.h;
        for (int i = 0; i < N; ++i) { const unsigned char* p = src.px.data() + (size_t)i * 4; b += p[0]; g += p[1]; r += p[2]; a += p[3]; }
        unsigned char avg[4] = { (unsigned char)(b / N),(unsigned char)(g / N),(unsigned char)(r / N),(unsigned char)(a / N) };
        for (int i = 0; i < N; ++i) memcpy(dst.px.data() + (size_t)i * 4, avg, 4);
        int sw = src.w * 3 / 4, sh = src.h * 3 / 4, ox = (src.w - sw) / 2, oy = (src.h - sh) / 2;
        for (int y = 0; y < sh; ++y) for (int x = 0; x < sw; ++x) {
            unsigned char px[4]; sample(x * src.w / sw, y * src.h / sh, px);
            memcpy(dst.px.data() + ((size_t)(oy + y) * src.w + (ox + x)) * 4, px, 4);
        }
        return;
    }
    if (v == 2) {
        int cw = src.w * 10 / 13, ch = src.h * 10 / 13, cx0 = (src.w - cw) / 2, cy0 = (src.h - ch) / 2;
        for (int y = 0; y < src.h; ++y) for (int x = 0; x < src.w; ++x) {
            unsigned char px[4]; sample(cx0 + x * cw / src.w, cy0 + y * ch / src.h, px);
            memcpy(dst.px.data() + ((size_t)y * src.w + x) * 4, px, 4);
        }
        return;
    }
    float f = (v == 3) ? 1.2f : 0.8f;
    for (int i = 0; i < src.w * src.h; ++i) {
        const unsigned char* p = src.px.data() + (size_t)i * 4;
        unsigned char* o = dst.px.data() + (size_t)i * 4;
        for (int c = 0; c < 3; ++c) { int val = (int)(p[c] * f); o[c] = (unsigned char)(val > 255 ? 255 : val); }
        o[3] = p[3];
    }
}

int trainFolder(const std::wstring& dir, const std::wstring& modelPath, int augLevel) {
    if (augLevel < 0) augLevel = 0; if (augLevel > 4) augLevel = 4;
    std::vector<LSample> samples;
    auto scan = [&](const std::wstring& folder, const std::wstring& label) {
        std::wstring pat = folder + L"\\*"; WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileW(pat.c_str(), &fd); if (hf == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = fd.cFileName; if (!isImg(name)) continue;
            LImg im; if (!lnLoad(folder + L"\\" + name, im)) continue;
            for (int v = 0; v <= augLevel; ++v) {
                LImg var; lnMakeVariant(im, v, var);
                std::vector<float> feat;
                if (lnExtract(var, feat)) { LSample s; s.label = label; s.feat = std::move(feat); samples.push_back(std::move(s)); }
            }
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    };
    scan(dir, L"(root)");
    std::wstring pat = dir + L"\\*"; WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring name = fd.cFileName; if (name == L"." || name == L"..") continue;
            scan(dir + L"\\" + name, name);
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    if (samples.empty()) return -1;
    if (!lnSave(modelPath, samples)) return -1;
    return (int)samples.size();
}

static double lnDist(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 1e9;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) { dot += (double)a[i] * b[i]; na += (double)a[i] * a[i]; nb += (double)b[i] * b[i]; }
    double denom = std::sqrt(na) * std::sqrt(nb); if (denom < 1e-9) return 1.0;
    return 1.0 - dot / denom;
}
static std::wstring lnClassifyFeat(const std::vector<LSample>& samples, const std::vector<float>& feat, int topK) {
    if (samples.empty() || feat.empty()) return L"";
    std::vector<std::pair<std::wstring, double>> best;
    for (auto& s : samples) {
        double d = lnDist(s.feat, feat); bool f = false;
        for (auto& b : best) if (b.first == s.label) { if (d < b.second) b.second = d; f = true; break; }
        if (!f) best.push_back({ s.label, d });
    }
    std::sort(best.begin(), best.end(), [](auto& a, auto& b) { return a.second < b.second; });
    if (topK <= 0) topK = 3;
    std::wstring out;
    for (int i = 0; i < (int)best.size() && i < topK; ++i) {
        double sim = (1.0 - best[i].second) * 100.0; if (sim < 0) sim = 0; if (sim > 100) sim = 100;
        wchar_t buf[32]; swprintf(buf, 32, L",%.1f", sim);
        if (!out.empty()) out += L";"; out += best[i].first; out += buf;
    }
    return out;
}

std::wstring classifyImage(const std::wstring& modelPath, const std::wstring& imgPath, int topK) {
    std::vector<LSample> samples; if (!lnLoadModel(modelPath, samples)) return L"";
    std::vector<float> feat; if (!lnExtractFile(imgPath, feat)) return L"";
    return lnClassifyFeat(samples, feat, topK);
}
std::wstring classifyScreen(const std::wstring& modelPath, int x, int y, int w, int h, int topK) {
    std::vector<LSample> samples; if (!lnLoadModel(modelPath, samples)) return L"";
    wchar_t dir[MAX_PATH], tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return L"";
    if (!GetTempFileNameW(dir, L"npl", 0, tmp)) return L"";
    std::wstring wpng = std::wstring(tmp) + L".png", res;
    if (captureRegionToPng(x, y, w, h, lnWToU8(wpng))) {
        std::vector<float> feat; if (lnExtractFile(wpng, feat)) res = lnClassifyFeat(samples, feat, topK);
    }
    DeleteFileW(tmp); DeleteFileW(wpng.c_str());
    return res;
}
std::wstring detectLearnedScreen(const std::wstring& modelPath, int rx, int ry, int rw, int rh,
                                 int win, int stepPct, double minScore, int topK) {
    std::vector<LSample> samples; if (!lnLoadModel(modelPath, samples)) return L"";
    if (win <= 0) win = 96; if (stepPct <= 0) stepPct = 50;
    int step = std::max(8, win * stepPct / 100);
    wchar_t dir[MAX_PATH], tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return L"";
    if (!GetTempFileNameW(dir, L"npL", 0, tmp)) return L"";
    std::wstring wpng = std::wstring(tmp) + L".png"; LImg scene;
    bool ok = captureRegionToPng(rx, ry, rw, rh, lnWToU8(wpng)) && lnLoad(wpng, scene);
    DeleteFileW(tmp); DeleteFileW(wpng.c_str());
    if (!ok) return L"";
    struct Hit { std::wstring label; int x, y, w, h; double score; };
    std::vector<Hit> hits;
    for (int cy = 0; cy + win <= scene.h; cy += step)
        for (int cx = 0; cx + win <= scene.w; cx += step) {
            LImg cell; cell.w = win; cell.h = win; cell.px.resize((size_t)win * win * 4);
            for (int yy = 0; yy < win; ++yy)
                memcpy(cell.px.data() + (size_t)yy * win * 4, scene.px.data() + ((size_t)(cy + yy) * scene.w + cx) * 4, (size_t)win * 4);
            std::vector<float> feat; if (!lnExtract(cell, feat)) continue;
            // top2 를 받아 "1등 유사도" 뿐 아니라 "1등-2등 마진"도 본다.
            // 배경/애매한 셀은 여러 클래스와 고루 비슷해(1등≈2등) 오탐이 된다.
            // 진짜 매치는 1등이 2등보다 뚜렷이 높다. 이 마진으로 오탐을 걸러낸다.
            std::wstring r = lnClassifyFeat(samples, feat, 2); if (r.empty()) continue;
            // r 예: "라벨1,88.5;라벨2,60.1"
            std::wstring lab1; double sc1 = 0, sc2 = 0;
            {
                size_t semi = r.find(L';');
                std::wstring first = (semi == std::wstring::npos) ? r : r.substr(0, semi);
                size_t c1 = first.find_last_of(L','); if (c1 == std::wstring::npos) continue;
                lab1 = first.substr(0, c1); sc1 = _wtof(first.substr(c1 + 1).c_str());
                if (semi != std::wstring::npos) {
                    std::wstring second = r.substr(semi + 1);
                    size_t c2 = second.find_last_of(L',');
                    if (c2 != std::wstring::npos) sc2 = _wtof(second.substr(c2 + 1).c_str());
                }
            }
            // 채택 조건: (1) 1등 유사도 >= minScore  AND  (2) 1등-2등 마진 >= margin.
            //   margin 은 minScore 에 비례(기본 minScore*0.25, 최소 8%p). 클래스 1개면 마진 무시.
            double margin = std::max(8.0, minScore * 0.25);
            bool single = true; { std::wstring f0 = samples.empty()?L"":samples[0].label; for (auto& s : samples) if (s.label != f0) { single = false; break; } }
            if (sc1 >= minScore && (single || (sc1 - sc2) >= margin)) {
                Hit hh; hh.label = lab1; hh.x = rx + cx; hh.y = ry + cy; hh.w = win; hh.h = win; hh.score = sc1; hits.push_back(hh);
            }
        }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    std::vector<bool> removed(hits.size(), false);
    for (size_t i = 0; i < hits.size(); ++i) {
        if (removed[i]) continue;
        for (size_t j = i + 1; j < hits.size(); ++j) {
            if (removed[j]) continue;
            if (std::abs(hits[i].x - hits[j].x) < win && std::abs(hits[i].y - hits[j].y) < win) removed[j] = true;
        }
    }
    std::wstring out;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (removed[i]) continue;
        wchar_t buf[128]; swprintf(buf, 128, L",%d,%d,%d,%d,%.1f", hits[i].x, hits[i].y, hits[i].w, hits[i].h, hits[i].score);
        if (!out.empty()) out += L"\n"; out += hits[i].label; out += buf;
    }
    return out;
}
std::wstring modelInfo(const std::wstring& modelPath) {
    std::vector<LSample> samples; if (!lnLoadModel(modelPath, samples)) return L"";
    std::vector<std::pair<std::wstring, int>> cnt;
    for (auto& s : samples) { bool f = false; for (auto& c : cnt) if (c.first == s.label) { c.second++; f = true; break; } if (!f) cnt.push_back({ s.label, 1 }); }
    std::wstring out;
    for (auto& c : cnt) { if (!out.empty()) out += L";"; out += c.first; out += L":"; out += std::to_wstring(c.second); }
    return out;
}

} // namespace notepad

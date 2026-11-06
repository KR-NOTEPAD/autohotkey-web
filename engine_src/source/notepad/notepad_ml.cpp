// ============================================================================
//  NOTEPAD.dll  -  notepad_ml.cpp
//  [NOTEPAD] 세트 AC: 고전 ML / 강건 매칭 (모델파일 불필요, 순수 C++, 의존성 0).
//  링크: gdiplus.lib user32.lib gdi32.lib (build.bat 에 이미 포함).
//
//  기존 세트 G(ImageSearch)는 "픽셀 완전일치(tolerance 내)"라 배율·밝기·안티앨리어싱
//  에 약하다. 세트 AC 는 그 한계를 보완한다:
//   * NCC(정규화상호상관) 템플릿매칭 - 밝기 변화에 강건, 유사도 점수(0~100) 반환.
//   * 멀티스케일 - 니들을 여러 배율로 리사이즈하며 탐색 -> 크기 달라도 매칭.
//   * 색 기반 물체탐지 - 특정 색 영역의 바운딩박스(연결요소 근사).
//   * k-NN 분류 - 작은 특징벡터를 라벨과 비교(사용자 정의 특징). 저사양 CPU 친화.
//
//  좌표계는 세트 G 와 동일(화면 픽셀). 결과는 문자열/JSON 로 반환.
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

#pragma comment(lib, "gdiplus.lib")

namespace notepad {

// 세트 G 에 이미 있는 헬퍼 재사용(같은 DLL 내). 선언만.
bool captureRegionToPng(int x, int y, int w, int h, const std::string& outPngUtf8);

// ---- 로컬 GDI+ / 이미지 로드 (세트 AC 전용 토큰) ---------------------------
static ULONG_PTR g_mlGpToken = 0; static bool g_mlGpUp = false;
static void mlEnsureGdiplus() {
    if (g_mlGpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_mlGpToken, &in, nullptr) == Gdiplus::Ok) g_mlGpUp = true;
}
static std::wstring mlU8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

struct GImg { int w = 0, h = 0; std::vector<unsigned char> px; }; // BGRA

static bool mlLoad(const std::wstring& wpath, GImg& img) {
    mlEnsureGdiplus();
    Gdiplus::Bitmap bmp(wpath.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;
    Gdiplus::Rect rc(0, 0, w, h); Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    img.w = w; img.h = h; img.px.resize((size_t)w * h * 4);
    for (int r = 0; r < h; ++r)
        memcpy(img.px.data() + (size_t)r * w * 4, (unsigned char*)bd.Scan0 + (size_t)r * bd.Stride, (size_t)w * 4);
    bmp.UnlockBits(&bd);
    return true;
}

// 화면 영역 캡처 -> GImg (세트 G 의 captureRegionToPng 를 임시파일 경유로 재사용)
static bool mlCaptureRegion(int x, int y, int w, int h, GImg& img) {
    if (w <= 0 || h <= 0) return false;
    wchar_t dir[MAX_PATH], tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return false;
    if (!GetTempFileNameW(dir, L"npm", 0, tmp)) return false;
    // .png 확장자 보장
    std::wstring wtmp = tmp; wtmp += L".png";
    int n = WideCharToMultiByte(CP_UTF8, 0, wtmp.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u8((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wtmp.c_str(), -1, &u8[0], n, nullptr, nullptr);
    bool ok = captureRegionToPng(x, y, w, h, u8) && mlLoad(wtmp, img);
    DeleteFileW(tmp); DeleteFileW(wtmp.c_str());
    return ok;
}

// 그레이 벡터로 변환(0~255 double)
static void toGray(const GImg& im, std::vector<double>& g) {
    g.resize((size_t)im.w * im.h);
    for (int i = 0; i < im.w * im.h; ++i) {
        const unsigned char* p = im.px.data() + (size_t)i * 4;
        g[i] = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000.0;
    }
}

// 리사이즈된 그레이 니들 생성(nearest, 빠름)
static void resizeGray(const std::vector<double>& src, int sw, int sh,
                       int dw, int dh, std::vector<double>& dst) {
    dst.resize((size_t)dw * dh);
    for (int y = 0; y < dh; ++y) {
        int sy = y * sh / dh; if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; ++x) {
            int sx = x * sw / dw; if (sx >= sw) sx = sw - 1;
            dst[(size_t)y * dw + x] = src[(size_t)sy * sw + sx];
        }
    }
}

// NCC(정규화상호상관) 한 위치 계산: haystack(hg, hw*hh) 의 (ox,oy)에 니들(ng, nw*nh)
static double nccAt(const std::vector<double>& hg, int hw,
                    const std::vector<double>& ng, int nw, int nh,
                    double nMean, double nNorm, int ox, int oy) {
    // haystack 창 평균
    double sum = 0; int cnt = nw * nh;
    for (int y = 0; y < nh; ++y) {
        const double* hr = &hg[(size_t)(oy + y) * hw + ox];
        for (int x = 0; x < nw; ++x) sum += hr[x];
    }
    double hMean = sum / cnt;
    double num = 0, hVar = 0;
    for (int y = 0; y < nh; ++y) {
        const double* hr = &hg[(size_t)(oy + y) * hw + ox];
        const double* nr = &ng[(size_t)y * nw];
        for (int x = 0; x < nw; ++x) {
            double hd = hr[x] - hMean, nd = nr[x] - nMean;
            num += hd * nd; hVar += hd * hd;
        }
    }
    double denom = std::sqrt(hVar) * nNorm;
    if (denom < 1e-9) return 0;
    return num / denom; // -1~1
}

// 멀티스케일 NCC 템플릿매칭.
//   반환: 최고 매치 "x,y,score,scalePct" (score 0~100). 못찾으면 "".
//   scaleMinPct/scaleMaxPct/scaleStepPct: 니들 배율 범위(100=원본).
//   step: haystack 스캔 픽셀 간격(1=정밀, 2~3=빠름).
bool matchTemplateMultiScale(const std::wstring& needlePath,
                             int x1, int y1, int x2, int y2,
                             int scaleMinPct, int scaleMaxPct, int scaleStepPct,
                             int step, int& fx, int& fy, double& fscore, int& fscalePct) {
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    int rw = x2 - x1, rh = y2 - y1;
    if (rw <= 0 || rh <= 0) return false;
    if (step < 1) step = 1;
    if (scaleStepPct < 1) scaleStepPct = 10;
    if (scaleMinPct < 1) scaleMinPct = 100;
    if (scaleMaxPct < scaleMinPct) scaleMaxPct = scaleMinPct;

    GImg needle, hay;
    if (!mlLoad(needlePath, needle)) return false;
    if (!mlCaptureRegion(x1, y1, rw, rh, hay)) return false;

    std::vector<double> ng0, hg;
    toGray(needle, ng0); toGray(hay, hg);

    double best = -2; int bx = 0, by = 0, bscale = 100;
    for (int sp = scaleMinPct; sp <= scaleMaxPct; sp += scaleStepPct) {
        int nw = std::max(1, needle.w * sp / 100);
        int nh = std::max(1, needle.h * sp / 100);
        if (nw > rw || nh > rh) continue;
        std::vector<double> ng;
        if (sp == 100) ng = ng0; else resizeGray(ng0, needle.w, needle.h, nw, nh, ng);
        // 니들 평균/노름 사전계산
        double nMean = 0; for (double v : ng) nMean += v; nMean /= (nw * nh);
        double nVar = 0; for (double v : ng) { double d = v - nMean; nVar += d * d; }
        double nNorm = std::sqrt(nVar); if (nNorm < 1e-9) continue;
        for (int oy = 0; oy <= rh - nh; oy += step) {
            for (int ox = 0; ox <= rw - nw; ox += step) {
                double s = nccAt(hg, rw, ng, nw, nh, nMean, nNorm, ox, oy);
                if (s > best) { best = s; bx = ox; by = oy; bscale = sp; }
            }
        }
    }
    if (best < -1) return false;
    fx = x1 + bx; fy = y1 + by;
    fscore = (best + 1.0) * 50.0; // -1~1 -> 0~100
    fscalePct = bscale;
    return true;
}

// 색 기반 물체탐지: 목표색(rgb) ± tol 픽셀들의 바운딩박스.
//   minPixels 이상 모여야 유효. 반환 "x,y,w,h,count" / "".
bool detectColorBBox(int x1, int y1, int x2, int y2, uint32_t rgb, int tol,
                     int minPixels, int& bx, int& by, int& bw, int& bh, int& count) {
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    int rw = x2 - x1, rh = y2 - y1;
    if (rw <= 0 || rh <= 0) return false;
    if (tol < 0) tol = 0;
    GImg hay;
    if (!mlCaptureRegion(x1, y1, rw, rh, hay)) return false;
    int R = (rgb >> 16) & 0xFF, G = (rgb >> 8) & 0xFF, B = rgb & 0xFF;
    int minx = rw, miny = rh, maxx = -1, maxy = -1, cnt = 0;
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            const unsigned char* p = hay.px.data() + ((size_t)y * rw + x) * 4;
            if (std::abs((int)p[2] - R) <= tol && std::abs((int)p[1] - G) <= tol &&
                std::abs((int)p[0] - B) <= tol) {
                if (x < minx) minx = x; if (y < miny) miny = y;
                if (x > maxx) maxx = x; if (y > maxy) maxy = y; cnt++;
            }
        }
    }
    if (cnt < (minPixels > 0 ? minPixels : 1) || maxx < 0) return false;
    bx = x1 + minx; by = y1 + miny; bw = maxx - minx + 1; bh = maxy - miny + 1; count = cnt;
    return true;
}

// k-NN 분류(1-NN, 유클리드). 학습셋은 문자열: 각 줄 "label,f1,f2,...".
//   query 는 콤마구분 특징. 반환: 가장 가까운 label. 실패 "".
//   저사양 CPU 친화(모델 학습 불필요, 즉석 비교). 사용자가 특징을 정의.
bool knnClassify(const std::wstring& trainData, const std::wstring& query,
                 std::wstring& labelOut, double& distOut) {
    // query 파싱
    std::vector<double> q;
    { std::wstring cur; for (wchar_t c : query) { if (c == L',') { q.push_back(_wtof(cur.c_str())); cur.clear(); } else cur.push_back(c); } if (!cur.empty()) q.push_back(_wtof(cur.c_str())); }
    if (q.empty()) return false;

    double best = 1e300; std::wstring bestLabel;
    std::wstring line;
    auto processLine = [&](const std::wstring& ln) {
        if (ln.empty()) return;
        size_t comma = ln.find(L',');
        if (comma == std::wstring::npos) return;
        std::wstring label = ln.substr(0, comma);
        std::vector<double> f;
        std::wstring cur; for (size_t i = comma + 1; i < ln.size(); ++i) { wchar_t c = ln[i]; if (c == L',') { f.push_back(_wtof(cur.c_str())); cur.clear(); } else cur.push_back(c); }
        if (!cur.empty()) f.push_back(_wtof(cur.c_str()));
        if (f.size() != q.size()) return;
        double d = 0; for (size_t i = 0; i < q.size(); ++i) { double dd = q[i] - f[i]; d += dd * dd; }
        if (d < best) { best = d; bestLabel = label; }
    };
    for (wchar_t c : trainData) {
        if (c == L'\n') { if (!line.empty() && line.back() == L'\r') line.pop_back(); processLine(line); line.clear(); }
        else line.push_back(c);
    }
    processLine(line);
    if (bestLabel.empty()) return false;
    labelOut = bestLabel; distOut = std::sqrt(best);
    return true;
}

} // namespace notepad

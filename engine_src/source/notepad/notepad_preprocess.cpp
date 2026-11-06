// ============================================================================
//  NOTEPAD.dll  -  notepad_preprocess.cpp
//  [NOTEPAD] 세트 AA: 이미지 전처리 (파이썬 OpenCV/Pillow 전처리 대응).
//  링크: gdiplus.lib (build.bat 에 이미 포함). 의존성 0.
//
//  목적: 이미지서치(세트 G/K)·템플릿매칭(세트 AC)·OCR(세트 H) 정확도를 높이는
//        전처리. 단일 동작(그레이/이진화/리사이즈 등) + 문자열 레시피로 여러
//        단계를 연쇄하는 파이프라인 엔진. 입출력은 PNG/JPEG 파일 경로.
//
//  [NOTEPAD] 규약: 성공 1 / 실패 0 (파일 처리형). 파이프라인은 exports 에서
//  2단계 버퍼로 처리결과 경로/로그를 돌려준다.
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cwctype>
#include <sstream>

#pragma comment(lib, "gdiplus.lib")

namespace notepad {

// ---- GDI+ 공용 (다른 파일과 토큰 분리해 독립 초기화) ------------------------
static ULONG_PTR g_ppGpToken = 0;
static bool g_ppGpUp = false;
static void ppEnsureGdiplus() {
    if (g_ppGpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_ppGpToken, &in, nullptr) == Gdiplus::Ok)
        g_ppGpUp = true;
}

static int ppGetEncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<unsigned char> buf(size);
    auto* p = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, p);
    for (UINT j = 0; j < num; ++j)
        if (wcscmp(p[j].MimeType, mime) == 0) { *clsid = p[j].Clsid; return (int)j; }
    return -1;
}

static bool ppIsJpeg(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext == L"jpg" || ext == L"jpeg";
}

// ---- 내부 이미지 표현: 32bpp BGRA(top-down), w*h*4 -------------------------
struct Image {
    int w = 0, h = 0;
    std::vector<unsigned char> px;   // BGRA
    bool valid() const { return w > 0 && h > 0 && px.size() == (size_t)w * h * 4; }
    unsigned char* at(int x, int y) { return px.data() + ((size_t)y * w + x) * 4; }
    const unsigned char* at(int x, int y) const { return px.data() + ((size_t)y * w + x) * 4; }
};

// 파일 -> Image (BGRA)
static bool loadImage(const std::wstring& wpath, Image& img) {
    ppEnsureGdiplus();
    Gdiplus::Bitmap bmp(wpath.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;
    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead,
                     PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    img.w = w; img.h = h; img.px.resize((size_t)w * h * 4);
    for (int r = 0; r < h; ++r)
        memcpy(img.px.data() + (size_t)r * w * 4,
               (unsigned char*)bd.Scan0 + (size_t)r * bd.Stride, (size_t)w * 4);
    bmp.UnlockBits(&bd);
    return true;
}

// Image -> 파일 (확장자로 JPEG/PNG 판정, JPEG quality 적용)
static bool saveImage(const Image& img, const std::wstring& outPath, int quality) {
    if (!img.valid()) return false;
    if (quality < 0) quality = 90; if (quality > 100) quality = 100;
    ppEnsureGdiplus();
    Gdiplus::Bitmap bmp(img.w, img.h, PixelFormat32bppARGB);
    Gdiplus::Rect rc(0, 0, img.w, img.h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite,
                     PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    for (int r = 0; r < img.h; ++r)
        memcpy((unsigned char*)bd.Scan0 + (size_t)r * bd.Stride,
               img.px.data() + (size_t)r * img.w * 4, (size_t)img.w * 4);
    bmp.UnlockBits(&bd);
    bool jpeg = ppIsJpeg(outPath);
    CLSID clsid;
    if (ppGetEncoderClsid(jpeg ? L"image/jpeg" : L"image/png", &clsid) < 0) return false;
    if (jpeg) {
        Gdiplus::EncoderParameters ep; ep.Count = 1;
        ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
        ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        ULONG q = (ULONG)quality; ep.Parameter[0].Value = &q;
        return bmp.Save(outPath.c_str(), &clsid, &ep) == Gdiplus::Ok;
    }
    return bmp.Save(outPath.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

static inline unsigned char clampu8(int v) { return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// ---- 개별 전처리 연산 -------------------------------------------------------

// 그레이스케일(가중치 Rec.601). 채널 3개를 같은 밝기로. 알파 유지.
static void opGray(Image& im) {
    for (int i = 0; i < im.w * im.h; ++i) {
        unsigned char* p = im.px.data() + (size_t)i * 4;
        int g = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000; // BGRA -> R=p[2]
        p[0] = p[1] = p[2] = (unsigned char)g;
    }
}

// 밝기(add) / 대비(mul, 100=1.0배)
static void opBrightnessContrast(Image& im, int brightness, int contrastPct) {
    double c = contrastPct / 100.0;
    for (int i = 0; i < im.w * im.h; ++i) {
        unsigned char* p = im.px.data() + (size_t)i * 4;
        for (int k = 0; k < 3; ++k) {
            int v = (int)((p[k] - 128) * c + 128 + brightness);
            p[k] = clampu8(v);
        }
    }
}

// 고정 임계값 이진화(회색조로 판단). thr 0~255. 결과는 흑/백.
static void opThreshold(Image& im, int thr, bool invert) {
    for (int i = 0; i < im.w * im.h; ++i) {
        unsigned char* p = im.px.data() + (size_t)i * 4;
        int g = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000;
        bool on = g >= thr;
        if (invert) on = !on;
        unsigned char v = on ? 255 : 0;
        p[0] = p[1] = p[2] = v;
    }
}

// Otsu 자동 임계값 이진화(히스토그램 기반 최적 임계값 자동 계산).
static int opOtsu(Image& im, bool invert) {
    int hist[256] = { 0 };
    int total = im.w * im.h;
    for (int i = 0; i < total; ++i) {
        const unsigned char* p = im.px.data() + (size_t)i * 4;
        int g = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000;
        hist[g]++;
    }
    double sum = 0;
    for (int t = 0; t < 256; ++t) sum += (double)t * hist[t];
    double sumB = 0; int wB = 0; double maxVar = -1; int thr = 0;
    for (int t = 0; t < 256; ++t) {
        wB += hist[t]; if (wB == 0) continue;
        int wF = total - wB; if (wF == 0) break;
        sumB += (double)t * hist[t];
        double mB = sumB / wB, mF = (sum - sumB) / wF;
        double var = (double)wB * wF * (mB - mF) * (mB - mF);
        if (var > maxVar) { maxVar = var; thr = t; }
    }
    opThreshold(im, thr, invert);
    return thr;
}

// 반전
static void opInvert(Image& im) {
    for (int i = 0; i < im.w * im.h; ++i) {
        unsigned char* p = im.px.data() + (size_t)i * 4;
        p[0] = 255 - p[0]; p[1] = 255 - p[1]; p[2] = 255 - p[2];
    }
}

// 3x3 컨벌루션 공용(경계는 클램프). div/bias 로 정규화.
static void conv3x3(Image& im, const int k[9], int divisor, int bias, bool grayFirst) {
    if (grayFirst) opGray(im);
    if (divisor == 0) divisor = 1;
    Image src = im;
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            int acc[3] = { 0,0,0 };
            for (int dy = -1; dy <= 1; ++dy) {
                int yy = std::min(std::max(y + dy, 0), im.h - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = std::min(std::max(x + dx, 0), im.w - 1);
                    const unsigned char* sp = src.at(xx, yy);
                    int kv = k[(dy + 1) * 3 + (dx + 1)];
                    acc[0] += sp[0] * kv; acc[1] += sp[1] * kv; acc[2] += sp[2] * kv;
                }
            }
            unsigned char* dp = im.at(x, y);
            dp[0] = clampu8(acc[0] / divisor + bias);
            dp[1] = clampu8(acc[1] / divisor + bias);
            dp[2] = clampu8(acc[2] / divisor + bias);
        }
    }
}

// 가우시안 블러(3x3 근사)
static void opBlur(Image& im) {
    static const int k[9] = { 1,2,1, 2,4,2, 1,2,1 };
    conv3x3(im, k, 16, 0, false);
}
// 샤픈
static void opSharpen(Image& im) {
    static const int k[9] = { 0,-1,0, -1,5,-1, 0,-1,0 };
    conv3x3(im, k, 1, 0, false);
}
// Sobel 에지(그레이 후 x/y 그라디언트 크기). 결과는 회색조 에지맵.
static void opEdgeSobel(Image& im) {
    opGray(im);
    Image src = im;
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            int gx = 0, gy = 0;
            static const int kx[9] = { -1,0,1, -2,0,2, -1,0,1 };
            static const int ky[9] = { -1,-2,-1, 0,0,0, 1,2,1 };
            for (int dy = -1; dy <= 1; ++dy) {
                int yy = std::min(std::max(y + dy, 0), im.h - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = std::min(std::max(x + dx, 0), im.w - 1);
                    int v = src.at(xx, yy)[0]; // 그레이라 채널 동일
                    gx += v * kx[(dy + 1) * 3 + (dx + 1)];
                    gy += v * ky[(dy + 1) * 3 + (dx + 1)];
                }
            }
            int mag = (int)std::sqrt((double)gx * gx + (double)gy * gy);
            unsigned char* dp = im.at(x, y);
            dp[0] = dp[1] = dp[2] = clampu8(mag);
        }
    }
}

// 리사이즈(비율 자유, GDI+ 고품질). newW/newH 중 하나가 <=0 이면 비율 유지.
static bool opResize(Image& im, int newW, int newH) {
    if (newW <= 0 && newH <= 0) return true;
    if (newW <= 0) newW = std::max(1, im.w * newH / im.h);
    if (newH <= 0) newH = std::max(1, im.h * newW / im.w);
    ppEnsureGdiplus();
    // Image -> Bitmap
    Gdiplus::Bitmap srcBmp(im.w, im.h, PixelFormat32bppARGB);
    {
        Gdiplus::Rect rc(0, 0, im.w, im.h); Gdiplus::BitmapData bd;
        srcBmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd);
        for (int r = 0; r < im.h; ++r)
            memcpy((unsigned char*)bd.Scan0 + (size_t)r * bd.Stride,
                   im.px.data() + (size_t)r * im.w * 4, (size_t)im.w * 4);
        srcBmp.UnlockBits(&bd);
    }
    Gdiplus::Bitmap dst(newW, newH, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&dst);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(&srcBmp, 0, 0, newW, newH);
    }
    Image out; out.w = newW; out.h = newH; out.px.resize((size_t)newW * newH * 4);
    {
        Gdiplus::Rect rc(0, 0, newW, newH); Gdiplus::BitmapData bd;
        dst.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd);
        for (int r = 0; r < newH; ++r)
            memcpy(out.px.data() + (size_t)r * newW * 4,
                   (unsigned char*)bd.Scan0 + (size_t)r * bd.Stride, (size_t)newW * 4);
        dst.UnlockBits(&bd);
    }
    im = std::move(out);
    return true;
}

// 배율 스케일(scalePct 예: 200 = 2배). 저해상도 텍스트 OCR 전 확대에 유용.
static bool opScale(Image& im, int scalePct) {
    if (scalePct <= 0) return false;
    int nw = std::max(1, im.w * scalePct / 100);
    int nh = std::max(1, im.h * scalePct / 100);
    return opResize(im, nw, nh);
}

// 크롭(경계 클램프)
static bool opCrop(Image& im, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    x = std::min(std::max(x, 0), im.w - 1);
    y = std::min(std::max(y, 0), im.h - 1);
    w = std::min(w, im.w - x);
    h = std::min(h, im.h - y);
    if (w <= 0 || h <= 0) return false;
    Image out; out.w = w; out.h = h; out.px.resize((size_t)w * h * 4);
    for (int r = 0; r < h; ++r)
        memcpy(out.at(0, r), im.at(x, y + r), (size_t)w * 4);
    im = std::move(out);
    return true;
}

// ============================================================================
//  [세트 AJ] 이미지 품질 향상 연산 (의존성 0, 순수 C++)
//   - 고품질 리샘플(Lanczos3/Bilinear/Nearest)  : upscale:P[,lanczos|bicubic|bilinear|nearest]
//   - 노이즈 제거(미디언/양방향)                 : denoise[:1|2]  bilateral:sS,sR
//   - 언샤프 마스크(디테일 강화)                  : unsharp:amount%,radius,threshold
//   - 손실 픽셀 복원(인페인팅)                    : inpaint:auto[,tol] | alpha | color,RRGGBB[,tol]
//   - ClearType 색번짐 제거                       : declear[:sat]
//   - 자동 레벨(대비 스트레치)                    : autolevel[:clipPct]
//   - 레터박스(비율 유지 맞춤+패딩)               : letterbox:W,H[,RRGGBB|auto]
//   - 알파 평탄화                                 : flatten[:RRGGBB]
// ============================================================================

// 알파 평탄화: 투명 픽셀을 배경색과 합성(불투명 이미지로).
static void opFlatten(Image& im, uint32_t rgb) {
    unsigned char br = (rgb >> 16) & 255, bg = (rgb >> 8) & 255, bb = rgb & 255;
    for (int i = 0; i < im.w * im.h; ++i) {
        unsigned char* p = im.px.data() + (size_t)i * 4;
        int a = p[3]; if (a == 255) continue;
        p[0] = (unsigned char)((p[0] * a + bb * (255 - a)) / 255);
        p[1] = (unsigned char)((p[1] * a + bg * (255 - a)) / 255);
        p[2] = (unsigned char)((p[2] * a + br * (255 - a)) / 255);
        p[3] = 255;
    }
}

// 테두리 픽셀 평균색(레터박스 패딩/증강 배경용)
static uint32_t edgeMeanColor(const Image& im) {
    long r = 0, g = 0, b = 0, n = 0;
    auto add = [&](int x, int y) { const unsigned char* p = im.at(x, y); b += p[0]; g += p[1]; r += p[2]; ++n; };
    for (int x = 0; x < im.w; ++x) { add(x, 0); if (im.h > 1) add(x, im.h - 1); }
    for (int y = 1; y < im.h - 1; ++y) { add(0, y); if (im.w > 1) add(im.w - 1, y); }
    if (n == 0) return 0;
    return ((uint32_t)(r / n) << 16) | ((uint32_t)(g / n) << 8) | (uint32_t)(b / n);
}

// ---- 리샘플 커널 ----
static inline float lanczosK(float x, int a) {
    if (x == 0.0f) return 1.0f;
    if (x <= -a || x >= a) return 0.0f;
    float px = 3.14159265f * x;
    return a * std::sin(px) * std::sin(px / a) / (px * px);
}
// 분리형(가로→세로) 리샘플. mode: 0=nearest 1=bilinear 3=lanczos3. (2=bicubic 은 GDI+ opResize)
static void resampleSeparable(const Image& src, Image& dst, int nw, int nh, int mode) {
    dst.w = nw; dst.h = nh; dst.px.assign((size_t)nw * nh * 4, 0);
    if (mode == 0) {
        for (int y = 0; y < nh; ++y) { int sy = std::min(src.h - 1, (int)((y + 0.5f) * src.h / nh));
            for (int x = 0; x < nw; ++x) { int sx = std::min(src.w - 1, (int)((x + 0.5f) * src.w / nw));
                memcpy(dst.at(x, y), src.at(sx, sy), 4); } }
        return;
    }
    int a = (mode == 3) ? 3 : 1;   // 커널 반경(lanczos3=3, bilinear=1)
    auto weightsFor = [&](int outLen, int inLen, std::vector<std::vector<std::pair<int, float>>>& tbl) {
        tbl.assign(outLen, {});
        float scale = (float)inLen / outLen;
        float support = (scale > 1.0f) ? a * scale : (float)a;   // 축소 시 커널 확장(안티앨리어싱)
        float inv = (scale > 1.0f) ? 1.0f / scale : 1.0f;
        for (int o = 0; o < outLen; ++o) {
            float center = (o + 0.5f) * scale - 0.5f;
            int lo = (int)std::floor(center - support), hi = (int)std::ceil(center + support);
            float sum = 0; std::vector<std::pair<int, float>> ws;
            for (int i = lo; i <= hi; ++i) {
                float d = (center - i) * inv;
                float w = (mode == 3) ? lanczosK(d, 3) : std::max(0.0f, 1.0f - std::abs(d));
                if (w == 0.0f) continue;
                int ii = std::min(std::max(i, 0), inLen - 1);
                ws.push_back({ ii, w }); sum += w;
            }
            if (sum != 0) for (auto& p : ws) p.second /= sum;
            tbl[o] = std::move(ws);
        }
    };
    std::vector<std::vector<std::pair<int, float>>> wx, wy;
    weightsFor(nw, src.w, wx); weightsFor(nh, src.h, wy);
    // 1) 가로: src(w×h) -> tmp(nw×h), float 로 유지
    std::vector<float> tmp((size_t)nw * src.h * 4);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < nw; ++x) {
            float acc[4] = { 0,0,0,0 };
            for (auto& p : wx[x]) { const unsigned char* s = src.at(p.first, y); for (int c = 0; c < 4; ++c) acc[c] += s[c] * p.second; }
            float* t = &tmp[((size_t)y * nw + x) * 4]; for (int c = 0; c < 4; ++c) t[c] = acc[c];
        }
    // 2) 세로: tmp -> dst
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            float acc[4] = { 0,0,0,0 };
            for (auto& p : wy[y]) { const float* t = &tmp[((size_t)p.first * nw + x) * 4]; for (int c = 0; c < 4; ++c) acc[c] += t[c] * p.second; }
            unsigned char* d = dst.at(x, y); for (int c = 0; c < 4; ++c) d[c] = clampu8((int)std::lround(acc[c]));
        }
}
// 배율 확대/축소(고품질). mode: 0 nearest / 1 bilinear / 2 bicubic(GDI+) / 3 lanczos3(기본)
static bool opUpscale(Image& im, int scalePct, int mode) {
    if (scalePct <= 0) return false;
    int nw = std::max(1, (int)std::lround(im.w * scalePct / 100.0)), nh = std::max(1, (int)std::lround(im.h * scalePct / 100.0));
    if (mode == 2) return opResize(im, nw, nh);
    Image out; resampleSeparable(im, out, nw, nh, mode); im = std::move(out); return true;
}
static bool opResampleTo(Image& im, int nw, int nh, int mode) {
    if (nw <= 0 && nh <= 0) return true;
    if (nw <= 0) nw = std::max(1, im.w * nh / im.h);
    if (nh <= 0) nh = std::max(1, im.h * nw / im.w);
    if (mode == 2) return opResize(im, nw, nh);
    Image out; resampleSeparable(im, out, nw, nh, mode); im = std::move(out); return true;
}
static int parseModeName(const std::wstring& s, int def) {
    std::wstring l = s; for (auto& c : l) c = (wchar_t)towlower(c);
    if (l == L"nearest" || l == L"nn") return 0;
    if (l == L"bilinear" || l == L"linear") return 1;
    if (l == L"bicubic" || l == L"cubic") return 2;
    if (l == L"lanczos" || l == L"lanczos3") return 3;
    return def;
}

// 미디언 3x3(소금·후추 노이즈 제거). 알파 유지.
static void opMedian3(Image& im) {
    Image src = im;
    for (int y = 0; y < im.h; ++y) for (int x = 0; x < im.w; ++x) {
        unsigned char v[3][9]; int n = 0;
        for (int dy = -1; dy <= 1; ++dy) { int yy = std::min(std::max(y + dy, 0), im.h - 1);
            for (int dx = -1; dx <= 1; ++dx) { int xx = std::min(std::max(x + dx, 0), im.w - 1);
                const unsigned char* s = src.at(xx, yy); v[0][n] = s[0]; v[1][n] = s[1]; v[2][n] = s[2]; ++n; } }
        unsigned char* d = im.at(x, y);
        for (int c = 0; c < 3; ++c) { std::nth_element(v[c], v[c] + 4, v[c] + 9); d[c] = v[c][4]; }
    }
}
// 양방향 필터(엣지 보존 스무딩). sigmaS=공간(px), sigmaR=색(0~255).
static void opBilateral(Image& im, float sigmaS, float sigmaR) {
    if (sigmaS <= 0) sigmaS = 2.0f; if (sigmaR <= 0) sigmaR = 25.0f;
    int r = std::max(1, (int)std::ceil(sigmaS * 2));
    Image src = im;
    std::vector<float> gs((size_t)(2 * r + 1) * (2 * r + 1));
    for (int dy = -r; dy <= r; ++dy) for (int dx = -r; dx <= r; ++dx)
        gs[(size_t)(dy + r) * (2 * r + 1) + (dx + r)] = std::exp(-(dx * dx + dy * dy) / (2 * sigmaS * sigmaS));
    float gr[256 * 3 + 1];
    for (int i = 0; i <= 255 * 3; ++i) gr[i] = std::exp(-(float)i * i / (2 * sigmaR * sigmaR * 3));
    for (int y = 0; y < im.h; ++y) for (int x = 0; x < im.w; ++x) {
        const unsigned char* c0 = src.at(x, y);
        float acc[3] = { 0,0,0 }, wsum = 0;
        for (int dy = -r; dy <= r; ++dy) { int yy = y + dy; if (yy < 0 || yy >= im.h) continue;
            for (int dx = -r; dx <= r; ++dx) { int xx = x + dx; if (xx < 0 || xx >= im.w) continue;
                const unsigned char* s = src.at(xx, yy);
                int cd = std::abs(s[0] - c0[0]) + std::abs(s[1] - c0[1]) + std::abs(s[2] - c0[2]);
                float w = gs[(size_t)(dy + r) * (2 * r + 1) + (dx + r)] * gr[cd];
                acc[0] += s[0] * w; acc[1] += s[1] * w; acc[2] += s[2] * w; wsum += w; } }
        unsigned char* d = im.at(x, y);
        if (wsum > 0) for (int c = 0; c < 3; ++c) d[c] = clampu8((int)std::lround(acc[c] / wsum));
    }
}
// 가우시안 블러(반경 임의, 분리형). radius px (sigma=radius/2 근사)
static void gaussianBlurF(const Image& src, Image& dst, float radius) {
    if (radius < 0.5f) { dst = src; return; }
    float sigma = radius * 0.5f; int r = (int)std::ceil(radius * 1.5f); if (r < 1) r = 1;
    std::vector<float> k(2 * r + 1); float s = 0;
    for (int i = -r; i <= r; ++i) { k[i + r] = std::exp(-(float)i * i / (2 * sigma * sigma)); s += k[i + r]; }
    for (auto& v : k) v /= s;
    std::vector<float> tmp((size_t)src.w * src.h * 3);
    for (int y = 0; y < src.h; ++y) for (int x = 0; x < src.w; ++x) {
        float acc[3] = { 0,0,0 };
        for (int i = -r; i <= r; ++i) { int xx = std::min(std::max(x + i, 0), src.w - 1); const unsigned char* p = src.at(xx, y);
            for (int c = 0; c < 3; ++c) acc[c] += p[c] * k[i + r]; }
        float* t = &tmp[((size_t)y * src.w + x) * 3]; for (int c = 0; c < 3; ++c) t[c] = acc[c];
    }
    dst = src;
    for (int y = 0; y < src.h; ++y) for (int x = 0; x < src.w; ++x) {
        float acc[3] = { 0,0,0 };
        for (int i = -r; i <= r; ++i) { int yy = std::min(std::max(y + i, 0), src.h - 1); const float* t = &tmp[((size_t)yy * src.w + x) * 3];
            for (int c = 0; c < 3; ++c) acc[c] += t[c] * k[i + r]; }
        unsigned char* d = dst.at(x, y); for (int c = 0; c < 3; ++c) d[c] = clampu8((int)std::lround(acc[c]));
    }
}
// 언샤프 마스크: out = src + amount*(src - blur), |diff|<=threshold 는 그대로(노이즈 증폭 방지)
static void opUnsharp(Image& im, int amountPct, float radius, int threshold) {
    if (amountPct <= 0) return;
    Image bl; gaussianBlurF(im, bl, radius <= 0 ? 1.0f : radius);
    float amt = amountPct / 100.0f;
    for (int i = 0; i < im.w * im.h; ++i) {
        unsigned char* p = im.px.data() + (size_t)i * 4; const unsigned char* b = bl.px.data() + (size_t)i * 4;
        for (int c = 0; c < 3; ++c) { int d = p[c] - b[c]; if (std::abs(d) <= threshold) continue; p[c] = clampu8((int)std::lround(p[c] + amt * d)); }
    }
}

// ---- 인페인팅(손실 픽셀 복원) ----
//  마스크(복원할 픽셀)를 만든 뒤, 경계에서 안쪽으로 "이웃 기지(known) 픽셀의 가중평균"으로
//  채운다(정규화 컨볼루션, Telea 알고리즘의 경량판). 그 후 채운 영역만 몇 번 완화(harmonic)해
//  이음새를 없앤다. 작은 구멍·점 노이즈·투명 구멍에 효과적. 큰 영역은 흐릿하게 채워진다
//  (큰 영역은 딥러닝 인페인팅(LaMa 등 ONNX)이 답 — 개선검토.md 참고).
//  mode 0=auto(주변 8이웃 모두와 tol 이상 다른 고립 픽셀) 1=alpha(투명) 2=color(지정색±tol)
static int buildInpaintMask(const Image& im, int mode, uint32_t rgb, int tol, std::vector<unsigned char>& mask) {
    mask.assign((size_t)im.w * im.h, 0); int cnt = 0;
    if (mode == 1) {
        for (int i = 0; i < im.w * im.h; ++i) if (im.px[(size_t)i * 4 + 3] < 128) { mask[i] = 1; ++cnt; }
        return cnt;
    }
    if (mode == 2) {
        int kr = (rgb >> 16) & 255, kg = (rgb >> 8) & 255, kb = rgb & 255;
        for (int i = 0; i < im.w * im.h; ++i) { const unsigned char* p = im.px.data() + (size_t)i * 4;
            if (std::abs(p[2] - kr) <= tol && std::abs(p[1] - kg) <= tol && std::abs(p[0] - kb) <= tol) { mask[i] = 1; ++cnt; } }
        return cnt;
    }
    // auto: 8이웃 전부와 색거리(채널합)가 tol*3 초과 → 고립 이상치(점 노이즈/데드픽셀)
    int t3 = tol * 3;
    for (int y = 0; y < im.h; ++y) for (int x = 0; x < im.w; ++x) {
        const unsigned char* p = im.at(x, y); int mn = 1 << 30, n = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue; int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= im.w || yy >= im.h) continue;
            const unsigned char* q = im.at(xx, yy);
            int d = std::abs(p[0] - q[0]) + std::abs(p[1] - q[1]) + std::abs(p[2] - q[2]); mn = std::min(mn, d); ++n; }
        if (n >= 3 && mn > t3) { mask[(size_t)y * im.w + x] = 1; ++cnt; }
    }
    return cnt;
}
static void inpaintFill(Image& im, std::vector<unsigned char>& mask) {
    const int W = im.w, H = im.h;
    std::vector<unsigned char> filled = mask;   // 1=아직 미복원
    std::vector<int> frontier; frontier.reserve(1024);
    // 가중치: 4이웃 2, 대각 1
    for (int iter = 0; iter < W + H + 8; ++iter) {
        frontier.clear();
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            if (!filled[(size_t)y * W + x]) continue;
            bool hasKnown = false;
            for (int dy = -1; dy <= 1 && !hasKnown; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                if (!filled[(size_t)yy * W + xx]) { hasKnown = true; break; } }
            if (hasKnown) frontier.push_back(y * W + x);
        }
        if (frontier.empty()) break;
        std::vector<unsigned char> newpx(frontier.size() * 4);
        for (size_t f = 0; f < frontier.size(); ++f) {
            int x = frontier[f] % W, y = frontier[f] / W; float acc[4] = { 0,0,0,0 }, ws = 0;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue; int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                if (filled[(size_t)yy * W + xx]) continue;
                float w = (dx && dy) ? 1.0f : 2.0f; const unsigned char* q = im.at(xx, yy);
                for (int c = 0; c < 3; ++c) acc[c] += q[c] * w; ws += w; }
            unsigned char* o = &newpx[f * 4];
            for (int c = 0; c < 3; ++c) o[c] = clampu8((int)std::lround(acc[c] / (ws > 0 ? ws : 1))); o[3] = 255;
        }
        for (size_t f = 0; f < frontier.size(); ++f) { memcpy(im.px.data() + (size_t)frontier[f] * 4, &newpx[f * 4], 4); filled[frontier[f]] = 0; }
    }
    // 완화: 마스크 영역만 4이웃 평균으로 몇 회 반복(이음새 제거)
    for (int it = 0; it < 4; ++it) {
        Image src = im;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            if (!mask[(size_t)y * W + x]) continue; float acc[3] = { 0,0,0 }; int n = 0;
            const int d4[4][2] = { {1,0},{-1,0},{0,1},{0,-1} };
            for (auto& d : d4) { int xx = x + d[0], yy = y + d[1]; if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                const unsigned char* q = src.at(xx, yy); for (int c = 0; c < 3; ++c) acc[c] += q[c]; ++n; }
            if (n) { unsigned char* o = im.at(x, y); for (int c = 0; c < 3; ++c) o[c] = clampu8((int)std::lround(acc[c] / n)); }
        }
    }
}
static int opInpaint(Image& im, int mode, uint32_t rgb, int tol) {
    std::vector<unsigned char> mask; int n = buildInpaintMask(im, mode, rgb, tol, mask);
    if (n == 0) return 0;
    if (n >= im.w * im.h) return 0;   // 전부 마스크면 복원 불가
    inpaintFill(im, mask);
    return n;
}

// ClearType(서브픽셀 렌더링) 색번짐 제거: 글자 가장자리의 채도 높은 픽셀을 밝기값(그레이)으로.
//  주변(5x5) 평균 채도가 낮은데(=본래 무채색 영역) 해당 픽셀만 채도가 높으면 색번짐으로 판정.
static int opDeClearType(Image& im, int satThr) {
    if (satThr <= 0) satThr = 40;
    Image src = im; int fixed = 0;
    auto sat = [](const unsigned char* p) { int mx = std::max(p[0], std::max(p[1], p[2])), mn = std::min(p[0], std::min(p[1], p[2])); return mx - mn; };
    for (int y = 0; y < im.h; ++y) for (int x = 0; x < im.w; ++x) {
        const unsigned char* p = src.at(x, y); int s = sat(p); if (s < satThr) continue;
        long ssum = 0; int n = 0;
        for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
            int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= im.w || yy >= im.h) continue;
            ssum += sat(src.at(xx, yy)); ++n; }
        int avg = n ? (int)(ssum / n) : 0;
        if (avg < satThr / 2 + 8) {   // 주변은 무채색 → 이 픽셀만 색번짐
            int g = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000;
            unsigned char* d = im.at(x, y); d[0] = d[1] = d[2] = (unsigned char)g; ++fixed;
        }
    }
    return fixed;
}

// 자동 레벨: 밝기 히스토그램 하위/상위 clipPct% 를 잘라 0~255 로 스트레치(채널 공통).
static void opAutoLevel(Image& im, int clipPct) {
    if (clipPct < 0) clipPct = 1; if (clipPct > 40) clipPct = 40;
    int hist[256] = { 0 }; int N = im.w * im.h;
    for (int i = 0; i < N; ++i) { const unsigned char* p = im.px.data() + (size_t)i * 4; hist[(p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000]++; }
    int lo = 0, hi = 255, acc = 0, clip = N * clipPct / 100;
    for (int t = 0; t < 256; ++t) { acc += hist[t]; if (acc > clip) { lo = t; break; } }
    acc = 0; for (int t = 255; t >= 0; --t) { acc += hist[t]; if (acc > clip) { hi = t; break; } }
    if (hi - lo < 8) return;
    float k = 255.0f / (hi - lo);
    for (int i = 0; i < N; ++i) { unsigned char* p = im.px.data() + (size_t)i * 4; for (int c = 0; c < 3; ++c) p[c] = clampu8((int)std::lround((p[c] - lo) * k)); }
}

// 레터박스: 비율 유지로 W×H 안에 맞추고(lanczos) 남는 곳은 색으로 패딩. useAuto=테두리 평균색.
static bool opLetterbox(Image& im, int W, int H, uint32_t rgb, bool useAuto) {
    if (W <= 0 || H <= 0) return false;
    if (useAuto) rgb = edgeMeanColor(im);
    double s = std::min((double)W / im.w, (double)H / im.h);
    int nw = std::max(1, (int)std::lround(im.w * s)), nh = std::max(1, (int)std::lround(im.h * s));
    Image scaled; resampleSeparable(im, scaled, nw, nh, 3);
    Image out; out.w = W; out.h = H; out.px.resize((size_t)W * H * 4);
    unsigned char bg[4] = { (unsigned char)(rgb & 255), (unsigned char)((rgb >> 8) & 255), (unsigned char)((rgb >> 16) & 255), 255 };
    for (int i = 0; i < W * H; ++i) memcpy(out.px.data() + (size_t)i * 4, bg, 4);
    int ox = (W - nw) / 2, oy = (H - nh) / 2;
    for (int y = 0; y < nh; ++y) memcpy(out.at(ox, oy + y), scaled.at(0, y), (size_t)nw * 4);
    im = std::move(out); return true;
}

// 품질 지표: 선명도(라플라시안 분산), 노이즈(미디언 잔차 MAD), 대비(밝기 표준편차)
static void qualityMetrics(const Image& im, double& sharp, double& noise, double& contrast) {
    int N = im.w * im.h; if (N == 0) { sharp = noise = contrast = 0; return; }
    std::vector<float> g((size_t)N);
    double mean = 0;
    for (int i = 0; i < N; ++i) { const unsigned char* p = im.px.data() + (size_t)i * 4; g[i] = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000.0f; mean += g[i]; }
    mean /= N; double var = 0; for (int i = 0; i < N; ++i) var += (g[i] - mean) * (g[i] - mean); contrast = std::sqrt(var / N);
    double lm = 0, lv = 0; int cnt = 0; std::vector<float> resid; resid.reserve((size_t)N);
    for (int y = 1; y < im.h - 1; ++y) for (int x = 1; x < im.w - 1; ++x) {
        float c = g[(size_t)y * im.w + x];
        float lap = -4 * c + g[(size_t)y * im.w + x - 1] + g[(size_t)y * im.w + x + 1] + g[(size_t)(y - 1) * im.w + x] + g[(size_t)(y + 1) * im.w + x];
        lm += lap; lv += lap * lap; ++cnt;
        float v[9]; int k = 0; for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) v[k++] = g[(size_t)(y + dy) * im.w + x + dx];
        std::nth_element(v, v + 4, v + 9); resid.push_back(std::abs(c - v[4]));
    }
    if (cnt) { lm /= cnt; sharp = lv / cnt - lm * lm; } else sharp = 0;
    if (!resid.empty()) { std::nth_element(resid.begin(), resid.begin() + resid.size() / 2, resid.end()); noise = resid[resid.size() / 2] * 1.4826; } else noise = 0;
}

// ---- 파이프라인 파서 --------------------------------------------------------
// 레시피 예: "gray|otsu|scale:200|blur"
//   토큰은 '|' 구분, 인자는 ':' 뒤에 콤마로. 대소문자 무시.
// 지원: gray, invert, blur, sharpen, edge, otsu[:inv], threshold:T[,inv],
//       brightness:N, contrast:P, scale:P, resize:W,H, crop:X,Y,W,H
static bool splitArgs(const std::wstring& s, std::vector<int>& nums, bool& invFlag,
                      std::vector<std::wstring>* words = nullptr) {
    invFlag = false;
    std::wstring cur;
    std::vector<std::wstring> parts;
    for (wchar_t c : s) {
        if (c == L',') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    for (auto& p : parts) {
        std::wstring t = p;
        while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
        while (!t.empty() && iswspace(t.back())) t.pop_back();
        if (t.empty()) continue;
        std::wstring low = t; for (auto& c : low) c = (wchar_t)towlower(c);
        if (low == L"inv" || low == L"invert") { invFlag = true; continue; }
        if (words) words->push_back(t);
        // 숫자(선택적 소수)만 nums 로. 단어(lanczos/auto/RRGGBB 등)는 words 에만.
        bool numeric = !t.empty() && (iswdigit(t[0]) || t[0] == L'-' || t[0] == L'.');
        if (numeric) nums.push_back(_wtoi(t.c_str()));
    }
    return true;
}
// "RRGGBB" / "0xRRGGBB" 16진 색 파싱. 실패 false.
static bool parseHexColor(const std::wstring& s, uint32_t& rgb) {
    std::wstring t = s; if (t.size() > 2 && (t[0] == L'0') && (t[1] == L'x' || t[1] == L'X')) t = t.substr(2);
    if (t.size() != 6) return false;
    uint32_t v = 0;
    for (wchar_t c : t) { int d; if (c >= L'0' && c <= L'9') d = c - L'0'; else if (c >= L'a' && c <= L'f') d = c - L'a' + 10; else if (c >= L'A' && c <= L'F') d = c - L'A' + 10; else return false; v = v * 16 + d; }
    rgb = v; return true;
}
static float wordFloat(const std::vector<std::wstring>& w, size_t i, float def) {
    if (i >= w.size()) return def; wchar_t* e = nullptr; float v = (float)wcstod(w[i].c_str(), &e); return (e == w[i].c_str()) ? def : v;
}

// 파이프라인 실행. 실패한 단계명을 errStep 에 담고 false.
bool runPipeline(Image& im, const std::wstring& recipe, std::wstring& errStep) {
    std::wstring token;
    std::vector<std::wstring> steps;
    for (wchar_t c : recipe) {
        if (c == L'|') { steps.push_back(token); token.clear(); }
        else token.push_back(c);
    }
    steps.push_back(token);

    for (auto& raw : steps) {
        std::wstring st = raw;
        while (!st.empty() && iswspace(st.front())) st.erase(st.begin());
        while (!st.empty() && iswspace(st.back())) st.pop_back();
        if (st.empty()) continue;

        std::wstring name = st, argstr;
        size_t colon = st.find(L':');
        if (colon != std::wstring::npos) { name = st.substr(0, colon); argstr = st.substr(colon + 1); }
        for (auto& c : name) c = (wchar_t)towlower(c);

        std::vector<int> nums; bool inv = false; std::vector<std::wstring> words;
        splitArgs(argstr, nums, inv, &words);

        if (name == L"gray" || name == L"grayscale") opGray(im);
        else if (name == L"invert") opInvert(im);
        else if (name == L"blur") opBlur(im);
        else if (name == L"sharpen") opSharpen(im);
        else if (name == L"edge" || name == L"sobel") opEdgeSobel(im);
        else if (name == L"otsu") opOtsu(im, inv);
        else if (name == L"threshold" || name == L"binary")
            opThreshold(im, nums.empty() ? 128 : nums[0], inv);
        else if (name == L"brightness") opBrightnessContrast(im, nums.empty() ? 0 : nums[0], 100);
        else if (name == L"contrast") opBrightnessContrast(im, 0, nums.empty() ? 100 : nums[0]);
        else if (name == L"scale") { if (!opScale(im, nums.empty() ? 100 : nums[0])) { errStep = name; return false; } }
        else if (name == L"resize") {
            int w = nums.size() > 0 ? nums[0] : 0, h = nums.size() > 1 ? nums[1] : 0;
            if (!opResize(im, w, h)) { errStep = name; return false; }
        }
        else if (name == L"crop") {
            if (nums.size() < 4) { errStep = name; return false; }
            if (!opCrop(im, nums[0], nums[1], nums[2], nums[3])) { errStep = name; return false; }
        }
        // ---- 세트 AJ: 품질 향상 연산 ----
        else if (name == L"upscale" || name == L"resample") {
            // upscale:P[,mode]  (P=배율%, mode=lanczos(기본)/bicubic/bilinear/nearest)
            int pct = nums.empty() ? 200 : nums[0];
            int mode = 3; for (auto& w : words) mode = parseModeName(w, mode);
            if (!opUpscale(im, pct, mode)) { errStep = name; return false; }
        }
        else if (name == L"fit") {   // fit:W,H[,mode]  비율 무시 리샘플(고품질)
            int w = nums.size() > 0 ? nums[0] : 0, h = nums.size() > 1 ? nums[1] : 0;
            int mode = 3; for (auto& s2 : words) mode = parseModeName(s2, mode);
            if (!opResampleTo(im, w, h, mode)) { errStep = name; return false; }
        }
        else if (name == L"denoise") {   // denoise[:1|2]  1=미디언3x3(기본) 2=양방향
            int lvl = nums.empty() ? 1 : nums[0];
            if (lvl >= 2) opBilateral(im, 2.0f, 25.0f); else opMedian3(im);
        }
        else if (name == L"median") opMedian3(im);
        else if (name == L"bilateral") opBilateral(im, wordFloat(words, 0, 2.0f), wordFloat(words, 1, 25.0f));
        else if (name == L"unsharp") {   // unsharp:amount%[,radius[,threshold]]
            int amt = nums.empty() ? 80 : nums[0];
            float rad = wordFloat(words, 1, 1.0f);
            int thr = nums.size() > 2 ? nums[2] : 2;
            opUnsharp(im, amt, rad, thr);
        }
        else if (name == L"gblur") { Image o; gaussianBlurF(im, o, wordFloat(words, 0, 1.5f)); im = std::move(o); }
        else if (name == L"inpaint") {
            // inpaint[:auto[,tol]] | inpaint:alpha | inpaint:color,RRGGBB[,tol]
            int mode = 0; uint32_t rgb = 0xFF00FF; int tol = 40;
            for (auto& w : words) {
                std::wstring l = w; for (auto& c : l) c = (wchar_t)towlower(c);
                uint32_t c;
                if (l == L"auto") mode = 0; else if (l == L"alpha") mode = 1; else if (l == L"color") mode = 2;
                else if (w.size() == 6 && parseHexColor(w, c)) rgb = c;
                else if (!w.empty() && iswdigit(w[0])) tol = _wtoi(w.c_str());
            }
            opInpaint(im, mode, rgb, tol);
        }
        else if (name == L"declear" || name == L"decleartype") opDeClearType(im, nums.empty() ? 40 : nums[0]);
        else if (name == L"autolevel") opAutoLevel(im, nums.empty() ? 1 : nums[0]);
        else if (name == L"letterbox" || name == L"pad") {
            // letterbox:W,H[,RRGGBB|auto]
            if (nums.size() < 2) { errStep = name; return false; }
            uint32_t rgb = 0; bool useAuto = true;
            for (auto& w : words) { uint32_t c; if (parseHexColor(w, c)) { rgb = c; useAuto = false; } }
            if (!opLetterbox(im, nums[0], nums[1], rgb, useAuto)) { errStep = name; return false; }
        }
        else if (name == L"flatten") {
            uint32_t rgb = 0xFFFFFF; for (auto& w : words) { uint32_t c; if (parseHexColor(w, c)) rgb = c; }
            opFlatten(im, rgb);
        }
        else { errStep = name; return false; } // 알 수 없는 단계
    }
    return true;
}

// ---- 공개 진입점(exports 에서 호출) -----------------------------------------

// 단일: 파일 -> 파이프라인 -> 파일. 성공 true.
bool preprocessFile(const std::wstring& inPath, const std::wstring& outPath,
                    const std::wstring& recipe, int quality, std::wstring& errStep) {
    Image im;
    if (!loadImage(inPath, im)) { errStep = L"load"; return false; }
    if (!runPipeline(im, recipe, errStep)) return false;
    if (!saveImage(im, outPath, quality)) { errStep = L"save"; return false; }
    return true;
}

// ============================================================================
//  [세트 AJ] 공개 진입점 — 이미지 품질 향상
// ============================================================================
// notepad_onnx.cpp (세션 캐시 추론; 스텁 빌드면 false)
bool onnxRunFloatsCached(const std::wstring& modelPath, const std::vector<float>& input,
                         const std::vector<int64_t>& shape, std::vector<float>& output,
                         std::vector<int64_t>& outShape);

// 원클릭 품질 향상(프리셋). opts 는 ',' 구분 플래그(대소문자 무시). 비어 있으면 "auto".
//   auto      = inpaint(auto) + declear + upscale(lanczos) + denoise(bilateral) + unsharp
//   inpaint / declear / denoise / median / sharpen / autolevel / flatten / nearest|bilinear|bicubic|lanczos
//   noinpaint / nodeclear / nodenoise / nosharpen  (auto 에서 특정 단계 제외)
//   text      = OCR/글자용(선명도 강함, 채도 정리). icon = 아이콘/버튼용(엣지 보존, 완만).
//   scalePct: 확대 배율 % (100=크기 유지). 반환: 성공 true. 적용한 레시피를 usedRecipe 에 돌려준다.
bool enhanceFile(const std::wstring& inPath, const std::wstring& outPath, int scalePct,
                 const std::wstring& opts, int quality, std::wstring& usedRecipe, std::wstring& errStep) {
    std::wstring low = opts; for (auto& c : low) c = (wchar_t)towlower(c);
    auto has = [&](const wchar_t* k) {   // 토큰 단위 포함 여부
        std::wstring cur; std::vector<std::wstring> toks;
        for (wchar_t c : low) { if (c == L',' || c == L'|' || c == L' ') { if (!cur.empty()) toks.push_back(cur); cur.clear(); } else cur.push_back(c); }
        if (!cur.empty()) toks.push_back(cur);
        for (auto& t : toks) if (t == k) return true; return false;
    };
    bool autoMode = low.empty() || has(L"auto") || has(L"text") || has(L"icon");
    bool text = has(L"text"), icon = has(L"icon");
    std::wstring mode = has(L"nearest") ? L"nearest" : has(L"bilinear") ? L"bilinear" : has(L"bicubic") ? L"bicubic" : L"lanczos";
    std::wstring r;
    auto add = [&](const std::wstring& s) { if (!r.empty()) r += L"|"; r += s; };
    if (has(L"flatten")) add(L"flatten:FFFFFF");
    if ((autoMode && !has(L"noinpaint")) || has(L"inpaint")) add(L"inpaint:auto,48");
    if ((autoMode && !has(L"nodeclear")) || has(L"declear")) add(text ? L"declear:30" : L"declear:40");
    if (scalePct > 0 && scalePct != 100) add(L"upscale:" + std::to_wstring(scalePct) + L"," + mode);
    if (has(L"median")) add(L"median");
    if ((autoMode && !has(L"nodenoise")) || has(L"denoise")) add(icon ? L"bilateral:1.5,20" : L"bilateral:2,25");
    if (has(L"autolevel") || text) add(L"autolevel:1");
    if ((autoMode && !has(L"nosharpen")) || has(L"sharpen")) add(text ? L"unsharp:120,1.2,2" : icon ? L"unsharp:60,1,3" : L"unsharp:80,1,2");
    if (r.empty()) r = L"upscale:100,lanczos";   // 아무 것도 없으면 무변환 복사
    usedRecipe = r;
    return preprocessFile(inPath, outPath, r, quality, errStep);
}

// 인페인팅 단독. mode 0=auto 1=alpha 2=color. 반환: 복원한 픽셀 수(-1 실패).
int inpaintFile(const std::wstring& inPath, const std::wstring& outPath, int mode, uint32_t rgb, int tol, int quality) {
    Image im; if (!loadImage(inPath, im)) return -1;
    int n = opInpaint(im, mode, rgb, tol < 0 ? 40 : tol);
    if (!saveImage(im, outPath, quality)) return -1;
    return n;
}

// 품질 지표 문자열: "w=..;h=..;sharp=..;noise=..;contrast=.." (sharp 클수록 선명, noise 작을수록 깨끗)
std::wstring imageQuality(const std::wstring& inPath) {
    Image im; if (!loadImage(inPath, im)) return L"";
    double s, n, c; qualityMetrics(im, s, n, c);
    wchar_t buf[160];
    swprintf(buf, 160, L"w=%d;h=%d;sharp=%.1f;noise=%.2f;contrast=%.1f", im.w, im.h, s, n, c);
    return buf;
}

// 딥러닝 초해상도(ONNX, 예: Real-ESRGAN x4). 입력 [1,3,H,W] RGB 0~1 → 출력 [1,3,H*s,W*s].
//   tile: 메모리/속도 위해 타일 분할(0=통짜). 모델이 없거나 스텁이면 Lanczos 폴백(fallback=1일 때).
//   반환: 1=DL 성공, 2=폴백(Lanczos)으로 처리, 0=실패.
// 버퍼(BGRA) 초해상도 코어. 반환 1=DL, 2=폴백, 0=실패. 성공 시 im 이 scale 배로 커진다.
static int superResImage(Image& im, const std::wstring& modelPath, int scale, int tile, int fallback, std::wstring& err) {
    if (scale <= 0) scale = 4;
    opFlatten(im, 0xFFFFFF);
    auto doFallback = [&]() -> int {
        if (!fallback) return 0;
        Image out; resampleSeparable(im, out, im.w * scale, im.h * scale, 3);
        opUnsharp(out, 60, 1.0f, 2);
        im = std::move(out);
        return 2;
    };
    if (modelPath.empty() || GetFileAttributesW(modelPath.c_str()) == INVALID_FILE_ATTRIBUTES) { err = L"model not found: " + modelPath; return doFallback(); }
    if (tile <= 0) tile = 128;
    const int PAD = 8;
    Image out; out.w = im.w * scale; out.h = im.h * scale; out.px.assign((size_t)out.w * out.h * 4, 255);
    for (int ty = 0; ty < im.h; ty += tile)
        for (int tx = 0; tx < im.w; tx += tile) {
            int x0 = std::max(0, tx - PAD), y0 = std::max(0, ty - PAD);
            int x1 = std::min(im.w, tx + tile + PAD), y1 = std::min(im.h, ty + tile + PAD);
            int tw = x1 - x0, th = y1 - y0;
            std::vector<float> in((size_t)3 * tw * th);
            for (int y = 0; y < th; ++y) for (int x = 0; x < tw; ++x) {
                const unsigned char* p = im.at(x0 + x, y0 + y);
                in[(size_t)0 * tw * th + (size_t)y * tw + x] = p[2] / 255.0f;
                in[(size_t)1 * tw * th + (size_t)y * tw + x] = p[1] / 255.0f;
                in[(size_t)2 * tw * th + (size_t)y * tw + x] = p[0] / 255.0f;
            }
            std::vector<int64_t> shape = { 1, 3, th, tw }, oshape; std::vector<float> o;
            if (!onnxRunFloatsCached(modelPath, in, shape, o, oshape) || o.size() < (size_t)3 * tw * th * scale * scale) {
                err = L"onnx inference failed (see %TEMP%\\notepad_onnx_err.txt)"; return doFallback();
            }
            int ow = tw * scale, oh = th * scale;
            int vx0 = (tx - x0) * scale, vy0 = (ty - y0) * scale;
            int vw = std::min(tile, im.w - tx) * scale, vh = std::min(tile, im.h - ty) * scale;
            for (int y = 0; y < vh; ++y) for (int x = 0; x < vw; ++x) {
                int sx = vx0 + x, sy = vy0 + y;
                unsigned char* d = out.at(tx * scale + x, ty * scale + y);
                d[2] = clampu8((int)std::lround(o[(size_t)0 * ow * oh + (size_t)sy * ow + sx] * 255.0f));
                d[1] = clampu8((int)std::lround(o[(size_t)1 * ow * oh + (size_t)sy * ow + sx] * 255.0f));
                d[0] = clampu8((int)std::lround(o[(size_t)2 * ow * oh + (size_t)sy * ow + sx] * 255.0f));
                d[3] = 255;
            }
        }
    im = std::move(out);
    return 1;
}

// 딥러닝 초해상도(ONNX, 동봉 models\realesrgan_x4.onnx = Real-ESRGAN general x4v3, 경량·CPU 실용).
//   입력 [1,3,H,W] RGB 0~1 → 출력 [1,3,H*s,W*s]. tile: 타일 분할(0=기본 128). 모델 없거나 스텁이면
//   fallback=1 일 때 Lanczos 폴백. 반환: 1=DL 성공, 2=폴백, 0=실패.
int superResFile(const std::wstring& inPath, const std::wstring& outPath, const std::wstring& modelPath,
                 int scale, int tile, int fallback, int quality, std::wstring& err) {
    Image im; if (!loadImage(inPath, im)) { err = L"load"; return 0; }
    int r = superResImage(im, modelPath, scale, tile, fallback, err);
    if (r == 0) return 0;
    if (!saveImage(im, outPath, quality)) { err += L"; save"; return 0; }
    return r;
}
// 버퍼 버전(OCR 등 다른 모듈용). 반환 동일. w/h 갱신.
int superResBGRA(int& w, int& h, std::vector<unsigned char>& px, const std::wstring& modelPath,
                 int scale, int tile, int fallback, std::wstring& err) {
    Image im; im.w = w; im.h = h; im.px = px;
    if (!im.valid()) { err = L"buffer"; return 0; }
    int r = superResImage(im, modelPath, scale, tile, fallback, err);
    if (r == 0) return 0;
    w = im.w; h = im.h; px = std::move(im.px);
    return r;
}

// 메모리 버퍼(BGRA top-down)에 레시피 적용. 다른 모듈(OCR/NN)이 파일 경유 없이 쓰는 진입점.
//   w/h 는 레시피(upscale 등)에 따라 갱신된다. 실패 시 버퍼는 원본 유지.
bool runRecipeBGRA(int& w, int& h, std::vector<unsigned char>& px, const std::wstring& recipe, std::wstring& errStep) {
    Image im; im.w = w; im.h = h; im.px = px;
    if (!im.valid()) { errStep = L"buffer"; return false; }
    if (!runPipeline(im, recipe, errStep)) return false;
    w = im.w; h = im.h; px = std::move(im.px);
    return true;
}

// 폴더 일괄 향상: srcDir 의 하위폴더 구조를 dstDir 에 그대로 만들며 이미지마다 enhanceFile.
//   학습셋(images\카테고리\*.png) 전체를 향상해 images_hq\ 로 만드는 용도. 반환: 처리 장수.
int enhanceFolder(const std::wstring& srcDir, const std::wstring& dstDir, int scalePct,
                  const std::wstring& opts, int quality) {
    CreateDirectoryW(dstDir.c_str(), nullptr);
    int done = 0;
    std::wstring pat = srcDir + L"\\*";
    WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            done += enhanceFolder(srcDir + L"\\" + name, dstDir + L"\\" + name, scalePct, opts, quality);
            continue;
        }
        size_t d = name.find_last_of(L'.'); if (d == std::wstring::npos) continue;
        std::wstring e = name.substr(d + 1); for (auto& c : e) c = (wchar_t)towlower(c);
        if (e != L"png" && e != L"jpg" && e != L"jpeg" && e != L"bmp" && e != L"gif") continue;
        std::wstring used, err;
        if (enhanceFile(srcDir + L"\\" + name, dstDir + L"\\" + name, scalePct, opts, quality, used, err)) ++done;
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return done;
}

} // namespace notepad

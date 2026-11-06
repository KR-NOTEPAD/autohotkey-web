// ============================================================================
//  NOTEPAD.dll  -  notepad_image.cpp
//  [NOTEPAD] 세트 O: 이미지 경량화 (GDI+ 리사이즈 + JPEG 품질 + 포맷 변환).
//  링크: gdiplus.lib (build.bat 에 이미 포함).
//  의존성 0: 외부 런타임 없음. 파이썬 Pillow 의 resize/save(quality) 대응.
//
//  포맷 판정: 출력 경로 확장자로 인코더 선택(.jpg/.jpeg -> JPEG, 그 외 -> PNG).
//  quality: JPEG 품질 0~100(작을수록 용량↓ 화질↓). PNG 에는 무관(무손실).
// ============================================================================
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cwctype>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

namespace notepad { bool captureBGRA(int x, int y, int w, int h, std::vector<unsigned char>& out); }   // notepad_screen.cpp

namespace {

std::string img_wToUtf8(const wchar_t* w) {
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

ULONG_PTR g_imgGpToken = 0;
bool g_imgGpUp = false;
void imgEnsureGdiplus() {
    if (g_imgGpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_imgGpToken, &in, nullptr) == Gdiplus::Ok)
        g_imgGpUp = true;
}

int imgGetEncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<unsigned char> buf(size);
    Gdiplus::ImageCodecInfo* p = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, p);
    for (UINT j = 0; j < num; ++j)
        if (wcscmp(p[j].MimeType, mime) == 0) { *clsid = p[j].Clsid; return (int)j; }
    return -1;
}

// 경로 확장자로 JPEG 여부 판정.
bool isJpegPath(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext == L"jpg" || ext == L"jpeg";
}

// ★ EXIF Orientation 적용(2026-09-05): 폰 사진은 픽셀이 눕혀진 채 + "세워서 보여라" 태그. GDI+ 는 무시하므로
//   여기서 태그대로 돌리고 태그를 지운다 → 변환/리사이즈 결과가 "사람이 보는 방향"의 픽셀이 된다(신경망 nnLoad 와 동일 규칙).
void imgApplyExif(Gdiplus::Bitmap& bmp) {
    UINT sz = bmp.GetPropertyItemSize(PropertyTagOrientation);
    if (sz == 0) return;
    std::vector<unsigned char> buf(sz);
    Gdiplus::PropertyItem* pi = (Gdiplus::PropertyItem*)buf.data();
    if (bmp.GetPropertyItem(PropertyTagOrientation, sz, pi) != Gdiplus::Ok) return;
    if (pi->type != PropertyTagTypeShort || !pi->value) return;
    unsigned short o = *(unsigned short*)pi->value;
    Gdiplus::RotateFlipType rf = Gdiplus::RotateNoneFlipNone; bool apply = true;
    switch (o) {
        case 2: rf = Gdiplus::RotateNoneFlipX; break;   case 3: rf = Gdiplus::Rotate180FlipNone; break;
        case 4: rf = Gdiplus::RotateNoneFlipY; break;   case 5: rf = Gdiplus::Rotate90FlipX; break;
        case 6: rf = Gdiplus::Rotate90FlipNone; break;  case 7: rf = Gdiplus::Rotate270FlipX; break;
        case 8: rf = Gdiplus::Rotate270FlipNone; break; default: apply = false; break;
    }
    if (apply) { bmp.RotateFlip(rf); bmp.RemovePropertyItem(PropertyTagOrientation); }
}

// bmp 를 outPath 로 저장(확장자로 JPEG/PNG 판정, JPEG 은 quality 적용). 성공 true.
bool saveBitmap(Gdiplus::Bitmap& bmp, const std::wstring& outPath, int quality) {
    if (quality < 0) quality = 85;
    if (quality > 100) quality = 100;

    bool jpeg = isJpegPath(outPath);
    CLSID clsid;
    if (imgGetEncoderClsid(jpeg ? L"image/jpeg" : L"image/png", &clsid) < 0)
        return false;

    if (jpeg) {
        Gdiplus::EncoderParameters params;
        params.Count = 1;
        params.Parameter[0].Guid = Gdiplus::EncoderQuality;
        params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        ULONG q = (ULONG)quality;
        params.Parameter[0].Value = &q;
        return bmp.Save(outPath.c_str(), &clsid, &params) == Gdiplus::Ok;
    }
    return bmp.Save(outPath.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

} // anonymous

// ============================================================================
//  Exports (세트 O)
// ============================================================================
extern "C" {

// 이미지 크기(픽셀)를 "WxH" 로 반환하는 대신, 폭/높이를 각각 얻는 정수 export 2종.
//   실패 시 -1.
__declspec(dllexport) int __stdcall Notepad_ImageWidth(const wchar_t* inPath) {
    imgEnsureGdiplus();
    std::wstring wp = inPath ? inPath : L"";
    if (wp.empty()) return -1;
    Gdiplus::Bitmap bmp(wp.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return -1;
    imgApplyExif(bmp);
    return (int)bmp.GetWidth();
}
__declspec(dllexport) int __stdcall Notepad_ImageHeight(const wchar_t* inPath) {
    imgEnsureGdiplus();
    std::wstring wp = inPath ? inPath : L"";
    if (wp.empty()) return -1;
    Gdiplus::Bitmap bmp(wp.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return -1;
    imgApplyExif(bmp);
    return (int)bmp.GetHeight();
}

// 비율 유지 리사이즈 + 저장. maxW/maxH 안에 들어가도록 축소(확대는 안 함).
//   maxW<=0 또는 maxH<=0 이면 해당 축 제한 없음. quality: JPEG 품질(0~100).
//   출력 포맷은 outPath 확장자(.jpg/.jpeg=JPEG, 그 외=PNG). 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_ImageResize(const wchar_t* inPath,
                                                        const wchar_t* outPath,
                                                        int maxW, int maxH, int quality) {
    imgEnsureGdiplus();
    std::wstring wi = inPath ? inPath : L"", wo = outPath ? outPath : L"";
    if (wi.empty() || wo.empty()) return 0;

    Gdiplus::Bitmap src(wi.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) return 0;
    imgApplyExif(src);
    int ow = (int)src.GetWidth(), oh = (int)src.GetHeight();
    if (ow <= 0 || oh <= 0) return 0;

    // 목표 크기 계산(비율 유지, 축소만).
    double scale = 1.0;
    if (maxW > 0) scale = min(scale, (double)maxW / ow);
    if (maxH > 0) scale = min(scale, (double)maxH / oh);
    if (scale > 1.0) scale = 1.0;                     // 확대 금지
    int nw = (int)(ow * scale + 0.5), nh = (int)(oh * scale + 0.5);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    Gdiplus::Bitmap dst(nw, nh, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&dst);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.DrawImage(&src, 0, 0, nw, nh);
    }
    return saveBitmap(dst, wo, quality) ? 1 : 0;
}

// ★ 임의 각도 회전 + 저장. angleDeg: 시계방향(+)/반시계(-) 도 단위(소수 가능).
//   keepSize=1: 원본 크기 유지(중심 회전, 모서리 잘림 / 빈 곳은 bgRgb) — 기울어진 차대번호 보정용.
//   keepSize=0: 캔버스 확장(잘림 없음). bgRgb: 0xRRGGBB. 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_ImageRotate(const wchar_t* inPath, const wchar_t* outPath,
                                                        double angleDeg, int keepSize, int bgRgb, int quality) {
    imgEnsureGdiplus();
    std::wstring wi = inPath ? inPath : L"", wo = outPath ? outPath : L"";
    if (wi.empty() || wo.empty()) return 0;
    Gdiplus::Bitmap src(wi.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) return 0;
    int ow = (int)src.GetWidth(), oh = (int)src.GetHeight();
    if (ow <= 0 || oh <= 0) return 0;
    double rad = angleDeg * 3.14159265358979 / 180.0, c = fabs(cos(rad)), s = fabs(sin(rad));
    int nw = keepSize ? ow : (int)(ow * c + oh * s + 0.5), nh = keepSize ? oh : (int)(ow * s + oh * c + 0.5);
    Gdiplus::Bitmap dst(nw, nh, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&dst);
        g.Clear(Gdiplus::Color(255, (bgRgb >> 16) & 255, (bgRgb >> 8) & 255, bgRgb & 255));
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.TranslateTransform(nw / 2.0f, nh / 2.0f);
        g.RotateTransform((float)angleDeg);
        g.TranslateTransform(-ow / 2.0f, -oh / 2.0f);
        g.DrawImage(&src, 0, 0, ow, oh);
    }
    return saveBitmap(dst, wo, quality) ? 1 : 0;
}

// ★ 회전 보정 캡처: 화면의 박스(x,y,w,h)를 중심 기준 angleDeg 만큼 돌려서 w×h 로 잘라 저장.
//   기울어진 차대번호를 드래그한 뒤, 각도를 맞춰 "똑바로 선" 학습 이미지를 만들기 위함.
//   내부: 박스 대각선 크기의 정사각을 캡처 → 회전 → 중앙 w×h 크롭(모서리 빈 곳 없음).
//   angleDeg 시계방향(+). 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_CaptureRegionRotated(int x, int y, int w, int h,
                                                                 double angleDeg, const wchar_t* outPath, int quality) {
    imgEnsureGdiplus();
    std::wstring wo = outPath ? outPath : L"";
    if (wo.empty() || w <= 0 || h <= 0) return 0;
    int R = (int)(sqrt((double)w * w + (double)h * h) + 0.5) + 4;   // 대각선 + 여유
    if (R % 2) ++R;
    int cx = x + w / 2, cy = y + h / 2;
    std::vector<unsigned char> px;
    if (!notepad::captureBGRA(cx - R / 2, cy - R / 2, R, R, px) || (int)px.size() < R * R * 4) return 0;
    Gdiplus::Bitmap src(R, R, R * 4, PixelFormat32bppRGB, px.data());
    Gdiplus::Bitmap dst(w, h, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&dst);
        g.Clear(Gdiplus::Color(255, 0, 0, 0));
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.TranslateTransform(w / 2.0f, h / 2.0f);
        g.RotateTransform((float)angleDeg);
        g.TranslateTransform(-R / 2.0f, -R / 2.0f);
        g.DrawImage(&src, 0, 0, R, R);
    }
    return saveBitmap(dst, wo, quality) ? 1 : 0;
}

// 크기 유지, 포맷/품질만 바꿔 저장(경량화 핵심). 예: PNG -> JPEG(품질75).
//   quality: JPEG 품질(0~100). 출력 포맷은 outPath 확장자. 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_ImageConvert(const wchar_t* inPath,
                                                         const wchar_t* outPath,
                                                         int quality) {
    imgEnsureGdiplus();
    std::wstring wi = inPath ? inPath : L"", wo = outPath ? outPath : L"";
    if (wi.empty() || wo.empty()) return 0;
    Gdiplus::Bitmap src(wi.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) return 0;
    imgApplyExif(src);
    return saveBitmap(src, wo, quality) ? 1 : 0;
}

} // extern "C"

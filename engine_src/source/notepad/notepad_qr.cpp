// ============================================================================
//  NOTEPAD.dll  -  notepad_qr.cpp
//  [NOTEPAD] 세트 N: QR 코드 생성 (Nayuki qrcodegen, MIT) + GDI+ PNG 저장.
//  링크: gdiplus.lib (build.bat 에 이미 포함). qrcodegen.c 함께 컴파일.
//  의존성 0: 외부 런타임 없음.
// ============================================================================
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdint>

extern "C" {
#include "qrcodegen.h"
}

#pragma comment(lib, "gdiplus.lib")

namespace {

std::string qr_wToUtf8(const wchar_t* w) {
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
// GDI+ 지연 초기화 (notepad_screen.cpp 와 독립적으로 안전 — 중복 startup 은 무해).
ULONG_PTR g_qrGpToken = 0;
bool g_qrGpUp = false;
void qrEnsureGdiplus() {
    if (g_qrGpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_qrGpToken, &in, nullptr) == Gdiplus::Ok)
        g_qrGpUp = true;
}

int qrGetEncoderClsid(const wchar_t* mime, CLSID* clsid) {
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

qrcodegen_Ecc eccFromInt(int level) {
    switch (level) {
        case 1:  return qrcodegen_Ecc_MEDIUM;
        case 2:  return qrcodegen_Ecc_QUARTILE;
        case 3:  return qrcodegen_Ecc_HIGH;
        default: return qrcodegen_Ecc_LOW;
    }
}

} // anonymous

// ============================================================================
//  Export (세트 N)
// ============================================================================
extern "C" {

// QR 코드를 PNG 로 저장.
//   text: 인코딩할 문자열(UTF-8). outPngPath: 저장 경로.
//   scale: 모듈 1개당 픽셀(예 8). border: 여백 모듈 수(권장 4).
//   ecc: 0=L(7%) 1=M(15%) 2=Q(25%) 3=H(30%).
//   반환: 성공 1 / 실패 0.
__declspec(dllexport) int __stdcall Notepad_QrEncode(const wchar_t* text,
                                                     const wchar_t* outPngPath,
                                                     int scale, int border, int ecc) {
    std::string data = qr_wToUtf8(text);
    if (scale <= 0) scale = 8;
    if (border < 0) border = 4;

    std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_MAX);
    std::vector<uint8_t> temp(qrcodegen_BUFFER_LEN_MAX);

    bool ok = qrcodegen_encodeText(data.c_str(), temp.data(), qr.data(),
                                   eccFromInt(ecc),
                                   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) return 0;

    int size = qrcodegen_getSize(qr.data());          // 모듈 수(가로=세로)
    if (size <= 0) return 0;
    int imgDim = (size + border * 2) * scale;         // 최종 픽셀 크기
    if (imgDim <= 0 || imgDim > 20000) return 0;      // 안전 상한

    qrEnsureGdiplus();
    Gdiplus::Bitmap bmp(imgDim, imgDim, PixelFormat32bppRGB);
    Gdiplus::Rect rc(0, 0, imgDim, imgDim);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite,
                     PixelFormat32bppRGB, &bd) != Gdiplus::Ok) return 0;

    // 픽셀 채우기: 흰 배경(0xFFFFFFFF), 검은 모듈(0xFF000000). BGRA 순.
    for (int py = 0; py < imgDim; ++py) {
        unsigned char* rowp = (unsigned char*)bd.Scan0 + (size_t)py * bd.Stride;
        int my = py / scale - border;                 // 모듈 좌표
        for (int px = 0; px < imgDim; ++px) {
            int mx = px / scale - border;
            bool dark = false;
            if (mx >= 0 && mx < size && my >= 0 && my < size)
                dark = qrcodegen_getModule(qr.data(), mx, my);
            unsigned char v = dark ? 0x00 : 0xFF;
            unsigned char* p = rowp + (size_t)px * 4;
            p[0] = v; p[1] = v; p[2] = v; p[3] = 0xFF; // B G R A
        }
    }
    bmp.UnlockBits(&bd);

    CLSID clsid;
    if (qrGetEncoderClsid(L"image/png", &clsid) < 0) return 0;
    std::wstring wpath = outPngPath ? std::wstring(outPngPath) : std::wstring();
    if (wpath.empty()) return 0;
    return bmp.Save(wpath.c_str(), &clsid, nullptr) == Gdiplus::Ok ? 1 : 0;
}

} // extern "C"

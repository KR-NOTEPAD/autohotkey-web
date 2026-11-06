// ============================================================================
//  NOTEPAD.dll  -  notepad_screen.cpp
//  [NOTEPAD] 화면 캡처 / 이미지서치 / 픽셀서치 (GDI 캡처 + GDI+ 이미지 I/O).
//  링크: gdi32.lib  user32.lib  gdiplus.lib
// ============================================================================
#include "notepad_crypto.h"
#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <cstring>
#include <cwctype>
#include <cstdlib>

#pragma comment(lib, "gdiplus.lib")

namespace notepad {

// UTF-8 -> wide
static std::wstring s_pathToW(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], n);
    return w;
}

// GDI+ 지연 초기화 (AHK 단일 스레드 가정)
static ULONG_PTR g_gpToken = 0;
static bool g_gpUp = false;
static void ensureGdiplus() {
    if (g_gpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_gpToken, &in, nullptr) == Gdiplus::Ok)
        g_gpUp = true;
}

static int getEncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<unsigned char> buf(size);
    Gdiplus::ImageCodecInfo* p = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, p);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(p[j].MimeType, mime) == 0) { *clsid = p[j].Clsid; return (int)j; }
    }
    return -1;
}

// 화면 영역을 32bpp BGRA(top-down) 바이트로 캡처. (다른 소스에서 재사용: non-static)
bool captureBGRA(int x, int y, int w, int h, std::vector<unsigned char>& out) {
    if (w <= 0 || h <= 0) return false;
    HDC hScreen = GetDC(nullptr);
    if (!hScreen) return false;
    HDC hMem = CreateCompatibleDC(hScreen);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hMem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (hBmp && bits) {
        HGDIOBJ oldObj = SelectObject(hMem, hBmp);
        if (BitBlt(hMem, 0, 0, w, h, hScreen, x, y, SRCCOPY)) {
            out.assign((unsigned char*)bits,
                       (unsigned char*)bits + (size_t)w * h * 4);
            ok = true;
        }
        SelectObject(hMem, oldObj);
    }
    if (hBmp) DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
    return ok;
}

// 이미지 파일 -> 32bpp BGRA + w,h
static bool loadImageBGRA(const std::wstring& wpath,
                          std::vector<unsigned char>& out, int& w, int& h) {
    ensureGdiplus();
    Gdiplus::Bitmap bmp(wpath.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    w = (int)bmp.GetWidth();
    h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;
    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead,
                     PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    out.resize((size_t)w * h * 4);
    for (int row = 0; row < h; ++row)
        memcpy(out.data() + (size_t)row * w * 4,
               (unsigned char*)bd.Scan0 + (size_t)row * bd.Stride, (size_t)w * 4);
    bmp.UnlockBits(&bd);
    return true;
}

bool getPixelColor(int x, int y, uint32_t& rgbOut) {
    HDC hScreen = GetDC(nullptr);
    if (!hScreen) return false;
    COLORREF c = GetPixel(hScreen, x, y);
    ReleaseDC(nullptr, hScreen);
    if (c == CLR_INVALID) return false;
    rgbOut = ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8)
           | (uint32_t)GetBValue(c);
    return true;
}

bool captureRegionToPng(int x, int y, int w, int h, const std::string& outPngUtf8) {
    std::vector<unsigned char> bgra;
    if (!captureBGRA(x, y, w, h, bgra)) return false;
    ensureGdiplus();
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppRGB);
    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite,
                     PixelFormat32bppRGB, &bd) != Gdiplus::Ok) return false;
    for (int row = 0; row < h; ++row)
        memcpy((unsigned char*)bd.Scan0 + (size_t)row * bd.Stride,
               bgra.data() + (size_t)row * w * 4, (size_t)w * 4);
    bmp.UnlockBits(&bd);
    CLSID clsid;
    if (getEncoderClsid(L"image/png", &clsid) < 0) return false;
    std::wstring wpath = s_pathToW(outPngUtf8);
    if (wpath.empty()) return false;
    return bmp.Save(wpath.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

// ----------------------------------------------------------------------------
//  [세트 AH] 드래그 캡처 -> 카테고리 폴더에 자동번호 저장 (학습샘플 수집)
//  saveDir(=카테고리 폴더) 안을 훑어 가장 큰 <n> 을 찾아 <prefix><n+1>.<ext> 로 저장.
//  selectRegionModal(overlay.cpp) + captureRegionToPng(위) 조합. 저장경로 반환.
// ----------------------------------------------------------------------------
// selectRegionModal 은 같은 notepad 네임스페이스(notepad_overlay.cpp)에 정의됨.
// 여기서는 그 선언만 앞세운다(이미 notepad 네임스페이스 안이므로 이름만).
bool selectRegionModal(int& ox, int& oy, int& ow, int& oh);

// saveDir 이 없으면 (중간 경로 포함) 만든다. 성공 true.
static bool ensureDirW(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    // 상위부터 순차 생성
    std::wstring cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') {
            if (!cur.empty() && !(cur.size() == 2 && cur[1] == L':'))
                CreateDirectoryW(cur.c_str(), nullptr);
            if (i < dir.size()) cur.push_back(L'\\');
        } else {
            cur.push_back(dir[i]);
        }
    }
    attr = GetFileAttributesW(dir.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

// saveDir 에서 "<prefix><숫자>.<ext>" 형태의 가장 큰 숫자를 찾아 반환(없으면 0).
static int maxIndexInDir(const std::wstring& dir, const std::wstring& prefix,
                         const std::wstring& ext) {
    std::wstring pat = dir + L"\\" + prefix + L"*." + ext;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    int mx = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring name = fd.cFileName;
        // prefix 와 .ext 사이의 숫자만 취함
        size_t p = prefix.size();
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos || dot < p) continue;
        std::wstring mid = name.substr(p, dot - p);
        if (mid.empty()) continue;
        bool allDigit = true;
        for (wchar_t c : mid) if (c < L'0' || c > L'9') { allDigit = false; break; }
        if (!allDigit) continue;
        int v = _wtoi(mid.c_str());
        if (v > mx) mx = v;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return mx;
}

// 드래그로 영역 선택 후 saveDir 에 <prefix><다음번호>.<ext> 로 저장.
//   ext: "png" 또는 "jpg". quality: JPEG 품질(png 은 무시). 취소/실패 시 "".
//   성공 시 저장한 전체 파일경로(UTF-8) 를 out 에 채운다.
bool captureRegionToFolder(const std::string& saveDirUtf8, const std::string& prefixUtf8,
                           const std::string& extUtf8, int quality, std::string& outPathUtf8) {
    int x, y, w, h;
    if (!selectRegionModal(x, y, w, h)) return false;   // 취소/빈드래그(같은 네임스페이스)
    if (w <= 0 || h <= 0) return false;

    std::wstring wdir = s_pathToW(saveDirUtf8);
    if (wdir.empty()) return false;
    // 끝의 슬래시 제거
    while (!wdir.empty() && (wdir.back() == L'\\' || wdir.back() == L'/')) wdir.pop_back();
    if (!ensureDirW(wdir)) return false;

    std::wstring wprefix = s_pathToW(prefixUtf8);
    std::wstring wext = s_pathToW(extUtf8);
    if (wext.empty()) wext = L"png";
    for (auto& c : wext) c = (wchar_t)towlower(c);
    if (wext == L"jpeg") wext = L"jpg";
    if (wext != L"png" && wext != L"jpg") wext = L"png";

    int next = maxIndexInDir(wdir, wprefix, wext) + 1;
    wchar_t num[16]; wsprintfW(num, L"%d", next);
    std::wstring wpath = wdir + L"\\" + wprefix + num + L"." + wext;

    // 이미 존재하면 다음 번호로 (동시성/이상 파일 대비, 최대 몇 회 시도)
    for (int guard = 0; guard < 10000 &&
         GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES; ++guard) {
        ++next;
        wsprintfW(num, L"%d", next);
        wpath = wdir + L"\\" + wprefix + num + L"." + wext;
    }

    // UTF-8 경로로 변환해 captureRegionToPng 로 저장. jpg 는 png 저장 후 변환은
    // 과하므로, 여기서는 png 로 통일(학습엔 png 가 무손실이라 유리). jpg 요청 시엔
    // captureRegionToPng 가 확장자 무관하게 PNG 인코딩하므로, 경로만 .jpg 로 두면
    // 확장자와 실제포맷 불일치가 생긴다 -> jpg 요청은 png 로 강제(권장).
    (void)quality;
    if (wext == L"jpg") {
        // 확장자 불일치 방지: png 로 저장
        wpath = wdir + L"\\" + wprefix + num + L".png";
    }

    // wpath -> UTF-8
    int n8 = WideCharToMultiByte(CP_UTF8, 0, wpath.data(), (int)wpath.size(), nullptr, 0, nullptr, nullptr);
    std::string savePathU8((size_t)(n8 > 0 ? n8 : 0), '\0');
    if (n8 > 0)
        WideCharToMultiByte(CP_UTF8, 0, wpath.data(), (int)wpath.size(), &savePathU8[0], n8, nullptr, nullptr);

    if (!captureRegionToPng(x, y, w, h, savePathU8)) return false;
    outPathUtf8 = savePathU8;
    return true;
}

static inline int iabs(int v) { return v < 0 ? -v : v; }

bool imageSearch(const std::string& needlePathUtf8, int x1, int y1, int x2, int y2,
                 int tolerance, int& fx, int& fy) {
    if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }
    int rw = x2 - x1, rh = y2 - y1;
    if (rw <= 0 || rh <= 0) return false;
    if (tolerance < 0) tolerance = 0;

    std::vector<unsigned char> nb; int nw = 0, nh = 0;
    if (!loadImageBGRA(s_pathToW(needlePathUtf8), nb, nw, nh)) return false;
    if (nw <= 0 || nh <= 0 || nw > rw || nh > rh) return false;

    std::vector<unsigned char> rb;
    if (!captureBGRA(x1, y1, rw, rh, rb)) return false;

    for (int sy = 0; sy <= rh - nh; ++sy) {
        for (int sx = 0; sx <= rw - nw; ++sx) {
            bool match = true;
            for (int ny = 0; ny < nh && match; ++ny) {
                const unsigned char* rrow = rb.data() + (size_t)(sy + ny) * rw * 4 + (size_t)sx * 4;
                const unsigned char* nrow = nb.data() + (size_t)ny * nw * 4;
                for (int nx = 0; nx < nw; ++nx) {
                    const unsigned char* rp = rrow + (size_t)nx * 4;
                    const unsigned char* np = nrow + (size_t)nx * 4;
                    if (iabs((int)rp[0] - np[0]) > tolerance ||
                        iabs((int)rp[1] - np[1]) > tolerance ||
                        iabs((int)rp[2] - np[2]) > tolerance) { match = false; break; }
                }
            }
            if (match) { fx = x1 + sx; fy = y1 + sy; return true; }
        }
    }
    return false;
}

// [세트 K] 이미지서치(모두 찾기). 겹치는 매치는 needle 폭만큼 건너뛰어 억제.
int imageSearchAll(const std::string& needlePathUtf8, int x1, int y1, int x2, int y2,
                   int tolerance, int maxMatches,
                   std::vector<int>& fxs, std::vector<int>& fys) {
    if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }
    int rw = x2 - x1, rh = y2 - y1;
    if (rw <= 0 || rh <= 0) return -1;
    if (tolerance < 0) tolerance = 0;

    std::vector<unsigned char> nb; int nw = 0, nh = 0;
    if (!loadImageBGRA(s_pathToW(needlePathUtf8), nb, nw, nh)) return -1;
    if (nw <= 0 || nh <= 0 || nw > rw || nh > rh) return -1;

    std::vector<unsigned char> rb;
    if (!captureBGRA(x1, y1, rw, rh, rb)) return -1;

    // 이미 매치한 영역을 표시해 겹침 중복 방지(간단 마스크).
    std::vector<unsigned char> used((size_t)rw * rh, 0);
    int found = 0;

    for (int sy = 0; sy <= rh - nh; ++sy) {
        for (int sx = 0; sx <= rw - nw; ++sx) {
            if (used[(size_t)sy * rw + sx]) continue;
            bool match = true;
            for (int ny = 0; ny < nh && match; ++ny) {
                const unsigned char* rrow = rb.data() + (size_t)(sy + ny) * rw * 4 + (size_t)sx * 4;
                const unsigned char* nrow = nb.data() + (size_t)ny * nw * 4;
                for (int nx = 0; nx < nw; ++nx) {
                    const unsigned char* rp = rrow + (size_t)nx * 4;
                    const unsigned char* np = nrow + (size_t)nx * 4;
                    if (iabs((int)rp[0] - np[0]) > tolerance ||
                        iabs((int)rp[1] - np[1]) > tolerance ||
                        iabs((int)rp[2] - np[2]) > tolerance) { match = false; break; }
                }
            }
            if (match) {
                fxs.push_back(x1 + sx);
                fys.push_back(y1 + sy);
                found++;
                // 매치 사각형 전체를 used 로 표시 -> 겹치는 인접 매치 억제.
                for (int my = sy; my < sy + nh && my < rh; ++my)
                    for (int mx = sx; mx < sx + nw && mx < rw; ++mx)
                        used[(size_t)my * rw + mx] = 1;
                if (maxMatches > 0 && found >= maxMatches) return found;
            }
        }
    }
    return found;
}

bool pixelSearch(int x1, int y1, int x2, int y2, uint32_t rgb, int tolerance,
                 int& fx, int& fy) {
    if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }
    int rw = x2 - x1, rh = y2 - y1;
    if (rw <= 0 || rh <= 0) return false;
    if (tolerance < 0) tolerance = 0;
    int R = (rgb >> 16) & 0xFF, G = (rgb >> 8) & 0xFF, B = rgb & 0xFF;

    std::vector<unsigned char> rb;
    if (!captureBGRA(x1, y1, rw, rh, rb)) return false;
    for (int yy = 0; yy < rh; ++yy) {
        for (int xx = 0; xx < rw; ++xx) {
            const unsigned char* p = rb.data() + ((size_t)yy * rw + xx) * 4;
            if (iabs((int)p[2] - R) <= tolerance &&
                iabs((int)p[1] - G) <= tolerance &&
                iabs((int)p[0] - B) <= tolerance) {
                fx = x1 + xx; fy = y1 + yy; return true;
            }
        }
    }
    return false;
}

} // namespace notepad

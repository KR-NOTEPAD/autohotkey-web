// ============================================================================
//  NOTEPAD.dll  -  notepad_extra.cpp
//  [NOTEPAD] 세트 AF: AHK v1 실전 빈칸 채우기 (의존성 0, 기존 기능과 중복 없음).
//  링크: user32 gdi32 kernel32 (이미 포함).
//
//  블로그 철학대로 "AHK v1 에서 아쉬웠던 것"을 네이티브로 채운다. 기존 세트와
//  겹치지 않는 것만 선별:
//   * 창 제어 확장 - 다른 창을 항상위/투명/클릭통과/이동/크기. (AHK WinSet 보완,
//     특히 다른 프로세스 창을 hwnd 로 직접 다루는 실전 케이스)
//   * 고해상도 타이머 - QueryPerformanceCounter 로 마이크로초 측정. AHK 는
//     A_TickCount(ms) 뿐이라 정밀 성능측정/프레임타이밍에 부족했다.
//   * 색상 변환/거리 - RGB<->HSV, 두 색 거리. 이미지서치 tolerance 계산·색판정에.
//   * INI 파일 - 구조적 설정 읽기/쓰기(유니코드). 자동화 설정 저장에 흔히 필요.
//   * 텍스트 인코딩 감지/변환 - UTF-8/UTF-16/CP949(한글) + BOM. 한글 파일 처리 핵심.
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

namespace notepad {

static std::wstring exU8ToW(const std::string& s, UINT cp = CP_UTF8) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string exWToU8(const std::wstring& w, UINT cp = CP_UTF8) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ============================================================================
//  창 제어 확장 (hwnd 대상)
// ============================================================================
// 항상 위 설정/해제.
int windowSetTopmost(HWND hwnd, int on) {
    if (!IsWindow(hwnd)) return 0;
    return SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) ? 1 : 0;
}
// 창 투명도(0=투명 ~ 255=불투명).
int windowSetOpacity(HWND hwnd, int alpha) {
    if (!IsWindow(hwnd)) return 0;
    if (alpha < 0) alpha = 0; if (alpha > 255) alpha = 255;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    return SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA) ? 1 : 0;
}
// 클릭 통과 설정/해제(마우스가 창을 통과해 아래로).
int windowSetClickThrough(HWND hwnd, int on) {
    if (!IsWindow(hwnd)) return 0;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (on) ex |= (WS_EX_LAYERED | WS_EX_TRANSPARENT);
    else ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
    return 1;
}
// 창 위치/크기 얻기 -> "x,y,w,h".
bool windowGetRect(HWND hwnd, std::wstring& out) {
    RECT r; if (!IsWindow(hwnd) || !GetWindowRect(hwnd, &r)) return false;
    wchar_t buf[80]; wsprintfW(buf, L"%d,%d,%d,%d", r.left, r.top, r.right - r.left, r.bottom - r.top);
    out = buf; return true;
}
// 창 이동/크기(둘 다). w<0 또는 h<0 이면 크기 유지.
int windowMove(HWND hwnd, int x, int y, int w, int h) {
    if (!IsWindow(hwnd)) return 0;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (w < 0 || h < 0) { flags |= SWP_NOSIZE; w = 0; h = 0; }
    return SetWindowPos(hwnd, nullptr, x, y, w, h, flags) ? 1 : 0;
}
// 마우스 커서 아래의 창 hwnd(그 지점 최상위 창).
HWND windowFromCursor() {
    POINT pt; GetCursorPos(&pt);
    HWND h = WindowFromPoint(pt);
    if (h) { HWND root = GetAncestor(h, GA_ROOT); if (root) return root; }
    return h;
}

// ============================================================================
//  고해상도 타이머 (QueryPerformanceCounter)
// ============================================================================
// 현재 카운터 값(틱). 두 시점 차이를 주파수로 나누면 초.
long long perfCounter() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart;
}
long long perfFrequency() {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart;
}
// 두 카운터 사이 경과 마이크로초.
long long perfElapsedUs(long long start, long long end) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    if (f.QuadPart == 0) return 0;
    return (long long)((double)(end - start) * 1000000.0 / f.QuadPart);
}

// ============================================================================
//  색상 변환 / 거리 (이미지서치·색판정 보조)
// ============================================================================
// RGB(0xRRGGBB) -> HSV. H 0~359, S 0~100, V 0~100. -> "h,s,v"
void rgbToHsv(uint32_t rgb, int& h, int& s, int& v) {
    double r = ((rgb >> 16) & 0xFF) / 255.0, g = ((rgb >> 8) & 0xFF) / 255.0, b = (rgb & 0xFF) / 255.0;
    double mx = std::max({ r,g,b }), mn = std::min({ r,g,b }), d = mx - mn;
    double hh = 0;
    if (d > 1e-9) {
        if (mx == r) hh = 60 * fmod((g - b) / d, 6);
        else if (mx == g) hh = 60 * ((b - r) / d + 2);
        else hh = 60 * ((r - g) / d + 4);
    }
    if (hh < 0) hh += 360;
    h = (int)(hh + 0.5); s = (int)(mx > 0 ? d / mx * 100 + 0.5 : 0); v = (int)(mx * 100 + 0.5);
    if (h >= 360) h -= 360;
}
// HSV -> RGB(0xRRGGBB).
uint32_t hsvToRgb(int h, int s, int v) {
    double S = s / 100.0, V = v / 100.0;
    double C = V * S, X = C * (1 - fabs(fmod(h / 60.0, 2) - 1)), m = V - C;
    double r = 0, g = 0, b = 0;
    if (h < 60) { r = C; g = X; }
    else if (h < 120) { r = X; g = C; }
    else if (h < 180) { g = C; b = X; }
    else if (h < 240) { g = X; b = C; }
    else if (h < 300) { r = X; b = C; }
    else { r = C; b = X; }
    int R = (int)((r + m) * 255 + 0.5), G = (int)((g + m) * 255 + 0.5), B = (int)((b + m) * 255 + 0.5);
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}
// 두 색의 유클리드 거리(0~441). 이미지서치 tolerance 감 잡기용.
int colorDistance(uint32_t a, uint32_t b) {
    int dr = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF);
    int dg = (int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF);
    int db = (int)(a & 0xFF) - (int)(b & 0xFF);
    return (int)(std::sqrt((double)(dr * dr + dg * dg + db * db)) + 0.5);
}

// ============================================================================
//  INI 파일 (유니코드)
// ============================================================================
std::wstring iniRead(const std::wstring& path, const std::wstring& section,
                     const std::wstring& key, const std::wstring& def) {
    wchar_t buf[4096];
    DWORD n = GetPrivateProfileStringW(section.c_str(), key.c_str(), def.c_str(),
                                       buf, 4096, path.c_str());
    return std::wstring(buf, n);
}
int iniWrite(const std::wstring& path, const std::wstring& section,
             const std::wstring& key, const std::wstring& val) {
    return WritePrivateProfileStringW(section.c_str(), key.c_str(), val.c_str(), path.c_str()) ? 1 : 0;
}
// 섹션 삭제(key=null) 또는 키 삭제.
int iniDelete(const std::wstring& path, const std::wstring& section, const std::wstring& key) {
    return WritePrivateProfileStringW(section.c_str(), key.empty() ? nullptr : key.c_str(),
                                      nullptr, path.c_str()) ? 1 : 0;
}
// 섹션의 키 목록(개행 구분).
std::wstring iniKeys(const std::wstring& path, const std::wstring& section) {
    std::vector<wchar_t> buf(8192);
    DWORD n = GetPrivateProfileStringW(section.c_str(), nullptr, L"", buf.data(), (DWORD)buf.size(), path.c_str());
    std::wstring res;
    for (DWORD i = 0; i < n; ++i) { if (buf[i] == L'\0') { if (i + 1 < n && buf[i + 1] != L'\0') res += L'\n'; } else res += buf[i]; }
    return res;
}

// ============================================================================
//  텍스트 인코딩 감지 / 변환 (한글 파일 처리)
// ============================================================================
// 파일 인코딩 감지 -> "utf8bom" / "utf8" / "utf16le" / "utf16be" / "ansi".
std::wstring detectEncoding(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    unsigned char b[4] = { 0 }; DWORD rd = 0;
    ReadFile(h, b, 4, &rd, nullptr);
    // 나머지 바이트로 UTF-8 유효성 러프 판단
    std::vector<unsigned char> all;
    if (rd > 0) { all.assign(b, b + rd); std::vector<unsigned char> more(65536); DWORD r2 = 0; while (ReadFile(h, more.data(), (DWORD)more.size(), &r2, nullptr) && r2) all.insert(all.end(), more.begin(), more.begin() + r2); }
    CloseHandle(h);
    if (rd >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return L"utf8bom";
    if (rd >= 2 && b[0] == 0xFF && b[1] == 0xFE) return L"utf16le";
    if (rd >= 2 && b[0] == 0xFE && b[1] == 0xFF) return L"utf16be";
    // UTF-8 유효성 검사(멀티바이트 시퀀스)
    bool validUtf8 = true; bool hasMulti = false;
    for (size_t i = 0; i < all.size(); ) {
        unsigned char c = all[i];
        if (c < 0x80) { i++; continue; }
        int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 0;
        if (len == 0) { validUtf8 = false; break; }
        hasMulti = true;
        if (i + len > all.size()) { validUtf8 = false; break; }
        for (int k = 1; k < len; ++k) if ((all[i + k] & 0xC0) != 0x80) { validUtf8 = false; break; }
        if (!validUtf8) break;
        i += len;
    }
    if (validUtf8 && hasMulti) return L"utf8";
    return L"ansi"; // CP949 등(한글 완성형)
}
// 파일을 UTF-8(BOM 없음)로 변환 저장. 원본 인코딩 자동 감지. 성공 1.
int convertToUtf8(const std::wstring& inPath, const std::wstring& outPath) {
    std::wstring enc = detectEncoding(inPath);
    if (enc.empty()) return 0;
    // 읽기
    HANDLE h = CreateFileW(inPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::vector<unsigned char> data; std::vector<unsigned char> buf(65536); DWORD rd = 0;
    while (ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd) data.insert(data.end(), buf.begin(), buf.begin() + rd);
    CloseHandle(h);

    std::wstring wide;
    if (enc == L"utf16le" || enc == L"utf16be") {
        size_t off = 2; std::vector<unsigned char> d(data.begin() + (data.size() >= 2 ? 2 : 0), data.end());
        if (enc == L"utf16be") for (size_t i = 0; i + 1 < d.size(); i += 2) std::swap(d[i], d[i + 1]);
        wide.assign((wchar_t*)d.data(), d.size() / 2);
    } else if (enc == L"utf8bom") {
        std::string s((char*)data.data() + 3, data.size() - 3);
        wide = exU8ToW(s, CP_UTF8);
    } else if (enc == L"utf8") {
        std::string s((char*)data.data(), data.size());
        wide = exU8ToW(s, CP_UTF8);
    } else { // ansi/cp949
        std::string s((char*)data.data(), data.size());
        wide = exU8ToW(s, CP_ACP); // 시스템 기본(한국어 Windows 면 949)
    }
    std::string u8 = exWToU8(wide, CP_UTF8);
    HANDLE o = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o == INVALID_HANDLE_VALUE) return 0;
    DWORD wr = 0; WriteFile(o, u8.data(), (DWORD)u8.size(), &wr, nullptr);
    CloseHandle(o);
    return 1;
}

} // namespace notepad

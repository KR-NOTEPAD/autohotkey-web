// ============================================================================
//  NOTEPAD.dll  -  notepad_input.cpp
//  [NOTEPAD] 세트 AE-1: 입력/시스템 차별기능 (AHK v1 이 약하거나 없는 것들).
//  링크: user32.lib gdi32.lib gdiplus.lib shcore.lib (build.bat 에 추가 필요).
//  의존성 0.
//
//   * 저수준 전역 후킹 카운트/마지막 이벤트 - 백그라운드 마우스/키 이벤트 관찰.
//   * 멀티모니터 열거(JSON) + DPI 인식 - 고해상도 좌표 정확도.
//   * 클립보드 이미지 읽기/쓰기(PNG) - AHK v1 은 텍스트만 쉬움.
//   * 입력 시뮬레이션 - SendInput 정밀 마우스 이동/클릭/드래그/휠.
//   * 폴더 변경 감시(1회 대기) - AHK 는 폴링밖에 없음(ReadDirectoryChangesW).
// ============================================================================
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <sstream>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")

namespace notepad {

static ULONG_PTR g_inGpToken = 0; static bool g_inGpUp = false;
static void inEnsureGdiplus() {
    if (g_inGpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_inGpToken, &in, nullptr) == Gdiplus::Ok) g_inGpUp = true;
}
static int inEncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0; Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<unsigned char> buf(size);
    auto* p = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, p);
    for (UINT j = 0; j < num; ++j) if (wcscmp(p[j].MimeType, mime) == 0) { *clsid = p[j].Clsid; return (int)j; }
    return -1;
}

// ============================================================================
//  DPI 인식 강제 + 멀티모니터 열거
// ============================================================================
// 프로세스 DPI 인식을 Per-Monitor V2 로 설정(좌표 왜곡 방지). 성공 1.
//   Win10 1703+ 의 SetProcessDpiAwarenessContext 를 동적 로드(구버전 안전).
int setDpiAwareV2() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef BOOL(WINAPI* FnCtx)(HANDLE);
        auto fn = (FnCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (fn && fn((HANDLE)-4)) return 1; // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }
    // 폴백: 구형 SetProcessDPIAware
    return SetProcessDPIAware() ? 1 : 0;
}

struct MonEnum { std::wstringstream ss; int idx = 0; };
static BOOL CALLBACK monProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    MonEnum* e = (MonEnum*)lp;
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
    if (e->idx) e->ss << L",";
    RECT& r = mi.rcMonitor;
    e->ss << L"{\"index\":" << e->idx
          << L",\"x\":" << r.left << L",\"y\":" << r.top
          << L",\"w\":" << (r.right - r.left) << L",\"h\":" << (r.bottom - r.top)
          << L",\"primary\":" << ((mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0) << L"}";
    e->idx++;
    return TRUE;
}
// 모든 모니터를 JSON 배열로. [{"index":0,"x":..,"y":..,"w":..,"h":..,"primary":1}, ...]
std::wstring enumMonitorsJson() {
    MonEnum e; e.ss << L"[";
    EnumDisplayMonitors(nullptr, nullptr, monProc, (LPARAM)&e);
    e.ss << L"]";
    return e.ss.str();
}

// ============================================================================
//  저수준 전역 후킹 (백그라운드 이벤트 카운트/마지막)
//  전용 스레드에서 WH_MOUSE_LL / WH_KEYBOARD_LL 설치, 메시지루프 유지.
//  Start -> 카운트 누적. GetStats -> "mouseCount,keyCount,lastVk,lastX,lastY".
//  Stop -> 후킹 해제.
// ============================================================================
static HANDLE g_hookThread = nullptr;
static DWORD g_hookThreadId = 0;
static volatile LONG g_mouseCount = 0, g_keyCount = 0;
static volatile LONG g_lastVk = 0, g_lastX = 0, g_lastY = 0;
static HHOOK g_hMouse = nullptr, g_hKey = nullptr;

static LRESULT CALLBACK llMouse(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lp;
        InterlockedIncrement(&g_mouseCount);
        InterlockedExchange(&g_lastX, m->pt.x);
        InterlockedExchange(&g_lastY, m->pt.y);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
static LRESULT CALLBACK llKey(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lp;
        InterlockedIncrement(&g_keyCount);
        InterlockedExchange(&g_lastVk, k->vkCode);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
static DWORD WINAPI hookThreadFn(LPVOID) {
    g_hMouse = SetWindowsHookExW(WH_MOUSE_LL, llMouse, GetModuleHandleW(nullptr), 0);
    g_hKey = SetWindowsHookExW(WH_KEYBOARD_LL, llKey, GetModuleHandleW(nullptr), 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { /* WM_QUIT 로 종료 */ }
    if (g_hMouse) { UnhookWindowsHookEx(g_hMouse); g_hMouse = nullptr; }
    if (g_hKey) { UnhookWindowsHookEx(g_hKey); g_hKey = nullptr; }
    return 0;
}
int hookStart() {
    if (g_hookThread) return 1;
    g_mouseCount = g_keyCount = g_lastVk = g_lastX = g_lastY = 0;
    g_hookThread = CreateThread(nullptr, 0, hookThreadFn, nullptr, 0, &g_hookThreadId);
    return g_hookThread ? 1 : 0;
}
int hookStop() {
    if (!g_hookThread) return 0;
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hookThread, 2000);
    CloseHandle(g_hookThread); g_hookThread = nullptr; g_hookThreadId = 0;
    return 1;
}
std::wstring hookStats() {
    wchar_t buf[128];
    wsprintfW(buf, L"%ld,%ld,%ld,%ld,%ld",
        (long)g_mouseCount, (long)g_keyCount, (long)g_lastVk, (long)g_lastX, (long)g_lastY);
    return buf;
}

// ============================================================================
//  클립보드 이미지 (PNG 파일 <-> 클립보드 비트맵)
// ============================================================================
// 클립보드의 이미지를 PNG 로 저장. 성공 1.
int clipboardImageToPng(const std::wstring& outPng) {
    if (!IsClipboardFormatAvailable(CF_DIB) && !IsClipboardFormatAvailable(CF_BITMAP)) return 0;
    if (!OpenClipboard(nullptr)) return 0;
    int ok = 0;
    HBITMAP hbmp = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (hbmp) {
        inEnsureGdiplus();
        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHBITMAP(hbmp, nullptr);
        if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
            CLSID clsid;
            if (inEncoderClsid(L"image/png", &clsid) >= 0 &&
                bmp->Save(outPng.c_str(), &clsid, nullptr) == Gdiplus::Ok) ok = 1;
        }
        if (bmp) delete bmp;
    }
    CloseClipboard();
    return ok;
}
// PNG 파일을 클립보드에 이미지로 넣기. 성공 1.
int pngToClipboardImage(const std::wstring& inPng) {
    inEnsureGdiplus();
    Gdiplus::Bitmap bmp(inPng.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return 0;
    HBITMAP hbmp = nullptr;
    if (bmp.GetHBITMAP(Gdiplus::Color(255, 255, 255), &hbmp) != Gdiplus::Ok || !hbmp) return 0;
    // HBITMAP -> DIB 로 변환해 클립보드에 (CF_BITMAP)
    int ok = 0;
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        // 클립보드 소유 비트맵 복제 필요 -> CopyImage
        HBITMAP copy = (HBITMAP)CopyImage(hbmp, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
        if (copy && SetClipboardData(CF_BITMAP, copy)) ok = 1;
        CloseClipboard();
    }
    DeleteObject(hbmp);
    return ok;
}

// ============================================================================
//  입력 시뮬레이션 (SendInput)
// ============================================================================
// 절대좌표 마우스 이동(가상화면 정규화). 성공 1.
int mouseMoveAbs(int x, int y) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return 0;
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = (LONG)((double)(x - vx) * 65535.0 / (vw - 1));
    in.mi.dy = (LONG)((double)(y - vy) * 65535.0 / (vh - 1));
    return SendInput(1, &in, sizeof(INPUT)) ? 1 : 0;
}
// 마우스 버튼: btn 0=좌 1=우 2=중, down 1=누름 0=뗌.
int mouseButton(int btn, int down) {
    INPUT in{}; in.type = INPUT_MOUSE;
    DWORD f = 0;
    if (btn == 0) f = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    else if (btn == 1) f = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    else f = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    in.mi.dwFlags = f;
    return SendInput(1, &in, sizeof(INPUT)) ? 1 : 0;
}
// 휠 스크롤(delta 양수=위). 성공 1.
int mouseWheel(int delta) {
    INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = delta;
    return SendInput(1, &in, sizeof(INPUT)) ? 1 : 0;
}
// 부드러운 드래그: (x1,y1)에서 누르고 steps 단계로 이동 후 (x2,y2)에서 뗌.
int mouseDragSmooth(int x1, int y1, int x2, int y2, int steps, int stepDelayMs) {
    if (steps < 1) steps = 20;
    mouseMoveAbs(x1, y1); Sleep(10); mouseButton(0, 1); Sleep(20);
    for (int i = 1; i <= steps; ++i) {
        int mx = x1 + (x2 - x1) * i / steps, my = y1 + (y2 - y1) * i / steps;
        mouseMoveAbs(mx, my); Sleep(stepDelayMs > 0 ? stepDelayMs : 8);
    }
    Sleep(20); mouseButton(0, 0);
    return 1;
}

// ============================================================================
//  폴더 변경 감시 (1회 대기 - 변경 발생 시 반환)
//  반환: "action\tpath" 개행구분 (action: added/removed/modified/renamed). 타임아웃 시 "".
// ============================================================================
std::wstring watchFolderOnce(const std::wstring& dir, int subtree, int timeoutMs) {
    HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::vector<unsigned char> buf(64 * 1024);
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
    std::wstring result;
    if (ReadDirectoryChangesW(h, buf.data(), (DWORD)buf.size(), subtree ? TRUE : FALSE,
                              filter, nullptr, &ov, nullptr)) {
        DWORD wr = WaitForSingleObject(ov.hEvent, timeoutMs > 0 ? timeoutMs : INFINITE);
        if (wr == WAIT_OBJECT_0) {
            DWORD bytes = 0;
            if (GetOverlappedResult(h, &ov, &bytes, FALSE) && bytes) {
                auto* info = (FILE_NOTIFY_INFORMATION*)buf.data();
                for (;;) {
                    std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
                    const wchar_t* act = L"modified";
                    switch (info->Action) {
                        case FILE_ACTION_ADDED: act = L"added"; break;
                        case FILE_ACTION_REMOVED: act = L"removed"; break;
                        case FILE_ACTION_MODIFIED: act = L"modified"; break;
                        case FILE_ACTION_RENAMED_NEW_NAME: act = L"renamed"; break;
                        case FILE_ACTION_RENAMED_OLD_NAME: act = L"renamed_old"; break;
                    }
                    if (!result.empty()) result += L"\n";
                    result += act; result += L"\t"; result += dir; result += L"\\"; result += name;
                    if (info->NextEntryOffset == 0) break;
                    info = (FILE_NOTIFY_INFORMATION*)((unsigned char*)info + info->NextEntryOffset);
                }
            }
        } else { CancelIo(h); }
    }
    CloseHandle(ov.hEvent); CloseHandle(h);
    return result;
}

} // namespace notepad

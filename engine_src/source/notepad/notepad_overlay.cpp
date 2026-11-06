// ============================================================================
//  NOTEPAD.dll  -  notepad_overlay.cpp
//  [NOTEPAD] 세트 AD: 화면 오버레이 / 영역선택 GUI (레이어드 윈도우, WinAPI).
//  링크: user32.lib gdi32.lib gdiplus.lib (build.bat 에 이미 포함). 의존성 0.
//
//  AHK v1 은 반투명 클릭통과 오버레이/드래그 영역선택을 GDI+DllCall 로 힘겹게
//  구현해야 한다. 이 세트가 그것을 export 로 제공:
//   * Notepad_SelectRegion  - 전체화면 어둡게 + 드래그로 사각형 선택 -> "x,y,w,h"
//   * Notepad_OverlayBox    - 화면에 강조 박스(테두리) 표시(비모달, id 로 갱신/제거)
//   * Notepad_OverlayText   - 화면에 텍스트 라벨 표시(탐지결과/좌표표시)
//   * Notepad_OverlayIcon   - 화면에 PNG 아이콘 표시(UI 아이콘 띄우기)
//   * Notepad_OverlayClear  - id(또는 전체) 오버레이 제거
//   * Notepad_ShowCrosshair - 현재 마우스에 십자선+좌표 HUD (지정 ms 동안)
//
//  오버레이는 클릭 통과(WS_EX_TRANSPARENT|LAYERED)라 아래 창 조작을 방해하지 않음.
//  좌표계는 세트 G 와 동일(가상 화면 픽셀). 멀티모니터 좌표 지원.
// ============================================================================
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <map>
#include <mutex>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace notepad {

static ULONG_PTR g_ovGpToken = 0; static bool g_ovGpUp = false;
static void ovEnsureGdiplus() {
    if (g_ovGpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_ovGpToken, &in, nullptr) == Gdiplus::Ok) g_ovGpUp = true;
}

// 가상 화면(모든 모니터) 경계
static void virtualScreen(int& vx, int& vy, int& vw, int& vh) {
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// ============================================================================
//  영역 선택 (모달) : 전체화면 반투명 + 드래그 사각형
// ============================================================================
struct SelState { bool dragging = false; POINT start{}; POINT cur{}; bool done = false; bool cancelled = false; };

// 저수준 마우스 훅용 전역 상태(영역선택은 한 번에 하나만 실행되므로 전역으로 충분).
// LL 훅은 포커스·스레드·캡처와 무관하게 모든 마우스 이벤트를 받으므로, AHK 가
// DllCall 로 메인 스레드에서 호출해도 첫 드래그를 확실히 잡는다.
static SelState* g_selState = nullptr;
static HWND g_selHwnd = nullptr;
static HHOOK g_selHook = nullptr;

static LRESULT CALLBACK SelMouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_selState) {
        MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lp;
        SelState* st = g_selState;
        // 버튼 클릭 잔상 처리(시간 가드 없음 → 빠른 첫 드래그도 즉시 인식):
        //   진입 시 버튼이 눌려있었다면(GUI 버튼 클릭이 진행 중) 그 클릭의 UP 한 번만
        //   무시한다. 그 사이의 DOWN 도 아직 클릭 중이므로 무시. UP 을 본 뒤 해제.
        //   진입 시 버튼이 이미 떼어져 있었다면(대부분) 잔상이 없으므로 곧바로 동작한다.
        //   (기존엔 진입 후 250~400ms 를 통째로 무시해, 빨리 드래그하면 첫 드래그가
        //    같이 무시되어 "두 번 드래그" 증상이 났다.)
        // 핵심: 진짜 첫 DOWN 은 절대 무시하지 않는다.
        //   런처 클릭의 잔상으로 우리가 볼 수 있는 건 오직 'UP' 뿐이다(그 DOWN 은
        //   훅 설치 전에 이미 발생). 따라서 DOWN 이 아직 없을 때 들어온 UP 한 번만
        //   (진입 시 버튼이 눌려 있었을 경우) 무시하면 충분하다. 그 외 모든 DOWN 은
        //   즉시 드래그 시작으로 처리 → 첫 드래그가 항상 인식된다.
        // 이 훅은 '런처 클릭이 완전히 떼어진 뒤'에 설치된다(아래 배출 대기 참조).
        // 따라서 여기서 보는 첫 DOWN 이 곧 사용자의 진짜 드래그 시작 → 잔상 처리 불필요.
        if (wp == WM_LBUTTONDOWN) {
            if (!st->done) {
                st->dragging = true; st->start = m->pt; st->cur = m->pt;
                if (g_selHwnd) InvalidateRect(g_selHwnd, nullptr, FALSE);
            }
        } else if (wp == WM_MOUSEMOVE) {
            if (st->dragging) { st->cur = m->pt; if (g_selHwnd) { InvalidateRect(g_selHwnd, nullptr, FALSE); UpdateWindow(g_selHwnd); } }
        } else if (wp == WM_LBUTTONUP) {
            if (st->dragging) { st->cur = m->pt; st->dragging = false; st->done = true; }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK SelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SelState* st = (SelState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    // 마우스 입력은 저수준 훅(SelMouseHook)이 처리한다. 여기서는 그리기만.
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        // 배경 살짝 어둡게(레이어드 알파로 처리되므로 여기선 사각형만)
        if (st && (st->dragging || st->done)) {
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int x1 = st->start.x - vx, y1 = st->start.y - vy, x2 = st->cur.x - vx, y2 = st->cur.y - vy;
            if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
            if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }
            HBRUSH fill = CreateSolidBrush(RGB(64, 128, 255));
            RECT sel{ x1, y1, x2, y2 };
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 180, 255));
            HGDIOBJ op = SelectObject(hdc, pen), ob = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, x1, y1, x2, y2);
            SelectObject(hdc, op); SelectObject(hdc, ob);
            DeleteObject(pen); DeleteObject(fill);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 반환: true 이면 x,y,w,h 채움. ESC/빈드래그면 false.
bool selectRegionModal(int& ox, int& oy, int& ow, int& oh) {
    static const wchar_t* CLS = L"NOTEPAD_SelOverlay";
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = SelWndProc; wc.hInstance = hInst;
    wc.lpszClassName = CLS; wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassW(&wc);

    int vx, vy, vw, vh; virtualScreen(vx, vy, vw, vh);
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLS, L"", WS_POPUP, vx, vy, vw, vh, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return false;

    SelState st;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&st);
    SetLayeredWindowAttributes(hwnd, 0, 90, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));

    // --- 저수준 마우스 훅 설치 ---
    // 진입 시 왼쪽 버튼이 눌려 있으면(GUI 버튼 클릭 잔상) armed=false 로 두고, 그
    // 버튼이 떼어지는 것을 본 뒤에야 진짜 드래그를 받는다(첫 드래그 = 진짜 드래그).
    g_selState = &st;
    g_selHwnd = hwnd;
    // 진입 시 버튼이 눌려있으면(클릭 진행 중) 그 클릭의 UP 한 번만 무시하도록 표시.
    // 이미 떼어져 있으면(대부분) 잔상이 없으니 즉시 첫 드래그를 받는다(시간 가드 제거).

    MSG msg;
    // 1) 런처 클릭 배출: 진입 시 버튼이 눌려 있으면(클릭으로 실행된 경우) 그 버튼이
    //    완전히 떼어질 때까지 기다린 뒤에야 훅을 건다. 이렇게 하면 훅은 오직 '진짜
    //    드래그'만 보게 되어, 첫 드래그가 런처 클릭 잔상과 절대 섞이지 않는다.
    while (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { st.cancelled = true; break; }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(5);
    }

    // 2) 이제 훅 설치 → 이후의 첫 DOWN 이 곧 진짜 드래그 시작.
    if (!st.cancelled)
        g_selHook = SetWindowsHookExW(WH_MOUSE_LL, SelMouseHook, GetModuleHandleW(nullptr), 0);

    // 3) 드래그 대기 루프. LL 훅 콜백은 이 스레드가 메시지를 펌프할 때 호출되므로
    //    PeekMessage 로 계속 비워준다.
    while (!st.cancelled && !st.done) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { st.cancelled = true; break; }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (st.done) break;
        }
        Sleep(5);
    }

    if (g_selHook) { UnhookWindowsHookEx(g_selHook); g_selHook = nullptr; }
    g_selState = nullptr; g_selHwnd = nullptr;

    DestroyWindow(hwnd);
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    if (st.cancelled) return false;
    int x1 = st.start.x, y1 = st.start.y, x2 = st.cur.x, y2 = st.cur.y;
    if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }
    ow = x2 - x1; oh = y2 - y1; ox = x1; oy = y1;
    if (ow < 2 || oh < 2) return false;
    return true;
}

// ============================================================================
//  비모달 오버레이 (박스/텍스트/아이콘) - id 로 관리
// ============================================================================
enum OvKind { OV_BOX, OV_TEXT, OV_ICON };
struct Overlay { HWND hwnd = nullptr; OvKind kind; };
static std::map<int, Overlay> g_overlays;
static std::mutex g_ovMutex;

static void drawLayered(HWND hwnd, int w, int h, void(*draw)(Gdiplus::Graphics&, void*), void* ctx) {
    ovEnsureGdiplus();
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    memset(bits, 0, (size_t)w * h * 4);
    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        draw(g, ctx);
    }
    POINT ptSrc{ 0,0 }; SIZE sz{ w,h };
    RECT wr; GetWindowRect(hwnd, &wr); POINT ptDst{ wr.left, wr.top };
    BLENDFUNCTION bf{}; bf.BlendOp = AC_SRC_OVER; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd, screen, &ptDst, &sz, mem, &ptSrc, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, screen);
}

static LRESULT CALLBACK OvWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static HWND createOverlayWindow(int x, int y, int w, int h) {
    static const wchar_t* CLS = L"NOTEPAD_Overlay";
    static bool reg = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!reg) { WNDCLASSW wc{}; wc.lpfnWndProc = OvWndProc; wc.hInstance = hInst; wc.lpszClassName = CLS; RegisterClassW(&wc); reg = true; }
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CLS, L"", WS_POPUP, x, y, w, h, nullptr, nullptr, hInst, nullptr);
}

struct BoxCtx { int w, h, thick; Gdiplus::Color color; };
static void drawBox(Gdiplus::Graphics& g, void* c) {
    BoxCtx* b = (BoxCtx*)c;
    Gdiplus::Pen pen(b->color, (Gdiplus::REAL)b->thick);
    g.DrawRectangle(&pen, b->thick / 2, b->thick / 2, b->w - b->thick, b->h - b->thick);
}

// 박스 오버레이 표시/갱신. argb 예: 0xFF00FF00(불투명 초록).
bool overlayBox(int id, int x, int y, int w, int h, uint32_t argb, int thick) {
    std::lock_guard<std::mutex> lk(g_ovMutex);
    if (w <= 0 || h <= 0) return false;
    if (thick < 1) thick = 3;
    auto it = g_overlays.find(id);
    HWND hwnd;
    if (it != g_overlays.end()) { hwnd = it->second.hwnd; SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW); }
    else { hwnd = createOverlayWindow(x, y, w, h); if (!hwnd) return false; g_overlays[id] = { hwnd, OV_BOX }; ShowWindow(hwnd, SW_SHOWNOACTIVATE); }
    BoxCtx ctx{ w, h, thick, Gdiplus::Color(argb) };
    drawLayered(hwnd, w, h, drawBox, &ctx);
    return true;
}

struct TextCtx { const wchar_t* text; Gdiplus::Color fg; Gdiplus::Color bg; int fontPx; int w, h; };
static void drawText(Gdiplus::Graphics& g, void* c) {
    TextCtx* t = (TextCtx*)c;
    Gdiplus::SolidBrush bgb(t->bg);
    g.FillRectangle(&bgb, 0, 0, t->w, t->h);
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, (Gdiplus::REAL)t->fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush fgb(t->fg);
    Gdiplus::PointF pt(6.0f, 4.0f);
    g.DrawString(t->text, -1, &font, pt, &fgb);
}

// 텍스트 라벨 오버레이. fgArgb/bgArgb, fontPx. 배경 반투명 권장(bg 알파 낮게).
bool overlayText(int id, int x, int y, const std::wstring& text, uint32_t fgArgb, uint32_t bgArgb, int fontPx) {
    std::lock_guard<std::mutex> lk(g_ovMutex);
    if (fontPx < 6) fontPx = 16;
    int w = (int)text.size() * fontPx + 16; if (w < 40) w = 40; int h = fontPx + 12;
    auto it = g_overlays.find(id);
    HWND hwnd;
    if (it != g_overlays.end()) { hwnd = it->second.hwnd; SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW); }
    else { hwnd = createOverlayWindow(x, y, w, h); if (!hwnd) return false; g_overlays[id] = { hwnd, OV_TEXT }; ShowWindow(hwnd, SW_SHOWNOACTIVATE); }
    TextCtx ctx{ text.c_str(), Gdiplus::Color(fgArgb), Gdiplus::Color(bgArgb), fontPx, w, h };
    drawLayered(hwnd, w, h, drawText, &ctx);
    return true;
}

struct IconCtx { Gdiplus::Bitmap* bmp; int w, h; };
static void drawIcon(Gdiplus::Graphics& g, void* c) {
    IconCtx* ic = (IconCtx*)c;
    if (ic->bmp) g.DrawImage(ic->bmp, 0, 0, ic->w, ic->h);
}

// PNG 아이콘 오버레이. w/h<=0 이면 원본 크기.
bool overlayIcon(int id, int x, int y, const std::wstring& pngPath, int w, int h) {
    std::lock_guard<std::mutex> lk(g_ovMutex);
    ovEnsureGdiplus();
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(pngPath.c_str());
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { if (bmp) delete bmp; return false; }
    if (w <= 0) w = (int)bmp->GetWidth();
    if (h <= 0) h = (int)bmp->GetHeight();
    auto it = g_overlays.find(id);
    HWND hwnd;
    if (it != g_overlays.end()) { hwnd = it->second.hwnd; SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW); }
    else { hwnd = createOverlayWindow(x, y, w, h); if (!hwnd) { delete bmp; return false; } g_overlays[id] = { hwnd, OV_ICON }; ShowWindow(hwnd, SW_SHOWNOACTIVATE); }
    IconCtx ctx{ bmp, w, h };
    drawLayered(hwnd, w, h, drawIcon, &ctx);
    delete bmp;
    return true;
}

// 오버레이 제거. id<0 이면 전체 제거.
bool overlayClear(int id) {
    std::lock_guard<std::mutex> lk(g_ovMutex);
    if (id < 0) {
        for (auto& kv : g_overlays) if (kv.second.hwnd) DestroyWindow(kv.second.hwnd);
        g_overlays.clear();
        return true;
    }
    auto it = g_overlays.find(id);
    if (it == g_overlays.end()) return false;
    if (it->second.hwnd) DestroyWindow(it->second.hwnd);
    g_overlays.erase(it);
    return true;
}

// ============================================================================
//  크로스헤어 HUD (지정 ms 동안 마우스에 십자선+좌표 표시)
// ============================================================================
bool showCrosshair(int durationMs) {
    if (durationMs <= 0) durationMs = 1500;
    int vx, vy, vw, vh; virtualScreen(vx, vy, vw, vh);
    HWND hwnd = createOverlayWindow(vx, vy, vw, vh);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    DWORD end = GetTickCount() + durationMs;
    ovEnsureGdiplus();
    while ((int)(end - GetTickCount()) > 0) {
        POINT pt; GetCursorPos(&pt);
        // 그리기 컨텍스트를 람다 대신 구조체로
        struct CHCtx { int mx, my, vw, vh, vx, vy; } ctx{ pt.x - vx, pt.y - vy, vw, vh, vx, vy };
        drawLayered(hwnd, vw, vh, [](Gdiplus::Graphics& g, void* c) {
            CHCtx* h = (CHCtx*)c;
            Gdiplus::Pen pen(Gdiplus::Color(200, 0, 200, 255), 1.0f);
            g.DrawLine(&pen, 0, h->my, h->vw, h->my);
            g.DrawLine(&pen, h->mx, 0, h->mx, h->vh);
            wchar_t buf[64]; wsprintfW(buf, L"%d, %d", h->mx + h->vx, h->my + h->vy);
            Gdiplus::FontFamily ff(L"Consolas");
            Gdiplus::Font font(&ff, 14.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush shadow(Gdiplus::Color(220, 0, 0, 0));
            Gdiplus::SolidBrush fg(Gdiplus::Color(255, 255, 255, 0));
            Gdiplus::PointF tp((Gdiplus::REAL)(h->mx + 14), (Gdiplus::REAL)(h->my + 14));
            Gdiplus::PointF sp((Gdiplus::REAL)(h->mx + 15), (Gdiplus::REAL)(h->my + 15));
            g.DrawString(buf, -1, &font, sp, &shadow);
            g.DrawString(buf, -1, &font, tp, &fg);
        }, &ctx);
        Sleep(16);
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    DestroyWindow(hwnd);
    return true;
}

} // namespace notepad

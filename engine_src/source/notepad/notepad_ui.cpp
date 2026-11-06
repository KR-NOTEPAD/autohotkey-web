// ============================================================================
//  NOTEPAD.dll  -  notepad_ui.cpp
//  [NOTEPAD] 세트 W: 알림 / 클립보드 / 사운드 (UI 보조).
//  의존성 0: user32/shell32/kernel32(기본).
//  참고: 전역 이벤트 훅 같은 능동형이 아니라, 호출하면 즉시 동작하는 수동형만.
// ============================================================================
#include <windows.h>
#include <shellapi.h>
#include <string>

namespace {
int ui_writeOut(const std::wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}
} // anonymous

extern "C" {

// 트레이 벌룬(풍선) 알림 표시. 잠깐 아이콘을 만들고 알림 후 제거한다.
//   iconType: 0=없음, 1=정보, 2=경고, 3=오류. 성공 1/0.
//   (AHK 의 TrayTip 과 유사하지만 스크립트 트레이와 독립적으로 동작)
__declspec(dllexport) int __stdcall Notepad_TrayNotify(const wchar_t* title, const wchar_t* msg,
                                                       int iconType) {
    NOTIFYICONDATAW nid; ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    // 숨은 메시지 윈도우 대신 데스크톱 핸들 사용(간이). 알림만 목적.
    nid.hWnd = GetDesktopWindow();
    nid.uID = 0x4E50;  // 'NP'
    nid.uFlags = NIF_ICON | NIF_INFO | NIF_TIP;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcsncpy_s(nid.szTip, L"NOTEPAD", _TRUNCATE);
    wcsncpy_s(nid.szInfoTitle, title ? title : L"", _TRUNCATE);
    wcsncpy_s(nid.szInfo, msg ? msg : L"", _TRUNCATE);
    switch (iconType) {
        case 1: nid.dwInfoFlags = NIIF_INFO; break;
        case 2: nid.dwInfoFlags = NIIF_WARNING; break;
        case 3: nid.dwInfoFlags = NIIF_ERROR; break;
        default: nid.dwInfoFlags = NIIF_NONE; break;
    }
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return 0;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    // 알림이 뜰 시간을 잠깐 준 뒤 아이콘 제거(알림 자체는 남는다).
    Sleep(10);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    return 1;
}

// 클립보드의 텍스트를 읽어 반환. 실패/텍스트없음 "".
__declspec(dllexport) int __stdcall Notepad_ClipboardGet(wchar_t* out, int outCch) {
    if (!OpenClipboard(nullptr)) return 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    std::wstring result;
    if (h) {
        wchar_t* p = (wchar_t*)GlobalLock(h);
        if (p) { result = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    return ui_writeOut(result, out, outCch);
}

// 클립보드에 텍스트를 설정. 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_ClipboardSet(const wchar_t* text) {
    std::wstring s = text ? text : L"";
    if (!OpenClipboard(nullptr)) return 0;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    int ok = 0;
    if (hMem) {
        void* p = GlobalLock(hMem);
        if (p) {
            memcpy(p, s.c_str(), bytes);
            GlobalUnlock(hMem);
            if (SetClipboardData(CF_UNICODETEXT, hMem)) ok = 1;
        }
        if (!ok) GlobalFree(hMem);
    }
    CloseClipboard();
    return ok;
}

// 클립보드 비우기. 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_ClipboardClear() {
    if (!OpenClipboard(nullptr)) return 0;
    int ok = EmptyClipboard() ? 1 : 0;
    CloseClipboard();
    return ok;
}

// 비프음. freq=주파수(Hz, 37~32767), durMs=지속(ms). 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_Beep(int freq, int durMs) {
    if (freq < 37) freq = 37;
    if (freq > 32767) freq = 32767;
    if (durMs < 0) durMs = 0;
    return Beep((DWORD)freq, (DWORD)durMs) ? 1 : 0;
}

// 시스템 사운드 재생. which: "asterisk","exclamation","hand","question","default". 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_PlaySystemSound(const wchar_t* which) {
    std::wstring w = which ? which : L"default";
    UINT t = MB_OK;
    if (w == L"asterisk") t = MB_ICONASTERISK;
    else if (w == L"exclamation") t = MB_ICONEXCLAMATION;
    else if (w == L"hand") t = MB_ICONHAND;
    else if (w == L"question") t = MB_ICONQUESTION;
    return MessageBeep(t) ? 1 : 0;
}

// 모니터 전원 끄기(대기). 마우스/키 입력 시 다시 켜진다. 항상 1.
__declspec(dllexport) int __stdcall Notepad_MonitorOff() {
    // WM_SYSCOMMAND / SC_MONITORPOWER (2=off). 브로드캐스트로 확실히 전달.
    SendMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)2);
    return 1;
}

// 환경변수 값 읽기. 없으면 "".
__declspec(dllexport) int __stdcall Notepad_GetEnv(const wchar_t* name, wchar_t* out, int outCch) {
    if (!name) return 0;
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return ui_writeOut(L"", out, outCch);   // 없음
    std::wstring buf(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, &buf[0], n);
    buf.resize(got);
    return ui_writeOut(buf, out, outCch);
}

// 현재 프로세스가 관리자 권한인지. 1=관리자, 0=아님.
__declspec(dllexport) int __stdcall Notepad_IsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin ? 1 : 0;
}

} // extern "C"

// ============================================================================
//  NOTEPAD.dll  -  notepad_sys.cpp
//  [NOTEPAD] 세트 R: 프로세스 / 윈도우 정보 (조회형).
//  의존성 0: kernel32/user32(기본). 파이썬 psutil 의 일부 대응.
//  결과는 cJSON 배열(JSON) 로 반환 -> 기존 Note_JsonGet 으로 파싱.
//  링크: user32.lib (build.bat 에 이미 포함). Toolhelp32 는 kernel32.
// ============================================================================
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cwctype>
#include "cJSON.h"

namespace {

std::string sys_wToUtf8(const wchar_t* w) {
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring sys_utf8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
int sys_writeOut(const std::wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}

// EnumWindows 콜백 컨텍스트.
struct WinCtx {
    cJSON* arr;
    bool visibleOnly;
};

BOOL CALLBACK enumWinProc(HWND hwnd, LPARAM lp) {
    WinCtx* ctx = (WinCtx*)lp;
    if (ctx->visibleOnly && !IsWindowVisible(hwnd)) return TRUE;

    int len = GetWindowTextLengthW(hwnd);
    if (ctx->visibleOnly && len == 0) return TRUE;   // 제목 없는 창 스킵(가시모드)

    std::wstring title(len + 1, L'\0');
    GetWindowTextW(hwnd, &title[0], len + 1);
    title.resize(len);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "title", sys_wToUtf8(title.c_str()).c_str());
    cJSON_AddNumberToObject(o, "pid", (double)pid);
    cJSON_AddNumberToObject(o, "hwnd", (double)(uintptr_t)hwnd);
    cJSON_AddBoolToObject(o, "visible", IsWindowVisible(hwnd) ? 1 : 0);
    cJSON_AddItemToArray(ctx->arr, o);
    return TRUE;
}

} // anonymous

// ============================================================================
//  Exports (세트 R)
// ============================================================================
extern "C" {

// 실행 중인 프로세스 목록을 JSON 배열로. 각 원소 {pid, name, threads, parentPid}.
//   실패 시 0. 예: Note_JsonGet(result, "0.name")
__declspec(dllexport) int __stdcall Notepad_ProcessList(wchar_t* out, int outCch) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    cJSON* arr = cJSON_CreateArray();
    if (Process32FirstW(snap, &pe)) {
        do {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "pid", (double)pe.th32ProcessID);
            cJSON_AddStringToObject(o, "name", sys_wToUtf8(pe.szExeFile).c_str());
            cJSON_AddNumberToObject(o, "threads", (double)pe.cntThreads);
            cJSON_AddNumberToObject(o, "parentPid", (double)pe.th32ParentProcessID);
            cJSON_AddItemToArray(arr, o);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    char* js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!js) return 0;
    std::wstring w = sys_utf8ToW(std::string(js));
    cJSON_free(js);
    return sys_writeOut(w, out, outCch);
}

// 프로세스 이름으로 PID 찾기(첫 일치). 대소문자 무시. 없으면 0.
__declspec(dllexport) int __stdcall Notepad_ProcessFind(const wchar_t* name) {
    if (!name) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    int found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { found = (int)pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// 최상위 윈도우 목록을 JSON 배열로. 각 원소 {title, pid, hwnd, visible}.
//   visibleOnly!=0 이면 보이는 + 제목 있는 창만. 실패 시 0.
__declspec(dllexport) int __stdcall Notepad_WindowList(int visibleOnly,
                                                       wchar_t* out, int outCch) {
    cJSON* arr = cJSON_CreateArray();
    WinCtx ctx{ arr, visibleOnly != 0 };
    EnumWindows(enumWinProc, (LPARAM)&ctx);

    char* js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!js) return 0;
    std::wstring w = sys_utf8ToW(std::string(js));
    cJSON_free(js);
    return sys_writeOut(w, out, outCch);
}

// 창 제목(부분 일치, 대소문자 무시)으로 hwnd 찾기. 없으면 0.
//   반환은 hwnd 를 정수로(0 아니면 유효). AHK 에서 WinActivate 등에 활용.
__declspec(dllexport) __int64 __stdcall Notepad_WindowFind(const wchar_t* titlePart) {
    if (!titlePart) return 0;
    std::wstring want = titlePart;
    for (auto& c : want) c = (wchar_t)towlower(c);

    struct FindCtx { std::wstring want; HWND hit; } ctx{ want, nullptr };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        FindCtx* c = (FindCtx*)lp;
        int len = GetWindowTextLengthW(hwnd);
        if (len == 0) return TRUE;
        std::wstring title(len + 1, L'\0');
        GetWindowTextW(hwnd, &title[0], len + 1);
        title.resize(len);
        for (auto& ch : title) ch = (wchar_t)towlower(ch);
        if (title.find(c->want) != std::wstring::npos) { c->hit = hwnd; return FALSE; }
        return TRUE;
    }, (LPARAM)&ctx);
    return (__int64)(uintptr_t)ctx.hit;
}

} // extern "C"

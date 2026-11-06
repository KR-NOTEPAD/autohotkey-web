// ============================================================================
//  NOTEPAD.dll  -  notepad_sys2.cpp
//  [NOTEPAD] 세트 S: 시스템 정보 (메모리/디스크/배터리/CPU/모니터/OS).
//  의존성 0: kernel32/user32(기본). 파이썬 psutil/platform 일부 대응.
//  링크: user32.lib (build.bat 에 이미 포함).
// ============================================================================
#include <windows.h>
#include <string>
#include <cstdio>

namespace {
int s2_writeOut(const std::wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}
} // anonymous

extern "C" {

// 전체 물리 메모리(MB). 실패 -1.
__declspec(dllexport) __int64 __stdcall Notepad_MemTotalMB() {
    MEMORYSTATUSEX m; m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m)) return -1;
    return (__int64)(m.ullTotalPhys / (1024 * 1024));
}
// 사용 가능한 물리 메모리(MB). 실패 -1.
__declspec(dllexport) __int64 __stdcall Notepad_MemAvailMB() {
    MEMORYSTATUSEX m; m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m)) return -1;
    return (__int64)(m.ullAvailPhys / (1024 * 1024));
}
// 메모리 사용률(%). 실패 -1.
__declspec(dllexport) int __stdcall Notepad_MemLoadPercent() {
    MEMORYSTATUSEX m; m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m)) return -1;
    return (int)m.dwMemoryLoad;
}
// 드라이브의 전체 용량(MB). path 예: "C:\\". 실패 -1.
__declspec(dllexport) __int64 __stdcall Notepad_DiskTotalMB(const wchar_t* path) {
    ULARGE_INTEGER freeAvail, total, totalFree;
    if (!GetDiskFreeSpaceExW(path ? path : L"C:\\", &freeAvail, &total, &totalFree))
        return -1;
    return (__int64)(total.QuadPart / (1024 * 1024));
}
// 드라이브의 남은 용량(MB). 실패 -1.
__declspec(dllexport) __int64 __stdcall Notepad_DiskFreeMB(const wchar_t* path) {
    ULARGE_INTEGER freeAvail, total, totalFree;
    if (!GetDiskFreeSpaceExW(path ? path : L"C:\\", &freeAvail, &total, &totalFree))
        return -1;
    return (__int64)(freeAvail.QuadPart / (1024 * 1024));
}
// 배터리 잔량(%). 데스크톱/알수없음 -1, AC 연결 등은 값이 있으면 반환.
__declspec(dllexport) int __stdcall Notepad_BatteryPercent() {
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps)) return -1;
    if (ps.BatteryLifePercent == 255) return -1;   // 알 수 없음
    return (int)ps.BatteryLifePercent;
}
// AC 전원 연결 여부. 1=연결, 0=배터리, -1=알수없음.
__declspec(dllexport) int __stdcall Notepad_BatteryCharging() {
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps)) return -1;
    if (ps.ACLineStatus == 255) return -1;
    return ps.ACLineStatus == 1 ? 1 : 0;
}
// 논리 CPU 코어 수. 실패 0.
__declspec(dllexport) int __stdcall Notepad_CpuCount() {
    SYSTEM_INFO si; GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
// 주 모니터 가로 해상도(px).
__declspec(dllexport) int __stdcall Notepad_ScreenWidth() {
    return GetSystemMetrics(SM_CXSCREEN);
}
// 주 모니터 세로 해상도(px).
__declspec(dllexport) int __stdcall Notepad_ScreenHeight() {
    return GetSystemMetrics(SM_CYSCREEN);
}
// 모니터 개수.
__declspec(dllexport) int __stdcall Notepad_MonitorCount() {
    return GetSystemMetrics(SM_CMONITORS);
}
// 시스템 부팅 후 경과 시간(ms). 64비트.
__declspec(dllexport) __int64 __stdcall Notepad_UptimeMs() {
    return (__int64)GetTickCount64();
}
// 컴퓨터 이름. 실패 0.
__declspec(dllexport) int __stdcall Notepad_ComputerName(wchar_t* out, int outCch) {
    wchar_t buf[256]; DWORD n = 256;
    if (!GetComputerNameW(buf, &n)) return 0;
    return s2_writeOut(std::wstring(buf, n), out, outCch);
}
// 현재 사용자 이름. 실패 0.
__declspec(dllexport) int __stdcall Notepad_UserName(wchar_t* out, int outCch) {
    wchar_t buf[256]; DWORD n = 256;
    if (!GetUserNameW(buf, &n)) return 0;
    return s2_writeOut(std::wstring(buf, n ? n - 1 : 0), out, outCch);
}
// Windows 빌드 번호(예 "22631"). 레지스트리 조회. 실패 "".
__declspec(dllexport) int __stdcall Notepad_WinBuild(wchar_t* out, int outCch) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0, KEY_READ, &hk) != ERROR_SUCCESS) return 0;
    wchar_t buf[64] = {0}; DWORD cb = sizeof(buf), type = 0;
    LONG r = RegQueryValueExW(hk, L"CurrentBuildNumber", nullptr, &type, (LPBYTE)buf, &cb);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS) return 0;
    return s2_writeOut(std::wstring(buf), out, outCch);
}

} // extern "C"

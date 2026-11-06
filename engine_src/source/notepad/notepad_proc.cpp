// ============================================================================
//  NOTEPAD.dll  -  notepad_proc.cpp
//  [NOTEPAD] 세트 X: 프로세스 / 실행 (종료/실행+출력캡처/대기/환경변수).
//  의존성 0: kernel32(기본). 파이썬 subprocess 일부 대응.
// ============================================================================
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

namespace {
int px_writeOut(const std::wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}
} // anonymous

extern "C" {

// PID 로 프로세스 강제 종료. 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_ProcessKill(int pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) return 0;
    int ok = TerminateProcess(h, 1) ? 1 : 0;
    CloseHandle(h);
    return ok;
}

// 이름으로 프로세스 종료(모든 인스턴스). 종료한 개수 반환.
__declspec(dllexport) int __stdcall Notepad_ProcessKillByName(const wchar_t* name) {
    if (!name) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    int killed = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) { if (TerminateProcess(h, 1)) killed++; CloseHandle(h); }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return killed;
}

// 프로그램 실행 후 종료까지 대기, 종료코드 반환. 실패 -1.
//   cmdLine: 실행할 명령(인자 포함). timeoutMs<=0 이면 무한 대기.
__declspec(dllexport) int __stdcall Notepad_RunWait(const wchar_t* cmdLine, int timeoutMs) {
    if (!cmdLine) return -1;
    std::wstring cl = cmdLine;
    std::vector<wchar_t> buf(cl.begin(), cl.end()); buf.push_back(L'\0');
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return -1;
    DWORD wait = timeoutMs > 0 ? (DWORD)timeoutMs : INFINITE;
    DWORD w = WaitForSingleObject(pi.hProcess, wait);
    DWORD code = (DWORD)-1;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}

// 프로그램을 실행하고 표준출력을 캡처해 반환(콘솔 프로그램용). 실패 "".
//   timeoutMs<=0 이면 무한. 대용량 출력은 주의.
__declspec(dllexport) int __stdcall Notepad_RunCapture(const wchar_t* cmdLine, int timeoutMs,
                                                       wchar_t* out, int outCch) {
    if (!cmdLine) return 0;
    SECURITY_ATTRIBUTES sa; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::wstring cl = cmdLine;
    std::vector<wchar_t> buf(cl.begin(), cl.end()); buf.push_back(L'\0');
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); return 0;
    }
    CloseHandle(wr);  // 쓰기 끝은 자식만 소유

    std::string outp;
    char tmp[4096]; DWORD read = 0;
    DWORD start = GetTickCount();
    DWORD wait = timeoutMs > 0 ? (DWORD)timeoutMs : INFINITE;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            if (ReadFile(rd, tmp, sizeof(tmp), &read, nullptr) && read > 0)
                outp.append(tmp, read);
        } else {
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                // 프로세스 종료 후 남은 출력 마저 읽기
                while (ReadFile(rd, tmp, sizeof(tmp), &read, nullptr) && read > 0)
                    outp.append(tmp, read);
                break;
            }
            if (wait != INFINITE && GetTickCount() - start > wait) break;
            Sleep(10);
        }
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);

    // 콘솔 출력은 보통 시스템 ANSI 코드페이지 -> UTF-16 변환
    if (outp.empty()) return px_writeOut(L"", out, outCch);
    int n = MultiByteToWideChar(CP_ACP, 0, outp.data(), (int)outp.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, outp.data(), (int)outp.size(), &w[0], n);
    return px_writeOut(w, out, outCch);
}

// 프로세스 실행 우선순위 설정. pid, level: 0=idle,1=belownormal,2=normal,3=abovenormal,4=high. 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_ProcessPriority(int pid, int level) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return 0;
    DWORD cls = NORMAL_PRIORITY_CLASS;
    switch (level) {
        case 0: cls = IDLE_PRIORITY_CLASS; break;
        case 1: cls = BELOW_NORMAL_PRIORITY_CLASS; break;
        case 2: cls = NORMAL_PRIORITY_CLASS; break;
        case 3: cls = ABOVE_NORMAL_PRIORITY_CLASS; break;
        case 4: cls = HIGH_PRIORITY_CLASS; break;
    }
    int ok = SetPriorityClass(h, cls) ? 1 : 0;
    CloseHandle(h);
    return ok;
}

// 환경변수 설정(현재 프로세스). 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_SetEnv(const wchar_t* name, const wchar_t* value) {
    if (!name) return 0;
    return SetEnvironmentVariableW(name, value ? value : L"") ? 1 : 0;
}

// 프로세스가 살아있는지. pid. 1=실행중, 0=아님.
__declspec(dllexport) int __stdcall Notepad_ProcessExists(int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return 0;
    DWORD code = 0;
    int alive = 0;
    if (GetExitCodeProcess(h, &code)) alive = (code == STILL_ACTIVE) ? 1 : 0;
    CloseHandle(h);
    return alive;
}

} // extern "C"

// ============================================================================
//  NOTEPAD.dll  -  notepad_pipe.cpp
//  [NOTEPAD] 세트 Q: Named Pipe IPC (동기/블로킹). 프로세스 간 메시지 교환.
//  의존성 0: kernel32(기본). 파이썬 소켓/멀티프로세싱 대응(로컬 IPC).
//
//  구조: 요청-응답 1왕복 모델(수동형 DLL 에 맞춘 블로킹 방식).
//   - 서버: Notepad_PipeServerOnce(name, timeoutMs) -> 클라이언트 접속 대기 후
//           메시지 1건 수신하여 반환. 필요 시 Notepad_PipeServerReply 로 응답.
//     (단순형: 수신만 하려면 ServerOnce 만 써도 됨. 응답까지면 아래 Reply 패턴.)
//   - 클라이언트: Notepad_PipeClientSend(name, msg, timeoutMs) -> 접속·송신 후
//           서버 응답을 반환(서버가 응답 안 하면 "" 또는 타임아웃).
//
//  파이프 경로: name 은 짧은 이름만 주면 \\.\pipe\NOTEPAD_<name> 으로 확장.
//  전 구간 UTF-8 바이트로 송수신하고, 경계는 메시지 모드 파이프로 구분.
// ============================================================================
#include <windows.h>
#include <string>
#include <vector>

namespace {

std::string pipe_wToUtf8(const wchar_t* w) {
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring pipe_utf8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
int pipe_writeOut(const std::wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}

std::wstring fullPipeName(const wchar_t* name) {
    std::wstring n = name ? name : L"default";
    // 이미 전체 경로면 그대로.
    if (n.rfind(L"\\\\", 0) == 0) return n;
    return L"\\\\.\\pipe\\NOTEPAD_" + n;
}

// 서버가 응답을 돌려줄 수 있도록, 마지막 연결 핸들을 보관(단일 세션 가정).
HANDLE g_serverPipe = INVALID_HANDLE_VALUE;

const DWORD PIPE_BUF = 65536;

// [중요] 출력버퍼 2단계 호출(길이조회→채우기)에서 부작용(실제 송수신)이
//   두 번 일어나지 않도록 결과를 캐시한다(세트 C/J HTTP 와 동일 전략).
//   서버 수신 캐시.
std::wstring g_srvKey, g_srvResult; bool g_srvValid = false;
//   클라 송수신 캐시.
std::wstring g_cliKey, g_cliResult; bool g_cliValid = false;

} // anonymous

// ============================================================================
//  Exports (세트 Q)
// ============================================================================
extern "C" {

// 서버: 파이프 생성 -> 클라이언트 접속 대기 -> 메시지 1건 수신 후 반환.
//   timeoutMs<=0 이면 무한 대기. 접속/수신 실패 시 0(빈 문자열 아님 주의).
//   수신 후 파이프 핸들을 열어둔 채 보관 -> 이어서 Notepad_PipeServerReply 가능.
//   응답이 필요 없으면 반환값만 쓰고 다음 호출 때 자동 정리된다.
__declspec(dllexport) int __stdcall Notepad_PipeServerOnce(const wchar_t* name,
                                                           int timeoutMs,
                                                           wchar_t* out, int outCch) {
    // 2단계 호출 캐시: 같은 name 재호출이면 이미 받은 메시지를 반환(재대기 안 함).
    std::wstring key = (name ? name : L"");
    if (g_srvValid && key == g_srvKey)
        return pipe_writeOut(g_srvResult, out, outCch);

    // 이전 세션 정리.
    if (g_serverPipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(g_serverPipe);
        CloseHandle(g_serverPipe);
        g_serverPipe = INVALID_HANDLE_VALUE;
    }
    std::wstring pname = fullPipeName(name);
    HANDLE hPipe = CreateNamedPipeW(pname.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, PIPE_BUF, PIPE_BUF,
        timeoutMs > 0 ? (DWORD)timeoutMs : NMPWAIT_WAIT_FOREVER, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) return 0;

    // 클라이언트 접속 대기(블로킹). ERROR_PIPE_CONNECTED 도 정상.
    BOOL connected = ConnectNamedPipe(hPipe, nullptr)
                   ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) { CloseHandle(hPipe); return 0; }

    std::vector<char> buf(PIPE_BUF);
    DWORD read = 0;
    BOOL ok = ReadFile(hPipe, buf.data(), (DWORD)buf.size(), &read, nullptr);
    if (!ok && GetLastError() != ERROR_MORE_DATA) {
        DisconnectNamedPipe(hPipe); CloseHandle(hPipe); return 0;
    }
    g_serverPipe = hPipe;   // Reply 용으로 보관
    std::string msg(buf.data(), read);
    g_srvKey = key; g_srvResult = pipe_utf8ToW(msg); g_srvValid = true;  // 캐시
    return pipe_writeOut(g_srvResult, out, outCch);
}

// 서버: 직전 ServerOnce 로 받은 클라이언트에게 응답 전송 후 연결 종료. 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_PipeServerReply(const wchar_t* text) {
    if (g_serverPipe == INVALID_HANDLE_VALUE) return 0;
    std::string data = pipe_wToUtf8(text);
    DWORD written = 0;
    BOOL ok = WriteFile(g_serverPipe, data.data(), (DWORD)data.size(), &written, nullptr);
    FlushFileBuffers(g_serverPipe);
    DisconnectNamedPipe(g_serverPipe);
    CloseHandle(g_serverPipe);
    g_serverPipe = INVALID_HANDLE_VALUE;
    g_srvValid = false;   // 응답까지 끝났으니 다음 ServerOnce 는 새로 대기.
    return (ok && written == data.size()) ? 1 : 0;
}

// 클라이언트: 파이프에 접속 -> msg 송신 -> 서버 응답 수신하여 반환.
//   timeoutMs: 접속 대기 최대 시간(<=0 이면 기본 5000). 실패 시 0.
//   서버가 응답을 안 보내면 빈 문자열(길이 0)을 반환할 수 있다.
__declspec(dllexport) int __stdcall Notepad_PipeClientSend(const wchar_t* name,
                                                           const wchar_t* msg,
                                                           int timeoutMs,
                                                           wchar_t* out, int outCch) {
    // 2단계 호출 캐시: 같은 (name+msg) 재호출이면 이미 받은 응답 반환(재송신 안 함).
    std::wstring cliKey = std::wstring(name ? name : L"") + L"\x01" + (msg ? msg : L"");
    if (g_cliValid && cliKey == g_cliKey)
        return pipe_writeOut(g_cliResult, out, outCch);

    std::wstring pname = fullPipeName(name);
    DWORD wait = timeoutMs > 0 ? (DWORD)timeoutMs : 5000;

    // 파이프가 바쁠 수 있으니 대기 후 열기.
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    DWORD start = GetTickCount();
    for (;;) {
        hPipe = CreateFileW(pname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (hPipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) {
            // 서버가 아직 없을 수 있음 -> 남은 시간 동안 재시도.
            if (GetTickCount() - start >= wait) return 0;
            Sleep(50);
            continue;
        }
        if (!WaitNamedPipeW(pname.c_str(), 200)) {
            if (GetTickCount() - start >= wait) return 0;
        }
    }

    // 메시지 읽기 모드로 설정.
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    std::string data = pipe_wToUtf8(msg);
    DWORD written = 0;
    if (!WriteFile(hPipe, data.data(), (DWORD)data.size(), &written, nullptr)) {
        CloseHandle(hPipe); return 0;
    }
    FlushFileBuffers(hPipe);

    // 서버 응답 수신(있으면). 없으면 곧 파이프가 끊겨 read=0.
    std::vector<char> buf(PIPE_BUF);
    DWORD read = 0;
    BOOL ok = ReadFile(hPipe, buf.data(), (DWORD)buf.size(), &read, nullptr);
    CloseHandle(hPipe);
    std::string resp = (ok || read > 0) ? std::string(buf.data(), read) : std::string();
    g_cliKey = cliKey; g_cliResult = pipe_utf8ToW(resp); g_cliValid = true;  // 캐시
    return pipe_writeOut(g_cliResult, out, outCch);
}

} // extern "C"

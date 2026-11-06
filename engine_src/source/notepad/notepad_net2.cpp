// ============================================================================
//  NOTEPAD.dll  -  notepad_net2.cpp
//  [NOTEPAD] 세트 AE-2: 비동기 다운로드 / WebSocket / 연속 프레임 캡처.
//  링크: winhttp.lib gdiplus.lib gdi32.lib user32.lib (build.bat 에 이미 포함).
//  의존성 0. AHK v1 이 특히 약한 3가지를 보완.
//
//   * 비동기 다운로드 - 백그라운드 스레드로 파일 받기(논블로킹). 진행률 폴링.
//     AHK v1 의 HTTP 는 동기라 큰 파일 받는 동안 스크립트가 멈춘다.
//   * WebSocket 클라이언트 - WinHTTP WebSocket 업그레이드. 연결/송신/수신/종료.
//     AHK v1 엔 WebSocket 이 사실상 없다(외부 dll 필요).
//   * 연속 프레임 캡처 - 영역을 fps 로 N프레임 PNG 연속 저장(폴더). 화면 녹화/
//     프레임 단위 객체탐지 파이프라인용.
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")

namespace notepad {

// 세트 G 캡처 재사용
bool captureRegionToPng(int x, int y, int w, int h, const std::string& outPngUtf8);

static std::wstring net2U8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string net2WToU8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// URL 분해
static bool crackUrl(const std::wstring& url, std::wstring& host, INTERNET_PORT& port,
                     std::wstring& path, bool& https) {
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256]{}, pathBuf[2048]{};
    uc.lpszHostName = hostBuf; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = pathBuf; uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    host = hostBuf; path = pathBuf[0] ? pathBuf : L"/";
    port = uc.nPort; https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

// ============================================================================
//  비동기 다운로드
// ============================================================================
struct DlJob {
    std::atomic<long long> total{ -1 };
    std::atomic<long long> received{ 0 };
    std::atomic<int> state{ 0 };  // 0=running 1=done 2=error
    std::thread th;
};
static std::mutex g_dlMutex;
static std::map<int, DlJob*> g_dlJobs;
static std::atomic<int> g_dlNextId{ 1 };

static void dlWorker(DlJob* job, std::wstring url, std::wstring outPath) {
    std::wstring host, path; INTERNET_PORT port; bool https;
    if (!crackUrl(url, host, port, path, https)) { job->state = 2; return; }
    HINTERNET hs = WinHttpOpen(L"NOTEPAD/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { job->state = 2; return; }
    HINTERNET hc = WinHttpConnect(hs, host.c_str(), port, 0);
    HINTERNET hr = hc ? WinHttpOpenRequest(hc, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        https ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (hr && WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(hr, nullptr)) {
        // Content-Length
        DWORD len = sizeof(DWORD); DWORD cl = 0;
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &cl, &len, WINHTTP_NO_HEADER_INDEX);
        if (cl) job->total = cl;
        HANDLE f = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<char> buf(32768); DWORD avail = 0; ok = true;
            do {
                if (!WinHttpQueryDataAvailable(hr, &avail)) { ok = false; break; }
                if (!avail) break;
                DWORD rd = 0;
                if (!WinHttpReadData(hr, buf.data(), (DWORD)std::min<DWORD>(avail, (DWORD)buf.size()), &rd)) { ok = false; break; }
                if (rd) { DWORD wr = 0; WriteFile(f, buf.data(), rd, &wr, nullptr); job->received += rd; }
            } while (avail > 0);
            CloseHandle(f);
        }
    }
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    if (hs) WinHttpCloseHandle(hs);
    job->state = ok ? 1 : 2;
}

// 시작 -> job id(>0) 반환. 0=실패.
int downloadAsyncStart(const std::wstring& url, const std::wstring& outPath) {
    DlJob* job = new DlJob();
    int id = g_dlNextId++;
    { std::lock_guard<std::mutex> lk(g_dlMutex); g_dlJobs[id] = job; }
    job->th = std::thread(dlWorker, job, url, outPath);
    return id;
}
// 상태 조회 -> "state,received,total" (state 0=진행 1=완료 2=오류).
std::wstring downloadAsyncStatus(int id) {
    std::lock_guard<std::mutex> lk(g_dlMutex);
    auto it = g_dlJobs.find(id);
    if (it == g_dlJobs.end()) return L"";
    DlJob* j = it->second;
    wchar_t buf[96];
    wsprintfW(buf, L"%d,%I64d,%I64d", (int)j->state.load(), j->received.load(), j->total.load());
    return buf;
}
// 정리(완료 후 호출). 성공 1.
int downloadAsyncFree(int id) {
    DlJob* j = nullptr;
    { std::lock_guard<std::mutex> lk(g_dlMutex); auto it = g_dlJobs.find(id); if (it == g_dlJobs.end()) return 0; j = it->second; g_dlJobs.erase(it); }
    if (j->th.joinable()) j->th.join();
    delete j; return 1;
}

// ============================================================================
//  WebSocket 클라이언트 (WinHTTP WebSocket, Win8+)
//  단일 연결 핸들을 정수 id 로 관리(동기 송/수신).
// ============================================================================
struct WsConn { HINTERNET hSession = nullptr, hConnect = nullptr, hWs = nullptr; };
static std::mutex g_wsMutex;
static std::map<int, WsConn*> g_wsConns;
static std::atomic<int> g_wsNextId{ 1 };

// 연결. ws:// 또는 wss:// URL. 성공 시 id(>0), 실패 0.
int wsConnect(const std::wstring& url) {
    std::wstring host, path; INTERNET_PORT port; bool https;
    // ws->http, wss->https 로 크랙되도록 스킴 치환
    std::wstring u = url;
    if (u.rfind(L"wss://", 0) == 0) u = L"https://" + u.substr(6);
    else if (u.rfind(L"ws://", 0) == 0) u = L"http://" + u.substr(5);
    if (!crackUrl(u, host, port, path, https)) return 0;

    HINTERNET hs = WinHttpOpen(L"NOTEPAD-WS/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) return 0;
    HINTERNET hc = WinHttpConnect(hs, host.c_str(), port, 0);
    HINTERNET hr = hc ? WinHttpOpenRequest(hs && hc ? hc : nullptr, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    HINTERNET hWs = nullptr;
    if (hr) {
        WinHttpSetOption(hr, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
        if (WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)
            && WinHttpReceiveResponse(hr, nullptr)) {
            hWs = WinHttpWebSocketCompleteUpgrade(hr, 0);
        }
    }
    if (hr) WinHttpCloseHandle(hr); // 업그레이드 후 요청 핸들 닫아도 됨
    if (!hWs) { if (hc) WinHttpCloseHandle(hc); if (hs) WinHttpCloseHandle(hs); return 0; }
    WsConn* c = new WsConn{ hs, hc, hWs };
    int id = g_wsNextId++;
    { std::lock_guard<std::mutex> lk(g_wsMutex); g_wsConns[id] = c; }
    return id;
}
// UTF-8 텍스트 송신. 성공 1.
int wsSend(int id, const std::wstring& text) {
    WsConn* c; { std::lock_guard<std::mutex> lk(g_wsMutex); auto it = g_wsConns.find(id); if (it == g_wsConns.end()) return 0; c = it->second; }
    std::string u8 = net2WToU8(text);
    DWORD r = WinHttpWebSocketSend(c->hWs, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (PVOID)u8.data(), (DWORD)u8.size());
    return r == NO_ERROR ? 1 : 0;
}
// 수신(한 메시지, timeoutMs 무시-동기). 텍스트 반환, 실패/종료 "".
std::wstring wsReceive(int id) {
    WsConn* c; { std::lock_guard<std::mutex> lk(g_wsMutex); auto it = g_wsConns.find(id); if (it == g_wsConns.end()) return L""; c = it->second; }
    std::string acc; std::vector<char> buf(8192);
    for (;;) {
        DWORD read = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD r = WinHttpWebSocketReceive(c->hWs, buf.data(), (DWORD)buf.size(), &read, &type);
        if (r != NO_ERROR) return L"";
        acc.append(buf.data(), read);
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return L"";
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) break; // 메시지 끝
        // *_FRAGMENT 면 계속 누적
    }
    return net2U8ToW(acc);
}
// 종료 + 정리. 성공 1.
int wsClose(int id) {
    WsConn* c; { std::lock_guard<std::mutex> lk(g_wsMutex); auto it = g_wsConns.find(id); if (it == g_wsConns.end()) return 0; c = it->second; g_wsConns.erase(it); }
    if (c->hWs) { WinHttpWebSocketClose(c->hWs, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0); WinHttpCloseHandle(c->hWs); }
    if (c->hConnect) WinHttpCloseHandle(c->hConnect);
    if (c->hSession) WinHttpCloseHandle(c->hSession);
    delete c; return 1;
}

// ============================================================================
//  연속 프레임 캡처 (영역을 fps 로 count 프레임 PNG 저장)
//  outDir\prefixNNNN.png 로 저장. 반환: 저장된 프레임 수.
// ============================================================================
int captureFrames(int x, int y, int w, int h, const std::wstring& outDir,
                  const std::wstring& prefix, int count, int fps) {
    if (w <= 0 || h <= 0 || count <= 0) return 0;
    if (fps <= 0) fps = 10;
    int intervalMs = 1000 / fps;
    CreateDirectoryW(outDir.c_str(), nullptr);
    int saved = 0;
    for (int i = 0; i < count; ++i) {
        DWORD t0 = GetTickCount();
        wchar_t fn[64]; wsprintfW(fn, L"%s%04d.png", prefix.c_str(), i);
        std::wstring full = outDir + L"\\" + fn;
        std::string u8 = net2WToU8(full);
        if (captureRegionToPng(x, y, w, h, u8)) saved++;
        DWORD elapsed = GetTickCount() - t0;
        if ((int)elapsed < intervalMs) Sleep(intervalMs - elapsed);
    }
    return saved;
}

} // namespace notepad

// ============================================================================
//  [세트 AK] HTTP 잡 풀 - 병렬 요청 · 재시도(지수 백오프+지터) · 타임아웃 · 동시수 제한
//  · 전역 rate-limit 게이트(429/503 Retry-After) · 연속 실패 시 동시수 자동 강등  (2026-09-07)
//
//  AHK v1 은 단일 스레드라 HTTP 를 N개 동시에 던지고 회수하는 구조가 약하다.
//  여기서는 잡을 큐에 넣고 워커 스레드(최대 동시수)가 WinHTTP 동기 호출로 처리한다.
//  AHK 쪽은 Start 로 id 를 받고 WaitAny/Status/Result 로 회수만 하면 된다.
//
//  상태(state): 0=대기 1=실행중 2=완료(HTTP 응답 수신, httpStatus 참조)
//               3=실패(전송 실패·타임아웃·취소, errCode 참조)
//  재시도 대상: 전송 실패/타임아웃, HTTP 408/429/502/503/504
//  (500 은 요청 자체가 잘못된 경우가 많아 재시도하지 않는다)
// ============================================================================
#include <deque>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <random>
#include <cwctype>

#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 0x00000003
#endif

namespace notepad {

struct HttpJob {
    int id = 0;
    std::wstring method, url, headers;   // headers: "K: V\r\nK2: V2" (Host/Content-Length/Connection/Accept-Encoding 는 제거됨)
    std::string  body;                   // UTF-8
    int timeoutMs = 30000;
    int maxRetry  = 3;                   // 첫 시도 이후 재시도 횟수 (총 시도 = maxRetry+1)
    std::atomic<int> state{ 0 };
    std::atomic<int> httpStatus{ 0 };
    std::atomic<int> tries{ 0 };
    std::atomic<int> errCode{ 0 };       // Win32/WinHTTP 오류코드, -1=취소, -2=전체 마감 초과
    std::atomic<int> elapsedMs{ 0 };
    std::string  response;               // UTF-8 본문 (state>=2 이후 접근)
    std::wstring respHeaders;
    std::wstring errText;
    bool reported = false;               // WaitAny 로 이미 반환됨
};

static std::mutex                 g_hjMutex;
static std::condition_variable    g_hjCv;       // 워커 깨우기
static std::condition_variable    g_hjDoneCv;   // WaitAny 깨우기
static std::map<int, std::shared_ptr<HttpJob>> g_hjJobs;
static std::deque<std::shared_ptr<HttpJob>>    g_hjQueue;
static int  g_hjMaxConc   = 6;
static int  g_hjActive    = 0;
static int  g_hjWorkers   = 0;
static int  g_hjDownshift = 0;
static int  g_hjConsecFail = 0;
static long long g_hjNotBefore = 0;     // 전역 게이트: 이 시각(GetTickCount64) 전에는 새 시도 금지
static long long g_hjMinGapMs  = 0;     // 요청 시작 최소 간격(ms), 0=제한 없음
static long long g_hjLastStart = 0;
static std::atomic<int>  g_hjNextId{ 1 };
static std::atomic<bool> g_hjCancel{ false };
static HINTERNET g_hjSession = nullptr;

static HINTERNET hjSession() {
    std::lock_guard<std::mutex> lk(g_hjMutex);
    if (!g_hjSession)
        g_hjSession = WinHttpOpen(L"NOTEPAD-HTTPJOB/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return g_hjSession;
}

static std::wstring hjErrText(DWORD err) {
    if (err == (DWORD)-1) return L"cancelled";
    if (err == (DWORD)-2) return L"deadline exceeded";
    wchar_t buf[512]{};
    DWORD n = 0;
    if (err >= 12000 && err < 13000) {
        HMODULE hm = GetModuleHandleW(L"winhttp.dll");
        if (hm) n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, hm, err, 0, buf, 511, nullptr);
    }
    if (!n) n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, buf, 511, nullptr);
    std::wstring s = n ? buf : L"";
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    wchar_t code[32]; wsprintfW(code, L"[%u] ", (unsigned)err);
    return code + s;
}

// 헤더 문자열 정리: 줄 단위로 나눠 WinHTTP 가 스스로 관리하는 헤더는 제거
static std::wstring hjCleanHeaders(const std::wstring& in) {
    std::wstring out;
    size_t pos = 0;
    while (pos <= in.size()) {
        size_t nl = in.find(L'\n', pos);
        std::wstring line = in.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = (nl == std::wstring::npos) ? in.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        size_t b = 0; while (b < line.size() && line[b] == L' ') ++b;
        line = line.substr(b);
        if (line.empty()) continue;
        std::wstring lower = line;
        for (auto& c : lower) c = (wchar_t)towlower(c);
        if (lower.rfind(L"host:", 0) == 0 || lower.rfind(L"content-length:", 0) == 0 ||
            lower.rfind(L"connection:", 0) == 0 || lower.rfind(L"accept-encoding:", 0) == 0)
            continue;
        out += line; out += L"\r\n";
    }
    return out;
}

static long long hjNow() { return (long long)GetTickCount64(); }

// 취소 감지하며 잠자기
static bool hjSleep(long long ms) {
    while (ms > 0) {
        if (g_hjCancel) return false;
        long long s = ms < 100 ? ms : 100;
        Sleep((DWORD)s); ms -= s;
    }
    return !g_hjCancel;
}

static long long hjBackoffMs(int attempt) {
    // 800ms * 2^(attempt-1) + 지터(0~300ms), 최대 10초
    static thread_local std::mt19937 rng{ std::random_device{}() };
    long long base = 800LL << (attempt - 1 < 5 ? attempt - 1 : 5);
    if (base > 10000) base = 10000;
    return base + (long long)(rng() % 301);
}

// 전역 게이트 + 최소 간격 대기 (새 시도 직전)
static bool hjWaitGate() {
    for (;;) {
        long long waitMs = 0;
        {
            std::lock_guard<std::mutex> lk(g_hjMutex);
            long long now = hjNow();
            long long allow = now;
            if (g_hjNotBefore > allow) allow = g_hjNotBefore;
            if (g_hjMinGapMs > 0 && g_hjLastStart + g_hjMinGapMs > allow) allow = g_hjLastStart + g_hjMinGapMs;
            if (allow <= now) { g_hjLastStart = now; return true; }
            waitMs = allow - now;
            if (waitMs <= 100) { g_hjLastStart = allow; }   // 곧 차례: 예약하고 짧게 잔다
        }
        if (waitMs <= 100) return hjSleep(waitMs);
        if (!hjSleep(waitMs > 200 ? 200 : waitMs)) return false;
    }
}

static void hjNoteFailure(int status, long long retryAfterMs, int attempt) {
    std::lock_guard<std::mutex> lk(g_hjMutex);
    g_hjConsecFail++;
    if (status == 429 || status == 503) {
        long long w = retryAfterMs > 0 ? retryAfterMs : hjBackoffMs(attempt);
        long long nb = hjNow() + w;
        if (nb > g_hjNotBefore) g_hjNotBefore = nb;
    }
    if (g_hjConsecFail >= 3 && g_hjMaxConc > 1) {
        g_hjMaxConc = g_hjMaxConc / 2; if (g_hjMaxConc < 1) g_hjMaxConc = 1;
        g_hjDownshift++;
        g_hjConsecFail = 0;
    }
}
static void hjNoteSuccess() {
    std::lock_guard<std::mutex> lk(g_hjMutex);
    g_hjConsecFail = 0;
}

// 1회 요청. 전송 성공(응답 수신) true / 전송 실패 false(err).
static bool hjDoRequest(const HttpJob& job, int& status, DWORD& err, std::string& resp,
                        std::wstring& respHeaders, long long& retryAfterMs) {
    status = 0; err = 0; resp.clear(); respHeaders.clear(); retryAfterMs = -1;
    std::wstring host, path; INTERNET_PORT port; bool https;
    if (!crackUrl(job.url, host, port, path, https)) { err = ERROR_INVALID_PARAMETER; return false; }
    HINTERNET hs = hjSession();
    if (!hs) { err = GetLastError(); return false; }
    HINTERNET hc = WinHttpConnect(hs, host.c_str(), port, 0);
    if (!hc) { err = GetLastError(); return false; }
    HINTERNET hr = WinHttpOpenRequest(hc, job.method.c_str(), path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) { err = GetLastError(); WinHttpCloseHandle(hc); return false; }

    int t = job.timeoutMs > 0 ? job.timeoutMs : 30000;
    int tc = t < 15000 ? t : 15000;
    WinHttpSetTimeouts(hr, tc, tc, t, t);
    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(hr, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof(dec));   // gzip/deflate 자동 해제(Win8.1+)
    DWORD dis = WINHTTP_DISABLE_COOKIES;                                     // 쿠키는 호출자가 헤더로 직접 관리
    WinHttpSetOption(hr, WINHTTP_OPTION_DISABLE_FEATURE, &dis, sizeof(dis));

    const wchar_t* hdr = job.headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : job.headers.c_str();
    DWORD hdrLen = job.headers.empty() ? 0 : (DWORD)-1L;
    void* data = job.body.empty() ? WINHTTP_NO_REQUEST_DATA : (void*)job.body.data();
    DWORD dlen = (DWORD)job.body.size();

    bool ok = WinHttpSendRequest(hr, hdr, hdrLen, data, dlen, dlen, 0) != FALSE
           && WinHttpReceiveResponse(hr, nullptr) != FALSE;
    if (!ok) { err = GetLastError(); WinHttpCloseHandle(hr); WinHttpCloseHandle(hc); return false; }

    DWORD sc = 0, len = sizeof(sc);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &sc, &len, WINHTTP_NO_HEADER_INDEX);
    status = (int)sc;

    wchar_t ra[64]{}; DWORD raLen = sizeof(ra);
    if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, ra, &raLen, WINHTTP_NO_HEADER_INDEX)) {
        long v = wcstol(ra, nullptr, 10);
        if (v > 0) retryAfterMs = (v > 60 ? 60 : v) * 1000LL;   // 초 단위, 최대 60초
    }
    DWORD hl = 0;
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hl, WINHTTP_NO_HEADER_INDEX);
    if (hl) {
        std::wstring h(hl / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &h[0], &hl, WINHTTP_NO_HEADER_INDEX)) {
            h.resize(hl / sizeof(wchar_t));
            while (!h.empty() && h.back() == L'\0') h.pop_back();
            respHeaders = h;
        }
    }

    std::vector<char> buf(65536); DWORD avail = 0;
    for (;;) {
        if (!WinHttpQueryDataAvailable(hr, &avail)) { err = GetLastError(); ok = false; break; }
        if (!avail) break;
        DWORD rd = 0;
        if (!WinHttpReadData(hr, buf.data(), (DWORD)std::min<DWORD>(avail, (DWORD)buf.size()), &rd)) { err = GetLastError(); ok = false; break; }
        if (!rd) break;
        resp.append(buf.data(), rd);
    }
    WinHttpCloseHandle(hr); WinHttpCloseHandle(hc);
    return ok;
}

static bool hjRetryableStatus(int s) {
    return s == 408 || s == 429 || s == 502 || s == 503 || s == 504;
}

static void hjRun(const std::shared_ptr<HttpJob>& job) {
    job->state = 1;
    long long t0 = hjNow();
    int attempt = 0;
    for (;;) {
        if (g_hjCancel || !hjWaitGate()) { job->errCode = -1; job->errText = L"cancelled"; job->state = 3; break; }
        attempt++; job->tries = attempt;
        int status = 0; DWORD err = 0; std::string resp; std::wstring rh; long long retryAfterMs = -1;
        bool transportOk = hjDoRequest(*job, status, err, resp, rh, retryAfterMs);
        bool retryable = !transportOk || hjRetryableStatus(status);
        if (transportOk) {
            job->response = std::move(resp);
            job->respHeaders = std::move(rh);
            job->httpStatus = status;
        }
        if (retryable) {
            hjNoteFailure(transportOk ? status : 0, retryAfterMs, attempt);
            if (!transportOk) job->errText = hjErrText(err);
            if (attempt <= job->maxRetry) {
                long long wait = retryAfterMs > 0 ? retryAfterMs : hjBackoffMs(attempt);
                if (!hjSleep(wait)) { job->errCode = -1; job->errText = L"cancelled"; job->state = 3; break; }
                continue;
            }
        } else {
            hjNoteSuccess();
        }
        job->errCode = transportOk ? 0 : (int)err;
        job->state = transportOk ? 2 : 3;
        break;
    }
    job->elapsedMs = (int)(hjNow() - t0);
}

static void hjWorker() {
    for (;;) {
        std::shared_ptr<HttpJob> job;
        {
            std::unique_lock<std::mutex> lk(g_hjMutex);
            bool got = g_hjCv.wait_for(lk, std::chrono::seconds(2),
                [] { return !g_hjQueue.empty() && g_hjActive < g_hjMaxConc; });
            if (!got) {
                if (g_hjQueue.empty()) { g_hjWorkers--; return; }   // 할 일 없음: 워커 종료
                continue;
            }
            job = g_hjQueue.front(); g_hjQueue.pop_front();
            g_hjActive++;
        }
        hjRun(job);
        { std::lock_guard<std::mutex> lk(g_hjMutex); g_hjActive--; }
        g_hjDoneCv.notify_all();
        g_hjCv.notify_one();
    }
}

// ---- 공개 API -------------------------------------------------------------
int httpJobStart(const std::wstring& method, const std::wstring& url, const std::wstring& headers,
                 const std::wstring& body, int timeoutMs, int maxRetry) {
    if (url.empty()) return 0;
    auto job = std::make_shared<HttpJob>();
    job->id = g_hjNextId++;
    job->method = method.empty() ? L"GET" : method;
    for (auto& c : job->method) c = (wchar_t)towupper(c);
    job->url = url;
    job->headers = hjCleanHeaders(headers);
    job->body = net2WToU8(body);
    job->timeoutMs = timeoutMs > 0 ? timeoutMs : 30000;
    job->maxRetry = maxRetry < 0 ? 0 : (maxRetry > 10 ? 10 : maxRetry);
    {
        std::lock_guard<std::mutex> lk(g_hjMutex);
        g_hjJobs[job->id] = job;
        g_hjQueue.push_back(job);
        if (g_hjWorkers < g_hjMaxConc) {
            g_hjWorkers++;
            try { std::thread(hjWorker).detach(); }
            catch (...) { g_hjWorkers--; }
        }
    }
    g_hjCv.notify_one();
    return job->id;
}

static std::shared_ptr<HttpJob> hjFind(int id) {
    std::lock_guard<std::mutex> lk(g_hjMutex);
    auto it = g_hjJobs.find(id);
    return it == g_hjJobs.end() ? nullptr : it->second;
}

// "state,httpStatus,tries,errCode,elapsedMs"  (없는 id 면 "")
std::wstring httpJobStatus(int id) {
    auto j = hjFind(id); if (!j) return L"";
    wchar_t buf[128];
    wsprintfW(buf, L"%d,%d,%d,%d,%d", (int)j->state.load(), (int)j->httpStatus.load(),
              (int)j->tries.load(), (int)j->errCode.load(), (int)j->elapsedMs.load());
    return buf;
}

// 응답 본문(UTF-8 → UTF-16, UTF-8 이 아니면 ANSI 로 재해석). 미완료/없음 "".
std::wstring httpJobResult(int id) {
    auto j = hjFind(id); if (!j || j->state < 2) return L"";
    const std::string& s = j->response;
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    UINT cp = CP_UTF8; DWORD fl = MB_ERR_INVALID_CHARS;
    if (n <= 0) { cp = CP_ACP; fl = 0; n = MultiByteToWideChar(cp, fl, s.data(), (int)s.size(), nullptr, 0); }
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, fl, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring httpJobHeaders(int id) {
    auto j = hjFind(id); if (!j || j->state < 2) return L"";
    return j->respHeaders;
}
std::wstring httpJobError(int id) {
    auto j = hjFind(id); if (!j) return L"";
    if (j->state == 3 && j->errText.empty()) return hjErrText((DWORD)j->errCode.load());
    return j->errText;
}
int httpJobFree(int id) {
    std::lock_guard<std::mutex> lk(g_hjMutex);
    auto it = g_hjJobs.find(id);
    if (it == g_hjJobs.end()) return 0;
    // 큐에 아직 있으면 제거(실행 중이면 워커가 shared_ptr 을 들고 있어 안전하게 끝남)
    for (auto q = g_hjQueue.begin(); q != g_hjQueue.end(); ++q)
        if ((*q)->id == id) { (*q)->state = 3; (*q)->errCode = -1; g_hjQueue.erase(q); break; }
    g_hjJobs.erase(it);
    return 1;
}

// 완료(state>=2)된 잡 중 아직 반환하지 않은 것 하나의 id. timeoutMs 안에 없으면 0.
int httpJobWaitAny(int timeoutMs) {
    std::unique_lock<std::mutex> lk(g_hjMutex);
    auto pick = [&]() -> int {
        for (auto& kv : g_hjJobs)
            if (!kv.second->reported && kv.second->state >= 2) { kv.second->reported = true; return kv.first; }
        return 0;
    };
    int id = pick(); if (id) return id;
    if (timeoutMs <= 0) return 0;
    // 미완료 잡이 하나도 없으면 기다릴 이유가 없다
    bool anyPending = false;
    for (auto& kv : g_hjJobs) if (kv.second->state < 2) { anyPending = true; break; }
    if (!anyPending) return 0;
    g_hjDoneCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
        for (auto& kv : g_hjJobs) if (!kv.second->reported && kv.second->state >= 2) return true;
        return false;
    });
    return pick();
}

// 동시수 설정(1~32). 새 배치 시작 전에 호출: 취소 플래그·연속실패·강등 카운터·게이트 초기화. 반환: 적용값.
int httpJobSetConcurrency(int n) {
    if (n < 1) n = 1; if (n > 32) n = 32;
    { std::lock_guard<std::mutex> lk(g_hjMutex);
      g_hjMaxConc = n; g_hjConsecFail = 0; g_hjDownshift = 0; g_hjNotBefore = 0; }
    g_hjCancel = false;
    g_hjCv.notify_all();
    return n;
}
int httpJobSetMinGap(int ms) {
    std::lock_guard<std::mutex> lk(g_hjMutex);
    g_hjMinGapMs = ms < 0 ? 0 : ms;
    return (int)g_hjMinGapMs;
}
// 대기 중 잡은 즉시 실패(-1), 실행 중 잡은 현재 시도 후 재시도 없이 종료. 반환: 취소된 대기 잡 수.
int httpJobCancelAll() {
    g_hjCancel = true;
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_hjMutex);
        for (auto& j : g_hjQueue) { j->state = 3; j->errCode = -1; j->errText = L"cancelled"; n++; }
        g_hjQueue.clear();
    }
    g_hjDoneCv.notify_all();
    g_hjCv.notify_all();
    return n;
}
// "maxConc,active,queued,pending,downshifts,gateWaitMs,minGapMs"
std::wstring httpJobPoolInfo() {
    std::lock_guard<std::mutex> lk(g_hjMutex);
    int pending = 0;
    for (auto& kv : g_hjJobs) if (kv.second->state < 2) pending++;
    long long gate = g_hjNotBefore - hjNow(); if (gate < 0) gate = 0;
    wchar_t buf[160];
    wsprintfW(buf, L"%d,%d,%d,%d,%d,%d,%d", g_hjMaxConc, g_hjActive, (int)g_hjQueue.size(), pending,
              g_hjDownshift, (int)gate, (int)g_hjMinGapMs);
    return buf;
}

} // namespace notepad

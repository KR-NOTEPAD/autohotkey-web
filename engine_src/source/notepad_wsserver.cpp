// ============================================================================
//  NOTEPAD.dll  -  notepad_wsserver.cpp
//  [NOTEPAD] 세트 AN — 로컬 WebSocket 서버 (RFC 6455, 127.0.0.1 전용)
//
//  용도: 크롬 확장(cdp_extension) ↔ AHK 브리지. 확장이 ws://127.0.0.1:PORT 로
//        접속해 오면 텍스트 메시지를 큐에 쌓고, AHK 가 Recv 로 꺼내 쓴다.
//        Send 는 접속된 모든 클라이언트에 브로드캐스트.
//  규약: 문자열 UTF-16, 출력버퍼 2단계 호출(cap=0 → 필요 길이, 재호출 → 복사).
//        Recv 는 cap=0 호출에서 메시지를 "머리(head)"에 고정해 두고, 버퍼 호출에서
//        복사 후 pop 하므로 2단계 호출로 메시지가 소실되지 않는다.
//  의존: ws2_32, bcrypt(SHA-1), crypt32(Base64). 외부 런타임 없음.
// ============================================================================
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>

#pragma comment(lib, "ws2_32.lib")

#ifndef NOTEPAD_API
#define NOTEPAD_API extern "C" __declspec(dllexport)
#endif

namespace notepad { namespace wss {

// ---------------------------------------------------------------- 상태 ----
struct Client {
    SOCKET s = INVALID_SOCKET;
    std::thread th;
    std::atomic<bool> alive{ false };   // 핸드셰이크 완료 후 true, 종료 시 false
    std::atomic<bool> done{ false };    // 스레드 종료(정리/join 가능)
    std::mutex sendMx;
};

static std::atomic<bool>                     g_running{ false };
static SOCKET                                g_listen = INVALID_SOCKET;
static int                                   g_port = 0;
static std::thread                           g_acceptTh;
static std::mutex                            g_mx;                 // 클라이언트 목록/큐 보호
static std::condition_variable               g_cv;
static std::vector<std::shared_ptr<Client>>  g_clients;
static std::deque<std::string>               g_queue;              // 수신 메시지(UTF-8)
static std::wstring                          g_head;               // 2단계 호출용 머리 메시지
static bool                                  g_hasHead = false;
static std::wstring                          g_lastErr;
static std::atomic<long long>                g_rxCount{ 0 }, g_txCount{ 0 };

static void setErr(const wchar_t* w) { std::lock_guard<std::mutex> lk(g_mx); g_lastErr = w; }

// ---------------------------------------------------------------- 유틸 ----
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
// SHA-1(bcrypt) → Base64 (핸드셰이크 Accept 키)
static std::string acceptKey(const std::string& clientKey) {
    std::string src = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char hash[20] = { 0 };
    BCRYPT_ALG_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return "";
    BCryptHash(h, nullptr, 0, (PUCHAR)src.data(), (ULONG)src.size(), hash, sizeof(hash));
    BCryptCloseAlgorithmProvider(h, 0);
    DWORD cch = 0;
    CryptBinaryToStringA(hash, sizeof(hash), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &cch);
    std::string b64(cch, '\0');
    CryptBinaryToStringA(hash, sizeof(hash), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64[0], &cch);
    while (!b64.empty() && (b64.back() == '\0')) b64.pop_back();
    return b64;
}
static bool sendAll(SOCKET s, const char* p, size_t n) {
    while (n > 0) {
        int k = send(s, p, (int)n, 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
static bool recvExact(SOCKET s, unsigned char* p, size_t n) {
    while (n > 0) {
        int k = recv(s, (char*)p, (int)n, 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
// 프레임 조립(서버→클라이언트: 마스크 없음)
static std::string buildFrame(uint8_t opcode, const std::string& payload) {
    std::string f;
    f.push_back((char)(0x80 | (opcode & 0x0F)));
    size_t n = payload.size();
    if (n < 126) f.push_back((char)n);
    else if (n <= 0xFFFF) { f.push_back((char)126); f.push_back((char)(n >> 8)); f.push_back((char)(n & 0xFF)); }
    else { f.push_back((char)127); for (int i = 7; i >= 0; --i) f.push_back((char)((n >> (i * 8)) & 0xFF)); }
    f += payload;
    return f;
}
static bool sendFrame(const std::shared_ptr<Client>& c, uint8_t opcode, const std::string& payload) {
    std::lock_guard<std::mutex> lk(c->sendMx);
    std::string f = buildFrame(opcode, payload);
    return sendAll(c->s, f.data(), f.size());
}
static void pushMessage(std::string&& msg) {
    { std::lock_guard<std::mutex> lk(g_mx); g_queue.push_back(std::move(msg)); }
    g_rxCount++;
    g_cv.notify_all();
}

// ---------------------------------------------------------------- 클라이언트 ----
static bool doHandshake(SOCKET s) {
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos) {
        int k = recv(s, buf, sizeof(buf), 0);
        if (k <= 0) return false;
        req.append(buf, k);
        if (req.size() > 65536) return false;
    }
    // Sec-WebSocket-Key 추출(대소문자 무시)
    std::string lower = req;
    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
    size_t p = lower.find("sec-websocket-key:");
    if (p == std::string::npos) {
        const char* bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        sendAll(s, bad, strlen(bad));
        return false;
    }
    p += 18;
    size_t e = req.find("\r\n", p);
    std::string key = req.substr(p, e - p);
    while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r')) key.pop_back();
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " + acceptKey(key) + "\r\n\r\n";
    return sendAll(s, resp.data(), resp.size());
}

static void clientLoop(std::shared_ptr<Client> c) {
    SOCKET s = c->s;
    if (!doHandshake(s)) { c->alive = false; closesocket(s); c->done = true; return; }
    c->alive = true;
    pushMessage("{\"event\":\"__open\"}");
    std::string fragment; uint8_t fragOp = 0;
    while (g_running) {
        unsigned char h[2];
        if (!recvExact(s, h, 2)) break;
        bool fin = (h[0] & 0x80) != 0;
        uint8_t op = h[0] & 0x0F;
        bool masked = (h[1] & 0x80) != 0;
        uint64_t len = h[1] & 0x7F;
        if (len == 126) { unsigned char x[2]; if (!recvExact(s, x, 2)) break; len = ((uint64_t)x[0] << 8) | x[1]; }
        else if (len == 127) { unsigned char x[8]; if (!recvExact(s, x, 8)) break; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | x[i]; }
        if (len > (64ull * 1024 * 1024)) break;                    // 64MB 상한
        unsigned char mk[4] = { 0,0,0,0 };
        if (masked && !recvExact(s, mk, 4)) break;
        std::string payload((size_t)len, '\0');
        if (len && !recvExact(s, (unsigned char*)&payload[0], (size_t)len)) break;
        if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= (char)mk[i & 3];

        if (op == 0x8) { sendFrame(c, 0x8, payload.substr(0, 2)); break; }      // close
        if (op == 0x9) { sendFrame(c, 0xA, payload); continue; }                 // ping → pong
        if (op == 0xA) continue;                                                 // pong
        if (op == 0x1 || op == 0x2 || op == 0x0) {
            if (op != 0x0) { fragment.clear(); fragOp = op; }
            fragment += payload;
            if (fin) { if (fragOp == 0x1) pushMessage(std::move(fragment)); fragment.clear(); }
        }
    }
    c->alive = false;
    shutdown(s, SD_BOTH);
    closesocket(s);
    pushMessage("{\"event\":\"__close\"}");
    c->done = true;
}

static void acceptLoop() {
    while (g_running) {
        sockaddr_in from{}; int fl = sizeof(from);
        SOCKET cs = accept(g_listen, (sockaddr*)&from, &fl);
        if (cs == INVALID_SOCKET) { if (!g_running) break; Sleep(20); continue; }
        // 127.0.0.1 이외 거부(바인딩이 루프백이므로 보조 검사)
        if (from.sin_addr.S_un.S_addr != htonl(INADDR_LOOPBACK)) { closesocket(cs); continue; }
        BOOL nd = TRUE; setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
        auto c = std::make_shared<Client>();
        c->s = cs;
        std::vector<std::shared_ptr<Client>> dead;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            // 죽은 클라이언트 분리(join 은 락 밖에서 — 종료 중 스레드가 큐 락을 잡을 수 있음)
            for (auto it = g_clients.begin(); it != g_clients.end();) {
                if ((*it)->done && (*it)->th.joinable()) { dead.push_back(*it); it = g_clients.erase(it); }
                else ++it;
            }
            g_clients.push_back(c);
        }
        for (auto& d : dead) d->th.join();
        c->th = std::thread(clientLoop, c);
    }
}

// ---------------------------------------------------------------- 공개 로직 ----
static bool serverStart(int port) {
    if (g_running) return g_port == port;
    WSADATA wd; if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) { setErr(L"WSAStartup 실패"); return false; }
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { setErr(L"socket 실패"); return false; }
    BOOL reuse = TRUE; setsockopt(g_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&reuse, sizeof(reuse));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    a.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_listen, (sockaddr*)&a, sizeof(a)) != 0) {
        wchar_t m[128]; swprintf_s(m, L"bind 실패(포트 %d 사용중?) err=%d", port, WSAGetLastError());
        setErr(m); closesocket(g_listen); g_listen = INVALID_SOCKET; return false;
    }
    if (listen(g_listen, 8) != 0) { setErr(L"listen 실패"); closesocket(g_listen); g_listen = INVALID_SOCKET; return false; }
    g_port = port;
    { std::lock_guard<std::mutex> lk(g_mx); g_queue.clear(); g_head.clear(); g_hasHead = false; }
    g_running = true;
    g_acceptTh = std::thread(acceptLoop);
    return true;
}
static void serverStop() {
    if (!g_running) return;
    g_running = false;
    g_cv.notify_all();
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    std::vector<std::shared_ptr<Client>> cs;
    { std::lock_guard<std::mutex> lk(g_mx); cs.swap(g_clients); }
    for (auto& c : cs) { if (c->s != INVALID_SOCKET) { shutdown(c->s, SD_BOTH); closesocket(c->s); } }
    for (auto& c : cs) { if (c->th.joinable()) c->th.join(); }
    if (g_acceptTh.joinable()) g_acceptTh.join();
    { std::lock_guard<std::mutex> lk(g_mx); g_queue.clear(); g_head.clear(); g_hasHead = false; }
    g_cv.notify_all();
    g_port = 0;
}
static int clientCount() {
    std::lock_guard<std::mutex> lk(g_mx);
    int n = 0; for (auto& c : g_clients) if (c->alive) ++n;
    return n;
}
static bool broadcast(const std::wstring& text) {
    std::vector<std::shared_ptr<Client>> cs;
    { std::lock_guard<std::mutex> lk(g_mx); cs = g_clients; }
    std::string u8 = wideToUtf8(text);
    bool any = false;
    for (auto& c : cs) if (c->alive) { if (sendFrame(c, 0x1, u8)) any = true; }
    if (any) g_txCount++;
    return any;
}
// 2단계 수신: cap=0 → (timeoutMs 까지 대기 후) 머리 메시지 길이, 버퍼 호출 → 복사+pop
static int recvMessage(int timeoutMs, wchar_t* out, int cap) {
    std::unique_lock<std::mutex> lk(g_mx);
    if (!g_hasHead) {
        if (g_queue.empty()) {
            if (timeoutMs > 0)
                g_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [] { return !g_queue.empty() || !g_running; });
            if (g_queue.empty()) return 0;
        }
        g_head = utf8ToWide(g_queue.front());
        g_queue.pop_front();
        g_hasHead = true;
    }
    int need = (int)g_head.size();
    if (out == nullptr || cap == 0) return need;
    if (cap < need + 1) return -need;
    memcpy(out, g_head.data(), g_head.size() * sizeof(wchar_t));
    out[g_head.size()] = L'\0';
    g_head.clear(); g_hasHead = false;
    return need;
}
static int pendingCount() {
    std::lock_guard<std::mutex> lk(g_mx);
    return (int)g_queue.size() + (g_hasHead ? 1 : 0);
}

}} // namespace notepad::wss

// ============================================================================
//  export (AHK v1 64비트 DllCall)
// ============================================================================
NOTEPAD_API int __stdcall Notepad_WsServerStart(int port) { return notepad::wss::serverStart(port) ? 1 : 0; }
NOTEPAD_API int __stdcall Notepad_WsServerStop() { notepad::wss::serverStop(); return 1; }
NOTEPAD_API int __stdcall Notepad_WsServerPort() { return notepad::wss::g_port; }
NOTEPAD_API int __stdcall Notepad_WsServerClients() { return notepad::wss::clientCount(); }
NOTEPAD_API int __stdcall Notepad_WsServerPending() { return notepad::wss::pendingCount(); }
NOTEPAD_API int __stdcall Notepad_WsServerSend(const wchar_t* text) {
    return notepad::wss::broadcast(text ? std::wstring(text) : std::wstring()) ? 1 : 0;
}
NOTEPAD_API int __stdcall Notepad_WsServerRecv(int timeoutMs, wchar_t* out, int outCch) {
    return notepad::wss::recvMessage(timeoutMs, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_WsServerLastError(wchar_t* out, int outCch) {
    std::wstring e; { std::lock_guard<std::mutex> lk(notepad::wss::g_mx); e = notepad::wss::g_lastErr; }
    int need = (int)e.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, e.data(), e.size() * sizeof(wchar_t)); out[e.size()] = L'\0';
    return need;
}

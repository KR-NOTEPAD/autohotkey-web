// ============================================================================
//  NOTEPAD.dll  -  notepad_httpserver.cpp
//  [NOTEPAD] 세트 AQ — 로컬 HTTP/1.1 요청·응답 서버 (2026-10)
//
//  구조: DLL 수신 스레드(접속당 1스레드)가 요청을 파싱해 큐에 쌓고,
//        AHK 가 Recv 로 꺼내 Reply 로 응답한다(AHK 는 단일 스레드라 소켓 서버를
//        직접 못 돌리므로 네트워크 I/O 는 전부 C++ 쪽).
//        staticRoot 를 주면 그 폴더의 파일은 C++ 가 직접 서빙(파이썬 http.server 대체).
//        파일이 없는 경로(/api/... 등)만 AHK 큐로 넘어간다.
//  flags: 1=LAN 허용(0.0.0.0 바인딩, 기본은 127.0.0.1 전용)
//         2=CORS(Access-Control-Allow-Origin:* + OPTIONS 자동 204)
//         4=폴더 목록 표시(index.html 없을 때)
//         8=정적 전용(파일 없으면 AHK 로 넘기지 않고 404)
//  지원: keep-alive, Content-Length / chunked 요청 본문, Expect: 100-continue,
//        multipart/form-data·x-www-form-urlencoded 필드, HEAD, Range 미지원.
//  규약: 문자열 UTF-16, 출력버퍼 2단계 호출(cap=0 → 필요 길이, 재호출 → 복사).
//  의존: ws2_32 만. 외부 런타임 없음.
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

#ifndef NOTEPAD_API
#define NOTEPAD_API extern "C" __declspec(dllexport)
#endif

namespace notepad { namespace hsv {

enum { F_LAN = 1, F_CORS = 2, F_LIST = 4, F_STATIC_ONLY = 8 };
static const size_t MAX_HEADER = 64 * 1024;
static const size_t MAX_BODY   = 256ull * 1024 * 1024;   // 256MB

struct Req {
    int id = 0;
    std::string method, target, path, query, version, remote;
    std::vector<std::pair<std::string, std::string>> headers;   // 이름 소문자
    std::string body;
    // 응답(AHK 가 채움)
    bool replied = false;
    bool isFile = false;
    int status = 0;
    std::string ctype, extra, respBody;
    std::wstring filePath;
};
struct Conn {
    SOCKET s = INVALID_SOCKET;
    std::thread th;
    std::atomic<bool> done{ false };
};

static std::atomic<bool>                   g_running{ false };
static SOCKET                              g_listen = INVALID_SOCKET;
static int                                 g_port = 0;
static int                                 g_flags = 0;
static std::wstring                        g_root;              // 정적 루트(끝 '\' 없음), 빈값=없음
static std::thread                         g_acceptTh;
static std::mutex                          g_mx;
static std::condition_variable             g_cvQueue, g_cvReply;
static std::deque<std::shared_ptr<Req>>    g_queue;
static std::map<int, std::shared_ptr<Req>> g_live;              // 응답 대기 중(id → 요청)
static std::vector<std::shared_ptr<Conn>>  g_conns;
static std::wstring                        g_head;
static bool                                g_hasHead = false;
static std::wstring                        g_lastErr;
static int                                 g_nextId = 0;
static std::atomic<int>                    g_timeoutMs{ 30000 };
static std::atomic<long long>              g_nReq{ 0 }, g_nStatic{ 0 }, g_nApi{ 0 }, g_nTimeout{ 0 };

static void setErr(const std::wstring& w) { std::lock_guard<std::mutex> lk(g_mx); g_lastErr = w; }

// ---------------------------------------------------------------- 유틸 ----
static std::wstring u2w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string w2u(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static std::string pctDecode(const std::string& s, bool plusSpace) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size() && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
            o.push_back((char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]))); i += 2;
        } else if (c == '+' && plusSpace) o.push_back(' ');
        else o.push_back(c);
    }
    return o;
}
static std::string pctEncodePath(const std::string& s) {   // 폴더 목록 링크용
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') o.push_back((char)c);
        else { o.push_back('%'); o.push_back(hx[c >> 4]); o.push_back(hx[c & 15]); }
    }
    return o;
}
static std::string htmlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;"; else o.push_back(c);
    }
    return o;
}
static std::string hdr(const Req& r, const std::string& nameLower) {
    for (auto& h : r.headers) if (h.first == nameLower) return h.second;
    return "";
}
// "a=1&b=2" 에서 name 찾기(디코드)
static bool formLookup(const std::string& qs, const std::string& name, std::string& out) {
    size_t p = 0;
    while (p <= qs.size()) {
        size_t e = qs.find('&', p); if (e == std::string::npos) e = qs.size();
        std::string kv = qs.substr(p, e - p);
        size_t eq = kv.find('=');
        std::string k = pctDecode(eq == std::string::npos ? kv : kv.substr(0, eq), true);
        if (k == name) { out = eq == std::string::npos ? "" : pctDecode(kv.substr(eq + 1), true); return true; }
        p = e + 1;
    }
    return false;
}
// 헤더 값의 속성 추출: boundary=..., name="..."
static std::string attr(const std::string& v, const std::string& key) {
    std::string lv = lower(v);
    size_t p = 0;
    while ((p = lv.find(key + "=", p)) != std::string::npos) {
        if (p == 0 || lv[p - 1] == ';' || lv[p - 1] == ' ' || lv[p - 1] == '\t') {
            p += key.size() + 1;
            if (p < v.size() && v[p] == '"') { size_t e = v.find('"', p + 1); return v.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1); }
            size_t e = v.find(';', p);
            return trim(v.substr(p, e == std::string::npos ? std::string::npos : e - p));
        }
        p += key.size();
    }
    return "";
}
struct Part { std::string name, filename, ctype; size_t off = 0, len = 0; };
static std::vector<Part> parseMultipart(const Req& r) {
    std::vector<Part> parts;
    std::string ct = hdr(r, "content-type");
    if (lower(ct).find("multipart/form-data") == std::string::npos) return parts;
    std::string b = attr(ct, "boundary");
    if (b.empty()) return parts;
    std::string delim = "--" + b;
    const std::string& s = r.body;
    size_t p = s.find(delim);
    while (p != std::string::npos) {
        p += delim.size();
        if (s.compare(p, 2, "--") == 0) break;             // 종료 경계
        if (s.compare(p, 2, "\r\n") == 0) p += 2;
        size_t he = s.find("\r\n\r\n", p);
        if (he == std::string::npos) break;
        Part pt;
        std::string hs = s.substr(p, he - p);
        size_t lp = 0;
        while (lp < hs.size()) {
            size_t le = hs.find("\r\n", lp); if (le == std::string::npos) le = hs.size();
            std::string line = hs.substr(lp, le - lp);
            size_t c = line.find(':');
            if (c != std::string::npos) {
                std::string k = lower(trim(line.substr(0, c))), v = trim(line.substr(c + 1));
                if (k == "content-disposition") { pt.name = attr(v, "name"); pt.filename = attr(v, "filename"); }
                else if (k == "content-type") pt.ctype = v;
            }
            lp = le + 2;
        }
        size_t ds = he + 4;
        size_t de = s.find("\r\n" + delim, ds);
        if (de == std::string::npos) break;
        pt.off = ds; pt.len = de - ds;
        parts.push_back(pt);
        p = de + 2;
    }
    return parts;
}

// ---------------------------------------------------------------- 소켓 ----
static bool sendAll(SOCKET s, const char* p, size_t n) {
    while (n > 0) {
        int k = send(s, p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
// 버퍼 inbuf 에 최소 need 바이트가 쌓일 때까지 수신
static bool fill(SOCKET s, std::string& in, size_t need) {
    char buf[65536];
    while (in.size() < need) {
        int k = recv(s, buf, sizeof(buf), 0);
        if (k <= 0) return false;
        in.append(buf, k);
    }
    return true;
}
static bool readLine(SOCKET s, std::string& in, std::string& line) {
    size_t e;
    while ((e = in.find("\r\n")) == std::string::npos) {
        if (in.size() > MAX_HEADER) return false;
        if (!fill(s, in, in.size() + 1)) return false;
    }
    line = in.substr(0, e); in.erase(0, e + 2);
    return true;
}

static const char* statusText(int c) {
    switch (c) {
    case 100: return "Continue"; case 200: return "OK"; case 201: return "Created"; case 204: return "No Content";
    case 301: return "Moved Permanently"; case 302: return "Found"; case 304: return "Not Modified";
    case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden";
    case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 409: return "Conflict";
    case 413: return "Payload Too Large"; case 415: return "Unsupported Media Type"; case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests"; case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error"; case 501: return "Not Implemented"; case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    }
    return "Status";
}
static std::string mimeOf(const std::wstring& path) {
    size_t d = path.find_last_of(L'.');
    std::string e = d == std::wstring::npos ? "" : lower(w2u(path.substr(d + 1)));
    static const std::pair<const char*, const char*> t[] = {
        {"html","text/html; charset=utf-8"},{"htm","text/html; charset=utf-8"},{"css","text/css; charset=utf-8"},
        {"js","text/javascript; charset=utf-8"},{"mjs","text/javascript; charset=utf-8"},{"json","application/json; charset=utf-8"},
        {"txt","text/plain; charset=utf-8"},{"md","text/plain; charset=utf-8"},{"csv","text/csv; charset=utf-8"},
        {"tsv","text/tab-separated-values; charset=utf-8"},{"ini","text/plain; charset=utf-8"},{"ahk","text/plain; charset=utf-8"},
        {"log","text/plain; charset=utf-8"},{"xml","application/xml"},{"svg","image/svg+xml"},
        {"png","image/png"},{"jpg","image/jpeg"},{"jpeg","image/jpeg"},{"gif","image/gif"},{"bmp","image/bmp"},
        {"webp","image/webp"},{"ico","image/x-icon"},{"pdf","application/pdf"},{"wasm","application/wasm"},
        {"mp4","video/mp4"},{"webm","video/webm"},{"mp3","audio/mpeg"},{"wav","audio/wav"},
        {"woff","font/woff"},{"woff2","font/woff2"},{"ttf","font/ttf"},{"zip","application/zip"},{"onnx","application/octet-stream"} };
    for (auto& x : t) if (e == x.first) return x.second;
    return "application/octet-stream";
}
static std::string normExtra(const std::string& h) {   // AHK `n / \r\n 혼용 → 각 줄 \r\n
    std::string o; size_t p = 0;
    while (p < h.size()) {
        size_t e = h.find('\n', p); if (e == std::string::npos) e = h.size();
        std::string line = trim(h.substr(p, e - p));
        if (!line.empty() && line.find(':') != std::string::npos) o += line + "\r\n";
        p = e + 1;
    }
    return o;
}
static std::string corsHeaders() {
    if (!(g_flags & F_CORS)) return "";
    return "Access-Control-Allow-Origin: *\r\n";
}
static bool sendHead(SOCKET s, int status, const std::string& ctype, unsigned long long len,
                     bool keep, const std::string& extra) {
    char line[128];
    sprintf_s(line, "HTTP/1.1 %d %s\r\n", status, statusText(status));
    std::string h = line;
    if (!ctype.empty()) h += "Content-Type: " + ctype + "\r\n";
    h += "Content-Length: " + std::to_string(len) + "\r\n";
    h += keep ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    h += "Server: AutoHotkeyWeb\r\n";
    h += corsHeaders();
    h += extra;
    h += "\r\n";
    return sendAll(s, h.data(), h.size());
}
static bool sendSimple(SOCKET s, int status, const std::string& ctype, const std::string& body,
                       bool keep, bool head, const std::string& extra = "") {
    if (!sendHead(s, status, ctype, body.size(), keep, extra)) return false;
    return head || body.empty() || sendAll(s, body.data(), body.size());
}
static bool sendFileBody(SOCKET s, const std::wstring& path, int status, const std::string& ctype,
                         bool keep, bool head, const std::string& extra) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return sendSimple(s, 404, "text/plain; charset=utf-8", "404 Not Found", keep, head);
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    bool ok = sendHead(s, status, ctype.empty() ? mimeOf(path) : ctype, (unsigned long long)sz.QuadPart, keep, extra);
    if (ok && !head) {
        std::vector<char> buf(1 << 18);
        DWORD rd = 0;
        while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd > 0) ok = sendAll(s, buf.data(), rd);
    }
    CloseHandle(f);
    return ok;
}

// ---------------------------------------------------------------- 정적 파일 ----
// 반환: 1=응답함, 0=해당 파일 없음(→ AHK 로), -1=전송 실패
static int tryStatic(SOCKET s, const Req& r, bool keep) {
    std::wstring root; int flags;
    { std::lock_guard<std::mutex> lk(g_mx); root = g_root; flags = g_flags; }
    if (root.empty()) return 0;
    bool head = (r.method == "HEAD");
    // 경로 검사: .. / 드라이브 / 대체 스트림 / 역슬래시 차단
    std::string p = r.path;
    if (p.empty() || p[0] != '/') return 0;
    if (p.find('\\') != std::string::npos || p.find(':') != std::string::npos || p.find('\0') != std::string::npos) return 0;
    size_t q = 0;
    while (q < p.size()) {
        size_t e = p.find('/', q + 1); if (e == std::string::npos) e = p.size();
        std::string seg = p.substr(q + 1, e - q - 1);
        if (seg == ".." || seg == ".") return 0;
        q = e;
    }
    std::wstring rel = u2w(p);
    for (auto& c : rel) if (c == L'/') c = L'\\';
    std::wstring full = root + rel;
    DWORD a = GetFileAttributesW(full.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (p.back() != '/') {
            std::string loc = pctEncodePath(p) + "/" + (r.query.empty() ? "" : "?" + r.query);
            return sendSimple(s, 301, "text/plain; charset=utf-8", "moved", keep, head, "Location: " + loc + "\r\n") ? 1 : -1;
        }
        std::wstring idx = full + L"index.html";
        DWORD ia = GetFileAttributesW(idx.c_str());
        if (ia != INVALID_FILE_ATTRIBUTES && !(ia & FILE_ATTRIBUTE_DIRECTORY)) {
            g_nStatic++;
            return sendFileBody(s, idx, 200, "", keep, head, "Cache-Control: no-cache\r\n") ? 1 : -1;
        }
        if (!(flags & F_LIST)) return 0;
        // 폴더 목록
        std::vector<std::pair<std::string, bool>> items;
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((full + L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                items.push_back({ w2u(n), (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 });
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(items.begin(), items.end(), [](const std::pair<std::string, bool>& x, const std::pair<std::string, bool>& y) {
            if (x.second != y.second) return x.second; return x.first < y.first; });
        std::string html = "<!doctype html><meta charset=utf-8><title>" + htmlEsc(p) + "</title>"
            "<style>body{font:14px/1.6 sans-serif;margin:24px}a{text-decoration:none}</style><h3>" + htmlEsc(p) + "</h3><ul>";
        if (p != "/") html += "<li><a href=\"../\">../</a></li>";
        for (auto& it : items) {
            std::string nm = it.first + (it.second ? "/" : "");
            html += "<li><a href=\"" + pctEncodePath(nm) + "\">" + htmlEsc(nm) + "</a></li>";
        }
        html += "</ul>";
        g_nStatic++;
        return sendSimple(s, 200, "text/html; charset=utf-8", html, keep, head) ? 1 : -1;
    }
    g_nStatic++;
    return sendFileBody(s, full, 200, "", keep, head, "Cache-Control: no-cache\r\n") ? 1 : -1;
}

// ---------------------------------------------------------------- 요청 수신 ----
// 반환: 1=정상, 0=연결 끊김, 음수=-HTTP 상태(오류 응답 후 종료)
static int readRequest(SOCKET s, std::string& in, Req& r) {
    std::string line;
    do { if (!readLine(s, in, line)) return 0; } while (line.empty());     // 앞쪽 빈 줄 허용
    size_t a = line.find(' '), b = line.rfind(' ');
    if (a == std::string::npos || b == a) return -400;
    r.method = line.substr(0, a);
    r.target = line.substr(a + 1, b - a - 1);
    r.version = line.substr(b + 1);
    if (r.version.compare(0, 5, "HTTP/") != 0) return -400;
    size_t total = line.size();
    while (true) {
        if (!readLine(s, in, line)) return 0;
        if (line.empty()) break;
        total += line.size();
        if (total > MAX_HEADER) return -431;
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        r.headers.push_back({ lower(trim(line.substr(0, c))), trim(line.substr(c + 1)) });
    }
    // 대상 → 경로/쿼리 (절대 URI 형식도 허용)
    std::string t = r.target;
    if (t.compare(0, 7, "http://") == 0) { size_t sl = t.find('/', 7); t = sl == std::string::npos ? "/" : t.substr(sl); }
    size_t qm = t.find('?');
    r.path = pctDecode(qm == std::string::npos ? t : t.substr(0, qm), false);
    r.query = qm == std::string::npos ? "" : t.substr(qm + 1);
    size_t hs = r.path.find('#'); if (hs != std::string::npos) r.path.erase(hs);

    if (lower(hdr(r, "expect")) == "100-continue") {
        const char* cont = "HTTP/1.1 100 Continue\r\n\r\n";
        sendAll(s, cont, strlen(cont));
    }
    std::string te = lower(hdr(r, "transfer-encoding"));
    if (te.find("chunked") != std::string::npos) {
        while (true) {
            if (!readLine(s, in, line)) return 0;
            size_t sz = strtoull(line.c_str(), nullptr, 16);
            if (sz == 0) { do { if (!readLine(s, in, line)) return 0; } while (!line.empty()); break; }
            if (r.body.size() + sz > MAX_BODY) return -413;
            if (!fill(s, in, sz + 2)) return 0;
            r.body.append(in, 0, sz); in.erase(0, sz + 2);
        }
    } else {
        std::string cl = hdr(r, "content-length");
        if (!cl.empty()) {
            unsigned long long n = strtoull(cl.c_str(), nullptr, 10);
            if (n > MAX_BODY) return -413;
            if (!fill(s, in, (size_t)n)) return 0;
            r.body.assign(in, 0, (size_t)n); in.erase(0, (size_t)n);
        }
    }
    return 1;
}

static void connLoop(std::shared_ptr<Conn> c, std::string remote) {
    SOCKET s = c->s;
    DWORD tmo = 120000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    std::string in;
    while (g_running) {
        auto r = std::make_shared<Req>();
        r->remote = remote;
        int rc = readRequest(s, in, *r);
        if (rc == 0) break;
        if (rc < 0) { sendSimple(s, -rc, "text/plain; charset=utf-8", statusText(-rc), false, false); break; }
        g_nReq++;
        std::string conn = lower(hdr(*r, "connection"));
        bool keep = (r->version == "HTTP/1.1") ? (conn.find("close") == std::string::npos)
                                               : (conn.find("keep-alive") != std::string::npos);
        bool head = (r->method == "HEAD");
        int flags; { std::lock_guard<std::mutex> lk(g_mx); flags = g_flags; }

        // CORS 사전요청
        if (r->method == "OPTIONS" && (flags & F_CORS)) {
            std::string ah = hdr(*r, "access-control-request-headers");
            std::string ex = "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n"
                             "Access-Control-Allow-Headers: " + (ah.empty() ? std::string("*") : ah) + "\r\n"
                             "Access-Control-Max-Age: 86400\r\n";
            if (!sendSimple(s, 204, "", "", keep, true, ex)) break;
            if (!keep) break; else continue;
        }
        // 정적 파일
        if (r->method == "GET" || r->method == "HEAD") {
            int st = tryStatic(s, *r, keep);
            if (st < 0) break;
            if (st > 0) { if (!keep) break; else continue; }
        }
        if (flags & F_STATIC_ONLY) {
            if (!sendSimple(s, 404, "text/plain; charset=utf-8", "404 Not Found", keep, head)) break;
            if (!keep) break; else continue;
        }
        // AHK 로 넘기고 응답 대기
        {
            std::lock_guard<std::mutex> lk(g_mx);
            r->id = ++g_nextId; if (g_nextId >= 0x7FFFFFF0) g_nextId = 0;
            g_live[r->id] = r;
            g_queue.push_back(r);
        }
        g_nApi++;
        g_cvQueue.notify_all();
        bool got;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            got = g_cvReply.wait_for(lk, std::chrono::milliseconds(g_timeoutMs.load()),
                                     [&] { return r->replied || !g_running; }) && r->replied;
            if (!got) {
                r->replied = true;              // 이후 늦은 Reply 무시
                g_live.erase(r->id);
                auto it = std::find(g_queue.begin(), g_queue.end(), r);
                if (it != g_queue.end()) g_queue.erase(it);
            }
        }
        bool ok;
        if (!got) {
            g_nTimeout++;
            ok = sendSimple(s, g_running ? 504 : 503, "text/plain; charset=utf-8",
                            g_running ? "504 handler timeout (AHK did not reply)" : "503 server stopping", false, head);
            break;
        }
        std::string extra = normExtra(r->extra);
        if (r->isFile) ok = sendFileBody(s, r->filePath, r->status, r->ctype, keep, head, extra);
        else           ok = sendSimple(s, r->status, r->ctype, r->respBody, keep, head, extra);
        if (!ok || !keep) break;
    }
    shutdown(s, SD_BOTH);
    closesocket(s);
    c->done = true;
}

static void acceptLoop() {
    while (g_running) {
        sockaddr_in from{}; int fl = sizeof(from);
        SOCKET cs = accept(g_listen, (sockaddr*)&from, &fl);
        if (cs == INVALID_SOCKET) { if (!g_running) break; Sleep(20); continue; }
        int flags; { std::lock_guard<std::mutex> lk(g_mx); flags = g_flags; }
        if (!(flags & F_LAN) && from.sin_addr.S_un.S_addr != htonl(INADDR_LOOPBACK)) { closesocket(cs); continue; }
        BOOL nd = TRUE; setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
        char ip[64] = { 0 }; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        auto c = std::make_shared<Conn>();
        c->s = cs;
        std::vector<std::shared_ptr<Conn>> dead;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            for (auto it = g_conns.begin(); it != g_conns.end();) {
                if ((*it)->done && (*it)->th.joinable()) { dead.push_back(*it); it = g_conns.erase(it); }
                else ++it;
            }
            g_conns.push_back(c);
        }
        for (auto& d : dead) d->th.join();
        c->th = std::thread(connLoop, c, std::string(ip));
    }
}

// ---------------------------------------------------------------- 공개 로직 ----
static std::wstring normRoot(const wchar_t* root) {
    if (!root || !*root) return L"";
    wchar_t full[MAX_PATH * 4]; DWORD n = GetFullPathNameW(root, (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    std::wstring r = (n > 0 && n < sizeof(full) / sizeof(full[0])) ? std::wstring(full, n) : std::wstring(root);
    while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    return r;
}
static bool serverStart(int port, const wchar_t* root, int flags) {
    std::wstring nr = normRoot(root);
    if (!nr.empty()) {
        DWORD a = GetFileAttributesW(nr.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { setErr(L"정적 루트 폴더 없음: " + nr); return false; }
    }
    if (g_running) {
        if (g_port != port) { setErr(L"이미 다른 포트(" + std::to_wstring(g_port) + L")로 실행 중. Stop 후 다시 시작"); return false; }
        bool lanNow = (g_flags & F_LAN) != 0, lanNew = (flags & F_LAN) != 0;
        if (lanNow != lanNew) { setErr(L"LAN 플래그 변경은 Stop 후 다시 시작"); return false; }
        std::lock_guard<std::mutex> lk(g_mx); g_root = nr; g_flags = flags;   // 루트/옵션만 갱신
        return true;
    }
    WSADATA wd; if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) { setErr(L"WSAStartup 실패"); return false; }
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { setErr(L"socket 실패"); return false; }
    BOOL ex = TRUE; setsockopt(g_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&ex, sizeof(ex));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    a.sin_addr.S_un.S_addr = (flags & F_LAN) ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    if (bind(g_listen, (sockaddr*)&a, sizeof(a)) != 0) {
        setErr(L"bind 실패(포트 " + std::to_wstring(port) + L" 사용중?) err=" + std::to_wstring(WSAGetLastError()));
        closesocket(g_listen); g_listen = INVALID_SOCKET; return false;
    }
    if (listen(g_listen, SOMAXCONN) != 0) { setErr(L"listen 실패"); closesocket(g_listen); g_listen = INVALID_SOCKET; return false; }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_root = nr; g_flags = flags; g_queue.clear(); g_live.clear(); g_head.clear(); g_hasHead = false; g_lastErr.clear();
    }
    g_port = port;
    g_nReq = g_nStatic = g_nApi = g_nTimeout = 0;
    g_running = true;
    g_acceptTh = std::thread(acceptLoop);
    return true;
}
static void serverStop() {
    if (!g_running) return;
    g_running = false;
    g_cvQueue.notify_all(); g_cvReply.notify_all();
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    if (g_acceptTh.joinable()) g_acceptTh.join();
    std::vector<std::shared_ptr<Conn>> cs;
    { std::lock_guard<std::mutex> lk(g_mx); cs.swap(g_conns); }
    for (auto& c : cs) if (c->s != INVALID_SOCKET) shutdown(c->s, SD_BOTH);
    g_cvReply.notify_all();
    for (auto& c : cs) if (c->th.joinable()) c->th.join();
    { std::lock_guard<std::mutex> lk(g_mx); g_queue.clear(); g_live.clear(); g_head.clear(); g_hasHead = false; }
    g_port = 0;
}

// 2단계 수신: cap=0 → (timeoutMs 대기 후) 머리 요청 요약 길이, 버퍼 호출 → 복사+pop
//   요약 = "id\tmethod\tpath\tquery\tremote\tcontent-type\tbodyLen"
static int recvRequest(int timeoutMs, wchar_t* out, int cap) {
    std::unique_lock<std::mutex> lk(g_mx);
    if (!g_hasHead) {
        if (g_queue.empty()) {
            if (timeoutMs > 0)
                g_cvQueue.wait_for(lk, std::chrono::milliseconds(timeoutMs), [] { return !g_queue.empty() || !g_running; });
            if (g_queue.empty()) return 0;
        }
        auto r = g_queue.front(); g_queue.pop_front();
        std::string p = r->path;
        for (auto& ch : p) if ((unsigned char)ch < 0x20) ch = '?';
        std::string ct = hdr(*r, "content-type");
        for (auto& ch : ct) if ((unsigned char)ch < 0x20) ch = ' ';
        std::string line = std::to_string(r->id) + "\t" + r->method + "\t" + p + "\t" + r->query + "\t" +
                           r->remote + "\t" + ct + "\t" + std::to_string(r->body.size());
        g_head = u2w(line);
        g_hasHead = true;
    }
    int need = (int)g_head.size();
    if (out == nullptr || cap == 0) return need;
    if (cap < need + 1) return -need;
    memcpy(out, g_head.data(), g_head.size() * sizeof(wchar_t)); out[g_head.size()] = L'\0';
    g_head.clear(); g_hasHead = false;
    return need;
}
static std::shared_ptr<Req> findLive(int id) {
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_live.find(id);
    return it == g_live.end() ? nullptr : it->second;
}
static int copyOut(const std::wstring& w, wchar_t* out, int cap) {
    int need = (int)w.size();
    if (out == nullptr || cap == 0) return need;
    if (cap < need + 1) return -need;
    memcpy(out, w.data(), w.size() * sizeof(wchar_t)); out[w.size()] = L'\0';
    return need;
}
static bool reply(int id, int status, const std::string& ctype, const std::string& body,
                  const std::string& extra, bool isFile, const std::wstring& file) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_live.find(id);
        if (it == g_live.end()) { g_lastErr = L"Reply: 없는/만료된 요청 id " + std::to_wstring(id); return false; }
        auto r = it->second;
        if (r->replied) return false;
        r->status = status > 0 ? status : 200;
        r->ctype = ctype; r->respBody = body; r->extra = extra; r->isFile = isFile; r->filePath = file;
        r->body.clear(); r->body.shrink_to_fit();            // 요청 본문 해제
        r->replied = true;
        g_live.erase(it);
    }
    g_cvReply.notify_all();
    return true;
}

}} // namespace notepad::hsv

// ============================================================================
//  export (AHK v1 64비트 DllCall)
// ============================================================================
using namespace notepad::hsv;

NOTEPAD_API int __stdcall Notepad_HttpServerStart(int port, const wchar_t* staticRoot, int flags) {
    return serverStart(port, staticRoot, flags) ? 1 : 0;
}
NOTEPAD_API int __stdcall Notepad_HttpServerStop() { serverStop(); return 1; }
NOTEPAD_API int __stdcall Notepad_HttpServerPort() { return g_running ? g_port : 0; }
NOTEPAD_API int __stdcall Notepad_HttpServerPending() {
    std::lock_guard<std::mutex> lk(g_mx);
    return (int)g_queue.size() + (g_hasHead ? 1 : 0);
}
// AHK 응답 대기 한도(ms). 초과 시 클라이언트에 504. 반환: 이전 값
NOTEPAD_API int __stdcall Notepad_HttpServerSetTimeout(int ms) {
    int prev = g_timeoutMs.load();
    if (ms > 0) g_timeoutMs = ms;
    return prev;
}
NOTEPAD_API int __stdcall Notepad_HttpServerRecv(int timeoutMs, wchar_t* out, int outCch) {
    return recvRequest(timeoutMs, out, outCch);
}
// 요청 헤더 값. name="" 이면 전체 "name: value\n"
NOTEPAD_API int __stdcall Notepad_HttpServerHeader(int id, const wchar_t* name, wchar_t* out, int outCch) {
    auto r = findLive(id); if (!r) return 0;
    std::string n = lower(w2u(name ? name : L""));
    if (n.empty()) {
        std::string all;
        for (auto& h : r->headers) all += h.first + ": " + h.second + "\n";
        return copyOut(u2w(all), out, outCch);
    }
    return copyOut(u2w(hdr(*r, n)), out, outCch);
}
// 파라미터: 쿼리스트링 → x-www-form-urlencoded 본문 → multipart 텍스트 필드 순으로 찾음
NOTEPAD_API int __stdcall Notepad_HttpServerParam(int id, const wchar_t* name, wchar_t* out, int outCch) {
    auto r = findLive(id); if (!r) return 0;
    std::string n = w2u(name ? name : L""), v;
    if (formLookup(r->query, n, v)) return copyOut(u2w(v), out, outCch);
    std::string ct = lower(hdr(*r, "content-type"));
    if (ct.find("application/x-www-form-urlencoded") != std::string::npos && formLookup(r->body, n, v))
        return copyOut(u2w(v), out, outCch);
    if (ct.find("multipart/form-data") != std::string::npos) {
        for (auto& p : parseMultipart(*r))
            if (p.name == n && p.filename.empty()) return copyOut(u2w(r->body.substr(p.off, p.len)), out, outCch);
    }
    return 0;
}
// 본문 전체(UTF-8 → UTF-16)
NOTEPAD_API int __stdcall Notepad_HttpServerBody(int id, wchar_t* out, int outCch) {
    auto r = findLive(id); if (!r) return 0;
    return copyOut(u2w(r->body), out, outCch);
}
// 본문(또는 multipart 파일 필드)을 바이너리 그대로 파일 저장. 반환: 바이트 수, -1 실패, -2 해당 필드 없음
//   field="" 이고 multipart 면 filename 있는 첫 파트. multipart 가 아니면 본문 전체.
//   fileNameOut(옵션, 2단계 아님 — 260자 버퍼): 업로드 원본 파일명
NOTEPAD_API long long __stdcall Notepad_HttpServerSaveBody(int id, const wchar_t* field, const wchar_t* outPath,
                                                           wchar_t* fileNameOut, int fileNameCch) {
    auto r = findLive(id); if (!r || !outPath) return -1;
    std::string f = w2u(field ? field : L"");
    size_t off = 0, len = r->body.size();
    std::string fname;
    if (lower(hdr(*r, "content-type")).find("multipart/form-data") != std::string::npos) {
        auto parts = parseMultipart(*r);
        const Part* hit = nullptr;
        for (auto& p : parts) {
            if (!f.empty() ? (p.name == f) : !p.filename.empty()) { hit = &p; break; }
        }
        if (!hit && f.empty() && !parts.empty()) hit = &parts[0];
        if (!hit) return -2;
        off = hit->off; len = hit->len; fname = hit->filename;
    }
    if (fileNameOut && fileNameCch > 0) {
        std::wstring w = u2w(fname);
        if ((int)w.size() >= fileNameCch) w.resize(fileNameCch - 1);
        memcpy(fileNameOut, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    }
    HANDLE h = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { setErr(std::wstring(L"SaveBody: 파일 생성 실패 ") + outPath); return -1; }
    size_t done = 0; bool ok = true;
    while (ok && done < len) {
        DWORD w = 0, chunk = (DWORD)std::min<size_t>(len - done, 1 << 24);
        ok = WriteFile(h, r->body.data() + off + done, chunk, &w, nullptr) && w == chunk;
        done += w;
    }
    CloseHandle(h);
    return ok ? (long long)len : -1;
}
// 텍스트 응답(UTF-16 → UTF-8). headers: "Name: value" 줄들(`n 구분)
NOTEPAD_API int __stdcall Notepad_HttpServerReply(int id, int status, const wchar_t* ctype, const wchar_t* body,
                                                  const wchar_t* headers) {
    return reply(id, status, w2u(ctype ? ctype : L""), w2u(body ? body : L""), w2u(headers ? headers : L""),
                 false, L"") ? 1 : 0;
}
// 파일 응답(스트리밍). ctype="" 이면 확장자로 추정
NOTEPAD_API int __stdcall Notepad_HttpServerReplyFile(int id, int status, const wchar_t* ctype, const wchar_t* path,
                                                      const wchar_t* headers) {
    return reply(id, status, w2u(ctype ? ctype : L""), "", w2u(headers ? headers : L""),
                 true, path ? path : L"") ? 1 : 0;
}
NOTEPAD_API int __stdcall Notepad_HttpServerLastError(wchar_t* out, int outCch) {
    std::wstring e; { std::lock_guard<std::mutex> lk(g_mx); e = g_lastErr; }
    return copyOut(e, out, outCch);
}
// "port=..;requests=..;static=..;api=..;timeouts=..;pending=..;live=..;conns=.."
NOTEPAD_API int __stdcall Notepad_HttpServerStats(wchar_t* out, int outCch) {
    size_t pend, live, conns = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        pend = g_queue.size() + (g_hasHead ? 1 : 0); live = g_live.size();
        for (auto& c : g_conns) if (!c->done) ++conns;
    }
    std::wstring s = L"port=" + std::to_wstring(g_running ? g_port : 0) + L";requests=" + std::to_wstring(g_nReq.load()) +
        L";static=" + std::to_wstring(g_nStatic.load()) + L";api=" + std::to_wstring(g_nApi.load()) +
        L";timeouts=" + std::to_wstring(g_nTimeout.load()) + L";pending=" + std::to_wstring(pend) +
        L";live=" + std::to_wstring(live) + L";conns=" + std::to_wstring(conns);
    return copyOut(s, out, outCch);
}

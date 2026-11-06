// ============================================================================
//  NOTEPAD.dll  -  notepad_exports.cpp
//  [NOTEPAD] AHK DllCall 인터페이스 구현.
// ============================================================================
// AutoHotkey Web: the engine already links notepad_json.cpp (JsonGet/Base64Decode + jsonNavigate for CDP).
// Rename this file's copies so both can be linked into the same EXE without duplicate symbols.
#define Notepad_JsonGet NotepadX_JsonGet
#define Notepad_Base64Decode NotepadX_Base64Decode
#define jsonNavigate NotepadX_jsonNavigate
#define NOTEPAD_EXPORTS
#include "notepad_exports.h"
#include "notepad_crypto.h"
#include "cJSON.h"
#include <string>
#include <vector>
#include <cstdint>

using std::string;
using std::wstring;

// ---- UTF 변환 ---------------------------------------------------------------
static string wToUtf8(const wchar_t* w) {
    if (!w) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return string();
    string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
static wstring utf8ToW(const string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return wstring();
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// ---- Base64 -----------------------------------------------------------------
static const char* B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static string b64encode(const string& in) {
    string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i+1] << 8 | (uint8_t)in[i+2];
        out += B64[(n >> 18) & 63]; out += B64[(n >> 12) & 63];
        out += B64[(n >> 6) & 63];  out += B64[n & 63];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        uint32_t n = (uint8_t)in[i] << 16;
        out += B64[(n >> 18) & 63]; out += B64[(n >> 12) & 63];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i+1] << 8;
        out += B64[(n >> 18) & 63]; out += B64[(n >> 12) & 63];
        out += B64[(n >> 6) & 63];  out += '=';
    }
    return out;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
// 성공 true. 잘못된 문자가 있으면 false.
static bool b64decode(const string& in, string& out) {
    out.clear();
    std::vector<int> vals;
    vals.reserve(in.size());
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64val(c);
        if (v < 0) return false;
        vals.push_back(v);
    }
    size_t i = 0;
    while (i + 4 <= vals.size()) {
        uint32_t n = vals[i] << 18 | vals[i+1] << 12 | vals[i+2] << 6 | vals[i+3];
        out += (char)((n >> 16) & 0xFF);
        out += (char)((n >> 8) & 0xFF);
        out += (char)(n & 0xFF);
        i += 4;
    }
    size_t rem = vals.size() - i;
    if (rem == 2) {
        uint32_t n = vals[i] << 18 | vals[i+1] << 12;
        out += (char)((n >> 16) & 0xFF);
    } else if (rem == 3) {
        uint32_t n = vals[i] << 18 | vals[i+1] << 12 | vals[i+2] << 6;
        out += (char)((n >> 16) & 0xFF);
        out += (char)((n >> 8) & 0xFF);
    } else if (rem == 1) {
        return false; // 잘못된 길이
    }
    return true;
}

// ---- 상수시간 비교 ----------------------------------------------------------
static bool ctEqual(const string& a, const string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

// ---- 출력 헬퍼: wstring -> out 버퍼 (반환 규약 구현) -------------------------
static int writeOut(const wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size(); // 널 제외 문자 수
    if (out == nullptr || outCch == 0) return need;    // 길이 조회
    if (outCch < need + 1) return -need;               // 버퍼 부족
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}

// ---- hex --------------------------------------------------------------------
static wstring toHexLower(const string& bytes) {
    static const wchar_t* H = L"0123456789abcdef";
    wstring w;
    w.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) { w += H[b >> 4]; w += H[b & 0xF]; }
    return w;
}

// ============================================================================
// Exports
// ============================================================================

NOTEPAD_API int __stdcall Notepad_SHA256(const wchar_t* text, wchar_t* out, int outCch) {
    string data = wToUtf8(text);
    string digest;
    if (!notepad::sha256(data, digest)) return 0;
    return writeOut(toHexLower(digest), out, outCch);
}

NOTEPAD_API int __stdcall Notepad_RandomBase64(int nBytes, wchar_t* out, int outCch) {
    if (nBytes < 0) return 0;
    string r;
    if (!notepad::randomBytes((uint32_t)nBytes, r)) return 0;
    return writeOut(utf8ToW(b64encode(r)), out, outCch);
}

NOTEPAD_API int __stdcall Notepad_EncryptPassword(const wchar_t* plaintext,
                                           const wchar_t* password,
                                           int iterations,
                                           wchar_t* out, int outCch) {
    if (iterations <= 0) iterations = 100000;
    string pt = wToUtf8(plaintext);
    string pw = wToUtf8(password);

    string salt, nonce;
    if (!notepad::randomBytes(16, salt)) return 0;
    if (!notepad::randomBytes(12, nonce)) return 0;

    string key;
    if (!notepad::pbkdf2(pw, salt, (uint32_t)iterations, 32, key)) return 0;

    string cipher, tag;
    if (!notepad::aesGcmEncrypt(key, nonce, pt, cipher, tag)) return 0;

    string token = "v1:" + b64encode(salt) + ":" + b64encode(nonce) + ":" +
                   b64encode(tag) + ":" + b64encode(cipher);
    return writeOut(utf8ToW(token), out, outCch);
}

// "v1:salt:nonce:tag:cipher" 파싱
static bool splitToken(const string& t, string parts[5]) {
    size_t start = 0; int idx = 0;
    for (size_t i = 0; i <= t.size() && idx < 5; ++i) {
        if (i == t.size() || t[i] == ':') {
            parts[idx++] = t.substr(start, i - start);
            start = i + 1;
        }
    }
    return idx == 5;
}

NOTEPAD_API int __stdcall Notepad_DecryptPassword(const wchar_t* token,
                                           const wchar_t* password,
                                           wchar_t* out, int outCch) {
    string t = wToUtf8(token);
    string pw = wToUtf8(password);

    string parts[5];
    if (!splitToken(t, parts)) return 0;
    if (parts[0] != "v1") return 0;

    string salt, nonce, tag, cipher;
    if (!b64decode(parts[1], salt))  return 0;
    if (!b64decode(parts[2], nonce)) return 0;
    if (!b64decode(parts[3], tag))   return 0;
    if (!b64decode(parts[4], cipher))return 0;

    // iteration은 EncryptPassword 기본값과 동일해야 함(토큰에 미포함, 규약 고정).
    // 유연성을 위해 아래 HashPassword는 iter를 토큰에 넣지만, 여기선 v1 고정.
    string key;
    if (!notepad::pbkdf2(pw, salt, 100000, 32, key)) return 0;

    string pt;
    if (!notepad::aesGcmDecrypt(key, nonce, cipher, tag, pt)) return 0; // 변조/틀린 pw
    return writeOut(utf8ToW(pt), out, outCch);
}

NOTEPAD_API int __stdcall Notepad_HashPassword(const wchar_t* password,
                                        int iterations,
                                        wchar_t* out, int outCch) {
    if (iterations <= 0) iterations = 100000;
    string pw = wToUtf8(password);
    string salt;
    if (!notepad::randomBytes(16, salt)) return 0;
    string dk;
    if (!notepad::pbkdf2(pw, salt, (uint32_t)iterations, 32, dk)) return 0;

    string token = "pbkdf2$" + std::to_string(iterations) + "$" +
                   b64encode(salt) + "$" + b64encode(dk);
    return writeOut(utf8ToW(token), out, outCch);
}

// "pbkdf2$iter$salt$hash" 파싱
static bool splitHashToken(const string& t, string parts[4]) {
    size_t start = 0; int idx = 0;
    for (size_t i = 0; i <= t.size() && idx < 4; ++i) {
        if (i == t.size() || t[i] == '$') {
            parts[idx++] = t.substr(start, i - start);
            start = i + 1;
        }
    }
    return idx == 4;
}

NOTEPAD_API int __stdcall Notepad_VerifyPassword(const wchar_t* password,
                                          const wchar_t* token) {
    string pw = wToUtf8(password);
    string t = wToUtf8(token);
    string parts[4];
    if (!splitHashToken(t, parts)) return 0;
    if (parts[0] != "pbkdf2") return 0;

    int iter = atoi(parts[1].c_str());
    if (iter <= 0) return 0;

    string salt, expected;
    if (!b64decode(parts[2], salt)) return 0;
    if (!b64decode(parts[3], expected)) return 0;

    string dk;
    if (!notepad::pbkdf2(pw, salt, (uint32_t)iter, (uint32_t)expected.size(), dk))
        return 0;

    return ctEqual(dk, expected) ? 1 : 0;
}

// ---- [NOTEPAD] 압축 / 해제 --------------------------------------------------

NOTEPAD_API int __stdcall Notepad_Compress(const wchar_t* text, int level,
                                           wchar_t* out, int outCch) {
    string data = wToUtf8(text);
    string comp;
    if (!notepad::zipDeflate(data, comp, level < 0 ? 6 : level)) return 0;
    return writeOut(utf8ToW(b64encode(comp)), out, outCch);
}

NOTEPAD_API int __stdcall Notepad_Decompress(const wchar_t* b64,
                                             wchar_t* out, int outCch) {
    string comp;
    if (!b64decode(wToUtf8(b64), comp)) return 0;
    string data;
    if (!notepad::zipInflate(comp, data)) return 0;
    return writeOut(utf8ToW(data), out, outCch);
}

// ---- [NOTEPAD] 파일 해시 ----------------------------------------------------

NOTEPAD_API int __stdcall Notepad_SHA256File(const wchar_t* path,
                                             wchar_t* out, int outCch) {
    string digest;
    if (!notepad::sha256File(wToUtf8(path), digest)) return 0;
    return writeOut(toHexLower(digest), out, outCch);
}

// ---- [NOTEPAD] 파일 암호화 --------------------------------------------------
// 파일 포맷: "NPE1"(4B) + salt(16B) + nonce(12B) + tag(16B) + ciphertext
static const char* NPE_MAGIC = "NPE1";

NOTEPAD_API int __stdcall Notepad_EncryptFile(const wchar_t* inPath,
                                              const wchar_t* outPath,
                                              const wchar_t* password,
                                              int iterations) {
    if (iterations <= 0) iterations = 100000;
    string data;
    if (!notepad::readFileBytes(wToUtf8(inPath), data)) return 0;

    string pw = wToUtf8(password);
    string salt, nonce;
    if (!notepad::randomBytes(16, salt)) return 0;
    if (!notepad::randomBytes(12, nonce)) return 0;

    string key;
    if (!notepad::pbkdf2(pw, salt, (uint32_t)iterations, 32, key)) return 0;

    string cipher, tag;
    if (!notepad::aesGcmEncrypt(key, nonce, data, cipher, tag)) return 0;

    string blob;
    blob.append(NPE_MAGIC, 4);
    blob.append(salt);
    blob.append(nonce);
    blob.append(tag);
    blob.append(cipher);

    return notepad::writeFileBytes(wToUtf8(outPath), blob) ? 1 : 0;
}

NOTEPAD_API int __stdcall Notepad_DecryptFile(const wchar_t* inPath,
                                              const wchar_t* outPath,
                                              const wchar_t* password) {
    string blob;
    if (!notepad::readFileBytes(wToUtf8(inPath), blob)) return 0;

    // 최소 크기: magic4 + salt16 + nonce12 + tag16 = 48
    if (blob.size() < 48) return 0;
    if (blob.compare(0, 4, NPE_MAGIC) != 0) return 0;

    string salt   = blob.substr(4, 16);
    string nonce  = blob.substr(20, 12);
    string tag    = blob.substr(32, 16);
    string cipher = blob.substr(48);

    string pw = wToUtf8(password);
    string key;
    if (!notepad::pbkdf2(pw, salt, 100000, 32, key)) return 0; // v1 고정 iter

    string plain;
    if (!notepad::aesGcmDecrypt(key, nonce, cipher, tag, plain)) return 0;

    return notepad::writeFileBytes(wToUtf8(outPath), plain) ? 1 : 0;
}

// ---- [NOTEPAD] URL 인코딩 ---------------------------------------------------

static bool isUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.' || c == '~';
}

NOTEPAD_API int __stdcall Notepad_UrlEncode(const wchar_t* text,
                                            wchar_t* out, int outCch) {
    string in = wToUtf8(text);            // UTF-8 바이트 기준 인코딩
    static const char* HEX = "0123456789ABCDEF";
    string enc;
    for (unsigned char c : in) {
        if (isUnreserved(c)) {
            enc += (char)c;
        } else {
            enc += '%';
            enc += HEX[c >> 4];
            enc += HEX[c & 0xF];
        }
    }
    return writeOut(utf8ToW(enc), out, outCch);
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

NOTEPAD_API int __stdcall Notepad_UrlDecode(const wchar_t* text,
                                            wchar_t* out, int outCch) {
    string in = wToUtf8(text);
    string dec;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            int h = hexv(in[i+1]), l = hexv(in[i+2]);
            if (h < 0 || l < 0) return 0;   // 잘못된 인코딩
            dec += (char)((h << 4) | l);
            i += 2;
        } else if (c == '+') {
            dec += ' ';                     // 폼 인코딩 관례
        } else {
            dec += c;
        }
    }
    return writeOut(utf8ToW(dec), out, outCch);
}

// ---- [NOTEPAD] JSON ---------------------------------------------------------

NOTEPAD_API int __stdcall Notepad_JsonValid(const wchar_t* json) {
    string j = wToUtf8(json);
    cJSON* root = cJSON_Parse(j.c_str());
    if (!root) return 0;
    cJSON_Delete(root);
    return 1;
}

// "a.b.0" 경로로 항목 탐색
static cJSON* jsonNavigate(cJSON* root, const string& path) {
    cJSON* cur = root;
    size_t start = 0;
    while (start <= path.size() && cur) {
        size_t dot = path.find('.', start);
        string key = (dot == string::npos)
                   ? path.substr(start)
                   : path.substr(start, dot - start);
        if (!key.empty()) {
            if (cJSON_IsArray(cur)) {
                int idx = atoi(key.c_str());
                cur = cJSON_GetArrayItem(cur, idx);
            } else if (cJSON_IsObject(cur)) {
                cur = cJSON_GetObjectItemCaseSensitive(cur, key.c_str());
            } else {
                return nullptr;
            }
        }
        if (dot == string::npos) break;
        start = dot + 1;
    }
    return cur;
}

NOTEPAD_API int __stdcall Notepad_JsonGet(const wchar_t* json,
                                          const wchar_t* path,
                                          wchar_t* out, int outCch) {
    string j = wToUtf8(json);
    string p = wToUtf8(path);
    cJSON* root = cJSON_Parse(j.c_str());
    if (!root) return 0;

    cJSON* node = p.empty() ? root : jsonNavigate(root, p);
    if (!node) { cJSON_Delete(root); return 0; }

    string result;
    bool ok = true;
    if (cJSON_IsString(node)) {
        result = node->valuestring ? node->valuestring : "";
    } else if (cJSON_IsNumber(node)) {
        // 정수면 정수로, 아니면 실수로
        double v = node->valuedouble;
        char buf[64];
        if (v == (double)(long long)v)
            snprintf(buf, sizeof(buf), "%lld", (long long)v);
        else
            snprintf(buf, sizeof(buf), "%g", v);
        result = buf;
    } else if (cJSON_IsBool(node)) {
        result = cJSON_IsTrue(node) ? "true" : "false";
    } else if (cJSON_IsNull(node)) {
        result = "null";
    } else {
        // 객체/배열은 그 부분 JSON을 문자열로
        char* s = cJSON_PrintUnformatted(node);
        if (s) { result = s; cJSON_free(s); }
        else ok = false;
    }
    cJSON_Delete(root);
    if (!ok) return 0;
    return writeOut(utf8ToW(result), out, outCch);
}

NOTEPAD_API int __stdcall Notepad_JsonPretty(const wchar_t* json,
                                             wchar_t* out, int outCch) {
    string j = wToUtf8(json);
    cJSON* root = cJSON_Parse(j.c_str());
    if (!root) return 0;
    char* s = cJSON_Print(root);
    int rc = 0;
    if (s) {
        rc = writeOut(utf8ToW(string(s)), out, outCch);
        cJSON_free(s);
    }
    cJSON_Delete(root);
    return rc;
}

// ============================================================================
//  [세트 A] 해시/인코딩 확장  (기존 b64/hex/sha 내부 헬퍼 재사용 — 중복 없음)
// ============================================================================
#include <bcrypt.h>
#include <cstdio>
#include <cstdlib>
#include <time.h>

// 일반 해시 export 공통: text -> algId 해시 -> hex 소문자
static int hashExport(const wchar_t* algId, const wchar_t* text,
                      wchar_t* out, int outCch) {
    string data = wToUtf8(text);
    string digest;
    if (!notepad::hashDigest(algId, data, digest)) return 0;
    return writeOut(toHexLower(digest), out, outCch);
}

NOTEPAD_API int __stdcall Notepad_SHA512(const wchar_t* text, wchar_t* out, int outCch) {
    return hashExport(BCRYPT_SHA512_ALGORITHM, text, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_SHA384(const wchar_t* text, wchar_t* out, int outCch) {
    return hashExport(BCRYPT_SHA384_ALGORITHM, text, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_SHA1(const wchar_t* text, wchar_t* out, int outCch) {
    return hashExport(BCRYPT_SHA1_ALGORITHM, text, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_MD5(const wchar_t* text, wchar_t* out, int outCch) {
    return hashExport(BCRYPT_MD5_ALGORITHM, text, out, outCch);
}

// HMAC (key/text UTF-8) -> hex 소문자
static int hmacExport(const wchar_t* algId, const wchar_t* text,
                      const wchar_t* key, wchar_t* out, int outCch) {
    string data = wToUtf8(text), k = wToUtf8(key);
    string mac;
    if (!notepad::hmacDigest(algId, k, data, mac)) return 0;
    return writeOut(toHexLower(mac), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_HMAC(const wchar_t* text, const wchar_t* key,
                                       wchar_t* out, int outCch) {
    return hmacExport(BCRYPT_SHA256_ALGORITHM, text, key, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_HMAC512(const wchar_t* text, const wchar_t* key,
                                          wchar_t* out, int outCch) {
    return hmacExport(BCRYPT_SHA512_ALGORITHM, text, key, out, outCch);
}

// Base64 (UTF-8 바이트 <-> Base64 문자열). b64encode/b64decode 재사용.
NOTEPAD_API int __stdcall Notepad_Base64Encode(const wchar_t* text,
                                               wchar_t* out, int outCch) {
    return writeOut(utf8ToW(b64encode(wToUtf8(text))), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_Base64Decode(const wchar_t* b64,
                                               wchar_t* out, int outCch) {
    string dec;
    if (!b64decode(wToUtf8(b64), dec)) return 0;
    return writeOut(utf8ToW(dec), out, outCch);
}

// Base64URL (- _ 사용, 패딩 '=' 제거). 표준 b64 결과를 치환.
static string toB64Url(const string& std) {
    string s;
    for (char c : std) {
        if (c == '+') s += '-';
        else if (c == '/') s += '_';
        else if (c == '=') { /* strip */ }
        else s += c;
    }
    return s;
}
static string fromB64Url(const string& u) {
    string s;
    for (char c : u) {
        if (c == '-') s += '+';
        else if (c == '_') s += '/';
        else s += c;
    }
    while (s.size() % 4) s += '=';
    return s;
}
NOTEPAD_API int __stdcall Notepad_Base64UrlEncode(const wchar_t* text,
                                                  wchar_t* out, int outCch) {
    return writeOut(utf8ToW(toB64Url(b64encode(wToUtf8(text)))), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_Base64UrlDecode(const wchar_t* text,
                                                  wchar_t* out, int outCch) {
    string dec;
    if (!b64decode(fromB64Url(wToUtf8(text)), dec)) return 0;
    return writeOut(utf8ToW(dec), out, outCch);
}

// Hex (UTF-8 바이트 <-> hex 소문자). toHexLower / hexv 재사용.
NOTEPAD_API int __stdcall Notepad_HexEncode(const wchar_t* text,
                                            wchar_t* out, int outCch) {
    return writeOut(toHexLower(wToUtf8(text)), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_HexDecode(const wchar_t* hex,
                                            wchar_t* out, int outCch) {
    string h = wToUtf8(hex);
    string bytes;
    // 공백/개행 무시
    string clean;
    for (char c : h) if (c != ' ' && c != '\r' && c != '\n' && c != '\t') clean += c;
    if (clean.size() % 2 != 0) return 0;
    for (size_t i = 0; i < clean.size(); i += 2) {
        int hi = hexv(clean[i]), lo = hexv(clean[i+1]);
        if (hi < 0 || lo < 0) return 0;
        bytes += (char)((hi << 4) | lo);
    }
    return writeOut(utf8ToW(bytes), out, outCch);
}

// CRC32 -> hex 8자 소문자
NOTEPAD_API int __stdcall Notepad_CRC32(const wchar_t* text,
                                        wchar_t* out, int outCch) {
    uint32_t c = notepad::crc32sum(wToUtf8(text));
    wchar_t buf[9];
    static const wchar_t* H = L"0123456789abcdef";
    for (int i = 0; i < 8; ++i) buf[i] = H[(c >> ((7 - i) * 4)) & 0xF];
    buf[8] = 0;
    return writeOut(wstring(buf), out, outCch);
}

// UUID v4 (RFC4122). 난수 16바이트 -> 8-4-4-4-12 hex.
NOTEPAD_API int __stdcall Notepad_UUID(wchar_t* out, int outCch) {
    string r;
    if (!notepad::randomBytes(16, r)) return 0;
    unsigned char* b = (unsigned char*)r.data();
    b[6] = (b[6] & 0x0F) | 0x40; // version 4
    b[8] = (b[8] & 0x3F) | 0x80; // variant
    static const wchar_t* H = L"0123456789abcdef";
    wstring u;
    for (int i = 0; i < 16; ++i) {
        u += H[b[i] >> 4]; u += H[b[i] & 0xF];
        if (i == 3 || i == 5 || i == 7 || i == 9) u += L'-';
    }
    return writeOut(u, out, outCch);
}

// ============================================================================
//  [세트 B] 압축/아카이브
// ============================================================================
NOTEPAD_API int __stdcall Notepad_CompressFile(const wchar_t* inPath,
                                               const wchar_t* outPath, int level) {
    return notepad::compressFileStream(wToUtf8(inPath), wToUtf8(outPath), level) ? 1 : 0;
}
NOTEPAD_API int __stdcall Notepad_DecompressFile(const wchar_t* inPath,
                                                 const wchar_t* outPath) {
    return notepad::decompressFileStream(wToUtf8(inPath), wToUtf8(outPath)) ? 1 : 0;
}
NOTEPAD_API int __stdcall Notepad_ZipAddFile(const wchar_t* zipPath,
                                             const wchar_t* srcPath,
                                             const wchar_t* nameInZip, int level) {
    return notepad::zipAddFile(wToUtf8(zipPath), wToUtf8(srcPath),
                               wToUtf8(nameInZip), level) ? 1 : 0;
}
NOTEPAD_API int __stdcall Notepad_ZipList(const wchar_t* zipPath,
                                          wchar_t* out, int outCch) {
    string names;
    if (!notepad::zipList(wToUtf8(zipPath), names)) return 0;
    return writeOut(utf8ToW(names), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_ZipExtractAll(const wchar_t* zipPath,
                                                const wchar_t* destDir) {
    return notepad::zipExtractAll(wToUtf8(zipPath), wToUtf8(destDir)) ? 1 : 0;
}
NOTEPAD_API int __stdcall Notepad_ZipExtractFile(const wchar_t* zipPath,
                                                 const wchar_t* nameInZip,
                                                 const wchar_t* outPath) {
    return notepad::zipExtractFile(wToUtf8(zipPath), wToUtf8(nameInZip),
                                   wToUtf8(outPath)) ? 1 : 0;
}

// ============================================================================
//  [세트 C] HTTP (WinHTTP) — 2단계 호출(길이조회/채우기)에서 중복 요청 방지 캐시
// ============================================================================
static string g_httpKey;
static string g_httpResp;
static int    g_httpStatus = 0;
static bool   g_httpValid = false;

static const string& doHttp(const string& method, const string& url,
                            const string& body, const string& ct) {
    string key = method + "\x01" + url + "\x01" + body + "\x01" + ct;
    if (g_httpValid && key == g_httpKey) return g_httpResp; // 캐시 히트(2번째 호출)
    string resp; int status = 0;
    bool ok = notepad::httpRequest(method, url, body, ct, resp, status);
    g_httpKey = key;
    g_httpResp = ok ? resp : string();
    g_httpStatus = ok ? status : 0;
    g_httpValid = true;
    return g_httpResp;
}

NOTEPAD_API int __stdcall Notepad_HttpGet(const wchar_t* url,
                                          wchar_t* out, int outCch) {
    const string& resp = doHttp("GET", wToUtf8(url), string(), string());
    if (!g_httpValid || g_httpStatus == 0) return 0;
    return writeOut(utf8ToW(resp), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_HttpPost(const wchar_t* url, const wchar_t* body,
                                           const wchar_t* contentType,
                                           wchar_t* out, int outCch) {
    const string& resp = doHttp("POST", wToUtf8(url), wToUtf8(body), wToUtf8(contentType));
    if (!g_httpValid || g_httpStatus == 0) return 0;
    return writeOut(utf8ToW(resp), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_HttpLastStatus() {
    return g_httpStatus;
}
NOTEPAD_API int __stdcall Notepad_HttpDownload(const wchar_t* url,
                                               const wchar_t* outPath) {
    int status = 0;
    bool ok = notepad::httpDownload(wToUtf8(url), wToUtf8(outPath), status);
    g_httpStatus = status; g_httpValid = false;
    return ok ? 1 : 0;
}

// [세트 J] multipart/form-data 파일 업로드. 응답 본문 반환(2단계 호출 캐시 사용).
//   fieldName: 폼 필드명(빈문자열이면 "file"). fileName: 폼에 보낼 파일명(빈문자열이면 "upload.bin").
//   extraFields: "k1=v1\nk2=v2" 추가 텍스트 필드(옵션). 상태코드는 Note_HttpStatus.
static string g_upKey;
static string g_upResp;
static bool   g_upValid = false;

NOTEPAD_API int __stdcall Notepad_HttpUploadFile(const wchar_t* url,
                                                 const wchar_t* fieldName,
                                                 const wchar_t* filePath,
                                                 const wchar_t* fileName,
                                                 const wchar_t* extraFields,
                                                 wchar_t* out, int outCch) {
    string su = wToUtf8(url), sfield = wToUtf8(fieldName), sfile = wToUtf8(filePath);
    string sname = wToUtf8(fileName), sextra = wToUtf8(extraFields);
    string key = su + "\x01" + sfield + "\x01" + sfile + "\x01" + sname + "\x01" + sextra;

    if (g_upValid && key == g_upKey) {          // 캐시 히트(2번째 호출)
        return writeOut(utf8ToW(g_upResp), out, outCch);
    }
    string resp; int status = 0;
    bool ok = notepad::httpUploadFile(su, sfield, sfile, sname, sextra, resp, status);
    g_upKey = key; g_upResp = ok ? resp : string(); g_upValid = true;
    g_httpStatus = ok ? status : 0;             // 상태코드는 기존 HttpStatus 로 조회
    if (!ok || status == 0) return 0;
    return writeOut(utf8ToW(resp), out, outCch);
}

// ============================================================================
//  [세트 D] 데이터/텍스트  (CSV / HTML 엔티티 / 쿼리스트링 / 시간)
// ============================================================================

// ---- CSV (RFC4180): 따옴표/줄바꿈/이스케이프 "" 처리 ----
static std::vector<std::vector<string>> parseCsv(const string& in) {
    std::vector<std::vector<string>> rows;
    std::vector<string> row;
    string field;
    bool inQ = false;
    size_t i = 0, n = in.size();
    bool started = false;
    while (i < n) {
        char c = in[i];
        started = true;
        if (inQ) {
            if (c == '"') {
                if (i + 1 < n && in[i+1] == '"') { field += '"'; i += 2; continue; }
                inQ = false; i++; continue;
            }
            field += c; i++; continue;
        }
        if (c == '"') { inQ = true; i++; continue; }
        if (c == ',') { row.push_back(field); field.clear(); i++; continue; }
        if (c == '\r') { i++; continue; }
        if (c == '\n') { row.push_back(field); field.clear();
                         rows.push_back(row); row.clear(); i++; continue; }
        field += c; i++;
    }
    if (started) { row.push_back(field); rows.push_back(row); }
    return rows;
}

NOTEPAD_API int __stdcall Notepad_CsvGet(const wchar_t* csv, int row, int col,
                                         wchar_t* out, int outCch) {
    auto rows = parseCsv(wToUtf8(csv));
    if (row < 0 || (size_t)row >= rows.size()) return 0;
    if (col < 0 || (size_t)col >= rows[row].size()) return 0;
    return writeOut(utf8ToW(rows[row][col]), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_CsvRowCount(const wchar_t* csv) {
    return (int)parseCsv(wToUtf8(csv)).size();
}
NOTEPAD_API int __stdcall Notepad_CsvColCount(const wchar_t* csv, int row) {
    auto rows = parseCsv(wToUtf8(csv));
    if (row < 0 || (size_t)row >= rows.size()) return 0;
    return (int)rows[row].size();
}

// ---- HTML 엔티티 ----
NOTEPAD_API int __stdcall Notepad_HtmlEncode(const wchar_t* text,
                                             wchar_t* out, int outCch) {
    string in = wToUtf8(text), o;
    for (char c : in) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return writeOut(utf8ToW(o), out, outCch);
}
static void appendUtf8(string& o, unsigned int cp) {
    if (cp <= 0x7F) o += (char)cp;
    else if (cp <= 0x7FF) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp <= 0xFFFF) { o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}
NOTEPAD_API int __stdcall Notepad_HtmlDecode(const wchar_t* text,
                                             wchar_t* out, int outCch) {
    string in = wToUtf8(text), o;
    size_t i = 0, n = in.size();
    while (i < n) {
        if (in[i] == '&') {
            size_t semi = in.find(';', i);
            if (semi != string::npos && semi - i <= 10) {
                string ent = in.substr(i + 1, semi - i - 1);
                bool done = true;
                if (ent == "amp") o += '&';
                else if (ent == "lt") o += '<';
                else if (ent == "gt") o += '>';
                else if (ent == "quot") o += '"';
                else if (ent == "apos" || ent == "#39") o += '\'';
                else if (ent.size() > 1 && ent[0] == '#') {
                    unsigned int cp = 0;
                    if (ent[1] == 'x' || ent[1] == 'X')
                        cp = (unsigned int)strtoul(ent.c_str() + 2, nullptr, 16);
                    else
                        cp = (unsigned int)strtoul(ent.c_str() + 1, nullptr, 10);
                    if (cp) appendUtf8(o, cp); else done = false;
                } else done = false;
                if (done) { i = semi + 1; continue; }
            }
        }
        o += in[i++];
    }
    return writeOut(utf8ToW(o), out, outCch);
}

// ---- URL 쿼리스트링 파싱 (parse_qs) ----
static string urlDecodeStr(const string& in) {
    string dec;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            int h = hexv(in[i+1]), l = hexv(in[i+2]);
            if (h >= 0 && l >= 0) { dec += (char)((h << 4) | l); i += 2; continue; }
        }
        if (c == '+') dec += ' '; else dec += c;
    }
    return dec;
}
NOTEPAD_API int __stdcall Notepad_QueryGet(const wchar_t* query, const wchar_t* key,
                                           wchar_t* out, int outCch) {
    string q = wToUtf8(query), want = wToUtf8(key);
    size_t qm = q.find('?');
    if (qm != string::npos) q = q.substr(qm + 1);
    size_t start = 0;
    while (start <= q.size()) {
        size_t amp = q.find('&', start);
        string pair = (amp == string::npos) ? q.substr(start) : q.substr(start, amp - start);
        size_t eq = pair.find('=');
        string k = (eq == string::npos) ? pair : pair.substr(0, eq);
        string v = (eq == string::npos) ? "" : pair.substr(eq + 1);
        if (urlDecodeStr(k) == want)
            return writeOut(utf8ToW(urlDecodeStr(v)), out, outCch);
        if (amp == string::npos) break;
        start = amp + 1;
    }
    return 0;
}

// ---- 시간: Unix epoch <-> ISO8601 (UTC) ----
NOTEPAD_API int __stdcall Notepad_NowUnix(wchar_t* out, int outCch) {
    __int64 t = (__int64)_time64(nullptr);
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)t);
    return writeOut(utf8ToW(string(buf)), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_UnixToIso(const wchar_t* unixStr,
                                            wchar_t* out, int outCch) {
    string s = wToUtf8(unixStr);
    __int64 t = _strtoi64(s.c_str(), nullptr, 10);
    struct tm g;
    if (_gmtime64_s(&g, &t) != 0) return 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
             g.tm_hour, g.tm_min, g.tm_sec);
    return writeOut(utf8ToW(string(buf)), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_IsoToUnix(const wchar_t* iso,
                                            wchar_t* out, int outCch) {
    string s = wToUtf8(iso);
    int Y=0, M=0, D=0, h=0, mi=0, se=0;
    // "YYYY-MM-DD[ T]HH:MM:SS" 형태만 인식 (뒤 Z/오프셋 무시)
    if (sscanf(s.c_str(), "%d-%d-%d%*c%d:%d:%d", &Y,&M,&D,&h,&mi,&se) < 3) return 0;
    struct tm g; memset(&g, 0, sizeof(g));
    g.tm_year = Y - 1900; g.tm_mon = M - 1; g.tm_mday = D;
    g.tm_hour = h; g.tm_min = mi; g.tm_sec = se;
    __int64 t = _mkgmtime64(&g);
    if (t == (__int64)-1) return 0;
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)t);
    return writeOut(utf8ToW(string(buf)), out, outCch);
}

// ============================================================================
//  [세트 E] 보호: 파일 해시 무결성 검증 (상수시간 비교)
// ============================================================================
NOTEPAD_API int __stdcall Notepad_VerifyFileHash(const wchar_t* path,
                                                 const wchar_t* expectedHex) {
    string digest;
    if (!notepad::sha256File(wToUtf8(path), digest)) return 0;
    wstring gotW = toHexLower(digest);
    string got = wToUtf8(gotW.c_str());
    string want = wToUtf8(expectedHex);
    // 대소문자 무시 비교를 위해 want 소문자화
    for (char& c : want) if (c >= 'A' && c <= 'F') c = c - 'A' + 'a';
    return ctEqual(got, want) ? 1 : 0;
}

// ============================================================================
//  [세트 M] ECDSA P-256 전자서명 (키/서명은 Base64 문자열로 주고받음)
// ============================================================================
// 키쌍 생성 -> "privB64:pubB64" 반환. 실패 0.
//   AHK 에서 StrSplit(result, ":") 로 개인키/공개키 분리.
NOTEPAD_API int __stdcall Notepad_GenKeyPair(wchar_t* out, int outCch) {
    string priv, pub;
    if (!notepad::eccGenerateKeyPair(priv, pub)) return 0;
    string combined = b64encode(priv) + ":" + b64encode(pub);
    return writeOut(utf8ToW(combined), out, outCch);
}
// 개인키(Base64)로 text 서명 -> 서명(Base64). 실패 0.
NOTEPAD_API int __stdcall Notepad_Sign(const wchar_t* text, const wchar_t* privKeyB64,
                                       wchar_t* out, int outCch) {
    string priv;
    if (!b64decode(wToUtf8(privKeyB64), priv)) return 0;
    string data = wToUtf8(text), sig;
    if (!notepad::eccSign(priv, data, sig)) return 0;
    return writeOut(utf8ToW(b64encode(sig)), out, outCch);
}
// 공개키(Base64)로 서명 검증. 유효 1 / 무효·오류 0.
NOTEPAD_API int __stdcall Notepad_Verify(const wchar_t* text, const wchar_t* sigB64,
                                         const wchar_t* pubKeyB64) {
    string pub, sig;
    if (!b64decode(wToUtf8(pubKeyB64), pub)) return 0;
    if (!b64decode(wToUtf8(sigB64), sig)) return 0;
    string data = wToUtf8(text);
    return notepad::eccVerify(pub, data, sig) ? 1 : 0;
}

// ============================================================================
//  [세트 P] DPAPI — 자동 암호화(비번 불필요, Windows 계정에 묶임). 결과 Base64.
// ============================================================================
// text 를 현재 사용자(또는 머신) 기준으로 암호화 -> Base64. 실패 0.
//   entropy: 추가 비밀(선택, "" 가능). machineScope: 1=머신전체, 0=사용자.
NOTEPAD_API int __stdcall Notepad_DpapiProtect(const wchar_t* text, const wchar_t* entropy,
                                               int machineScope, wchar_t* out, int outCch) {
    string data = wToUtf8(text), ent = wToUtf8(entropy), enc;
    if (!notepad::dpapiProtect(data, ent, machineScope != 0, enc)) return 0;
    return writeOut(utf8ToW(b64encode(enc)), out, outCch);
}
// Base64 암호문 -> 평문. 다른 사용자/머신/변조 시 실패 0.
NOTEPAD_API int __stdcall Notepad_DpapiUnprotect(const wchar_t* b64, const wchar_t* entropy,
                                                 wchar_t* out, int outCch) {
    string blob;
    if (!b64decode(wToUtf8(b64), blob)) return 0;
    string ent = wToUtf8(entropy), plain;
    if (!notepad::dpapiUnprotect(blob, ent, plain)) return 0;
    return writeOut(utf8ToW(plain), out, outCch);
}

// ============================================================================
//  [세트 F] 유사도 분석  (순수 C++, UTF-16 코드유닛 기준)
// ============================================================================
#include <algorithm>

// 레벤슈타인 편집 거리 (2행 DP)
static int levenshtein(const wstring& a, const wstring& b) {
    size_t n = a.size(), m = b.size();
    if (n == 0) return (int)m;
    if (m == 0) return (int)n;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int del = prev[j] + 1, ins = cur[j-1] + 1, sub = prev[j-1] + cost;
            int mn = del < ins ? del : ins;      // windows.h 의 min 매크로 회피 (수동 min)
            if (sub < mn) mn = sub;
            cur[j] = mn;
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

// 유사도 비율 0~100 (레벤슈타인 기반)
static int simRatio(const wstring& a, const wstring& b) {
    size_t maxLen = a.size() > b.size() ? a.size() : b.size();
    if (maxLen == 0) return 100; // 둘 다 빈 문자열
    int dist = levenshtein(a, b);
    double r = (1.0 - (double)dist / (double)maxLen) * 100.0;
    if (r < 0) r = 0;
    return (int)(r + 0.5);
}

// Jaro-Winkler 유사도 0~100
static double jaroWinklerD(const wstring& s1, const wstring& s2) {
    size_t l1 = s1.size(), l2 = s2.size();
    if (l1 == 0 && l2 == 0) return 1.0;
    if (l1 == 0 || l2 == 0) return 0.0;
    int matchDist = (int)((l1 > l2 ? l1 : l2) / 2) - 1;
    if (matchDist < 0) matchDist = 0;
    std::vector<char> s1m(l1, 0), s2m(l2, 0);
    int matches = 0;
    for (size_t i = 0; i < l1; ++i) {
        int start = (int)i - matchDist; if (start < 0) start = 0;
        int end = (int)i + matchDist + 1; if (end > (int)l2) end = (int)l2;
        for (int j = start; j < end; ++j) {
            if (!s2m[j] && s1[i] == s2[j]) { s1m[i] = s2m[j] = 1; matches++; break; }
        }
    }
    if (matches == 0) return 0.0;
    double t = 0; size_t k = 0;
    for (size_t i = 0; i < l1; ++i) {
        if (s1m[i]) {
            while (!s2m[k]) k++;
            if (s1[i] != s2[k]) t += 0.5;
            k++;
        }
    }
    double m = (double)matches;
    double jaro = (m / l1 + m / l2 + (m - t) / m) / 3.0;
    // Winkler: 공통 접두(최대 4) 가중
    int prefix = 0;
    for (size_t i = 0; i < l1 && i < l2 && i < 4; ++i) {
        if (s1[i] == s2[i]) prefix++; else break;
    }
    return jaro + prefix * 0.1 * (1.0 - jaro);
}

NOTEPAD_API int __stdcall Notepad_Levenshtein(const wchar_t* a, const wchar_t* b) {
    return levenshtein(a ? wstring(a) : wstring(), b ? wstring(b) : wstring());
}
NOTEPAD_API int __stdcall Notepad_SimilarityRatio(const wchar_t* a, const wchar_t* b) {
    return simRatio(a ? wstring(a) : wstring(), b ? wstring(b) : wstring());
}
NOTEPAD_API int __stdcall Notepad_JaroWinkler(const wchar_t* a, const wchar_t* b) {
    double d = jaroWinklerD(a ? wstring(a) : wstring(), b ? wstring(b) : wstring());
    return (int)(d * 100.0 + 0.5);
}
// 후보 목록(개행 구분)에서 needle 과 가장 유사한 줄 반환. 동점이면 첫 번째.
NOTEPAD_API int __stdcall Notepad_FuzzyBestMatch(const wchar_t* needle,
                                                 const wchar_t* listNl,
                                                 wchar_t* out, int outCch) {
    wstring need = needle ? needle : L"";
    wstring list = listNl ? listNl : L"";
    wstring best; int bestScore = -1;
    size_t start = 0;
    while (start <= list.size()) {
        size_t nl = list.find(L'\n', start);
        wstring line = (nl == wstring::npos) ? list.substr(start)
                                             : list.substr(start, nl - start);
        // \r 제거
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        int score = simRatio(need, line);
        if (score > bestScore) { bestScore = score; best = line; }
        if (nl == wstring::npos) break;
        start = nl + 1;
    }
    if (bestScore < 0) return 0;
    return writeOut(best, out, outCch);
}

// ============================================================================
//  [세트 F+] 유사도 알고리즘 확장  (표준 라이브러리 대응 보완)
//  기존 levenshtein/simRatio/jaroWinklerD 헬퍼 재사용. 모두 UTF-16 코드유닛 기준.
// ============================================================================
#include <set>
#include <map>
#include <cmath>

// Damerau-Levenshtein 거리(인접 문자 전치까지 1로 계산). "ab"<->"ba" = 1.
static int damerauLev(const wstring& a, const wstring& b) {
    int n = (int)a.size(), m = (int)b.size();
    if (n == 0) return m;
    if (m == 0) return n;
    std::vector<std::vector<int>> d(n + 1, std::vector<int>(m + 1));
    for (int i = 0; i <= n; ++i) d[i][0] = i;
    for (int j = 0; j <= m; ++j) d[0][j] = j;
    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int del = d[i-1][j] + 1, ins = d[i][j-1] + 1, sub = d[i-1][j-1] + cost;
            int mn = del < ins ? del : ins; if (sub < mn) mn = sub;
            if (i > 1 && j > 1 && a[i-1] == b[j-2] && a[i-2] == b[j-1]) {
                int tr = d[i-2][j-2] + 1; if (tr < mn) mn = tr;   // 전치
            }
            d[i][j] = mn;
        }
    }
    return d[n][m];
}

// bigram(2-gram) 멀티셋 생성
static std::map<wstring,int> bigrams(const wstring& s) {
    std::map<wstring,int> g;
    for (size_t i = 0; i + 1 < s.size(); ++i) g[s.substr(i, 2)]++;
    return g;
}

// ---- exports ----

// Damerau-Levenshtein 편집거리(전치 포함). 정수.
NOTEPAD_API int __stdcall Notepad_DamerauLevenshtein(const wchar_t* a, const wchar_t* b) {
    return damerauLev(a ? wstring(a) : wstring(), b ? wstring(b) : wstring());
}
// Damerau-Levenshtein 기반 유사도(일치율) 0~100. (SimilarityRatio 의 Damerau 버전)
//   1 - 거리/최대길이. 둘 다 빈 문자열이면 100.
NOTEPAD_API int __stdcall Notepad_DamerauRatio(const wchar_t* a, const wchar_t* b) {
    wstring x = a ? a : L"", y = b ? b : L"";
    size_t maxLen = x.size() > y.size() ? x.size() : y.size();
    if (maxLen == 0) return 100;
    int dist = damerauLev(x, y);
    double r = (1.0 - (double)dist / (double)maxLen) * 100.0;
    if (r < 0) r = 0;
    return (int)(r + 0.5);
}

// Hamming 거리(같은 길이에서 다른 위치 수). 길이 다르면 -1.
NOTEPAD_API int __stdcall Notepad_Hamming(const wchar_t* a, const wchar_t* b) {
    wstring x = a ? a : L"", y = b ? b : L"";
    if (x.size() != y.size()) return -1;
    int d = 0;
    for (size_t i = 0; i < x.size(); ++i) if (x[i] != y[i]) d++;
    return d;
}

// Jaccard 유사도(문자 집합 기준) 0~100. |교집합|/|합집합|.
NOTEPAD_API int __stdcall Notepad_JaccardChar(const wchar_t* a, const wchar_t* b) {
    wstring x = a ? a : L"", y = b ? b : L"";
    std::set<wchar_t> sa(x.begin(), x.end()), sb(y.begin(), y.end());
    if (sa.empty() && sb.empty()) return 100;
    int inter = 0;
    for (wchar_t c : sa) if (sb.count(c)) inter++;
    int uni = (int)sa.size() + (int)sb.size() - inter;
    if (uni == 0) return 0;
    return (int)((double)inter / uni * 100 + 0.5);
}

// Sørensen-Dice 계수(bigram 기준) 0~100. 2|교|/(|A|+|B|). 이름 매칭에 강함.
NOTEPAD_API int __stdcall Notepad_DiceCoefficient(const wchar_t* a, const wchar_t* b) {
    wstring x = a ? a : L"", y = b ? b : L"";
    if (x.size() < 2 && y.size() < 2) return (x == y) ? 100 : 0;
    auto ga = bigrams(x), gb = bigrams(y);
    int ta = 0, tb = 0, inter = 0;
    for (auto& kv : ga) ta += kv.second;
    for (auto& kv : gb) tb += kv.second;
    for (auto& kv : ga) { auto it = gb.find(kv.first); if (it != gb.end()) inter += (kv.second < it->second ? kv.second : it->second); }
    if (ta + tb == 0) return 0;
    return (int)((double)(2 * inter) / (ta + tb) * 100 + 0.5);
}

// Cosine 유사도(문자 빈도 벡터) 0~100. 길이차 큰 텍스트에 적합.
NOTEPAD_API int __stdcall Notepad_CosineChar(const wchar_t* a, const wchar_t* b) {
    wstring x = a ? a : L"", y = b ? b : L"";
    std::map<wchar_t,int> fa, fb;
    for (wchar_t c : x) fa[c]++;
    for (wchar_t c : y) fb[c]++;
    double dot = 0, na = 0, nb = 0;
    for (auto& kv : fa) { na += (double)kv.second * kv.second; auto it = fb.find(kv.first); if (it != fb.end()) dot += (double)kv.second * it->second; }
    for (auto& kv : fb) nb += (double)kv.second * kv.second;
    if (na == 0 || nb == 0) return (na == 0 && nb == 0) ? 100 : 0;
    double cos = dot / (std::sqrt(na) * std::sqrt(nb));
    if (cos < 0) cos = 0; if (cos > 1) cos = 1;
    return (int)(cos * 100 + 0.5);
}

// 최장 공통 부분수열(LCS) 길이. 정수.
NOTEPAD_API int __stdcall Notepad_LcsLength(const wchar_t* a, const wchar_t* b) {
    wstring x = a ? a : L"", y = b ? b : L"";
    int n = (int)x.size(), m = (int)y.size();
    if (n == 0 || m == 0) return 0;
    std::vector<int> prev(m + 1, 0), cur(m + 1, 0);
    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            if (x[i-1] == y[j-1]) cur[j] = prev[j-1] + 1;
            else cur[j] = (prev[j] > cur[j-1]) ? prev[j] : cur[j-1];
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

// N-gram 유사도(q-gram 겹침 비율) 0~100. n 은 2~5 권장.
NOTEPAD_API int __stdcall Notepad_NgramSimilarity(const wchar_t* a, const wchar_t* b, int n) {
    if (n < 1) n = 2;
    wstring x = a ? a : L"", y = b ? b : L"";
    if (x.size() < (size_t)n && y.size() < (size_t)n) return (x == y) ? 100 : 0;
    std::map<wstring,int> ga, gb;
    for (size_t i = 0; i + n <= x.size(); ++i) ga[x.substr(i, n)]++;
    for (size_t i = 0; i + n <= y.size(); ++i) gb[y.substr(i, n)]++;
    int ta = 0, tb = 0, inter = 0;
    for (auto& kv : ga) ta += kv.second;
    for (auto& kv : gb) tb += kv.second;
    for (auto& kv : ga) { auto it = gb.find(kv.first); if (it != gb.end()) inter += (kv.second < it->second ? kv.second : it->second); }
    int uni = ta + tb - inter;
    if (uni == 0) return 0;
    return (int)((double)inter / uni * 100 + 0.5);
}

// 알고리즘 지정 최적 매치. algo: "lev","jaro","dice","cosine","jaccard","ngram".
//   결과: "매치줄\t점수" (탭 구분). 점수 0~100. 목록 비었으면 "".
NOTEPAD_API int __stdcall Notepad_FuzzyBestMatchEx(const wchar_t* needle, const wchar_t* listNl,
                                                   const wchar_t* algo, wchar_t* out, int outCch) {
    wstring need = needle ? needle : L"";
    wstring list = listNl ? listNl : L"";
    wstring alg = algo ? algo : L"lev";
    for (auto& c : alg) c = (wchar_t)towlower(c);

    auto score = [&](const wstring& cand)->int {
        if (alg == L"jaro") return (int)(jaroWinklerD(need, cand) * 100 + 0.5);
        if (alg == L"dam" || alg == L"damerau") return Notepad_DamerauRatio(need.c_str(), cand.c_str());
        if (alg == L"dice") return Notepad_DiceCoefficient(need.c_str(), cand.c_str());
        if (alg == L"cosine") return Notepad_CosineChar(need.c_str(), cand.c_str());
        if (alg == L"jaccard") return Notepad_JaccardChar(need.c_str(), cand.c_str());
        if (alg == L"ngram") return Notepad_NgramSimilarity(need.c_str(), cand.c_str(), 2);
        return simRatio(need, cand);   // 기본 lev
    };

    wstring best; int bestScore = -1;
    size_t start = 0;
    while (start <= list.size()) {
        size_t nl = list.find(L'\n', start);
        wstring line = (nl == wstring::npos) ? list.substr(start) : list.substr(start, nl - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        int sc = score(line);
        if (sc > bestScore) { bestScore = sc; best = line; }
        if (nl == wstring::npos) break;
        start = nl + 1;
    }
    if (bestScore < 0) return 0;
    wchar_t buf[16]; _snwprintf_s(buf, _TRUNCATE, L"%d", bestScore);
    return writeOut(best + L"\t" + buf, out, outCch);
}

// ============================================================================
//  [세트 L] 정규식 그룹추출 / 전체매치  (std::wregex, ECMAScript 문법)
//  AHK v1 의 RegExMatch 는 결과 캡처를 ByRef 배열로만 주고 "모든 매치"가 번거롭다.
//  여기서는 캡처 그룹 전체를 JSON 으로, 모든 매치를 JSON 배열로 반환한다.
//  patternFlags: "i"=대소문자무시. wregex 라 한글(UTF-16) 안전.
// ============================================================================
#include <regex>

static std::wregex::flag_type regexFlags(const wstring& flags) {
    std::wregex::flag_type f = std::regex::ECMAScript;
    for (wchar_t c : flags) {
        if (c == L'i' || c == L'I') f |= std::regex::icase;
        // 참고: 'm'(multiline)은 일부 MSVC 버전의 <regex>에 없어 지원하지 않는다.
        //   (^/$ 의 멀티라인 앵커는 미지원. 필요 시 패턴을 줄 단위로 나눠 처리)
    }
    return f;
}

// 첫 매치의 캡처 그룹들을 JSON 배열로. [0]=전체매치, [1..]=그룹.
//   매치 없으면 빈 문자열(길이 0). 패턴 오류 시 0.
NOTEPAD_API int __stdcall Notepad_RegexMatch(const wchar_t* text, const wchar_t* pattern,
                                             const wchar_t* flags,
                                             wchar_t* out, int outCch) {
    wstring t = text ? text : L"", p = pattern ? pattern : L"", fl = flags ? flags : L"";
    try {
        std::wregex re(p, regexFlags(fl));
        std::wsmatch m;
        if (!std::regex_search(t, m, re)) return writeOut(L"", out, outCch); // 매치 없음
        cJSON* arr = cJSON_CreateArray();
        for (size_t i = 0; i < m.size(); ++i) {
            string g = m[i].matched ? wToUtf8(m[i].str().c_str()) : string();
            cJSON_AddItemToArray(arr, cJSON_CreateString(g.c_str()));
        }
        char* js = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (!js) return 0;
        wstring w = utf8ToW(string(js));
        cJSON_free(js);
        return writeOut(w, out, outCch);
    } catch (const std::regex_error&) { return 0; }
}

// 이름 없는 그룹 index(0=전체) 하나만 반환. 매치/그룹 없으면 빈 문자열, 패턴오류 0.
NOTEPAD_API int __stdcall Notepad_RegexGroup(const wchar_t* text, const wchar_t* pattern,
                                             int groupIndex, const wchar_t* flags,
                                             wchar_t* out, int outCch) {
    wstring t = text ? text : L"", p = pattern ? pattern : L"", fl = flags ? flags : L"";
    if (groupIndex < 0) return 0;
    try {
        std::wregex re(p, regexFlags(fl));
        std::wsmatch m;
        if (!std::regex_search(t, m, re)) return writeOut(L"", out, outCch);
        if ((size_t)groupIndex >= m.size() || !m[groupIndex].matched)
            return writeOut(L"", out, outCch);
        return writeOut(m[groupIndex].str(), out, outCch);
    } catch (const std::regex_error&) { return 0; }
}

// 모든 매치를 JSON 배열의 배열로. 각 원소=[전체, 그룹1, ...]. 매치없으면 "[]", 오류 0.
NOTEPAD_API int __stdcall Notepad_RegexMatchAll(const wchar_t* text, const wchar_t* pattern,
                                                const wchar_t* flags,
                                                wchar_t* out, int outCch) {
    wstring t = text ? text : L"", p = pattern ? pattern : L"", fl = flags ? flags : L"";
    try {
        std::wregex re(p, regexFlags(fl));
        cJSON* top = cJSON_CreateArray();
        auto begin = std::wsregex_iterator(t.begin(), t.end(), re);
        auto end = std::wsregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const std::wsmatch& m = *it;
            cJSON* one = cJSON_CreateArray();
            for (size_t i = 0; i < m.size(); ++i) {
                string g = m[i].matched ? wToUtf8(m[i].str().c_str()) : string();
                cJSON_AddItemToArray(one, cJSON_CreateString(g.c_str()));
            }
            cJSON_AddItemToArray(top, one);
        }
        char* js = cJSON_PrintUnformatted(top);
        cJSON_Delete(top);
        if (!js) return 0;
        wstring w = utf8ToW(string(js));
        cJSON_free(js);
        return writeOut(w, out, outCch);
    } catch (const std::regex_error&) { return 0; }
}

// [헬퍼] N번째 매치(0-기반)로 iterator 를 전진시켜 wsmatch 를 채운다.
//   성공(해당 매치 존재) 시 true, 없으면 false.
static bool nthMatch(const wstring& t, const std::wregex& re, int matchIndex,
                     std::wsmatch& outM) {
    if (matchIndex < 0) return false;
    auto it = std::wsregex_iterator(t.begin(), t.end(), re);
    auto end = std::wsregex_iterator();
    int idx = 0;
    for (; it != end; ++it, ++idx) {
        if (idx == matchIndex) { outM = *it; return true; }
    }
    return false;
}

// matchIndex 번째 매치(0-기반)의 groupIndex 그룹(0=전체) 하나를 반환.
//   매치/그룹 없으면 빈 문자열(길이 0). 패턴 오류 시 0.
NOTEPAD_API int __stdcall Notepad_RegexGroupAt(const wchar_t* text, const wchar_t* pattern,
                                               int matchIndex, int groupIndex,
                                               const wchar_t* flags,
                                               wchar_t* out, int outCch) {
    wstring t = text ? text : L"", p = pattern ? pattern : L"", fl = flags ? flags : L"";
    if (matchIndex < 0 || groupIndex < 0) return 0;
    try {
        std::wregex re(p, regexFlags(fl));
        std::wsmatch m;
        if (!nthMatch(t, re, matchIndex, m)) return writeOut(L"", out, outCch); // 해당 매치 없음
        if ((size_t)groupIndex >= m.size() || !m[groupIndex].matched)
            return writeOut(L"", out, outCch);
        return writeOut(m[groupIndex].str(), out, outCch);
    } catch (const std::regex_error&) { return 0; }
}

// matchIndex 번째 매치(0-기반)의 캡처 그룹들을 JSON 배열로. [0]=전체, [1..]=그룹.
//   해당 매치 없으면 빈 문자열(길이 0). 패턴 오류 시 0.
NOTEPAD_API int __stdcall Notepad_RegexMatchAt(const wchar_t* text, const wchar_t* pattern,
                                               int matchIndex, const wchar_t* flags,
                                               wchar_t* out, int outCch) {
    wstring t = text ? text : L"", p = pattern ? pattern : L"", fl = flags ? flags : L"";
    if (matchIndex < 0) return 0;
    try {
        std::wregex re(p, regexFlags(fl));
        std::wsmatch m;
        if (!nthMatch(t, re, matchIndex, m)) return writeOut(L"", out, outCch);
        cJSON* arr = cJSON_CreateArray();
        for (size_t i = 0; i < m.size(); ++i) {
            string g = m[i].matched ? wToUtf8(m[i].str().c_str()) : string();
            cJSON_AddItemToArray(arr, cJSON_CreateString(g.c_str()));
        }
        char* js = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (!js) return 0;
        wstring w = utf8ToW(string(js));
        cJSON_free(js);
        return writeOut(w, out, outCch);
    } catch (const std::regex_error&) { return 0; }
}

// 정규식 치환(전역). replacement 는 $1,$2 캡처 참조 사용 가능(ECMAScript). 오류 0.
NOTEPAD_API int __stdcall Notepad_RegexReplace(const wchar_t* text, const wchar_t* pattern,
                                               const wchar_t* replacement, const wchar_t* flags,
                                               wchar_t* out, int outCch) {
    wstring t = text ? text : L"", p = pattern ? pattern : L"";
    wstring rep = replacement ? replacement : L"", fl = flags ? flags : L"";
    try {
        std::wregex re(p, regexFlags(fl));
        wstring res = std::regex_replace(t, re, rep);
        return writeOut(res, out, outCch);
    } catch (const std::regex_error&) { return 0; }
}

// ============================================================================
//  [세트 G] 화면 캡처 / 이미지서치 / 픽셀서치
// ============================================================================
static wstring iToW(int v) { char b[32]; snprintf(b, sizeof(b), "%d", v); return utf8ToW(string(b)); }

// (x,y) 픽셀 색 0xRRGGBB. 실패 -1.
NOTEPAD_API int __stdcall Notepad_GetPixelColor(int x, int y) {
    uint32_t rgb;
    if (!notepad::getPixelColor(x, y, rgb)) return -1;
    return (int)rgb;
}
// 화면 영역(좌상 x,y / 크기 w,h) -> PNG. 성공 1/0.
NOTEPAD_API int __stdcall Notepad_CaptureRegion(int x, int y, int w, int h,
                                                const wchar_t* outPath) {
    return notepad::captureRegionToPng(x, y, w, h, wToUtf8(outPath)) ? 1 : 0;
}
// 이미지서치: 찾으면 "x,y"(화면좌표) 반환, 못 찾으면 0. (단일 호출 권장: 넉넉한 버퍼)
NOTEPAD_API int __stdcall Notepad_ImageSearch(const wchar_t* imgPath,
                                              int x1, int y1, int x2, int y2,
                                              int tolerance, wchar_t* out, int outCch) {
    int fx = 0, fy = 0;
    if (!notepad::imageSearch(wToUtf8(imgPath), x1, y1, x2, y2, tolerance, fx, fy))
        return 0;
    return writeOut(iToW(fx) + L"," + iToW(fy), out, outCch);
}
// [세트 K] 이미지서치(모두 찾기): 매치들을 "x1,y1;x2,y2;..." 로 반환.
//   못 찾으면 빈 문자열(반환 0 아님 주의 — 길이 0). 로드 실패 등 오류 시 0.
//   maxMatches<=0 이면 제한 없음. 큰 영역/작은 needle 은 느릴 수 있으니 영역을 좁혀 쓸 것.
NOTEPAD_API int __stdcall Notepad_ImageSearchAll(const wchar_t* imgPath,
                                                 int x1, int y1, int x2, int y2,
                                                 int tolerance, int maxMatches,
                                                 wchar_t* out, int outCch) {
    std::vector<int> fxs, fys;
    int n = notepad::imageSearchAll(wToUtf8(imgPath), x1, y1, x2, y2,
                                    tolerance, maxMatches, fxs, fys);
    if (n < 0) return 0;                 // 오류(로드 실패/영역 무효)
    wstring result;
    for (size_t i = 0; i < fxs.size(); ++i) {
        if (i) result += L";";
        result += iToW(fxs[i]) + L"," + iToW(fys[i]);
    }
    return writeOut(result, out, outCch); // 0개면 빈 문자열(길이 0) 반환
}

// 픽셀서치: rgb(0xRRGGBB)±tolerance 첫 픽셀 "x,y", 없으면 0.
NOTEPAD_API int __stdcall Notepad_PixelSearch(int x1, int y1, int x2, int y2,
                                              int colorRgb, int tolerance,
                                              wchar_t* out, int outCch) {
    int fx = 0, fy = 0;
    if (!notepad::pixelSearch(x1, y1, x2, y2, (uint32_t)colorRgb, tolerance, fx, fy))
        return 0;
    return writeOut(iToW(fx) + L"," + iToW(fy), out, outCch);
}

// ============================================================================
//  [세트 H] 내장 OCR (Windows.Media.Ocr)
// ============================================================================
// 이미지 파일 OCR. lang 예: "ko"(빈 문자열=사용자 프로필 언어).
NOTEPAD_API int __stdcall Notepad_OcrImage(const wchar_t* imgPath, const wchar_t* lang,
                                           wchar_t* out, int outCch) {
    string text;
    if (!notepad::ocrImageFile(wToUtf8(imgPath), wToUtf8(lang), text)) return 0;
    return writeOut(utf8ToW(text), out, outCch);
}
// 화면 영역 OCR (좌상 x,y / 크기 w,h).
NOTEPAD_API int __stdcall Notepad_OcrScreen(int x, int y, int w, int h,
                                            const wchar_t* lang, wchar_t* out, int outCch) {
    string text;
    if (!notepad::ocrScreenRegion(x, y, w, h, wToUtf8(lang), text)) return 0;
    return writeOut(utf8ToW(text), out, outCch);
}
// 사용 가능한 OCR 언어 목록(공백 구분). 실패 0.
NOTEPAD_API int __stdcall Notepad_OcrLanguages(wchar_t* out, int outCch) {
    string langs;
    if (!notepad::ocrAvailableLanguages(langs)) return 0;
    return writeOut(utf8ToW(langs), out, outCch);
}

// ============================================================================
//  [세트 U] 텍스트 유틸  (순수 문자열 처리, UTF-16 기준)
// ============================================================================
#include <cwctype>

// 양끝 공백 제거
NOTEPAD_API int __stdcall Notepad_Trim(const wchar_t* text, wchar_t* out, int outCch) {
    wstring s = text ? text : L"";
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b-1])) b--;
    return writeOut(s.substr(a, b - a), out, outCch);
}
// 대문자화
NOTEPAD_API int __stdcall Notepad_Upper(const wchar_t* text, wchar_t* out, int outCch) {
    wstring s = text ? text : L"";
    for (auto& c : s) c = (wchar_t)towupper(c);
    return writeOut(s, out, outCch);
}
// 소문자화
NOTEPAD_API int __stdcall Notepad_Lower(const wchar_t* text, wchar_t* out, int outCch) {
    wstring s = text ? text : L"";
    for (auto& c : s) c = (wchar_t)towlower(c);
    return writeOut(s, out, outCch);
}
// 단어 첫글자 대문자(Title Case). 공백/구분자 뒤 글자를 대문자로.
NOTEPAD_API int __stdcall Notepad_TitleCase(const wchar_t* text, wchar_t* out, int outCch) {
    wstring s = text ? text : L"";
    bool start = true;
    for (auto& c : s) {
        if (iswspace(c) || c == L'-' || c == L'_') { start = true; }
        else { c = start ? (wchar_t)towupper(c) : (wchar_t)towlower(c); start = false; }
    }
    return writeOut(s, out, outCch);
}
// 문자열 n회 반복
NOTEPAD_API int __stdcall Notepad_Repeat(const wchar_t* text, int n, wchar_t* out, int outCch) {
    wstring s = text ? text : L"", r;
    if (n < 0) n = 0;
    r.reserve(s.size() * n);
    for (int i = 0; i < n; ++i) r += s;
    return writeOut(r, out, outCch);
}
// 모든 old 를 rep 로 치환(단순 문자열, 정규식 아님)
NOTEPAD_API int __stdcall Notepad_Replace(const wchar_t* text, const wchar_t* oldS,
                                          const wchar_t* rep, wchar_t* out, int outCch) {
    wstring s = text ? text : L"", o = oldS ? oldS : L"", r = rep ? rep : L"";
    if (o.empty()) return writeOut(s, out, outCch);
    wstring res; size_t pos = 0, p;
    while ((p = s.find(o, pos)) != wstring::npos) {
        res.append(s, pos, p - pos); res += r; pos = p + o.size();
    }
    res.append(s, pos, wstring::npos);
    return writeOut(res, out, outCch);
}
// 왼쪽을 pad 문자로 채워 총 width 길이로. (오른쪽 정렬)
NOTEPAD_API int __stdcall Notepad_PadLeft(const wchar_t* text, int width, const wchar_t* pad,
                                          wchar_t* out, int outCch) {
    wstring s = text ? text : L"";
    wchar_t pc = (pad && pad[0]) ? pad[0] : L' ';
    while ((int)s.size() < width) s = pc + s;
    return writeOut(s, out, outCch);
}
// 오른쪽을 pad 문자로 채워 총 width 길이로. (왼쪽 정렬)
NOTEPAD_API int __stdcall Notepad_PadRight(const wchar_t* text, int width, const wchar_t* pad,
                                           wchar_t* out, int outCch) {
    wstring s = text ? text : L"";
    wchar_t pc = (pad && pad[0]) ? pad[0] : L' ';
    while ((int)s.size() < width) s += pc;
    return writeOut(s, out, outCch);
}
// 문자열 뒤집기 (코드유닛 기준)
NOTEPAD_API int __stdcall Notepad_Reverse(const wchar_t* text, wchar_t* out, int outCch) {
    wstring s = text ? text : L"";
    std::reverse(s.begin(), s.end());
    return writeOut(s, out, outCch);
}
// 부분문자열 등장 횟수
NOTEPAD_API int __stdcall Notepad_CountSub(const wchar_t* text, const wchar_t* sub) {
    wstring s = text ? text : L"", n = sub ? sub : L"";
    if (n.empty()) return 0;
    int cnt = 0; size_t pos = 0, p;
    while ((p = s.find(n, pos)) != wstring::npos) { cnt++; pos = p + n.size(); }
    return cnt;
}
// 자연 정렬 비교("a2" < "a10"). a<b:-1, a==b:0, a>b:1
NOTEPAD_API int __stdcall Notepad_NaturalCompare(const wchar_t* aa, const wchar_t* bb) {
    const wchar_t* a = aa ? aa : L""; const wchar_t* b = bb ? bb : L"";
    while (*a && *b) {
        if (iswdigit(*a) && iswdigit(*b)) {
            // 숫자 블록 비교
            long long na = 0, nb = 0;
            while (*a == L'0') a++;   // 선행 0 스킵
            while (*b == L'0') b++;
            const wchar_t* sa = a; const wchar_t* sb = b;
            while (iswdigit(*a)) a++;
            while (iswdigit(*b)) b++;
            long long la = a - sa, lb = b - sb;
            if (la != lb) return la < lb ? -1 : 1;
            for (long long i = 0; i < la; ++i) { if (sa[i] != sb[i]) return sa[i] < sb[i] ? -1 : 1; }
        } else {
            wchar_t ca = (wchar_t)towlower(*a), cb = (wchar_t)towlower(*b);
            if (ca != cb) return ca < cb ? -1 : 1;
            a++; b++;
        }
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

// ============================================================================
//  [세트 V] 인코딩 / 변환
// ============================================================================
// 10진 정수를 지정 진법(2~36) 문자열로. 실패 ""
NOTEPAD_API int __stdcall Notepad_ToBase(const wchar_t* decStr, int base, wchar_t* out, int outCch) {
    if (base < 2 || base > 36) return 0;
    string s = wToUtf8(decStr);
    long long v = _strtoi64(s.c_str(), nullptr, 10);
    bool neg = v < 0; unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    static const char* D = "0123456789abcdefghijklmnopqrstuvwxyz";
    string r;
    if (u == 0) r = "0";
    while (u) { r += D[u % base]; u /= base; }
    if (neg) r += '-';
    std::reverse(r.begin(), r.end());
    return writeOut(utf8ToW(r), out, outCch);
}
// 지정 진법(2~36) 문자열을 10진 정수 문자열로. 실패 ""
NOTEPAD_API int __stdcall Notepad_FromBase(const wchar_t* str, int base, wchar_t* out, int outCch) {
    if (base < 2 || base > 36) return 0;
    string s = wToUtf8(str);
    char* end = nullptr;
    long long v = _strtoi64(s.c_str(), &end, base);
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", v);
    return writeOut(utf8ToW(string(buf)), out, outCch);
}
// URL 슬러그화: 소문자 + 영숫자만 남기고 나머지는 하이픈. 예: "Hello World!" -> "hello-world"
NOTEPAD_API int __stdcall Notepad_Slugify(const wchar_t* text, wchar_t* out, int outCch) {
    wstring s = text ? text : L"", r;
    bool lastDash = true;
    for (wchar_t c : s) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) {
            r += (wchar_t)towlower(c); lastDash = false;
        } else if (!lastDash) { r += L'-'; lastDash = true; }
    }
    while (!r.empty() && r.back() == L'-') r.pop_back();
    return writeOut(r, out, outCch);
}
// 바이트 수를 사람이 읽는 크기로. 예: 1536 -> "1.5 KB"
NOTEPAD_API int __stdcall Notepad_FormatBytes(const wchar_t* bytesStr, wchar_t* out, int outCch) {
    string s = wToUtf8(bytesStr);
    double b = _atoi64(s.c_str());
    const char* units[] = {"B","KB","MB","GB","TB","PB"};
    int u = 0;
    while (b >= 1024.0 && u < 5) { b /= 1024.0; u++; }
    char buf[64];
    if (u == 0) snprintf(buf, sizeof(buf), "%d B", (int)b);
    else snprintf(buf, sizeof(buf), "%.2f %s", b, units[u]);
    return writeOut(utf8ToW(string(buf)), out, outCch);
}
// 정수를 로마 숫자로(1~3999). 실패 ""
NOTEPAD_API int __stdcall Notepad_ToRoman(int n, wchar_t* out, int outCch) {
    if (n < 1 || n > 3999) return 0;
    static const int val[] = {1000,900,500,400,100,90,50,40,10,9,5,4,1};
    static const wchar_t* sym[] = {L"M",L"CM",L"D",L"CD",L"C",L"XC",L"L",L"XL",L"X",L"IX",L"V",L"IV",L"I"};
    wstring r;
    for (int i = 0; i < 13; ++i) while (n >= val[i]) { r += sym[i]; n -= val[i]; }
    return writeOut(r, out, outCch);
}
// 로마 숫자를 정수로. 실패 -1
NOTEPAD_API int __stdcall Notepad_FromRoman(const wchar_t* roman) {
    wstring s = roman ? roman : L"";
    for (auto& c : s) c = (wchar_t)towupper(c);
    auto v = [](wchar_t c)->int{ switch(c){case L'I':return 1;case L'V':return 5;case L'X':return 10;
        case L'L':return 50;case L'C':return 100;case L'D':return 500;case L'M':return 1000;default:return 0;} };
    int total = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        int cur = v(s[i]); if (cur == 0) return -1;
        int nxt = (i + 1 < s.size()) ? v(s[i+1]) : 0;
        total += (cur < nxt) ? -cur : cur;
    }
    return total;
}
// 한글 문자열의 초성 추출. 예: "안녕하세요" -> "ㅇㄴㅎㅅㅇ". 한글 아닌 글자는 그대로.
NOTEPAD_API int __stdcall Notepad_HangulChosung(const wchar_t* text, wchar_t* out, int outCch) {
    static const wchar_t* CHO = L"ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ";
    wstring s = text ? text : L"", r;
    for (wchar_t c : s) {
        if (c >= 0xAC00 && c <= 0xD7A3) {   // 완성형 한글
            int idx = (c - 0xAC00) / (21 * 28);
            r += CHO[idx];
        } else r += c;
    }
    return writeOut(r, out, outCch);
}
// 전각 영숫자/기호를 반각으로 정규화. 예: "ＡＢＣ１２３" -> "ABC123"
NOTEPAD_API int __stdcall Notepad_ToHalfWidth(const wchar_t* text, wchar_t* out, int outCch) {
    wstring s = text ? text : L"", r;
    for (wchar_t c : s) {
        if (c >= 0xFF01 && c <= 0xFF5E) r += (wchar_t)(c - 0xFEE0);   // 전각 -> 반각
        else if (c == 0x3000) r += L' ';                              // 전각 공백
        else r += c;
    }
    return writeOut(r, out, outCch);
}

// ============================================================================
//  [세트 Y] 수학 / 통계  (구분자로 나뉜 숫자 목록 입력. 기본 구분자 콤마/공백/개행)
// ============================================================================
#include <vector>
#include <cmath>

// 입력 문자열에서 숫자들을 파싱(콤마/공백/탭/개행/세미콜론 구분).
static std::vector<double> parseNums(const wchar_t* text) {
    std::vector<double> v;
    string s = wToUtf8(text);
    string cur;
    auto flush = [&](){ if (!cur.empty()) { v.push_back(atof(cur.c_str())); cur.clear(); } };
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';') flush();
        else cur += c;
    }
    flush();
    return v;
}
static int numToStr(double v, wchar_t* out, int outCch) {
    char buf[64];
    if (v == (double)(long long)v) snprintf(buf, sizeof(buf), "%lld", (long long)v);
    else snprintf(buf, sizeof(buf), "%.10g", v);
    return writeOut(utf8ToW(string(buf)), out, outCch);
}

// 합계
NOTEPAD_API int __stdcall Notepad_Sum(const wchar_t* nums, wchar_t* out, int outCch) {
    auto v = parseNums(nums); double s = 0; for (double x : v) s += x;
    return numToStr(s, out, outCch);
}
// 평균(산술)
NOTEPAD_API int __stdcall Notepad_Mean(const wchar_t* nums, wchar_t* out, int outCch) {
    auto v = parseNums(nums); if (v.empty()) return 0;
    double s = 0; for (double x : v) s += x;
    return numToStr(s / v.size(), out, outCch);
}
// 중앙값
NOTEPAD_API int __stdcall Notepad_Median(const wchar_t* nums, wchar_t* out, int outCch) {
    auto v = parseNums(nums); if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    double m = (n % 2) ? v[n/2] : (v[n/2 - 1] + v[n/2]) / 2.0;
    return numToStr(m, out, outCch);
}
// 표준편차(모집단)
NOTEPAD_API int __stdcall Notepad_Stdev(const wchar_t* nums, wchar_t* out, int outCch) {
    auto v = parseNums(nums); if (v.empty()) return 0;
    double s = 0; for (double x : v) s += x; double mean = s / v.size();
    double var = 0; for (double x : v) var += (x - mean) * (x - mean); var /= v.size();
    return numToStr(std::sqrt(var), out, outCch);
}
// 최댓값
NOTEPAD_API int __stdcall Notepad_Max(const wchar_t* nums, wchar_t* out, int outCch) {
    auto v = parseNums(nums); if (v.empty()) return 0;
    double m = v[0]; for (double x : v) if (x > m) m = x;
    return numToStr(m, out, outCch);
}
// 최솟값
NOTEPAD_API int __stdcall Notepad_Min(const wchar_t* nums, wchar_t* out, int outCch) {
    auto v = parseNums(nums); if (v.empty()) return 0;
    double m = v[0]; for (double x : v) if (x < m) m = x;
    return numToStr(m, out, outCch);
}
// 최대공약수(두 정수)
NOTEPAD_API int __stdcall Notepad_Gcd(int a, int b) {
    a = a < 0 ? -a : a; b = b < 0 ? -b : b;
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}
// 최소공배수(두 정수). 오버플로 방지 위해 __int64.
NOTEPAD_API __int64 __stdcall Notepad_Lcm(int a, int b) {
    if (a == 0 || b == 0) return 0;
    int x = a < 0 ? -a : a, y = b < 0 ? -b : b;
    int g = x; int yy = y; while (yy) { int t = g % yy; g = yy; yy = t; }
    return (__int64)x / g * y;
}
// [min,max] 범위의 안전한 난수 정수. CNG 난수 사용.
NOTEPAD_API int __stdcall Notepad_RandInt(int lo, int hi) {
    if (lo > hi) { int t = lo; lo = hi; hi = t; }
    unsigned int range = (unsigned int)(hi - lo) + 1;
    if (range == 0) return lo;   // 전 범위
    string r;
    if (!notepad::randomBytes(4, r) || r.size() < 4) return lo;
    unsigned int u = (unsigned char)r[0] | ((unsigned char)r[1] << 8)
                   | ((unsigned char)r[2] << 16) | ((unsigned char)r[3] << 24);
    return lo + (int)(u % range);
}

// ============================================================================
//  [세트 Z] 검증 / 체크섬
// ============================================================================
// Adler-32 체크섬 -> hex 8자
NOTEPAD_API int __stdcall Notepad_Adler32(const wchar_t* text, wchar_t* out, int outCch) {
    string s = wToUtf8(text);
    const unsigned int MOD = 65521;
    unsigned int a = 1, b = 0;
    for (unsigned char c : s) { a = (a + c) % MOD; b = (b + a) % MOD; }
    unsigned int sum = (b << 16) | a;
    wchar_t buf[9]; static const wchar_t* H = L"0123456789abcdef";
    for (int i = 0; i < 8; ++i) buf[i] = H[(sum >> ((7 - i) * 4)) & 0xF];
    buf[8] = 0;
    return writeOut(wstring(buf), out, outCch);
}
// 파일 CRC32 -> hex 8자. 실패 ""
NOTEPAD_API int __stdcall Notepad_CRC32File(const wchar_t* path, wchar_t* out, int outCch) {
    string data;
    if (!notepad::readFileBytes(wToUtf8(path), data)) return 0;
    unsigned int c = notepad::crc32sum(data);
    wchar_t buf[9]; static const wchar_t* H = L"0123456789abcdef";
    for (int i = 0; i < 8; ++i) buf[i] = H[(c >> ((7 - i) * 4)) & 0xF];
    buf[8] = 0;
    return writeOut(wstring(buf), out, outCch);
}
// Luhn 검증(신용카드/식별번호 체크섬). 유효 1 / 무효 0. (숫자 외 문자는 무시)
NOTEPAD_API int __stdcall Notepad_LuhnCheck(const wchar_t* number) {
    wstring s = number ? number : L"";
    int sum = 0; bool alt = false; int cnt = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) {
        if (s[i] < L'0' || s[i] > L'9') continue;
        int d = s[i] - L'0'; cnt++;
        if (alt) { d *= 2; if (d > 9) d -= 9; }
        sum += d; alt = !alt;
    }
    if (cnt == 0) return 0;
    return (sum % 10 == 0) ? 1 : 0;
}
// 문자열의 섀넌 엔트로피 x1000 정수 반환(비트/문자). 비밀번호 강도 등에.
NOTEPAD_API int __stdcall Notepad_Entropy(const wchar_t* text) {
    wstring s = text ? text : L"";
    if (s.empty()) return 0;
    static int f[65536];     // BMP 코드유닛 빈도(간이). static 으로 스택 절약.
    memset(f, 0, sizeof(f));
    for (wchar_t c : s) f[(unsigned short)c]++;
    double H = 0; double n = (double)s.size();
    for (int i = 0; i < 65536; ++i) if (f[i]) { double p = f[i] / n; H -= p * (std::log(p) / std::log(2.0)); }
    return (int)(H * 1000 + 0.5);
}
// FNV-1a 32비트 해시 -> hex 8자 (빠른 비암호 해시, 해시테이블/중복검출용)
NOTEPAD_API int __stdcall Notepad_Fnv1a(const wchar_t* text, wchar_t* out, int outCch) {
    string s = wToUtf8(text);
    unsigned int h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    wchar_t buf[9]; static const wchar_t* H = L"0123456789abcdef";
    for (int i = 0; i < 8; ++i) buf[i] = H[(h >> ((7 - i) * 4)) & 0xF];
    buf[8] = 0;
    return writeOut(wstring(buf), out, outCch);
}

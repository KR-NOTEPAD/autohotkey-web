// ============================================================================
//  NOTEPAD.dll  -  notepad_crypto.cpp
//  [NOTEPAD] Windows CNG(bcrypt) 암호화 + miniz 압축/ZIP + WinHTTP 핵심 구현.
//  링크: bcrypt.lib  winhttp.lib  (miniz.c / cJSON.c 는 함께 컴파일).
// ============================================================================
#include "notepad_crypto.h"
#include "miniz.h"
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <wincrypt.h>   // [NOTEPAD] 세트 P: DPAPI (CryptProtectData)
#include <vector>
#include <cstdio>

#pragma comment(lib, "crypt32.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace notepad {

// ---- UTF-8 경로 -> 와이드 (파일 I/O 공통) ----------------------------------
static std::wstring pathToW(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                                nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], n);
    return w;
}

// ============================================================================
//  해시 (일반화) — SHA-256/384/512, SHA-1, MD5, HMAC 모두 이 두 함수로 처리
//  -> 알고리즘별 CNG 보일러플레이트 중복 제거.
// ============================================================================
static bool hashCore(const wchar_t* algId, bool hmac, const std::string& key,
                     const std::string& data, std::string& out) {
    out.clear();
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;
    DWORD hashLen = 0, cb = 0;

    DWORD flags = hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, algId, nullptr, flags)))
        return false;
    if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                                      (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0)))
        goto cleanup;
    if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                                     hmac ? (PUCHAR)key.data() : nullptr,
                                     hmac ? (ULONG)key.size() : 0, 0)))
        goto cleanup;
    if (!data.empty() &&
        !NT_SUCCESS(BCryptHashData(hHash, (PUCHAR)data.data(),
                                   (ULONG)data.size(), 0)))
        goto cleanup;
    out.resize(hashLen);
    if (!NT_SUCCESS(BCryptFinishHash(hHash, (PUCHAR)out.data(), hashLen, 0))) {
        out.clear();
        goto cleanup;
    }
    ok = true;
cleanup:
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg)  BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool hashDigest(const wchar_t* algId, const std::string& data, std::string& out) {
    return hashCore(algId, false, std::string(), data, out);
}
bool hmacDigest(const wchar_t* algId, const std::string& key,
                const std::string& data, std::string& out) {
    return hashCore(algId, true, key, data, out);
}
bool sha256(const std::string& data, std::string& out) {
    return hashDigest(BCRYPT_SHA256_ALGORITHM, data, out);
}

// ---- PBKDF2-HMAC-SHA256 ----------------------------------------------------
bool pbkdf2(const std::string& password, const std::string& salt,
            unsigned int iterations, unsigned int dkLen, std::string& out) {
    out.clear();
    if (dkLen == 0 || iterations == 0) return false;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(
            &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
            BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return false;
    out.resize(dkLen);
    NTSTATUS st = BCryptDeriveKeyPBKDF2(
        hAlg,
        (PUCHAR)password.data(), (ULONG)password.size(),
        (PUCHAR)salt.data(), (ULONG)salt.size(),
        (ULONGLONG)iterations,
        (PUCHAR)out.data(), (ULONG)dkLen, 0);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!NT_SUCCESS(st)) { out.clear(); return false; }
    return true;
}

// ---- 난수 ------------------------------------------------------------------
bool randomBytes(unsigned int n, std::string& out) {
    out.clear();
    if (n == 0) return true;
    out.resize(n);
    NTSTATUS st = BCryptGenRandom(nullptr, (PUCHAR)out.data(), (ULONG)n,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(st)) { out.clear(); return false; }
    return true;
}

// ---- AES-256-GCM -----------------------------------------------------------
static bool makeAesKey(BCRYPT_ALG_HANDLE& hAlg, BCRYPT_KEY_HANDLE& hKey,
                       const std::string& key) {
    hAlg = nullptr; hKey = nullptr;
    if (key.size() != 32) return false;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM,
                                                nullptr, 0)))
        return false;
    if (!NT_SUCCESS(BCryptSetProperty(
            hAlg, BCRYPT_CHAINING_MODE,
            (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(
            hAlg, &hKey, nullptr, 0,
            (PUCHAR)key.data(), (ULONG)key.size(), 0))) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    return true;
}

bool aesGcmEncrypt(const std::string& key, const std::string& nonce,
                   const std::string& plaintext, std::string& out,
                   std::string& tag) {
    out.clear(); tag.clear();
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (!makeAesKey(hAlg, hKey, key)) return false;
    bool ok = false;
    tag.resize(16);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)nonce.data();
    info.cbNonce = (ULONG)nonce.size();
    info.pbTag = (PUCHAR)tag.data();
    info.cbTag = (ULONG)tag.size();
    ULONG cbResult = 0;
    out.resize(plaintext.size());
    NTSTATUS st = BCryptEncrypt(
        hKey, (PUCHAR)plaintext.data(), (ULONG)plaintext.size(),
        &info, nullptr, 0,
        (PUCHAR)out.data(), (ULONG)out.size(), &cbResult, 0);
    if (NT_SUCCESS(st)) { out.resize(cbResult); ok = true; }
    else { out.clear(); tag.clear(); }
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool aesGcmDecrypt(const std::string& key, const std::string& nonce,
                   const std::string& ciphertext, const std::string& tag,
                   std::string& out) {
    out.clear();
    if (tag.size() != 16) return false;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (!makeAesKey(hAlg, hKey, key)) return false;
    bool ok = false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)nonce.data();
    info.cbNonce = (ULONG)nonce.size();
    info.pbTag = (PUCHAR)tag.data();
    info.cbTag = (ULONG)tag.size();
    ULONG cbResult = 0;
    out.resize(ciphertext.size());
    NTSTATUS st = BCryptDecrypt(
        hKey, (PUCHAR)ciphertext.data(), (ULONG)ciphertext.size(),
        &info, nullptr, 0,
        (PUCHAR)out.data(), (ULONG)out.size(), &cbResult, 0);
    if (NT_SUCCESS(st)) { out.resize(cbResult); ok = true; }
    else { out.clear(); }
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// ============================================================================
//  [세트 M] ECDSA P-256 전자서명 (CNG / bcrypt)
//  서명 대상은 data 의 SHA-256 다이제스트(표준 관행). 서명은 raw (r||s) 64B.
// ============================================================================
bool eccGenerateKeyPair(std::string& privBlob, std::string& pubBlob) {
    privBlob.clear(); pubBlob.clear();
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool ok = false;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
        return false;
    if (!NT_SUCCESS(BCryptGenerateKeyPair(hAlg, &hKey, 256, 0))) goto cleanup;
    if (!NT_SUCCESS(BCryptFinalizeKeyPair(hKey, 0))) goto cleanup;
    {
        DWORD cb = 0;
        // private blob
        if (!NT_SUCCESS(BCryptExportKey(hKey, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                        nullptr, 0, &cb, 0))) goto cleanup;
        privBlob.resize(cb);
        if (!NT_SUCCESS(BCryptExportKey(hKey, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                        (PUCHAR)privBlob.data(), cb, &cb, 0))) goto cleanup;
        privBlob.resize(cb);
        // public blob
        cb = 0;
        if (!NT_SUCCESS(BCryptExportKey(hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                        nullptr, 0, &cb, 0))) goto cleanup;
        pubBlob.resize(cb);
        if (!NT_SUCCESS(BCryptExportKey(hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                        (PUCHAR)pubBlob.data(), cb, &cb, 0))) goto cleanup;
        pubBlob.resize(cb);
        ok = true;
    }
cleanup:
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!ok) { privBlob.clear(); pubBlob.clear(); }
    return ok;
}

bool eccSign(const std::string& privBlob, const std::string& data, std::string& sigOut) {
    sigOut.clear();
    std::string digest;
    if (!sha256(data, digest)) return false;   // 32바이트 해시에 서명
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool ok = false;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
        return false;
    if (!NT_SUCCESS(BCryptImportKeyPair(hAlg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &hKey,
                                        (PUCHAR)privBlob.data(), (ULONG)privBlob.size(), 0)))
        goto cleanup;
    {
        DWORD cbSig = 0;
        if (!NT_SUCCESS(BCryptSignHash(hKey, nullptr,
                (PUCHAR)digest.data(), (ULONG)digest.size(),
                nullptr, 0, &cbSig, 0))) goto cleanup;
        sigOut.resize(cbSig);
        if (!NT_SUCCESS(BCryptSignHash(hKey, nullptr,
                (PUCHAR)digest.data(), (ULONG)digest.size(),
                (PUCHAR)sigOut.data(), cbSig, &cbSig, 0))) { sigOut.clear(); goto cleanup; }
        sigOut.resize(cbSig);
        ok = true;
    }
cleanup:
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool eccVerify(const std::string& pubBlob, const std::string& data, const std::string& sig) {
    std::string digest;
    if (!sha256(data, digest)) return false;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool ok = false;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
        return false;
    if (!NT_SUCCESS(BCryptImportKeyPair(hAlg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &hKey,
                                        (PUCHAR)pubBlob.data(), (ULONG)pubBlob.size(), 0)))
        goto cleanup;
    {
        NTSTATUS st = BCryptVerifySignature(hKey, nullptr,
                (PUCHAR)digest.data(), (ULONG)digest.size(),
                (PUCHAR)sig.data(), (ULONG)sig.size(), 0);
        ok = NT_SUCCESS(st);   // STATUS_INVALID_SIGNATURE 면 false
    }
cleanup:
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// ============================================================================
//  [세트 P] DPAPI — 사용자/머신 계정 기반 자동 암호화 (비밀번호 불필요)
// ============================================================================
bool dpapiProtect(const std::string& data, const std::string& optionalEntropy,
                  bool machineScope, std::string& out) {
    out.clear();
    DATA_BLOB in;  in.pbData = (BYTE*)data.data();  in.cbData = (DWORD)data.size();
    DATA_BLOB ent; ent.pbData = (BYTE*)optionalEntropy.data();
    ent.cbData = (DWORD)optionalEntropy.size();
    DATA_BLOB outBlob; outBlob.pbData = nullptr; outBlob.cbData = 0;

    DWORD flags = CRYPTPROTECT_UI_FORBIDDEN;
    if (machineScope) flags |= CRYPTPROTECT_LOCAL_MACHINE;

    if (!CryptProtectData(&in, L"NOTEPAD", optionalEntropy.empty() ? nullptr : &ent,
                          nullptr, nullptr, flags, &outBlob))
        return false;
    out.assign((const char*)outBlob.pbData, outBlob.cbData);
    if (outBlob.pbData) LocalFree(outBlob.pbData);
    return true;
}

bool dpapiUnprotect(const std::string& blob, const std::string& optionalEntropy,
                    std::string& out) {
    out.clear();
    DATA_BLOB in;  in.pbData = (BYTE*)blob.data();  in.cbData = (DWORD)blob.size();
    DATA_BLOB ent; ent.pbData = (BYTE*)optionalEntropy.data();
    ent.cbData = (DWORD)optionalEntropy.size();
    DATA_BLOB outBlob; outBlob.pbData = nullptr; outBlob.cbData = 0;

    if (!CryptUnprotectData(&in, nullptr, optionalEntropy.empty() ? nullptr : &ent,
                            nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob))
        return false;   // 다른 사용자/머신/변조 시 실패
    out.assign((const char*)outBlob.pbData, outBlob.cbData);
    if (outBlob.pbData) LocalFree(outBlob.pbData);
    return true;
}

// ---- 텍스트/인메모리 압축 (기존 형식 유지) ---------------------------------
bool zipDeflate(const std::string& data, std::string& out, int level) {
    out.clear();
    if (level < 0) level = 0;
    if (level > 9) level = 9;
    mz_ulong srcLen = (mz_ulong)data.size();
    mz_ulong bound = mz_compressBound(srcLen);
    std::vector<unsigned char> buf(bound ? bound : 1);
    int r = mz_compress2(buf.data(), &bound,
                         (const unsigned char*)data.data(), srcLen, level);
    if (r != MZ_OK) return false;
    out.resize(4);
    unsigned int n = (unsigned int)data.size();
    out[0] = (char)(n & 0xFF);
    out[1] = (char)((n >> 8) & 0xFF);
    out[2] = (char)((n >> 16) & 0xFF);
    out[3] = (char)((n >> 24) & 0xFF);
    out.append((const char*)buf.data(), (size_t)bound);
    return true;
}

bool zipInflate(const std::string& data, std::string& out) {
    out.clear();
    if (data.size() < 4) return false;
    unsigned int n = (unsigned char)data[0]
                   | ((unsigned char)data[1] << 8)
                   | ((unsigned char)data[2] << 16)
                   | ((unsigned char)data[3] << 24);
    if (n == 0) { out.clear(); return true; }
    std::vector<unsigned char> buf(n);
    mz_ulong destLen = (mz_ulong)n;
    int r = mz_uncompress(buf.data(), &destLen,
                          (const unsigned char*)data.data() + 4,
                          (mz_ulong)(data.size() - 4));
    if (r != MZ_OK || destLen != n) return false;
    out.assign((const char*)buf.data(), (size_t)destLen);
    return true;
}

uint32_t crc32sum(const std::string& data) {
    return (uint32_t)mz_crc32(MZ_CRC32_INIT,
                              (const unsigned char*)data.data(), data.size());
}

// ---- 파일 I/O --------------------------------------------------------------
bool readFileBytes(const std::string& pathUtf8, std::string& out) {
    out.clear();
    std::wstring wpath = pathToW(pathUtf8);
    if (wpath.empty()) return false;
    FILE* fp = _wfopen(wpath.c_str(), L"rb");
    if (!fp) return false;
    std::vector<char> chunk(64 * 1024);
    size_t rd;
    while ((rd = fread(chunk.data(), 1, chunk.size(), fp)) > 0)
        out.append(chunk.data(), rd);
    fclose(fp);
    return true;
}

bool writeFileBytes(const std::string& pathUtf8, const std::string& data) {
    std::wstring wpath = pathToW(pathUtf8);
    if (wpath.empty()) return false;
    FILE* fp = _wfopen(wpath.c_str(), L"wb");
    if (!fp) return false;
    bool ok = true;
    if (!data.empty())
        ok = (fwrite(data.data(), 1, data.size(), fp) == data.size());
    fclose(fp);
    return ok;
}

// ---- 파일 해시 (스트리밍) --------------------------------------------------
bool hashFile(const wchar_t* algId, const std::string& pathUtf8, std::string& out) {
    out.clear();
    std::wstring wpath = pathToW(pathUtf8);
    if (wpath.empty()) return false;
    FILE* fp = _wfopen(wpath.c_str(), L"rb");
    if (!fp) return false;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;
    DWORD hashLen = 0, cb = 0;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, algId, nullptr, 0)))
        { fclose(fp); return false; }
    if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                                      (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0)))
        goto cleanup;
    if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0)))
        goto cleanup;
    {
        std::vector<unsigned char> chunk(64 * 1024);
        size_t rd;
        while ((rd = fread(chunk.data(), 1, chunk.size(), fp)) > 0) {
            if (!NT_SUCCESS(BCryptHashData(hHash, chunk.data(), (ULONG)rd, 0)))
                goto cleanup;
        }
    }
    out.resize(hashLen);
    if (!NT_SUCCESS(BCryptFinishHash(hHash, (PUCHAR)out.data(), hashLen, 0))) {
        out.clear();
        goto cleanup;
    }
    ok = true;
cleanup:
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg)  BCryptCloseAlgorithmProvider(hAlg, 0);
    fclose(fp);
    return ok;
}

bool sha256File(const std::string& pathUtf8, std::string& out) {
    return hashFile(BCRYPT_SHA256_ALGORITHM, pathUtf8, out);
}

// ---- 대용량 파일 압축/해제 (스트리밍) --------------------------------------
// 형식: "NPZ1"(4B) + 원본크기(8B LE) + zlib-wrapped DEFLATE 스트림.
static const char* NPZ_MAGIC = "NPZ1";

bool compressFileStream(const std::string& inPathUtf8, const std::string& outPathUtf8,
                        int level) {
    if (level < 0) level = 6;
    if (level > 9) level = 9;
    std::wstring win = pathToW(inPathUtf8), wout = pathToW(outPathUtf8);
    if (win.empty() || wout.empty()) return false;

    FILE* fin = _wfopen(win.c_str(), L"rb");
    if (!fin) return false;
    FILE* fout = _wfopen(wout.c_str(), L"wb");
    if (!fout) { fclose(fin); return false; }

    // 헤더 예약: magic(4) + size(8). 크기는 마지막에 다시 채운다.
    unsigned long long total = 0;
    char header[12] = {0};
    memcpy(header, NPZ_MAGIC, 4);
    fwrite(header, 1, 12, fout);

    mz_stream s; memset(&s, 0, sizeof(s));
    if (mz_deflateInit2(&s, level, MZ_DEFLATED, MZ_DEFAULT_WINDOW_BITS, 9,
                        MZ_DEFAULT_STRATEGY) != MZ_OK) {
        fclose(fin); fclose(fout); return false;
    }

    std::vector<unsigned char> inbuf(64 * 1024), outbuf(64 * 1024);
    bool ok = true;
    int flush = MZ_NO_FLUSH;
    do {
        size_t rd = fread(inbuf.data(), 1, inbuf.size(), fin);
        total += rd;
        if (rd < inbuf.size()) flush = MZ_FINISH; // 마지막 청크
        s.next_in = inbuf.data();
        s.avail_in = (unsigned int)rd;
        do {
            s.next_out = outbuf.data();
            s.avail_out = (unsigned int)outbuf.size();
            int r = mz_deflate(&s, flush);
            if (r == MZ_STREAM_ERROR) { ok = false; break; }
            size_t have = outbuf.size() - s.avail_out;
            if (have && fwrite(outbuf.data(), 1, have, fout) != have) { ok = false; break; }
        } while (s.avail_out == 0);
        if (!ok) break;
    } while (flush != MZ_FINISH);

    mz_deflateEnd(&s);
    fclose(fin);

    if (ok) {
        // 헤더의 8바이트 원본 크기 채우기
        fseek(fout, 4, SEEK_SET);
        unsigned char sz[8];
        for (int i = 0; i < 8; ++i) sz[i] = (unsigned char)((total >> (8 * i)) & 0xFF);
        fwrite(sz, 1, 8, fout);
    }
    fclose(fout);
    return ok;
}

bool decompressFileStream(const std::string& inPathUtf8, const std::string& outPathUtf8) {
    std::wstring win = pathToW(inPathUtf8), wout = pathToW(outPathUtf8);
    if (win.empty() || wout.empty()) return false;
    FILE* fin = _wfopen(win.c_str(), L"rb");
    if (!fin) return false;

    char header[12];
    if (fread(header, 1, 12, fin) != 12 || memcmp(header, NPZ_MAGIC, 4) != 0) {
        fclose(fin); return false;
    }
    FILE* fout = _wfopen(wout.c_str(), L"wb");
    if (!fout) { fclose(fin); return false; }

    mz_stream s; memset(&s, 0, sizeof(s));
    if (mz_inflateInit(&s) != MZ_OK) { fclose(fin); fclose(fout); return false; }

    std::vector<unsigned char> inbuf(64 * 1024), outbuf(64 * 1024);
    bool ok = true; int r = MZ_OK;
    do {
        size_t rd = fread(inbuf.data(), 1, inbuf.size(), fin);
        if (rd == 0) break;
        s.next_in = inbuf.data();
        s.avail_in = (unsigned int)rd;
        do {
            s.next_out = outbuf.data();
            s.avail_out = (unsigned int)outbuf.size();
            r = mz_inflate(&s, MZ_NO_FLUSH);
            if (r == MZ_STREAM_ERROR || r == MZ_DATA_ERROR || r == MZ_MEM_ERROR) {
                ok = false; break;
            }
            size_t have = outbuf.size() - s.avail_out;
            if (have && fwrite(outbuf.data(), 1, have, fout) != have) { ok = false; break; }
        } while (s.avail_out == 0 && r != MZ_STREAM_END);
        if (!ok || r == MZ_STREAM_END) break;
    } while (true);

    mz_inflateEnd(&s);
    fclose(fin); fclose(fout);
    return ok && (r == MZ_STREAM_END);
}

// ---- ZIP 아카이브 (in-memory + wide 경로 안전) -----------------------------
// 중간 디렉터리 생성 (extract 시 하위 경로 대비)
static void makeDirsW(const std::wstring& fullPath) {
    // fullPath 는 파일 경로. 상위 디렉터리만 생성.
    size_t pos = fullPath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return;
    std::wstring dir = fullPath.substr(0, pos);
    std::wstring build;
    for (size_t i = 0; i < dir.size(); ++i) {
        wchar_t c = dir[i];
        if (c == L'/') c = L'\\';
        build += c;
        if (c == L'\\' && build.size() > 3)
            CreateDirectoryW(build.c_str(), nullptr);
    }
    if (!build.empty()) CreateDirectoryW(build.c_str(), nullptr);
}

bool zipAddFile(const std::string& zipPathUtf8, const std::string& srcPathUtf8,
                const std::string& nameInZip, int level) {
    if (level < 0) level = 6;
    if (level > 9) level = 9;
    std::string src;
    if (!readFileBytes(srcPathUtf8, src)) return false; // 원본 읽기

    // 기존 zip 이 있으면 in-memory 로 읽어 새 힙 아카이브에 항목을 그대로 복사한 뒤
    // 새 항목을 추가하고 다시 저장한다(경로/유니코드 안전, in-place fopen 회피).
    std::string existing;
    bool hasExisting = readFileBytes(zipPathUtf8, existing) && existing.size() > 22;

    mz_zip_archive rd; memset(&rd, 0, sizeof(rd));
    bool haveReader = false;
    if (hasExisting) {
        if (mz_zip_reader_init_mem(&rd, existing.data(), existing.size(), 0))
            haveReader = true;
        else
            return false; // 손상된 zip
    }

    mz_zip_archive wr; memset(&wr, 0, sizeof(wr));
    if (!mz_zip_writer_init_heap(&wr, 0, 0)) {
        if (haveReader) mz_zip_reader_end(&rd);
        return false;
    }

    bool ok = true;
    if (haveReader) {
        mz_uint n = mz_zip_reader_get_num_files(&rd);
        for (mz_uint i = 0; i < n && ok; ++i) {
            // 같은 이름이면 새 것으로 대체(기존 것 건너뜀)
            mz_zip_archive_file_stat st;
            if (mz_zip_reader_file_stat(&rd, i, &st) && nameInZip == st.m_filename)
                continue;
            if (!mz_zip_writer_add_from_zip_reader(&wr, &rd, i)) ok = false;
        }
    }
    if (ok)
        ok = mz_zip_writer_add_mem(&wr, nameInZip.c_str(), src.data(), src.size(),
                                   (mz_uint)level) != 0;

    void* pBuf = nullptr; size_t bufSize = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&wr, &pBuf, &bufSize) != 0;
    if (ok) ok = writeFileBytes(zipPathUtf8, std::string((char*)pBuf, bufSize));
    if (pBuf) mz_free(pBuf);

    mz_zip_writer_end(&wr);
    if (haveReader) mz_zip_reader_end(&rd);
    return ok;
}

bool zipList(const std::string& zipPathUtf8, std::string& namesOut) {
    namesOut.clear();
    std::string data;
    if (!readFileBytes(zipPathUtf8, data) || data.empty()) return false;
    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return false;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    bool ok = true;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { ok = false; break; }
        if (!namesOut.empty()) namesOut += '\n';
        namesOut += st.m_filename;
    }
    mz_zip_reader_end(&zip);
    return ok;
}

bool zipExtractFile(const std::string& zipPathUtf8, const std::string& nameInZip,
                    const std::string& outPathUtf8) {
    std::string data;
    if (!readFileBytes(zipPathUtf8, data) || data.empty()) return false;
    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return false;
    size_t sz = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&zip, nameInZip.c_str(), &sz, 0);
    bool ok = false;
    if (p) {
        std::wstring wout = pathToW(outPathUtf8);
        makeDirsW(wout);
        ok = writeFileBytes(outPathUtf8, std::string((char*)p, sz));
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

bool zipExtractAll(const std::string& zipPathUtf8, const std::string& destDirUtf8) {
    std::string data;
    if (!readFileBytes(zipPathUtf8, data) || data.empty()) return false;
    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return false;

    std::wstring wdest = pathToW(destDirUtf8);
    if (!wdest.empty()) CreateDirectoryW(wdest.c_str(), nullptr);

    mz_uint n = mz_zip_reader_get_num_files(&zip);
    bool ok = true;
    for (mz_uint i = 0; i < n && ok; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { ok = false; break; }
        std::string name = st.m_filename;
        // 항목 이름 안의 '/' -> '\\'
        std::string rel;
        for (char c : name) rel += (c == '/') ? '\\' : c;
        std::string outPath = destDirUtf8;
        if (!outPath.empty() && outPath.back() != '\\' && outPath.back() != '/')
            outPath += '\\';
        outPath += rel;
        std::wstring wout = pathToW(outPath);
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            if (!wout.empty()) { CreateDirectoryW(wout.c_str(), nullptr); }
            continue;
        }
        makeDirsW(wout);
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!p) { ok = false; break; }
        ok = writeFileBytes(outPath, std::string((char*)p, sz));
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

// ============================================================================
//  HTTP (WinHTTP)
// ============================================================================
bool httpRequest(const std::string& method, const std::string& url,
                 const std::string& body, const std::string& contentType,
                 std::string& respOut, int& statusOut) {
    respOut.clear(); statusOut = 0;
    std::wstring wurl = pathToW(url);
    if (wurl.empty()) return false;

    URL_COMPONENTS uc; memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = {0}, path[8192] = {0}, extra[8192] = {0};
    uc.lpszHostName = host;     uc.dwHostNameLength = 512;
    uc.lpszUrlPath = path;      uc.dwUrlPathLength = 8192;
    uc.lpszExtraInfo = extra;   uc.dwExtraInfoLength = 8192;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;

    std::wstring fullPath = path;
    if (extra[0]) fullPath += extra;
    if (fullPath.empty()) fullPath = L"/";

    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    std::wstring wmethod = pathToW(method);
    if (wmethod.empty()) wmethod = L"GET";

    HINTERNET hSession = WinHttpOpen(L"NOTEPAD/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    HINTERNET hRequest = nullptr;
    if (hConnect) {
        DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
        hRequest = WinHttpOpenRequest(hConnect, wmethod.c_str(), fullPath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    }
    if (hRequest) {
        std::wstring headers;
        if (!contentType.empty())
            headers = L"Content-Type: " + pathToW(contentType) + L"\r\n";

        BOOL sent = WinHttpSendRequest(hRequest,
            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            headers.empty() ? 0 : (DWORD)-1L,
            body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
            (DWORD)body.size(), (DWORD)body.size(), 0);

        if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD code = 0, codeLen = sizeof(code);
            WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeLen,
                WINHTTP_NO_HEADER_INDEX);
            statusOut = (int)code;

            DWORD avail = 0;
            ok = true;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &avail)) { ok = false; break; }
                if (avail == 0) break;
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) { ok = false; break; }
                respOut.append(buf.data(), read);
            } while (avail > 0);
        }
    }
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

bool httpDownload(const std::string& url, const std::string& outPathUtf8, int& statusOut) {
    std::string body;
    if (!httpRequest("GET", url, std::string(), std::string(), body, statusOut))
        return false;
    if (statusOut < 200 || statusOut >= 300) return false;
    return writeFileBytes(outPathUtf8, body);
}

// ============================================================================
//  [세트 J] multipart/form-data 파일 업로드 (WinHTTP)
// ============================================================================
// "key=val\nkey2=val2" -> multipart 텍스트 파트들로 변환.
static void appendTextFields(std::string& body, const std::string& boundary,
                             const std::string& extraFields) {
    size_t start = 0;
    while (start <= extraFields.size()) {
        size_t nl = extraFields.find('\n', start);
        std::string line = (nl == std::string::npos)
                         ? extraFields.substr(start)
                         : extraFields.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            size_t eq = line.find('=');
            std::string k = (eq == std::string::npos) ? line : line.substr(0, eq);
            std::string v = (eq == std::string::npos) ? "" : line.substr(eq + 1);
            if (!k.empty()) {
                body += "--" + boundary + "\r\n";
                body += "Content-Disposition: form-data; name=\"" + k + "\"\r\n\r\n";
                body += v + "\r\n";
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

bool httpUploadFile(const std::string& url, const std::string& fieldName,
                    const std::string& filePathUtf8, const std::string& fileNameInForm,
                    const std::string& extraFields,
                    std::string& respOut, int& statusOut) {
    respOut.clear(); statusOut = 0;

    // 파일 읽기(대용량은 메모리 주의 — 실무 파일 업로드 규모 가정).
    std::string fileData;
    if (!readFileBytes(filePathUtf8, fileData)) return false;

    // boundary 생성(충돌 회피용 난수 hex).
    std::string rnd;
    if (!randomBytes(16, rnd)) rnd = std::string(16, 'A');
    static const char* HEX = "0123456789abcdef";
    std::string boundary = "----NotepadBoundary";
    for (unsigned char b : rnd) { boundary += HEX[b >> 4]; boundary += HEX[b & 0xF]; }

    std::string field = fieldName.empty() ? "file" : fieldName;
    std::string fname = fileNameInForm.empty() ? "upload.bin" : fileNameInForm;

    // 바디 구성: 텍스트 필드들 + 파일 파트 + 종료 boundary.
    std::string body;
    if (!extraFields.empty()) appendTextFields(body, boundary, extraFields);
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"" + field +
            "\"; filename=\"" + fname + "\"\r\n";
    body += "Content-Type: application/octet-stream\r\n\r\n";
    body += fileData;
    body += "\r\n--" + boundary + "--\r\n";

    // URL 파싱
    std::wstring wurl = pathToW(url);
    if (wurl.empty()) return false;
    URL_COMPONENTS uc; memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = {0}, path[8192] = {0}, extra[8192] = {0};
    uc.lpszHostName = host;   uc.dwHostNameLength = 512;
    uc.lpszUrlPath = path;    uc.dwUrlPathLength = 8192;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 8192;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    std::wstring fullPath = path;
    if (extra[0]) fullPath += extra;
    if (fullPath.empty()) fullPath = L"/";
    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"NOTEPAD/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    HINTERNET hRequest = nullptr;
    if (hConnect) {
        DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
        hRequest = WinHttpOpenRequest(hConnect, L"POST", fullPath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    }
    if (hRequest) {
        std::wstring ctype = L"Content-Type: multipart/form-data; boundary=" +
                             pathToW(boundary) + L"\r\n";
        BOOL sent = WinHttpSendRequest(hRequest, ctype.c_str(), (DWORD)-1L,
            (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
        if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD code = 0, codeLen = sizeof(code);
            WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeLen,
                WINHTTP_NO_HEADER_INDEX);
            statusOut = (int)code;
            DWORD avail = 0;
            ok = true;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &avail)) { ok = false; break; }
                if (avail == 0) break;
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) { ok = false; break; }
                respOut.append(buf.data(), read);
            } while (avail > 0);
        }
    }
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

} // namespace notepad

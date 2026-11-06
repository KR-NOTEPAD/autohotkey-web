/* ============================================================================
 *  AhkWeb2Exe - AutoHotkey Web script compiler
 *
 *  Produces a standalone .exe by copying the AutoHotkey Web self-contained base
 *  (Unicode 64-bit .bin) and embedding:
 *    - the script as RCDATA ">AHKWEB SCRIPT<"  (UTF-8, BOM stripped), and
 *    - optionally a web asset folder, zipped, as RCDATA ">AHKWEB ASSETS<",
 *      when the script contains a directive:  ;@AhkWeb-Assets <folder>
 *      The engine extracts it at runtime and exposes A_AhkWebAssets.
 *
 *  Usage (Ahk2Exe-compatible subset, as SciTE4 invokes it):
 *      AhkWeb2Exe /in <script.ahk> [/out <out.exe>] [/bin <base.bin>]
 *                 [/icon <file.ico>]
 *
 *  Build (x64 Native Tools):
 *      cl /nologo /O2 /W3 ahkweb2exe.c miniz.c /link user32.lib
 * ============================================================================ */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniz.h"
#include "script_crypt.h"

static const wchar_t *RES_SCRIPT = L">AHKWEB SCRIPT<";
static const wchar_t *RES_ASSETS = L">AHKWEB ASSETS<";
/* RT_RCDATA expands to MAKEINTRESOURCEA; the *W resource APIs need the wide form. */
#define RT_RCDATA_W MAKEINTRESOURCEW(10)

static void die(const wchar_t *msg) { fwprintf(stderr, L"AhkWeb2Exe: %s\n", msg); exit(2); }

static unsigned char *readFile(const wchar_t *path, DWORD *outLen)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL);
    unsigned char *buf = (unsigned char *)malloc(sz ? sz : 1);
    DWORD got = 0;
    if (!buf || !ReadFile(h, buf, sz, &got, NULL) || got != sz) { CloseHandle(h); free(buf); return NULL; }
    CloseHandle(h);
    *outLen = sz;
    return buf;
}

static int endsWithI(const wchar_t *s, const wchar_t *suf)
{
    size_t ls = wcslen(s), lf = wcslen(suf);
    return ls >= lf && _wcsicmp(s + ls - lf, suf) == 0;
}

/* --- Recursively add a folder's files to the zip, storing paths relative to
 *     the folder root with '/' separators (UTF-8). --------------------------- */
static int zipAddFolder(mz_zip_archive *zip, const wchar_t *root, const wchar_t *sub)
{
    wchar_t pattern[MAX_PATH];
    if (sub && *sub) _snwprintf(pattern, MAX_PATH, L"%s\\%s\\*", root, sub);
    else             _snwprintf(pattern, MAX_PATH, L"%s\\*", root);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1; /* empty or missing: not fatal */
    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;

        wchar_t relW[MAX_PATH];
        if (sub && *sub) _snwprintf(relW, MAX_PATH, L"%s/%s", sub, fd.cFileName);
        else             _snwprintf(relW, MAX_PATH, L"%s", fd.cFileName);
        for (wchar_t *p = relW; *p; ++p) if (*p == L'\\') *p = L'/';

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (!zipAddFolder(zip, root, relW)) { ok = 0; break; }
            continue;
        }

        wchar_t fullW[MAX_PATH];
        _snwprintf(fullW, MAX_PATH, L"%s\\%s%s%s", root,
                   (sub && *sub) ? sub : L"", (sub && *sub) ? L"\\" : L"", fd.cFileName);

        DWORD flen = 0;
        unsigned char *data = readFile(fullW, &flen);
        if (!data) { fwprintf(stderr, L"AhkWeb2Exe: cannot read asset %s\n", fullW); ok = 0; break; }

        char relU8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, relW, -1, relU8, sizeof(relU8), NULL, NULL);

        if (!mz_zip_writer_add_mem(zip, relU8, data, flen, MZ_BEST_COMPRESSION))
        { free(data); fwprintf(stderr, L"AhkWeb2Exe: zip add failed for %s\n", relW); ok = 0; break; }
        free(data);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* Parse ";@AhkWeb-Assets <folder>" from the (BOM-stripped) UTF-8 script bytes.
 * Returns 1 and fills folderW (resolved against the script's directory) if found. */
static int findAssetsDirective(const unsigned char *s, DWORD n,
                               const wchar_t *scriptPath, wchar_t *folderW, size_t folderWcch)
{
    const char *key = ";@AhkWeb-Assets";
    size_t kl = strlen(key);
    for (DWORD i = 0; i + kl < n; ++i)
    {
        if (i != 0 && s[i-1] != '\n' && s[i-1] != '\r') continue;
        DWORD j = i;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j + kl >= n) break;
        if (_strnicmp((const char*)s + j, key, kl) != 0) continue;
        j += kl;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        DWORD e = j;
        while (e < n && s[e] != '\r' && s[e] != '\n') e++;
        while (e > j && (s[e-1] == ' ' || s[e-1] == '\t')) e--;
        if (e <= j) return 0;

        char rel[MAX_PATH]; DWORD rl = e - j; if (rl >= MAX_PATH) rl = MAX_PATH - 1;
        memcpy(rel, s + j, rl); rel[rl] = 0;
        wchar_t relW[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, rel, -1, relW, MAX_PATH);

        wchar_t dir[MAX_PATH]; wcsncpy_s(dir, MAX_PATH, scriptPath, _TRUNCATE);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) slash[1] = 0; else dir[0] = 0;
        wchar_t joined[MAX_PATH];
        if (relW[0] && (relW[1] == L':' || relW[0] == L'\\'))
            wcsncpy_s(joined, MAX_PATH, relW, _TRUNCATE);
        else
            _snwprintf(joined, MAX_PATH, L"%s%s", dir, relW);
        GetFullPathNameW(joined, (DWORD)folderWcch, folderW, NULL);
        return 1;
    }
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *in = NULL, *out = NULL, *bin = NULL, *icon = NULL;
    for (int i = 1; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"/in")   && i + 1 < argc) in   = argv[++i];
        else if (!_wcsicmp(argv[i], L"/out")  && i + 1 < argc) out  = argv[++i];
        else if (!_wcsicmp(argv[i], L"/bin")  && i + 1 < argc) bin  = argv[++i];
        else if (!_wcsicmp(argv[i], L"/icon") && i + 1 < argc) icon = argv[++i];
    }
    if (!in) die(L"missing /in <script.ahk>");

    wchar_t binBuf[MAX_PATH];
    if (!bin)
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        wchar_t *slash = wcsrchr(self, L'\\');
        if (slash) slash[1] = 0;
        _snwprintf(binBuf, MAX_PATH, L"%sAutoHotkeyWeb.bin", self);
        bin = binBuf;
    }

    wchar_t outBuf[MAX_PATH];
    if (!out)
    {
        wcsncpy_s(outBuf, MAX_PATH, in, _TRUNCATE);
        wchar_t *dot = wcsrchr(outBuf, L'.');
        if (dot && endsWithI(in, L".ahk")) wcscpy_s(dot, MAX_PATH - (dot - outBuf), L".exe");
        else wcscat_s(outBuf, MAX_PATH, L".exe");
        out = outBuf;
    }

    /* 1) Copy base -> output. */
    if (!CopyFileW(bin, out, FALSE))
    {
        fwprintf(stderr, L"AhkWeb2Exe: cannot copy base '%s' (err %lu). "
                         L"Build the engine first (produces the .bin).\n", bin, GetLastError());
        return 2;
    }

    /* 2) Read the script; strip a UTF-8 BOM if present. */
    DWORD slen = 0;
    unsigned char *script = readFile(in, &slen);
    if (!script) { DeleteFileW(out); die(L"cannot read /in script"); }
    unsigned char *sp = script; DWORD sl = slen;
    if (sl >= 3 && sp[0] == 0xEF && sp[1] == 0xBB && sp[2] == 0xBF) { sp += 3; sl -= 3; }

    /* 3) Optional: zip the ;@AhkWeb-Assets folder into memory. */
    void *zipBuf = NULL; size_t zipSize = 0;
    wchar_t assetsDir[MAX_PATH];
    if (findAssetsDirective(sp, sl, in, assetsDir, MAX_PATH))
    {
        mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
        if (!mz_zip_writer_init_heap(&zip, 0, 0))
        { free(script); DeleteFileW(out); die(L"zip init failed"); }
        if (!zipAddFolder(&zip, assetsDir, L""))
        { mz_zip_writer_end(&zip); free(script); DeleteFileW(out); die(L"failed to add assets"); }
        if (!mz_zip_writer_finalize_heap_archive(&zip, &zipBuf, &zipSize))
        { mz_zip_writer_end(&zip); free(script); DeleteFileW(out); die(L"zip finalize failed"); }
        mz_zip_writer_end(&zip);
        wprintf(L"AhkWeb2Exe: embedded assets from %s (%zu bytes zipped)\n", assetsDir, zipSize);
    }

    /* 3b) Obfuscate + compress the script into the AHKW1 container so it is not
     *     readable as plain UTF-8 in the output exe. The engine reverses this in
     *     memory before parsing (see script_crypt.h, script.cpp). */
    unsigned char *scriptOut = sp;    /* what we actually embed */
    DWORD scriptOutLen = sl;
    unsigned char *encBuf = NULL;
    {
        mz_ulong bound = mz_compressBound(sl ? sl : 1);
        encBuf = (unsigned char *)malloc(AHKW_SCRIPT_HDR_LEN + bound);
        if (!encBuf) { free(script); if (zipBuf) mz_free(zipBuf); DeleteFileW(out); die(L"out of memory"); }
        mz_ulong compLen = bound;
        int zr = mz_compress2(encBuf + AHKW_SCRIPT_HDR_LEN, &compLen, sp, sl, MZ_BEST_COMPRESSION);
        if (zr != MZ_OK) { free(encBuf); free(script); if (zipBuf) mz_free(zipBuf); DeleteFileW(out); die(L"script compress failed"); }
        ahkw_write_header(encBuf, (uint32_t)sl);
        ahkw_xor_keystream(encBuf + AHKW_SCRIPT_HDR_LEN, (uint32_t)compLen,
                           AHKW_SCRIPT_SEED ^ (uint64_t)sl);
        scriptOut = encBuf;
        scriptOutLen = AHKW_SCRIPT_HDR_LEN + (DWORD)compLen;
    }

    /* 4) Embed resources (script + optional assets) in one update session. */
    HANDLE upd = BeginUpdateResourceW(out, FALSE);
    if (!upd) { free(encBuf); free(script); if (zipBuf) mz_free(zipBuf); DeleteFileW(out); die(L"BeginUpdateResource failed"); }

    if (!UpdateResourceW(upd, RT_RCDATA_W, RES_SCRIPT,
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), scriptOut, scriptOutLen))
    { free(encBuf); free(script); if (zipBuf) mz_free(zipBuf); EndUpdateResourceW(upd, TRUE); DeleteFileW(out); die(L"UpdateResource(script) failed"); }

    if (zipBuf && zipSize)
    {
        if (!UpdateResourceW(upd, RT_RCDATA_W, RES_ASSETS,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), zipBuf, (DWORD)zipSize))
        { free(script); mz_free(zipBuf); EndUpdateResourceW(upd, TRUE); DeleteFileW(out); die(L"UpdateResource(assets) failed"); }
    }

    if (icon)
    {
        DWORD ilen = 0;
        unsigned char *ic = readFile(icon, &ilen);
        if (ic)
        {
            UpdateResourceW(upd, RT_RCDATA_W, L">AHKWEB ICON<",
                            MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), ic, ilen);
            free(ic);
        }
    }

    if (!EndUpdateResourceW(upd, FALSE))
    { free(encBuf); free(script); if (zipBuf) mz_free(zipBuf); DeleteFileW(out); die(L"EndUpdateResource failed"); }
    free(encBuf);
    free(script);
    if (zipBuf) mz_free(zipBuf);

    wprintf(L"AhkWeb2Exe: wrote %s (script encoded)\n", out);
    return 0;
}

/* ============================================================================
 *  AhkWeb2Exe - AutoHotkey Web script compiler (core + command line)
 *
 *  Produces a standalone .exe by copying the AutoHotkey Web self-contained base
 *  (Unicode 64-bit .bin) and embedding:
 *    - the script, with #Include / #IncludeAgain / Lib auto-includes merged in
 *      (Ahk2Exe-style preprocessing), as RCDATA ">AHKWEB SCRIPT<" (AHKW1 container),
 *    - FileInstall sources as RCDATA named after the upper-cased source text,
 *    - ;@AhkWeb-Assets folder as ">AHKWEB ASSETS<" (A_AhkWebAssets),
 *    - ;@AhkWeb-Include files/folders as ">AHKWEB FILES<" + ">AHKWEB FILESCFG<",
 *      extracted at startup (A_AhkWebFiles),
 *    - custom icons (main 159, tray 206/207/208), version info, manifest
 *      (RequireAdmin), extra resources.
 *  Optionally wraps the result in a native setup wizard (AhkWebSetup.bin) and/or
 *  signs it with signtool.
 *
 *  Usage:
 *      AhkWeb2Exe                              -> GUI
 *      AhkWeb2Exe [/gui] <script.ahk>          -> GUI, source prefilled
 *      AhkWeb2Exe /in <script> [/out <exe>] [/bin|/base <bin>] [/icon <ico>]
 *                 [/setup] [/silent]
 *
 *  Directives (;@Ahk2Exe-... or ;@AhkWeb-..., override GUI/CLI): see README.
 * ============================================================================ */
#include "a2e.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wctype.h>
#include <locale.h>
#include "miniz.h"
#include "script_crypt.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t *RES_SCRIPT   = L">AHKWEB SCRIPT<";
static const wchar_t *RES_ASSETS   = L">AHKWEB ASSETS<";
static const wchar_t *RES_FILES    = L">AHKWEB FILES<";
static const wchar_t *RES_FILESCFG = L">AHKWEB FILESCFG<";
static const wchar_t *RES_SETUPCFG = L">SETUP CFG<";
static const wchar_t *RES_SETUPPAY = L">SETUP PAYLOAD<";
static const wchar_t *RES_SETUPLIC = L">SETUP LICENSE<";
#define RT_BITMAP_W     MAKEINTRESOURCEW(2)
#define RT_ICON_W       MAKEINTRESOURCEW(3)
#define RT_RCDATA_W     MAKEINTRESOURCEW(10)
#define RT_GROUP_ICON_W MAKEINTRESOURCEW(14)
#define RT_VERSION_W    MAKEINTRESOURCEW(16)
#define RT_MANIFEST_W   MAKEINTRESOURCEW(24)
#define IDI_MAIN_GROUP  159   /* engine resource.h: IDI_MAIN (exe + tray icon) */
#define IDI_SUSPEND     206
#define IDI_PAUSE       207
#define IDI_PAUSE_SUSP  208
#define SETUP_ICON_ID   1     /* setup_stub.rc */
#define LANG_NEUTRAL_ID MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL)

const wchar_t *VI_STD[] = {
    L"FileDescription", L"FileVersion", L"ProductVersion", L"ProductName",
    L"CompanyName", L"LegalCopyright", L"OriginalFilename", L"InternalName",
    L"LegalTrademarks", L"Comments"
};
const int VI_STD_N = sizeof(VI_STD) / sizeof(VI_STD[0]);

/* ---------------------------------------------------------------- logging -- */
int     g_haveOut = 0;
wchar_t g_lastExe[MAX_PATH];
wchar_t g_log[LOG_CCH];
wchar_t g_err[2048];

void info(const wchar_t *fmt, ...)
{
    wchar_t b[1024]; va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(b, 1024, _TRUNCATE, fmt, ap); va_end(ap);
    if (g_haveOut) { wprintf(L"AhkWeb2Exe: %s\n", b); fflush(stdout); }
    if (wcslen(g_log) + wcslen(b) + 2 < LOG_CCH) { wcscat_s(g_log, LOG_CCH, b); wcscat_s(g_log, LOG_CCH, L"\n"); }
}

int fail(const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(g_err, 2048, _TRUNCATE, fmt, ap); va_end(ap);
    return 0;
}

/* ---------------------------------------------------------------- helpers -- */
void viSet(VIPair *arr, int *n, const wchar_t *key, const wchar_t *val)
{
    for (int i = 0; i < *n; i++)
        if (!_wcsicmp(arr[i].key, key)) { wcsncpy_s(arr[i].val, 256, val, _TRUNCATE); return; }
    if (*n >= MAX_VI) return;
    wcsncpy_s(arr[*n].key, 64, key, _TRUNCATE);
    wcsncpy_s(arr[*n].val, 256, val, _TRUNCATE);
    (*n)++;
}

const wchar_t *viGet(const VIPair *arr, int n, const wchar_t *key)
{
    for (int i = 0; i < n; i++) if (!_wcsicmp(arr[i].key, key)) return arr[i].val;
    return L"";
}

static unsigned char *readFile(const wchar_t *path, DWORD *outLen)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
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

static int writeFileAll(const wchar_t *path, const void *data, DWORD len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wr = 0; BOOL ok = WriteFile(h, data, len, &wr, NULL) && wr == len;
    CloseHandle(h);
    return ok;
}

/* Decode a text file (UTF-8 BOM / UTF-16 LE BOM / UTF-8 / ANSI) to a malloc'd wide string. */
static wchar_t *readTextFileW(const wchar_t *path)
{
    DWORD n = 0; unsigned char *b = readFile(path, &n);
    if (!b) return NULL;
    wchar_t *w = NULL;
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE)
    {
        DWORD wc = (n - 2) / 2;
        w = (wchar_t *)malloc((wc + 1) * sizeof(wchar_t));
        if (w) { memcpy(w, b + 2, wc * sizeof(wchar_t)); w[wc] = 0; }
    }
    else
    {
        unsigned char *s = b; DWORD sl = n;
        if (sl >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { s += 3; sl -= 3; }
        UINT cp = CP_UTF8;
        int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)s, (int)sl, NULL, 0);
        if (!wn && sl) { cp = CP_ACP; wn = MultiByteToWideChar(CP_ACP, 0, (const char *)s, (int)sl, NULL, 0); }
        w = (wchar_t *)malloc(((size_t)wn + 1) * sizeof(wchar_t));
        if (w) { if (wn) MultiByteToWideChar(cp, 0, (const char *)s, (int)sl, w, wn); w[wn] = 0; }
    }
    free(b);
    return w;
}

int endsWithI(const wchar_t *s, const wchar_t *suf)
{
    size_t ls = wcslen(s), lf = wcslen(suf);
    return ls >= lf && _wcsicmp(s + ls - lf, suf) == 0;
}

int fileExists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int dirExists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void trimW(wchar_t *s)
{
    size_t n = wcslen(s);
    while (n && (s[n-1] == L' ' || s[n-1] == L'\t' || s[n-1] == L'\r' || s[n-1] == L'\n')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == L' ' || s[i] == L'\t') i++;
    if (i) memmove(s, s + i, (wcslen(s + i) + 1) * sizeof(wchar_t));
}

void dirOf(const wchar_t *path, wchar_t *dir, size_t cch)
{
    wcsncpy_s(dir, cch, path, _TRUNCATE);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0; else dir[0] = 0;
}

static const wchar_t *fileNameOf(const wchar_t *p)
{
    const wchar_t *s = wcsrchr(p, L'\\'); return s ? s + 1 : p;
}

void compilerDir(wchar_t *dir, size_t cch)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    dirOf(self, dir, cch);
}

void defaultBin(wchar_t *out, size_t cch)
{
    wchar_t d[MAX_PATH]; compilerDir(d, MAX_PATH);
    _snwprintf_s(out, cch, _TRUNCATE, L"%sAutoHotkeyWeb.bin", d);
}

/* Resolve rel against base directory (absolute paths pass through). */
static void resolvePath(const wchar_t *baseDir, const wchar_t *rel, wchar_t *out, size_t cch)
{
    wchar_t joined[MAX_PATH * 2];
    if (rel[0] && (rel[1] == L':' || rel[0] == L'\\' || rel[0] == L'/'))
        wcsncpy_s(joined, MAX_PATH * 2, rel, _TRUNCATE);
    else
        _snwprintf_s(joined, MAX_PATH * 2, _TRUNCATE, L"%s%s", baseDir, rel);
    for (wchar_t *p = joined; *p; ++p) if (*p == L'/') *p = L'\\';
    GetFullPathNameW(joined, (DWORD)cch, out, NULL);
}

/* Default output: script path with .ahk/.ahkw replaced by .exe */
void deriveOut(const wchar_t *in, wchar_t *out, size_t cch)
{
    wcsncpy_s(out, cch, in, _TRUNCATE);
    wchar_t *dot = wcsrchr(out, L'.');
    wchar_t *slash = wcsrchr(out, L'\\');
    if (dot && (!slash || dot > slash) && (endsWithI(in, L".ahk") || endsWithI(in, L".ahkw")))
        wcscpy_s(dot, cch - (dot - out), L".exe");
    else
        wcscat_s(out, cch, L".exe");
}

/* Setup default: <exe folder>\<AppName or exe name>_Setup.exe */
void deriveSetupOut(const Opts *o, const wchar_t *exeOut, wchar_t *out, size_t cch)
{
    wchar_t dir[MAX_PATH]; dirOf(exeOut, dir, MAX_PATH);
    wchar_t base[MAX_PATH];
    const wchar_t *prod = viGet(o->vi, o->nvi, L"ProductName");
    if (o->appName[0]) wcsncpy_s(base, MAX_PATH, o->appName, _TRUNCATE);
    else if (prod[0]) wcsncpy_s(base, MAX_PATH, prod, _TRUNCATE);
    else
    {
        wcsncpy_s(base, MAX_PATH, fileNameOf(exeOut), _TRUNCATE);
        wchar_t *dot = wcsrchr(base, L'.'); if (dot) *dot = 0;
    }
    for (wchar_t *p = base; *p; p++) if (wcschr(L"\\/:*?\"<>|", *p)) *p = L'_';
    _snwprintf_s(out, cch, _TRUNCATE, L"%s%s_Setup.exe", dir, base);
}

void optsDefaults(Opts *o)
{
    memset(o, 0, sizeof(*o));
    wcscpy_s(o->extractTo, MAX_PATH, L"temp");
    wcscpy_s(o->extractMode, 16, L"newer");
    o->scDesktop = 1; o->scStartMenu = 1; o->runAfterInstall = 1;
    wcscpy_s(o->signTs, 256, L"http://timestamp.digicert.com");
}

/* Expand %A_ScriptDir% / %A_ScriptName% / %A_ScriptFullPath% / %A_LineFile% /
 * %A_AppData% / %A_AppDataCommon% / %A_AhkPath% (case-insensitive). */
static void expandVarsEx(const wchar_t *src, wchar_t *dst, size_t cch, const wchar_t *scriptPath,
                         const wchar_t *lineFile, const wchar_t *ahkPath)
{
    wchar_t dir[MAX_PATH]; dirOf(scriptPath, dir, MAX_PATH);
    size_t dl = wcslen(dir); if (dl && dir[dl-1] == L'\\') dir[dl-1] = 0;
    wchar_t appdata[MAX_PATH] = L"", common[MAX_PATH] = L"";
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata);
    SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, common);
    static const wchar_t *VN[] = { L"%A_ScriptDir%", L"%A_ScriptName%", L"%A_ScriptFullPath%",
                                   L"%A_LineFile%", L"%A_AppData%", L"%A_AppDataCommon%", L"%A_AhkPath%" };
    const wchar_t *VV[] = { dir, fileNameOf(scriptPath), scriptPath,
                            lineFile ? lineFile : scriptPath, appdata, common, ahkPath ? ahkPath : L"" };
    size_t o = 0;
    while (*src && o + 1 < cch)
    {
        int hit = 0;
        for (int i = 0; i < 7; i++)
        {
            size_t l = wcslen(VN[i]);
            if (!_wcsnicmp(src, VN[i], l))
            {
                for (const wchar_t *v = VV[i]; *v && o + 1 < cch; ) dst[o++] = *v++;
                src += l; hit = 1; break;
            }
        }
        if (!hit) dst[o++] = *src++;
    }
    dst[o] = 0;
}

static void expandVars(const wchar_t *src, wchar_t *dst, size_t cch, const wchar_t *scriptPath)
{
    expandVarsEx(src, dst, cch, scriptPath, NULL, NULL);
}

/* ----------------------------------------------------- wide string buffer -- */
typedef struct { wchar_t *p; size_t len, cap; } WBuf;
static int wbAppend(WBuf *b, const wchar_t *s, size_t n)
{
    if (b->len + n + 1 > b->cap)
    {
        size_t nc = b->cap ? b->cap * 2 : 65536; while (nc < b->len + n + 1) nc *= 2;
        wchar_t *np = (wchar_t *)realloc(b->p, nc * sizeof(wchar_t)); if (!np) return 0;
        b->p = np; b->cap = nc;
    }
    wmemcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0;
    return 1;
}

/* ------------------------------------------------------------ run process -- */
/* Runs cmdLine hidden, captures stdout+stderr (decoded from the OEM/ANSI code
 * page) into out. Returns the exit code, or -1 if it could not start. */
static int runProcess(const wchar_t *cmdLine, const wchar_t *workDir, wchar_t *out, size_t outCch)
{
    if (out && outCch) out[0] = 0;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    size_t cl = wcslen(cmdLine) + 1;
    wchar_t *cmd = (wchar_t *)malloc(cl * sizeof(wchar_t));
    if (!cmd) { CloseHandle(rd); CloseHandle(wr); return -1; }
    wcscpy_s(cmd, cl, cmdLine);
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, workDir, &si, &pi);
    free(cmd);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    char *acc = NULL; size_t an = 0, ac = 0; char tmp[4096]; DWORD got;
    while (ReadFile(rd, tmp, sizeof(tmp), &got, NULL) && got)
    {
        if (an + got + 1 > ac) { ac = (an + got + 1) * 2; char *np = (char *)realloc(acc, ac); if (!np) break; acc = np; }
        memcpy(acc + an, tmp, got); an += got;
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (acc && out && outCch)
    {
        UINT cp = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, acc, (int)an, NULL, 0) ? CP_UTF8 : CP_OEMCP;
        int wn = MultiByteToWideChar(cp, 0, acc, (int)an, out, (int)outCch - 1);
        out[wn > 0 ? wn : 0] = 0;
        trimW(out);
    }
    free(acc);
    return (int)code;
}

/* -------------------------------------------------------------------- zip -- */
static void toZipName(const wchar_t *relW, char *out, size_t cch)
{
    wchar_t tmp[MAX_PATH * 2]; wcsncpy_s(tmp, MAX_PATH * 2, relW, _TRUNCATE);
    for (wchar_t *p = tmp; *p; ++p) if (*p == L'\\') *p = L'/';
    WideCharToMultiByte(CP_UTF8, 0, tmp, -1, out, (int)cch, NULL, NULL);
}

static int zipAddFile(mz_zip_archive *zip, const wchar_t *full, const wchar_t *relW, unsigned long long *total)
{
    DWORD flen = 0;
    unsigned char *data = readFile(full, &flen);
    if (!data) return fail(L"cannot read %s", full);
    char relU8[MAX_PATH * 3]; toZipName(relW, relU8, sizeof(relU8));
    /* keep the file's own timestamp so the engine's "newer" mode can compare */
    WIN32_FILE_ATTRIBUTE_DATA fa; time_t mt = 0;
    if (GetFileAttributesExW(full, GetFileExInfoStandard, &fa))
    {
        ULONGLONG t = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
        mt = (time_t)(t / 10000000ULL - 11644473600ULL);
    }
    int ok = mz_zip_writer_add_mem_ex_v2(zip, relU8, data, flen, NULL, 0, MZ_BEST_COMPRESSION,
                                         0, 0, mt ? &mt : NULL, NULL, 0, NULL, 0);
    free(data);
    if (!ok) return fail(L"zip add failed for %s", relW);
    if (total) *total += flen;
    return 1;
}

/* Recursively add a folder's files under relative prefix (may be empty). */
static int zipAddFolder(mz_zip_archive *zip, const wchar_t *root, const wchar_t *prefix, unsigned long long *total)
{
    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", root);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1; /* empty or missing: not fatal */
    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t full[MAX_PATH], rel[MAX_PATH];
        _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", root, fd.cFileName);
        if (prefix && *prefix) _snwprintf_s(rel, MAX_PATH, _TRUNCATE, L"%s\\%s", prefix, fd.cFileName);
        else                   wcsncpy_s(rel, MAX_PATH, fd.cFileName, _TRUNCATE);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = zipAddFolder(zip, full, rel, total);
        else ok = zipAddFile(zip, full, rel, total);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* Add an Include item: a folder (contents under dest, or under its own name) or a file. */
static int zipAddInclude(mz_zip_archive *zip, const IncItem *it, unsigned long long *total)
{
    wchar_t dest[MAX_PATH]; wcsncpy_s(dest, MAX_PATH, it->dest, _TRUNCATE);
    for (wchar_t *p = dest; *p; p++) if (*p == L'/') *p = L'\\';
    while (dest[0] == L'\\' || dest[0] == L'.') { if (dest[0] == L'.' && dest[1] != L'\\') break; memmove(dest, dest + 1, wcslen(dest) * sizeof(wchar_t)); }
    size_t dl = wcslen(dest); while (dl && dest[dl-1] == L'\\') dest[--dl] = 0;
    if (wcsstr(dest, L"..") || wcschr(dest, L':')) return fail(L"Include destination must be relative: %s", it->dest);

    if (dirExists(it->src))
        return zipAddFolder(zip, it->src, dest[0] ? dest : fileNameOf(it->src), total);
    if (!fileExists(it->src)) return fail(L"Include: not found: %s", it->src);
    wchar_t rel[MAX_PATH];
    /* dest naming a folder (trailing slash given, or no extension) keeps the file name */
    size_t ol = wcslen(it->dest);
    int destIsDir = dest[0] && ((ol && (it->dest[ol-1] == L'\\' || it->dest[ol-1] == L'/')) || !wcschr(fileNameOf(dest), L'.'));
    if (!dest[0]) wcsncpy_s(rel, MAX_PATH, fileNameOf(it->src), _TRUNCATE);
    else if (destIsDir) _snwprintf_s(rel, MAX_PATH, _TRUNCATE, L"%s\\%s", dest, fileNameOf(it->src));
    else wcsncpy_s(rel, MAX_PATH, dest, _TRUNCATE);
    return zipAddFile(zip, it->src, rel, total);
}

/* ------------------------------------------------------------- directives -- */
/* Split "a, b" at the first comma not escaped by a backtick. */
static void splitComma(const wchar_t *s, wchar_t *a, wchar_t *b, size_t cch)
{
    const wchar_t *c = s;
    while (*c && !(*c == L',' && (c == s || c[-1] != L'`'))) c++;
    size_t al = (size_t)(c - s); if (al >= cch) al = cch - 1;
    wmemcpy(a, s, al); a[al] = 0;
    if (*c) wcsncpy_s(b, cch, c + 1, _TRUNCATE); else b[0] = 0;
    trimW(a); trimW(b);
}

/* Remove Ahk2Exe-style backtick escapes: `; `, `` -> ; , ` */
static void unescape(wchar_t *s)
{
    wchar_t *r = s, *w = s;
    while (*r)
    {
        if (*r == L'`' && (r[1] == L';' || r[1] == L',' || r[1] == L'`')) r++;
        *w++ = *r++;
    }
    *w = 0;
}

static int truthy(const wchar_t *v) { return !v[0] || !_wcsicmp(v, L"1") || !_wcsicmp(v, L"true") || !_wcsicmp(v, L"yes") || !_wcsicmp(v, L"on"); }

static void setVer(Opts *o, const wchar_t *key, const wchar_t *val) { viSet(o->vi, &o->nvi, key, val); }

/* Map Ahk2Exe "Set" property aliases to StringFileInfo keys. */
static void setProp(Opts *o, const wchar_t *prop, const wchar_t *val)
{
    static const struct { const wchar_t *alias, *key; } AL[] = {
        { L"Description", L"FileDescription" }, { L"Copyright", L"LegalCopyright" },
        { L"OrigFilename", L"OriginalFilename" }, { L"Trademarks", L"LegalTrademarks" },
        { L"LegalTrademarks", L"LegalTrademarks" }, { L"Company", L"CompanyName" },
    };
    if (!_wcsicmp(prop, L"Version"))  { setVer(o, L"FileVersion", val); setVer(o, L"ProductVersion", val); return; }
    if (!_wcsicmp(prop, L"Name"))     { setVer(o, L"ProductName", val); setVer(o, L"InternalName", val); return; }
    for (size_t i = 0; i < sizeof(AL) / sizeof(AL[0]); i++)
        if (!_wcsicmp(prop, AL[i].alias)) { setVer(o, AL[i].key, val); return; }
    for (int i = 0; i < VI_STD_N; i++)
        if (!_wcsicmp(prop, VI_STD[i])) { setVer(o, VI_STD[i], val); return; }
    if (prop[0]) setVer(o, prop, val);   /* custom property */
}

static void addResourceDirective(Opts *o, const wchar_t *args, const wchar_t *scriptDir)
{
    if (o->nres >= MAX_RES) { info(L"warning: too many AddResource directives (max %d)", MAX_RES); return; }
    wchar_t buf[MAX_PATH * 2]; wcsncpy_s(buf, MAX_PATH * 2, args, _TRUNCATE);
    WORD type = 0;
    wchar_t *p = buf;
    if (*p == L'*')
    {
        type = (WORD)wcstoul(p + 1, &p, 10);
        while (*p == L' ' || *p == L'\t') p++;
    }
    wchar_t path[MAX_PATH], name[MAX_PATH];
    splitComma(p, path, name, MAX_PATH);
    unescape(path); unescape(name);
    if (!path[0]) return;

    ResItem *r = &o->res[o->nres];
    memset(r, 0, sizeof(*r));
    resolvePath(scriptDir, path, r->path, MAX_PATH);
    r->type = type;
    if (!name[0]) wcsncpy_s(name, MAX_PATH, fileNameOf(r->path), _TRUNCATE);
    if (name[0] == L'#') memmove(name, name + 1, wcslen(name) * sizeof(wchar_t));
    wchar_t *end = NULL; unsigned long n = wcstoul(name, &end, 10);
    if (name[0] && end && !*end && n > 0 && n < 65536) { r->nameIsInt = 1; r->nameInt = (WORD)n; }
    else { wcsncpy_s(r->name, 64, name, _TRUNCATE); _wcsupr_s(r->name, 64); }
    o->nres++;
}

static void addInclude(Opts *o, const wchar_t *src, const wchar_t *dest, const wchar_t *baseDir)
{
    if (o->ninc >= MAX_INC) { info(L"warning: too many Include items (max %d)", MAX_INC); return; }
    IncItem *it = &o->inc[o->ninc++];
    resolvePath(baseDir, src, it->src, MAX_PATH);
    size_t l = wcslen(it->src); while (l > 3 && it->src[l-1] == L'\\') it->src[--l] = 0;
    wcsncpy_s(it->dest, MAX_PATH, dest, _TRUNCATE);
}

/* Handle one directive "name args" (already stripped of ;@Ahk2Exe- / ;@AhkWeb-). */
static void applyDirective(Opts *o, int isWeb, const wchar_t *name, const wchar_t *rawArgs,
                           const wchar_t *scriptPath, wchar_t *assetsDir, size_t assetsCch)
{
    wchar_t scriptDir[MAX_PATH]; dirOf(scriptPath, scriptDir, MAX_PATH);
    wchar_t ex[1024]; expandVars(rawArgs, ex, 1024, scriptPath);
    wchar_t v[1024]; wcscpy_s(v, 1024, ex); unescape(v);   /* single-argument form */
    wchar_t a1[1024], a2[1024]; splitComma(ex, a1, a2, 1024); unescape(a1); unescape(a2);

    #define IS(n) (!_wcsicmp(name, n))
    if (IS(L"Assets")) { if (v[0]) resolvePath(scriptDir, v, assetsDir, assetsCch); }
    else if (IS(L"SetMainIcon")) { if (v[0]) resolvePath(scriptDir, v, o->icon, MAX_PATH); else o->icon[0] = 0; }
    else if (IS(L"SuspendIcon") || IS(L"SetSuspendIcon")) { if (v[0]) resolvePath(scriptDir, v, o->suspendIcon, MAX_PATH); }
    else if (IS(L"PauseIcon") || IS(L"SetPauseIcon")) { if (v[0]) resolvePath(scriptDir, v, o->pauseIcon, MAX_PATH); }
    else if (IS(L"PauseSuspendIcon") || IS(L"SetPauseSuspendIcon")) { if (v[0]) resolvePath(scriptDir, v, o->pauseSuspendIcon, MAX_PATH); }
    else if (IS(L"ExeName"))
    {
        if (v[0])
        {
            wchar_t full[MAX_PATH]; resolvePath(scriptDir, v, full, MAX_PATH);
            size_t L = wcslen(v);
            if (v[L-1] == L'\\' || v[L-1] == L'/')
            {
                wchar_t d[MAX_PATH]; deriveOut(scriptPath, d, MAX_PATH);
                size_t fl = wcslen(full);
                if (fl && full[fl-1] != L'\\') wcscat_s(full, MAX_PATH, L"\\");
                wcscat_s(full, MAX_PATH, fileNameOf(d));
            }
            else if (!wcschr(fileNameOf(full), L'.')) wcscat_s(full, MAX_PATH, L".exe");
            wcscpy_s(o->out, MAX_PATH, full);
        }
    }
    else if (IS(L"Base") || IS(L"Bin"))
    {
        if (isWeb) { if (v[0]) resolvePath(scriptDir, v, o->bin, MAX_PATH); }
        else info(L"note: ;@Ahk2Exe-%s ignored (stock AutoHotkey base); use ;@AhkWeb-Base", name);
    }
    else if (IS(L"SetVersion"))  setProp(o, L"Version", v);
    else if (IS(L"SetName"))     setProp(o, L"Name", v);
    else if (IS(L"Set"))         setProp(o, a1, a2);
    else if (IS(L"AddResource")) addResourceDirective(o, ex, scriptDir);
    else if (IS(L"UpdateManifest")) o->requireAdmin = (_wtoi(a1) != 0);
    else if (IS(L"RequireAdmin")) o->requireAdmin = truthy(v);
    else if (IS(L"PostExec"))
    {   /* ;@Ahk2Exe-PostExec <command> [, When]  (When 0/blank = after compile) */
        if (a1[0] && (!a2[0] || _wtoi(a2) == 0) && o->npost < MAX_POST)
            wcsncpy_s(o->postExec[o->npost++], 1024, a1, _TRUNCATE);
    }
    /* ---- AutoHotkey Web: bundled files ---- */
    else if (IS(L"Include") || IS(L"IncludeFiles")) { if (a1[0]) addInclude(o, a1, a2, scriptDir); }
    else if (IS(L"ExtractTo"))
    {
        if (!_wcsicmp(v, L"temp") || !_wcsicmp(v, L"exe") || !_wcsicmp(v, L"appdata") || !_wcsicmp(v, L"localappdata"))
            { wcsncpy_s(o->extractTo, MAX_PATH, v, _TRUNCATE); _wcslwr_s(o->extractTo, MAX_PATH); }
        else if (v[0]) { wchar_t p[MAX_PATH]; resolvePath(scriptDir, v, p, MAX_PATH); _snwprintf_s(o->extractTo, MAX_PATH, _TRUNCATE, L"path:%s", p); }
    }
    else if (IS(L"ExtractMode"))
    {
        if (!_wcsicmp(v, L"always") || !_wcsicmp(v, L"newer") || !_wcsicmp(v, L"missing"))
            { wcsncpy_s(o->extractMode, 16, v, _TRUNCATE); _wcslwr_s(o->extractMode, 16); }
        else info(L"warning: ExtractMode must be always|newer|missing");
    }
    else if (IS(L"KeepFiles")) o->keepFiles = truthy(v);
    /* ---- AutoHotkey Web: setup wizard ---- */
    else if (IS(L"Setup"))          o->outputMode = truthy(v) ? OUT_SETUP : OUT_EXE;
    else if (IS(L"SetupName"))      wcsncpy_s(o->appName, 128, v, _TRUNCATE);
    else if (IS(L"SetupPublisher")) wcsncpy_s(o->publisher, 128, v, _TRUNCATE);
    else if (IS(L"SetupDir"))       wcsncpy_s(o->installDir, 128, v, _TRUNCATE);
    else if (IS(L"SetupScope"))     o->perMachine = (!_wcsicmp(v, L"ask") || !_wcsicmp(v, L"choose")) ? 2
                                                   : (!_wcsicmp(v, L"machine") || !_wcsicmp(v, L"allusers") || !_wcsicmp(v, L"all"));
    else if (IS(L"SetupLicense"))   { if (v[0]) resolvePath(scriptDir, v, o->license, MAX_PATH); else o->license[0] = 0; }
    else if (IS(L"SetupShortcuts"))
    {
        o->scDesktop = wcsstr(_wcslwr(v), L"desktop") != NULL;
        o->scStartMenu = wcsstr(v, L"start") != NULL;
    }
    else if (IS(L"SetupRun"))       o->runAfterInstall = truthy(v);
    else if (IS(L"SetupOut"))       { if (v[0]) resolvePath(scriptDir, v, o->setupOut, MAX_PATH); }
    /* ---- AutoHotkey Web: code signing ---- */
    else if (IS(L"Sign"))
    {   /* ;@AhkWeb-Sign [cert.pfx [, password]]  (blank = best certificate from the store) */
        o->sign = 1;
        if (a1[0]) resolvePath(scriptDir, a1, o->signPfx, MAX_PATH);
        wcsncpy_s(o->signPass, 128, a2, _TRUNCATE);
    }
    else if (IS(L"SignSubject"))    { o->sign = 1; wcsncpy_s(o->signSubject, 128, v, _TRUNCATE); }
    else if (IS(L"SignTimestamp"))  wcsncpy_s(o->signTs, 256, v, _TRUNCATE);
    else if (IS(L"IgnoreBegin") || IS(L"IgnoreEnd") || IS(L"Keep")) { /* handled by the preprocessor */ }
    else if (!_wcsnicmp(name, L"Set", 3) && name[3]) setProp(o, name + 3, v);
    else info(L"note: directive ;@%s%s not supported, ignored", isWeb ? L"AhkWeb-" : L"Ahk2Exe-", name);
    #undef IS
}

/* Parse ";@Ahk2Exe-Name args" / ";@AhkWeb-Name args" at the start of a line.
 * Returns 0 if the line is not a directive; fills name/args and isWeb. */
static int parseDirectiveLine(const wchar_t *s, const wchar_t *eol, int *isWeb, wchar_t *name, size_t nameCch,
                              wchar_t *args, size_t argsCch)
{
    while (s < eol && (*s == L' ' || *s == L'\t')) s++;
    size_t pl = 0;
    if (eol - s > 2 && s[0] == L';' && s[1] == L'@')
    {
        if (!_wcsnicmp(s + 2, L"Ahk2Exe-", 8)) pl = 10;
        else if (!_wcsnicmp(s + 2, L"AhkWeb-", 7)) pl = 9;
    }
    if (!pl) return 0;
    *isWeb = (pl == 9);
    wchar_t raw[1024]; size_t rl = (size_t)(eol - (s + pl)); if (rl >= 1024) rl = 1023;
    wmemcpy(raw, s + pl, rl); raw[rl] = 0; trimW(raw);
    size_t ni = 0; const wchar_t *a = raw;
    while (*a && *a != L' ' && *a != L'\t' && ni + 1 < nameCch) name[ni++] = *a++;
    name[ni] = 0;
    wcsncpy_s(args, argsCch, a, _TRUNCATE);
    for (wchar_t *c = args; *c; c++)   /* strip trailing " ;comment" (unescaped) */
        if (*c == L';' && c > args && (c[-1] == L' ' || c[-1] == L'\t') && (c - 1 == args || c[-2] != L'`'))
        { *c = 0; break; }
    trimW(args);
    return 1;
}

static void parseDirectives(const wchar_t *text, const wchar_t *scriptPath, Opts *o,
                            wchar_t *assetsDir, size_t assetsCch)
{
    const wchar_t *line = text;
    while (*line)
    {
        const wchar_t *eol = line;
        while (*eol && *eol != L'\n') eol++;
        int isWeb; wchar_t name[64], args[1024];
        if (parseDirectiveLine(line, eol, &isWeb, name, 64, args, 1024))
            applyDirective(o, isWeb, name, args, scriptPath, assetsDir, assetsCch);
        line = *eol ? eol + 1 : eol;
    }
}

/* ----------------------------------------------------------- preprocessor -- */
/* The self-contained base cannot #Include files at run time (and the target PC
 * would not have them), so - like Ahk2Exe - every #Include / #IncludeAgain and
 * every Lib function file is merged into one script here. FileInstall sources
 * are collected so they can be embedded as RCDATA. */
#define MAX_FI    256
#define MAX_INCF  512
typedef struct { wchar_t name[MAX_PATH]; wchar_t path[MAX_PATH]; } FIItem;
typedef struct {
    WBuf     out;
    wchar_t  mainPath[MAX_PATH];
    wchar_t  ahkExe[MAX_PATH];          /* interpreter, for %A_AhkPath% and /iLib */
    wchar_t  libDirs[3][MAX_PATH]; int nlib;
    wchar_t *included[MAX_INCF]; int nincl;
    FIItem  *fi; int nfi;
    int      depth;
} PP;

static void findInterpreter(wchar_t *out, size_t cch)
{
    wchar_t d[MAX_PATH]; compilerDir(d, MAX_PATH);
    const wchar_t *cand[] = { L"..\\AutoHotkeyWeb.exe", L"AutoHotkeyWeb.exe" };
    for (int i = 0; i < 2; i++)
    {
        wchar_t p[MAX_PATH]; _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s%s", d, cand[i]);
        wchar_t f[MAX_PATH]; GetFullPathNameW(p, MAX_PATH, f, NULL);
        if (fileExists(f)) { wcsncpy_s(out, cch, f, _TRUNCATE); return; }
    }
    wchar_t inst[MAX_PATH]; DWORD sz = sizeof(inst);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AutoHotkeyWeb", L"InstallDir", RRF_RT_REG_SZ, NULL, inst, &sz) == ERROR_SUCCESS)
    {
        _snwprintf_s(out, cch, _TRUNCATE, L"%s\\AutoHotkeyWeb.exe", inst);
        if (fileExists(out)) return;
    }
    out[0] = 0;
}

static int ppAlreadyIncluded(PP *pp, const wchar_t *full)
{
    for (int i = 0; i < pp->nincl; i++) if (!_wcsicmp(pp->included[i], full)) return 1;
    return 0;
}

/* <LibName>: Lib\LibName.ahk, or the prefix before the first underscore. */
static int ppFindLib(PP *pp, const wchar_t *name, wchar_t *out, size_t cch)
{
    wchar_t nm[MAX_PATH]; wcsncpy_s(nm, MAX_PATH, name, _TRUNCATE);
    for (int pass = 0; pass < 2; pass++)
    {
        if (pass == 1) { wchar_t *us = wcschr(nm, L'_'); if (!us) break; *us = 0; }
        for (int i = 0; i < pp->nlib; i++)
        {
            _snwprintf_s(out, cch, _TRUNCATE, L"%s\\%s.ahk", pp->libDirs[i], nm);
            if (fileExists(out)) return 1;
        }
    }
    return 0;
}

static int ppFile(PP *pp, const wchar_t *path);

/* FileInstall, Source, Dest  ->  remember Source (literal text) for embedding. */
static int ppFileInstall(PP *pp, const wchar_t *args, const wchar_t *workDir)
{
    wchar_t src[MAX_PATH], rest[MAX_PATH * 2];
    splitComma(args, src, rest, MAX_PATH);
    unescape(src);
    if (!src[0]) return 1;
    if (wcschr(src, L'%')) { info(L"warning: FileInstall source with variables cannot be embedded: %s", src); return 1; }
    wchar_t up[MAX_PATH]; wcsncpy_s(up, MAX_PATH, src, _TRUNCATE); _wcsupr_s(up, MAX_PATH);
    for (int i = 0; i < pp->nfi; i++) if (!wcscmp(pp->fi[i].name, up)) return 1;
    if (pp->nfi >= MAX_FI) return fail(L"too many FileInstall sources (max %d)", MAX_FI);
    wchar_t full[MAX_PATH]; resolvePath(workDir, src, full, MAX_PATH);
    if (!fileExists(full)) return fail(L"FileInstall: source file not found: %s", full);
    wcscpy_s(pp->fi[pp->nfi].name, MAX_PATH, up);
    wcscpy_s(pp->fi[pp->nfi].path, MAX_PATH, full);
    pp->nfi++;
    return 1;
}

static int ppText(PP *pp, const wchar_t *text, const wchar_t *filePath)
{
    wchar_t workDir[MAX_PATH]; dirOf(filePath, workDir, MAX_PATH);   /* #Include <dir> changes it */
    int inComment = 0, keep = 0, ignore = 0;
    const wchar_t *line = text;
    while (*line)
    {
        const wchar_t *eol = line; while (*eol && *eol != L'\n') eol++;
        const wchar_t *next = *eol ? eol + 1 : eol;
        const wchar_t *s = line; while (s < eol && (*s == L' ' || *s == L'\t')) s++;
        size_t sl = (size_t)(eol - s); while (sl && (s[sl-1] == L'\r' || s[sl-1] == L' ' || s[sl-1] == L'\t')) sl--;

        /* ;@Ahk2Exe-IgnoreBegin ... IgnoreEnd : dropped from the compiled script */
        int isWeb; wchar_t dn[64], da[1024];
        if (parseDirectiveLine(line, eol, &isWeb, dn, 64, da, 1024))
        {
            if (!_wcsicmp(dn, L"IgnoreBegin")) { ignore = 1; line = next; continue; }
            if (!_wcsicmp(dn, L"IgnoreEnd"))   { ignore = 0; line = next; continue; }
        }
        if (ignore) { line = next; continue; }

        if (inComment || keep)
        {
            int closes = (sl >= 2 && s[0] == L'*' && s[1] == L'/');
            if (keep)
            {   /* Keep block: inner lines are emitted as code, the closing line is dropped */
                if (closes) keep = 0; else wbAppend(&pp->out, line, (size_t)(next - line));
                line = next; continue;
            }
            if (closes) inComment = 0;
            wbAppend(&pp->out, line, (size_t)(next - line));
            line = next; continue;
        }
        /* Block comment, or an "@Ahk2Exe-Keep" block whose content becomes live code when compiled */
        if (sl >= 2 && s[0] == L'/' && s[1] == L'*')
        {
            if (sl >= 7 && (!_wcsnicmp(s + 2, L"@Ahk2Exe-Keep", 13) || !_wcsnicmp(s + 2, L"@AhkWeb-Keep", 12)))
            { keep = 1; line = next; continue; }
            inComment = !(sl >= 4 && s[sl-2] == L'*' && s[sl-1] == L'/');
            wbAppend(&pp->out, line, (size_t)(next - line));
            line = next; continue;
        }

        /* #Include / #IncludeAgain */
        int again = 0; size_t kl = 0;
        if (sl > 8 && !_wcsnicmp(s, L"#IncludeAgain", 13) && (sl == 13 || wcschr(L" \t,", s[13]))) { again = 1; kl = 13; }
        else if (sl > 8 && !_wcsnicmp(s, L"#Include", 8) && (sl == 8 || wcschr(L" \t,", s[8]))) kl = 8;
        if (kl)
        {
            wchar_t arg[MAX_PATH * 2]; size_t al = sl - kl; if (al >= MAX_PATH * 2) al = MAX_PATH * 2 - 1;
            wmemcpy(arg, s + kl, al); arg[al] = 0;
            for (wchar_t *c = arg; *c; c++)
                if (*c == L';' && c > arg && (c[-1] == L' ' || c[-1] == L'\t')) { *c = 0; break; }
            trimW(arg);
            if (arg[0] == L',') { memmove(arg, arg + 1, wcslen(arg) * sizeof(wchar_t)); trimW(arg); }
            int optional = 0;
            if (!_wcsnicmp(arg, L"*i", 2) && (arg[2] == L' ' || arg[2] == L'\t')) { optional = 1; memmove(arg, arg + 3, (wcslen(arg + 3) + 1) * sizeof(wchar_t)); trimW(arg); }
            wchar_t full[MAX_PATH];
            size_t L = wcslen(arg);
            if (L > 2 && arg[0] == L'<' && arg[L-1] == L'>')
            {
                arg[L-1] = 0;
                if (!ppFindLib(pp, arg + 1, full, MAX_PATH))
                { if (optional) { line = next; continue; } return fail(L"#Include: library <%s> not found (%s)", arg + 1, filePath); }
            }
            else
            {
                wchar_t ex[MAX_PATH * 2]; expandVarsEx(arg, ex, MAX_PATH * 2, pp->mainPath, filePath, pp->ahkExe);
                resolvePath(workDir, ex, full, MAX_PATH);
                if (dirExists(full))
                {   /* directory: becomes the base for later relative #Includes */
                    wcsncpy_s(workDir, MAX_PATH, full, _TRUNCATE);
                    size_t wl = wcslen(workDir); if (wl && workDir[wl-1] != L'\\') wcscat_s(workDir, MAX_PATH, L"\\");
                    line = next; continue;
                }
                if (!fileExists(full))
                { if (optional) { line = next; continue; } return fail(L"#Include file not found: %s (in %s)", full, filePath); }
            }
            if (!again && ppAlreadyIncluded(pp, full)) { line = next; continue; }
            if (!ppFile(pp, full)) return 0;
            line = next; continue;
        }

        /* FileInstall, Source, Dest */
        if (sl > 11 && !_wcsnicmp(s, L"FileInstall", 11) && wcschr(L" \t,", s[11]))
        {
            wchar_t a[MAX_PATH * 3]; size_t al = sl - 11; if (al >= MAX_PATH * 3) al = MAX_PATH * 3 - 1;
            wmemcpy(a, s + 11, al); a[al] = 0; trimW(a);
            if (a[0] == L',') { memmove(a, a + 1, wcslen(a) * sizeof(wchar_t)); trimW(a); }
            if (!ppFileInstall(pp, a, workDir)) return 0;
        }

        wbAppend(&pp->out, line, (size_t)(next - line));
        if (!*eol) wbAppend(&pp->out, L"\n", 1);
        line = next;
    }
    return 1;
}

static int ppFile(PP *pp, const wchar_t *path)
{
    if (pp->depth > 64) return fail(L"#Include nesting too deep at %s", path);
    if (pp->nincl >= MAX_INCF) return fail(L"too many #Include files");
    pp->included[pp->nincl++] = _wcsdup(path);
    wchar_t *text = readTextFileW(path);
    if (!text) return fail(L"cannot read %s", path);
    pp->depth++;
    int ok = ppText(pp, text, path);
    pp->depth--;
    free(text);
    if (ok && pp->out.len && pp->out.p[pp->out.len - 1] != L'\n') wbAppend(&pp->out, L"\n", 1);
    return ok;
}

/* Merge the script with its includes and Lib functions. Returns the merged text
 * (caller frees) or NULL with g_err set. */
static wchar_t *preprocessScript(const wchar_t *mainPath, FIItem *fi, int *nfi)
{
    PP pp; memset(&pp, 0, sizeof(pp));
    wcscpy_s(pp.mainPath, MAX_PATH, mainPath);
    pp.fi = fi;
    findInterpreter(pp.ahkExe, MAX_PATH);
    /* Lib folders, in AutoHotkey's search order: script, user, interpreter */
    wchar_t d[MAX_PATH]; dirOf(mainPath, d, MAX_PATH);
    _snwprintf_s(pp.libDirs[pp.nlib++], MAX_PATH, _TRUNCATE, L"%sLib", d);
    wchar_t docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        _snwprintf_s(pp.libDirs[pp.nlib++], MAX_PATH, _TRUNCATE, L"%s\\AutoHotkey\\Lib", docs);
    if (pp.ahkExe[0]) { dirOf(pp.ahkExe, d, MAX_PATH); _snwprintf_s(pp.libDirs[pp.nlib++], MAX_PATH, _TRUNCATE, L"%sLib", d); }

    int ok = ppFile(&pp, mainPath);

    /* Lib auto-includes: ask the interpreter which library files the script needs
     * (/iLib also syntax-checks the script, like Ahk2Exe does). */
    if (ok && pp.ahkExe[0])
    {
        wchar_t tmpDir[MAX_PATH], ilib[MAX_PATH];
        GetTempPathW(MAX_PATH, tmpDir);
        _snwprintf_s(ilib, MAX_PATH, _TRUNCATE, L"%sahkw2e_ilib_%lu.ahk", tmpDir, GetCurrentProcessId());
        DeleteFileW(ilib);
        wchar_t cmd[MAX_PATH * 4];
        _snwprintf_s(cmd, MAX_PATH * 4, _TRUNCATE, L"\"%s\" /ErrorStdOut /iLib \"%s\" \"%s\"", pp.ahkExe, ilib, mainPath);
        static wchar_t outTxt[8192];
        wchar_t scriptDir[MAX_PATH]; dirOf(mainPath, scriptDir, MAX_PATH);
        int rc = runProcess(cmd, scriptDir, outTxt, 8192);
        if (rc == -1) info(L"note: could not run %s for Lib auto-includes", pp.ahkExe);
        else if (rc != 0 && outTxt[0]) { ok = fail(L"script error:\n%s", outTxt); }
        else if (fileExists(ilib))
        {
            wchar_t *t = readTextFileW(ilib);
            if (t)
            {
                if (wcsstr(t, L"#Include")) info(L"Lib auto-includes merged");
                ok = ppText(&pp, t, mainPath);
                free(t);
            }
        }
        DeleteFileW(ilib);
    }
    else if (ok) info(L"note: AutoHotkeyWeb.exe not found - Lib auto-includes and syntax check skipped");

    for (int i = 0; i < pp.nincl; i++) free(pp.included[i]);
    *nfi = pp.nfi;
    if (!ok) { free(pp.out.p); return NULL; }
    if (!pp.out.p) wbAppend(&pp.out, L"", 0);
    return pp.out.p;
}

/* -------------------------------------------------------------- resources -- */
typedef struct { WORD langs[16]; int n; } LangList;

static BOOL CALLBACK langCb(HMODULE h, LPCWSTR t, LPCWSTR n, WORD lang, LONG_PTR p)
{
    (void)h; (void)t; (void)n;
    LangList *l = (LangList *)p;
    if (l->n < 16) l->langs[l->n++] = lang;
    return TRUE;
}

static void getLangs(HMODULE h, LPCWSTR type, LPCWSTR name, LangList *l)
{
    l->n = 0;
    if (h) EnumResourceLanguagesW(h, type, name, langCb, (LONG_PTR)l);
}

static BOOL CALLBACK maxIdCb(HMODULE h, LPCWSTR t, LPWSTR name, LONG_PTR p)
{
    (void)h; (void)t;
    if (IS_INTRESOURCE(name)) { WORD id = (WORD)(ULONG_PTR)name; WORD *mx = (WORD *)p; if (id > *mx) *mx = id; }
    return TRUE;
}

/* Replace (or add) one resource, deleting any other language variants. */
static int replaceRaw(HANDLE upd, HMODULE hb, LPCWSTR type, LPCWSTR name, const void *data, DWORD len)
{
    LangList ll; getLangs(hb, type, name, &ll);
    WORD lang = ll.n ? ll.langs[0] : LANG_NEUTRAL_ID;
    for (int i = 1; i < ll.n; i++) UpdateResourceW(upd, type, name, ll.langs[i], NULL, 0);
    return UpdateResourceW(upd, type, name, lang, (void *)data, len);
}

#pragma pack(push, 2)
typedef struct { BYTE w, h, colors, res; WORD planes, bpp; DWORD bytes; DWORD offset; } ICOENTRY;  /* .ico file */
typedef struct { BYTE w, h, colors, res; WORD planes, bpp; DWORD bytes; WORD id; } GRPENTRY;       /* RT_GROUP_ICON */
#pragma pack(pop)

static int icoValidate(const unsigned char *d, DWORD n)
{
    if (n < 6) return 0;
    WORD r = *(const WORD *)d, t = *(const WORD *)(d + 2), c = *(const WORD *)(d + 4);
    if (r != 0 || t != 1 || c == 0 || c > 64) return 0;
    if (6u + 16u * c > n) return 0;
    const ICOENTRY *e = (const ICOENTRY *)(d + 6);
    for (WORD i = 0; i < c; i++)
        if (!e[i].bytes || (unsigned long long)e[i].offset + e[i].bytes > n) return 0;
    return 1;
}

/* Real icon replacement (what Ahk2Exe does): drop the group's old RT_ICON images,
 * write the .ico images as RT_ICON (reusing old IDs first), then the new group. */
static int writeIconGroup(HANDLE upd, HMODULE hb, LPCWSTR name, const unsigned char *ico, WORD *nextId)
{
    WORD cnt = *(const WORD *)(ico + 4);
    const ICOENTRY *e = (const ICOENTRY *)(ico + 6);
    LangList ll; getLangs(hb, RT_GROUP_ICON_W, name, &ll);
    WORD lang = ll.n ? ll.langs[0] : LANG_NEUTRAL_ID;
    WORD reuse[64]; int nreuse = 0;

    for (int li = 0; li < ll.n; li++)
    {
        HRSRC r = FindResourceExW(hb, RT_GROUP_ICON_W, name, ll.langs[li]);
        if (!r) continue;
        DWORD rs = SizeofResource(hb, r);
        const BYTE *g = (const BYTE *)LockResource(LoadResource(hb, r));
        if (g && rs >= 6)
        {
            WORD gc = *(const WORD *)(g + 4);
            if (6u + 14u * gc > rs) gc = (WORD)((rs - 6) / 14);
            const GRPENTRY *ge = (const GRPENTRY *)(g + 6);
            for (WORD k = 0; k < gc; k++)
            {
                WORD id = ge[k].id;
                LangList il; getLangs(hb, RT_ICON_W, MAKEINTRESOURCEW(id), &il);
                for (int j = 0; j < il.n; j++) UpdateResourceW(upd, RT_ICON_W, MAKEINTRESOURCEW(id), il.langs[j], NULL, 0);
                int dup = 0; for (int q = 0; q < nreuse; q++) if (reuse[q] == id) dup = 1;
                if (!dup && nreuse < 64) reuse[nreuse++] = id;
            }
        }
        UpdateResourceW(upd, RT_GROUP_ICON_W, name, ll.langs[li], NULL, 0);
    }

    DWORD gsz = 6 + 14u * cnt;
    BYTE *grp = (BYTE *)calloc(1, gsz);
    if (!grp) return fail(L"out of memory");
    ((WORD *)grp)[1] = 1; ((WORD *)grp)[2] = cnt;
    GRPENTRY *ge = (GRPENTRY *)(grp + 6);
    for (WORD i = 0; i < cnt; i++)
    {
        WORD id = (i < nreuse) ? reuse[i] : (*nextId)++;
        if (!UpdateResourceW(upd, RT_ICON_W, MAKEINTRESOURCEW(id), lang, (void *)(ico + e[i].offset), e[i].bytes))
        { free(grp); return fail(L"UpdateResource(icon image) failed (err %lu)", GetLastError()); }
        ge[i].w = e[i].w; ge[i].h = e[i].h; ge[i].colors = e[i].colors; ge[i].res = 0;
        ge[i].planes = e[i].planes ? e[i].planes : 1;
        ge[i].bpp = e[i].bpp ? e[i].bpp : 32;
        ge[i].bytes = e[i].bytes; ge[i].id = id;
    }
    int ok = UpdateResourceW(upd, RT_GROUP_ICON_W, name, lang, grp, gsz);
    free(grp);
    return ok ? 1 : fail(L"UpdateResource(icon group) failed (err %lu)", GetLastError());
}

typedef struct { unsigned char *data; DWORD len; } Blob;

static int loadIco(const wchar_t *path, Blob *b, const wchar_t *what)
{
    b->data = readFile(path, &b->len);
    if (!b->data) return fail(L"cannot read %s: %s", what, path);
    if (!icoValidate(b->data, b->len)) return fail(L"not a valid .ico file (%s): %s", what, path);
    return 1;
}

/* ---- VS_VERSIONINFO builder ---------------------------------------------- */
typedef struct { unsigned char *p; size_t len, cap; } Buf;

static void bput(Buf *b, const void *d, size_t n)
{
    if (b->len + n > b->cap)
    {
        size_t nc = b->cap ? b->cap * 2 : 1024; while (nc < b->len + n) nc *= 2;
        unsigned char *np = (unsigned char *)realloc(b->p, nc); if (!np) return;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, d, n); b->len += n;
}
static void balign(Buf *b) { static const BYTE z[4] = {0}; if (b->len & 3) bput(b, z, 4 - (b->len & 3)); }
static size_t bbegin(Buf *b, const wchar_t *key, WORD valLen, WORD type)
{
    balign(b);
    size_t at = b->len;
    WORD hdr[3] = { 0, valLen, type };
    bput(b, hdr, 6);
    bput(b, key, (wcslen(key) + 1) * sizeof(wchar_t));
    balign(b);
    return at;
}
static void bend(Buf *b, size_t at) { WORD L = (WORD)(b->len - at); memcpy(b->p + at, &L, 2); }

typedef struct {
    VS_FIXEDFILEINFO ffi;
    WORD lang, cp;
    VIPair s[MAX_VI]; int n;
} VerInfo;

static void loadBaseVersion(const wchar_t *bin, VerInfo *v)
{
    memset(v, 0, sizeof(*v));
    v->ffi.dwSignature = 0xFEEF04BD; v->ffi.dwStrucVersion = 0x00010000;
    v->ffi.dwFileFlagsMask = 0x3F; v->ffi.dwFileOS = VOS_NT_WINDOWS32; v->ffi.dwFileType = VFT_APP;
    v->lang = 0x0409; v->cp = 1200;

    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(bin, &dummy);
    if (!sz) return;
    void *buf = malloc(sz);
    if (!buf) return;
    if (GetFileVersionInfoW(bin, 0, sz, buf))
    {
        VS_FIXEDFILEINFO *ffi; UINT l = 0;
        if (VerQueryValueW(buf, L"\\", (void **)&ffi, &l) && l >= sizeof(*ffi)) v->ffi = *ffi;
        WORD *tr;
        if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void **)&tr, &l) && l >= 4)
        { v->lang = tr[0]; v->cp = tr[1]; }
        for (int i = 0; i < VI_STD_N; i++)
        {
            wchar_t q[128], *val;
            _snwprintf_s(q, 128, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\%s", v->lang, v->cp, VI_STD[i]);
            if (VerQueryValueW(buf, q, (void **)&val, &l) && l > 0 && val[0])
                viSet(v->s, &v->n, VI_STD[i], val);
        }
    }
    free(buf);
}

static int parseVer(const wchar_t *s, DWORD *ms, DWORD *ls)
{
    unsigned v[4] = {0}; int n = 0;
    while (*s == L' ' || *s == L'v' || *s == L'V') s++;
    while (*s && n < 4)
    {
        if (!iswdigit(*s)) break;
        wchar_t *e; v[n++] = (unsigned)wcstoul(s, &e, 10); s = e;
        if (*s == L'.') s++; else break;
    }
    if (!n) return 0;
    *ms = MAKELONG(v[1] & 0xFFFF, v[0] & 0xFFFF);
    *ls = MAKELONG(v[3] & 0xFFFF, v[2] & 0xFFFF);
    return 1;
}

static int buildVersion(VerInfo *v, Buf *b)
{
    for (int i = 0; i < v->n; i++)
    {
        if (!_wcsicmp(v->s[i].key, L"FileVersion"))
            parseVer(v->s[i].val, &v->ffi.dwFileVersionMS, &v->ffi.dwFileVersionLS);
        else if (!_wcsicmp(v->s[i].key, L"ProductVersion"))
            parseVer(v->s[i].val, &v->ffi.dwProductVersionMS, &v->ffi.dwProductVersionLS);
    }
    size_t root = bbegin(b, L"VS_VERSION_INFO", sizeof(VS_FIXEDFILEINFO), 0);
    bput(b, &v->ffi, sizeof(VS_FIXEDFILEINFO));

    size_t sfi = bbegin(b, L"StringFileInfo", 0, 1);
    wchar_t tkey[16]; _snwprintf_s(tkey, 16, _TRUNCATE, L"%04x%04x", v->lang, v->cp);
    size_t tbl = bbegin(b, tkey, 0, 1);
    for (int i = 0; i < v->n; i++)
    {
        if (!v->s[i].val[0]) continue;
        size_t st = bbegin(b, v->s[i].key, (WORD)(wcslen(v->s[i].val) + 1), 1);
        bput(b, v->s[i].val, (wcslen(v->s[i].val) + 1) * sizeof(wchar_t));
        bend(b, st);
    }
    bend(b, tbl);
    bend(b, sfi);

    size_t vfi = bbegin(b, L"VarFileInfo", 0, 1);
    size_t var = bbegin(b, L"Translation", 4, 0);
    WORD tr[2] = { v->lang, v->cp };
    bput(b, tr, 4);
    bend(b, var);
    bend(b, vfi);
    bend(b, root);
    return b->p != NULL;
}

/* ---- manifest: requestedExecutionLevel ----------------------------------- */
static int setRequireAdmin(HANDLE upd, HMODULE hb)
{
    static const char *TRUST =
        "<trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\"><security><requestedPrivileges>"
        "<requestedExecutionLevel level=\"requireAdministrator\" uiAccess=\"false\"/>"
        "</requestedPrivileges></security></trustInfo>";
    HRSRC r = hb ? FindResourceW(hb, MAKEINTRESOURCEW(1), RT_MANIFEST_W) : NULL;
    char *m = NULL; size_t ml = 0;
    if (r)
    {
        DWORD rs = SizeofResource(hb, r);
        const char *src = (const char *)LockResource(LoadResource(hb, r));
        m = (char *)malloc(rs + strlen(TRUST) + 64);
        if (!m) return fail(L"out of memory");
        memcpy(m, src, rs); m[rs] = 0; ml = rs;
        char *lv = strstr(m, "level=\"");
        if (lv)
        {   /* swap whatever level is there for requireAdministrator */
            char *q = lv + 7, *qe = strchr(q, '"');
            if (qe)
            {
                const char *nv = "requireAdministrator"; size_t nl = strlen(nv), ol = (size_t)(qe - q);
                memmove(q + nl, qe, ml - (size_t)(qe - m) + 1);
                memcpy(q, nv, nl); ml = ml - ol + nl;
            }
        }
        else
        {
            char *end = strstr(m, "</assembly>");
            if (!end) { free(m); return fail(L"base manifest has no </assembly>"); }
            size_t tl = strlen(TRUST);
            memmove(end + tl, end, ml - (size_t)(end - m) + 1);
            memcpy(end, TRUST, tl); ml += tl;
        }
    }
    else
    {
        const char *H = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">";
        ml = strlen(H) + strlen(TRUST) + 11;
        m = (char *)malloc(ml + 1);
        if (!m) return fail(L"out of memory");
        sprintf_s(m, ml + 1, "%s%s</assembly>", H, TRUST);
    }
    int ok = replaceRaw(upd, hb, RT_MANIFEST_W, MAKEINTRESOURCEW(1), m, (DWORD)ml);
    free(m);
    return ok ? 1 : fail(L"UpdateResource(manifest) failed");
}

/* ---------------------------------------------------------------- signing -- */
static int findSigntool(wchar_t *out, size_t cch)
{
    wchar_t base[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%\\Windows Kits\\10\\bin", base, MAX_PATH);
    wchar_t best[MAX_PATH] = L"", pat[MAX_PATH];
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\10.*", base);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            wchar_t p[MAX_PATH];
            _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\%s\\x64\\signtool.exe", base, fd.cFileName);
            if (fileExists(p) && wcscmp(p, best) > 0) wcscpy_s(best, MAX_PATH, p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!best[0]) { _snwprintf_s(best, MAX_PATH, _TRUNCATE, L"%s\\x64\\signtool.exe", base); if (!fileExists(best)) best[0] = 0; }
    if (!best[0]) { wchar_t *fp; if (!SearchPathW(NULL, L"signtool.exe", NULL, MAX_PATH, best, &fp)) best[0] = 0; }
    wcsncpy_s(out, cch, best, _TRUNCATE);
    return best[0] != 0;
}

static int signFile(const Opts *o, const wchar_t *file)
{
    wchar_t st[MAX_PATH];
    if (!findSigntool(st, MAX_PATH))
        return fail(L"code signing: signtool.exe not found (install the Windows SDK \"Signing Tools\")");
    wchar_t who[MAX_PATH + 160];
    if (o->signPfx[0])
    {
        if (o->signPass[0]) _snwprintf_s(who, MAX_PATH + 160, _TRUNCATE, L"/f \"%s\" /p \"%s\"", o->signPfx, o->signPass);
        else                _snwprintf_s(who, MAX_PATH + 160, _TRUNCATE, L"/f \"%s\"", o->signPfx);
    }
    else if (o->signSubject[0]) _snwprintf_s(who, MAX_PATH + 160, _TRUNCATE, L"/n \"%s\"", o->signSubject);
    else wcscpy_s(who, MAX_PATH + 160, L"/a");
    static wchar_t outTxt[4096];
    wchar_t cmd[4096];
    for (int attempt = 0; attempt < 2; attempt++)
    {
        int withTs = (attempt == 0 && o->signTs[0]);
        if (withTs) _snwprintf_s(cmd, 4096, _TRUNCATE, L"\"%s\" sign /fd SHA256 %s /tr \"%s\" /td SHA256 \"%s\"", st, who, o->signTs, file);
        else        _snwprintf_s(cmd, 4096, _TRUNCATE, L"\"%s\" sign /fd SHA256 %s \"%s\"", st, who, file);
        int rc = runProcess(cmd, NULL, outTxt, 4096);
        if (rc == 0) { info(withTs ? L"signed: %s" : L"signed (no timestamp): %s", fileNameOf(file)); return 1; }
        if (!withTs) break;
        info(L"warning: signing with timestamp failed, retrying without timestamp");
    }
    return fail(L"code signing failed for %s:\n%s", fileNameOf(file), outTxt);
}

/* ;@Ahk2Exe-PostExec <command>: %A_WorkFileName% = the produced exe. */
static void runPostExec(const Opts *o, const wchar_t *exe, const wchar_t *scriptPath)
{
    for (int i = 0; i < o->npost; i++)
    {
        wchar_t c1[2048] = L"", c2[2048];
        const wchar_t *s = o->postExec[i]; size_t k = 0;
        while (*s && k + 1 < 2048)
        {
            if (!_wcsnicmp(s, L"%A_WorkFileName%", 16))
            { for (const wchar_t *v = exe; *v && k + 1 < 2048; ) c1[k++] = *v++; s += 16; }
            else c1[k++] = *s++;
        }
        c1[k] = 0;
        expandVars(c1, c2, 2048, scriptPath);
        static wchar_t outTxt[4096];
        wchar_t sd[MAX_PATH]; dirOf(scriptPath, sd, MAX_PATH);
        int rc = runProcess(c2, sd, outTxt, 4096);
        if (rc == -1)
        {
            wchar_t viaCmd[2100]; _snwprintf_s(viaCmd, 2100, _TRUNCATE, L"cmd.exe /c %s", c2);
            rc = runProcess(viaCmd, sd, outTxt, 4096);
        }
        if (rc == 0) info(L"PostExec ok: %s", c2);
        else info(L"warning: PostExec exit %d: %s\n%s", rc, c2, outTxt);
    }
}

/* ------------------------------------------------------------- build .exe -- */
static int finishZip(mz_zip_archive *zip, void **buf, size_t *size)
{
    if (!mz_zip_writer_finalize_heap_archive(zip, buf, size)) { mz_zip_writer_end(zip); return fail(L"zip finalize failed"); }
    mz_zip_writer_end(zip);
    return 1;
}

static int buildExe(const Opts *o, const wchar_t *merged, const FIItem *fi, int nfi,
                    const wchar_t *assetsDir, int embedIncludes)
{
    int ok = 0, outCreated = 0;
    unsigned char *u8 = NULL, *encBuf = NULL;
    void *zipBuf = NULL, *filesBuf = NULL; size_t zipSize = 0, filesSize = 0;
    HMODULE hb = NULL; HANDLE upd = NULL;
    Blob ico[4] = {0}, resData[MAX_RES] = {0}, fiData[MAX_FI] = {0};
    Buf ver = {0};
    const wchar_t *icoPath[4] = { o->icon, o->suspendIcon, o->pauseIcon, o->pauseSuspendIcon };
    static const WORD icoId[4] = { IDI_MAIN_GROUP, IDI_SUSPEND, IDI_PAUSE, IDI_PAUSE_SUSP };
    static const wchar_t *icoWhat[4] = { L"icon", L"suspend icon", L"pause icon", L"pause+suspend icon" };

    if (!fileExists(o->bin))
    { fail(L"base file not found: %s (build the engine Self-contained|x64 config first)", o->bin); goto done; }
    if (!_wcsicmp(o->out, o->in) || !_wcsicmp(o->out, o->bin)) { fail(L"output path must differ from script/base"); goto done; }

    /* 1) Load and validate everything BEFORE touching the output. */
    for (int i = 0; i < 4; i++) if (icoPath[i][0] && !loadIco(icoPath[i], &ico[i], icoWhat[i])) goto done;
    for (int i = 0; i < o->nres; i++)
    {
        resData[i].data = readFile(o->res[i].path, &resData[i].len);
        if (!resData[i].data) { fail(L"AddResource: cannot read %s", o->res[i].path); goto done; }
        if (!o->res[i].type && endsWithI(o->res[i].path, L".ico") && !icoValidate(resData[i].data, resData[i].len))
        { fail(L"AddResource: not a valid .ico file: %s", o->res[i].path); goto done; }
    }
    for (int i = 0; i < nfi; i++)
    {
        fiData[i].data = readFile(fi[i].path, &fiData[i].len);
        if (!fiData[i].data) { fail(L"FileInstall: cannot read %s", fi[i].path); goto done; }
    }

    /* 2) ;@AhkWeb-Assets folder -> zip */
    if (assetsDir[0])
    {
        if (!dirExists(assetsDir)) { fail(L"assets folder not found: %s", assetsDir); goto done; }
        mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
        if (!mz_zip_writer_init_heap(&zip, 0, 0)) { fail(L"zip init failed"); goto done; }
        if (!zipAddFolder(&zip, assetsDir, L"", NULL)) { mz_zip_writer_end(&zip); goto done; }
        if (!finishZip(&zip, &zipBuf, &zipSize)) goto done;
        info(L"embedded assets from %s (%zu bytes zipped)", assetsDir, zipSize);
    }

    /* 3) ;@AhkWeb-Include files -> zip (single-exe mode only) */
    if (embedIncludes && o->ninc)
    {
        mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
        unsigned long long total = 0;
        if (!mz_zip_writer_init_heap(&zip, 0, 0)) { fail(L"zip init failed"); goto done; }
        for (int i = 0; i < o->ninc; i++)
            if (!zipAddInclude(&zip, &o->inc[i], &total)) { mz_zip_writer_end(&zip); goto done; }
        if (!finishZip(&zip, &filesBuf, &filesSize)) goto done;
        info(L"bundled %d item(s): %llu bytes -> %zu zipped, extract to %s (%s)",
             o->ninc, total, filesSize, o->extractTo, o->extractMode);
    }

    /* 4) Script -> UTF-8 -> AHKW1 container (script_crypt.h). */
    DWORD scriptOutLen;
    {
        int ul = WideCharToMultiByte(CP_UTF8, 0, merged, -1, NULL, 0, NULL, NULL);
        u8 = (unsigned char *)malloc(ul > 0 ? ul : 1);
        if (!u8) { fail(L"out of memory"); goto done; }
        WideCharToMultiByte(CP_UTF8, 0, merged, -1, (char *)u8, ul, NULL, NULL);
        DWORD sl = ul > 0 ? (DWORD)(ul - 1) : 0;
        mz_ulong bound = mz_compressBound(sl ? sl : 1);
        encBuf = (unsigned char *)malloc(AHKW_SCRIPT_HDR_LEN + bound);
        if (!encBuf) { fail(L"out of memory"); goto done; }
        mz_ulong compLen = bound;
        if (mz_compress2(encBuf + AHKW_SCRIPT_HDR_LEN, &compLen, u8, sl, MZ_BEST_COMPRESSION) != MZ_OK)
        { fail(L"script compress failed"); goto done; }
        ahkw_write_header(encBuf, (uint32_t)sl);
        ahkw_xor_keystream(encBuf + AHKW_SCRIPT_HDR_LEN, (uint32_t)compLen, AHKW_SCRIPT_SEED ^ (uint64_t)sl);
        scriptOutLen = AHKW_SCRIPT_HDR_LEN + (DWORD)compLen;
    }

    /* 5) Version info: base strings + overrides (only if anything was set). */
    if (o->nvi)
    {
        VerInfo vi; loadBaseVersion(o->bin, &vi);
        for (int i = 0; i < o->nvi; i++) viSet(vi.s, &vi.n, o->vi[i].key, o->vi[i].val);
        if (!buildVersion(&vi, &ver)) { fail(L"out of memory (version)"); goto done; }
    }

    /* 6) Copy base -> output (creating the destination folder if needed). */
    { wchar_t od[MAX_PATH]; dirOf(o->out, od, MAX_PATH); if (od[0]) SHCreateDirectoryExW(NULL, od, NULL); }
    if (!CopyFileW(o->bin, o->out, FALSE))
    { fail(L"cannot write output %s (err %lu) - is it running?", o->out, GetLastError()); goto done; }
    outCreated = 1;
    SetFileAttributesW(o->out, FILE_ATTRIBUTE_NORMAL);

    hb = LoadLibraryExW(o->bin, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    WORD nextIconId = 0;
    if (hb) EnumResourceNamesW(hb, RT_ICON_W, maxIdCb, (LONG_PTR)&nextIconId);
    nextIconId++;

    /* 7) Embed resources in one update session. */
    upd = BeginUpdateResourceW(o->out, FALSE);
    if (!upd) { fail(L"BeginUpdateResource failed (err %lu)", GetLastError()); goto done; }

    if (!UpdateResourceW(upd, RT_RCDATA_W, RES_SCRIPT, LANG_NEUTRAL_ID, encBuf, scriptOutLen))
    { fail(L"UpdateResource(script) failed"); goto done; }
    if (zipBuf && !UpdateResourceW(upd, RT_RCDATA_W, RES_ASSETS, LANG_NEUTRAL_ID, zipBuf, (DWORD)zipSize))
    { fail(L"UpdateResource(assets) failed"); goto done; }
    if (filesBuf)
    {
        wchar_t cfg[MAX_PATH + 128];
        _snwprintf_s(cfg, MAX_PATH + 128, _TRUNCATE, L"dest=%s\nmode=%s\ncleanup=%d\n",
                     o->extractTo, o->extractMode, o->keepFiles ? 0 : 1);
        if (!UpdateResourceW(upd, RT_RCDATA_W, RES_FILES, LANG_NEUTRAL_ID, filesBuf, (DWORD)filesSize) ||
            !UpdateResourceW(upd, RT_RCDATA_W, RES_FILESCFG, LANG_NEUTRAL_ID, cfg, (DWORD)(wcslen(cfg) * sizeof(wchar_t))))
        { fail(L"UpdateResource(files) failed"); goto done; }
    }
    for (int i = 0; i < nfi; i++)
    {
        if (!UpdateResourceW(upd, RT_RCDATA_W, fi[i].name, LANG_NEUTRAL_ID, fiData[i].data, fiData[i].len))
        { fail(L"UpdateResource(FileInstall %s) failed", fi[i].path); goto done; }
    }
    if (nfi) info(L"FileInstall: %d file(s) embedded", nfi);

    for (int i = 0; i < 4; i++)
    {
        if (!ico[i].data) continue;
        if (!writeIconGroup(upd, hb, MAKEINTRESOURCEW(icoId[i]), ico[i].data, &nextIconId)) goto done;
        info(L"%s: %s", icoWhat[i], icoPath[i]);
    }

    for (int i = 0; i < o->nres; i++)
    {
        const ResItem *r = &o->res[i];
        LPCWSTR nm = r->nameIsInt ? MAKEINTRESOURCEW(r->nameInt) : r->name;
        unsigned char *d = resData[i].data; DWORD dl = resData[i].len;
        LPCWSTR type;
        if (r->type) type = MAKEINTRESOURCEW(r->type);
        else if (endsWithI(r->path, L".ico"))
        {
            if (!writeIconGroup(upd, hb, nm, d, &nextIconId)) goto done;
            if (r->nameIsInt) info(L"icon group %u <- %s", (unsigned)r->nameInt, r->path);
            else              info(L"icon group %s <- %s", r->name, r->path);
            continue;
        }
        else if (endsWithI(r->path, L".bmp") || endsWithI(r->path, L".dib"))
        {
            type = RT_BITMAP_W;
            if (dl > 14 && d[0] == 'B' && d[1] == 'M') { d += 14; dl -= 14; }   /* strip BITMAPFILEHEADER */
        }
        else if (endsWithI(r->path, L".htm") || endsWithI(r->path, L".html")) type = MAKEINTRESOURCEW(23);
        else if (endsWithI(r->path, L".manifest")) type = RT_MANIFEST_W;
        else type = RT_RCDATA_W;
        if (!replaceRaw(upd, hb, type, nm, d, dl)) { fail(L"AddResource failed for %s", r->path); goto done; }
        info(L"resource added: %s", r->path);
    }

    if (ver.p)
    {
        if (!replaceRaw(upd, hb, RT_VERSION_W, MAKEINTRESOURCEW(1), ver.p, (DWORD)ver.len))
        { fail(L"UpdateResource(version) failed"); goto done; }
        info(L"version info updated");
    }
    if (o->requireAdmin)
    {
        if (!setRequireAdmin(upd, hb)) goto done;
        info(L"manifest: requireAdministrator");
    }

    if (!EndUpdateResourceW(upd, FALSE)) { upd = NULL; fail(L"EndUpdateResource failed (err %lu)", GetLastError()); goto done; }
    upd = NULL;
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, o->out, NULL);
    info(L"wrote %s", o->out);
    ok = 1;

done:
    if (upd) EndUpdateResourceW(upd, TRUE);
    if (hb) FreeLibrary(hb);
    if (!ok && outCreated) DeleteFileW(o->out);
    free(ver.p);
    for (int i = 0; i < 4; i++) free(ico[i].data);
    for (int i = 0; i < MAX_RES; i++) free(resData[i].data);
    for (int i = 0; i < MAX_FI; i++) free(fiData[i].data);
    if (zipBuf) mz_free(zipBuf);
    if (filesBuf) mz_free(filesBuf);
    free(encBuf);
    free(u8);
    return ok;
}

/* ---------------------------------------------------------- build setup -- */
/* Setup.exe = AhkWebSetup.bin (native wizard) + cfg + license + payload zip
 * (the app exe and the Include items, installed side by side). */
static void appendCfg(wchar_t *cfg, size_t cch, const wchar_t *k, const wchar_t *v)
{
    wchar_t line[600]; _snwprintf_s(line, 600, _TRUNCATE, L"%s=%s\n", k, v);
    for (wchar_t *p = line + wcslen(k) + 1; *p && *p != L'\n'; p++) if (*p == L'\r') *p = L' ';
    wcscat_s(cfg, cch, line);
}

static int buildSetup(Opts *o, const wchar_t *exePath, const wchar_t *setupOut)
{
    int ok = 0, outCreated = 0;
    void *pay = NULL; size_t paySize = 0;
    wchar_t *lic = NULL; HANDLE upd = NULL; HMODULE hs = NULL;
    Blob ico = {0}; Buf ver = {0};
    wchar_t stub[MAX_PATH], cd[MAX_PATH]; compilerDir(cd, MAX_PATH);
    _snwprintf_s(stub, MAX_PATH, _TRUNCATE, L"%sAhkWebSetup.bin", cd);
    if (!fileExists(stub)) { fail(L"setup stub not found: %s (run build_compiler.bat)", stub); goto done; }

    /* names */
    const wchar_t *exeName = fileNameOf(exePath);
    wchar_t appName[128], publisher[128], dirName[128], version[64];
    wcsncpy_s(appName, 128, o->appName[0] ? o->appName : viGet(o->vi, o->nvi, L"ProductName"), _TRUNCATE);
    if (!appName[0]) { wcsncpy_s(appName, 128, exeName, _TRUNCATE); wchar_t *d = wcsrchr(appName, L'.'); if (d) *d = 0; }
    wcsncpy_s(publisher, 128, o->publisher[0] ? o->publisher : viGet(o->vi, o->nvi, L"CompanyName"), _TRUNCATE);
    wcsncpy_s(dirName, 128, o->installDir[0] ? o->installDir : appName, _TRUNCATE);
    for (wchar_t *p = dirName; *p; p++) if (wcschr(L"/:*?\"<>|", *p)) *p = L'_';
    wcsncpy_s(version, 64, viGet(o->vi, o->nvi, L"ProductVersion"), _TRUNCATE);
    if (!version[0]) wcsncpy_s(version, 64, viGet(o->vi, o->nvi, L"FileVersion"), _TRUNCATE);
    if (!version[0]) wcscpy_s(version, 64, L"1.0");

    /* payload */
    unsigned long long total = 0;
    {
        mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
        if (!mz_zip_writer_init_heap(&zip, 0, 0)) { fail(L"zip init failed"); goto done; }
        if (!zipAddFile(&zip, exePath, exeName, &total)) { mz_zip_writer_end(&zip); goto done; }
        for (int i = 0; i < o->ninc; i++)
            if (!zipAddInclude(&zip, &o->inc[i], &total)) { mz_zip_writer_end(&zip); goto done; }
        if (!finishZip(&zip, &pay, &paySize)) goto done;
    }

    /* license */
    if (o->license[0])
    {
        lic = readTextFileW(o->license);
        if (!lic) { fail(L"cannot read license file: %s", o->license); goto done; }
    }

    /* icon: app icon if set, otherwise the stub keeps its own */
    if (o->icon[0] && !loadIco(o->icon, &ico, L"icon")) goto done;

    /* version info of the setup itself */
    {
        VerInfo vi; loadBaseVersion(stub, &vi);
        wchar_t desc[200]; _snwprintf_s(desc, 200, _TRUNCATE, L"%s Setup", appName);
        viSet(vi.s, &vi.n, L"FileDescription", desc);
        viSet(vi.s, &vi.n, L"ProductName", appName);
        viSet(vi.s, &vi.n, L"FileVersion", version);
        viSet(vi.s, &vi.n, L"ProductVersion", version);
        if (publisher[0]) viSet(vi.s, &vi.n, L"CompanyName", publisher);
        const wchar_t *cp = viGet(o->vi, o->nvi, L"LegalCopyright");
        if (cp[0]) viSet(vi.s, &vi.n, L"LegalCopyright", cp);
        wchar_t on[MAX_PATH]; wcsncpy_s(on, MAX_PATH, fileNameOf(setupOut), _TRUNCATE);
        viSet(vi.s, &vi.n, L"OriginalFilename", on);
        if (!buildVersion(&vi, &ver)) { fail(L"out of memory"); goto done; }
    }

    /* config */
    wchar_t cfg[4096] = L"", num[32], appId[160];
    _snwprintf_s(appId, 160, _TRUNCATE, L"AhkWeb_%s", appName);
    for (wchar_t *p = appId; *p; p++) if (!iswalnum(*p) && *p != L'_' && *p != L'-' && *p != L'.') *p = L'_';
    appendCfg(cfg, 4096, L"name", appName);
    appendCfg(cfg, 4096, L"version", version);
    appendCfg(cfg, 4096, L"publisher", publisher);
    appendCfg(cfg, 4096, L"exe", exeName);
    appendCfg(cfg, 4096, L"dir", dirName);
    appendCfg(cfg, 4096, L"appid", appId);
    appendCfg(cfg, 4096, L"scope", o->perMachine == 2 ? L"ask" : o->perMachine ? L"machine" : L"user");
    appendCfg(cfg, 4096, L"desktop", o->scDesktop ? L"1" : L"0");
    appendCfg(cfg, 4096, L"startmenu", o->scStartMenu ? L"1" : L"0");
    appendCfg(cfg, 4096, L"run", o->runAfterInstall ? L"1" : L"0");
    _snwprintf_s(num, 32, _TRUNCATE, L"%llu", (total + 1023) / 1024);
    appendCfg(cfg, 4096, L"sizekb", num);

    /* write */
    { wchar_t od[MAX_PATH]; dirOf(setupOut, od, MAX_PATH); if (od[0]) SHCreateDirectoryExW(NULL, od, NULL); }
    if (!CopyFileW(stub, setupOut, FALSE)) { fail(L"cannot write %s (err %lu)", setupOut, GetLastError()); goto done; }
    outCreated = 1;
    SetFileAttributesW(setupOut, FILE_ATTRIBUTE_NORMAL);
    hs = LoadLibraryExW(stub, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    WORD nextIconId = 0;
    if (hs) EnumResourceNamesW(hs, RT_ICON_W, maxIdCb, (LONG_PTR)&nextIconId);
    nextIconId++;
    upd = BeginUpdateResourceW(setupOut, FALSE);
    if (!upd) { fail(L"BeginUpdateResource(setup) failed (err %lu)", GetLastError()); goto done; }
    if (!UpdateResourceW(upd, RT_RCDATA_W, RES_SETUPCFG, LANG_NEUTRAL_ID, cfg, (DWORD)(wcslen(cfg) * sizeof(wchar_t))) ||
        !UpdateResourceW(upd, RT_RCDATA_W, RES_SETUPPAY, LANG_NEUTRAL_ID, pay, (DWORD)paySize))
    { fail(L"UpdateResource(setup payload) failed"); goto done; }
    if (lic && !UpdateResourceW(upd, RT_RCDATA_W, RES_SETUPLIC, LANG_NEUTRAL_ID, lic, (DWORD)(wcslen(lic) * sizeof(wchar_t))))
    { fail(L"UpdateResource(license) failed"); goto done; }
    if (ico.data && !writeIconGroup(upd, hs, MAKEINTRESOURCEW(SETUP_ICON_ID), ico.data, &nextIconId)) goto done;
    if (!replaceRaw(upd, hs, RT_VERSION_W, MAKEINTRESOURCEW(1), ver.p, (DWORD)ver.len)) { fail(L"UpdateResource(setup version) failed"); goto done; }
    if (!EndUpdateResourceW(upd, FALSE)) { upd = NULL; fail(L"EndUpdateResource(setup) failed (err %lu)", GetLastError()); goto done; }
    upd = NULL;
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, setupOut, NULL);
    info(L"setup wizard: %s (%s %s, %s install, payload %zu bytes)", setupOut, appName, version,
         o->perMachine == 2 ? L"user-chosen" : o->perMachine ? L"all-users" : L"per-user", paySize);
    ok = 1;

done:
    if (upd) EndUpdateResourceW(upd, TRUE);
    if (hs) FreeLibrary(hs);
    if (!ok && outCreated) DeleteFileW(setupOut);
    free(ver.p); free(ico.data); free(lic);
    if (pay) mz_free(pay);
    return ok;
}

/* Include items declared by ;@AhkWeb-Include directives in the script file itself
 * (shown read-only in the GUI). Returns the count. */
int scriptDirectiveIncludes(const wchar_t *script, IncItem *out, int max)
{
    wchar_t *text = readTextFileW(script);
    if (!text) return 0;
    static Opts tmp; optsDefaults(&tmp);
    wchar_t assets[MAX_PATH] = L"", full[MAX_PATH];
    GetFullPathNameW(script, MAX_PATH, full, NULL);
    size_t logLen = wcslen(g_log);
    int saveOut = g_haveOut; g_haveOut = 0;
    parseDirectives(text, full, &tmp, assets, MAX_PATH);
    g_haveOut = saveOut; g_log[logLen] = 0;      /* notes from this scan are not compile output */
    free(text);
    int n = tmp.ninc < max ? tmp.ninc : max;
    for (int i = 0; i < n; i++) out[i] = tmp.inc[i];
    return n;
}

/* ---------------------------------------------------------------- compile -- */
int compileScript(const Opts *src, wchar_t *finalOut, size_t cch)
{
    static Opts o; o = *src;
    static FIItem fi[MAX_FI];
    int nfi = 0, ok = 0;
    wchar_t *merged = NULL;
    g_err[0] = 0;

    if (!o.in[0]) return fail(L"no script specified (/in)");
    wchar_t inFull[MAX_PATH]; GetFullPathNameW(o.in, MAX_PATH, inFull, NULL);
    wcscpy_s(o.in, MAX_PATH, inFull);
    if (!fileExists(inFull)) return fail(L"script not found: %s", inFull);

    /* 1) #Include / Lib merge, FileInstall collection, syntax check */
    merged = preprocessScript(inFull, fi, &nfi);
    if (!merged) return 0;

    /* 2) directives from the whole merged script (override GUI/CLI) */
    wchar_t assetsDir[MAX_PATH] = L"";
    parseDirectives(merged, inFull, &o, assetsDir, MAX_PATH);
    if (!o.out[0]) deriveOut(inFull, o.out, MAX_PATH);
    else { wchar_t t[MAX_PATH]; GetFullPathNameW(o.out, MAX_PATH, t, NULL); wcscpy_s(o.out, MAX_PATH, t); }
    if (!o.bin[0]) defaultBin(o.bin, MAX_PATH);
    if (!o.extractTo[0]) wcscpy_s(o.extractTo, MAX_PATH, L"temp");
    if (!o.extractMode[0]) wcscpy_s(o.extractMode, 16, L"newer");

    /* 3) the exe (bundled files embedded only in single-exe mode) */
    if (!buildExe(&o, merged, fi, nfi, assetsDir, o.outputMode == OUT_EXE)) goto done;
    runPostExec(&o, o.out, inFull);
    if (o.sign && !signFile(&o, o.out)) goto done;
    wcscpy_s(g_lastExe, MAX_PATH, o.out);
    if (finalOut) wcsncpy_s(finalOut, cch, o.out, _TRUNCATE);

    /* 4) optional setup wizard */
    if (o.outputMode == OUT_SETUP)
    {
        wchar_t setupOut[MAX_PATH];
        if (o.setupOut[0]) wcscpy_s(setupOut, MAX_PATH, o.setupOut);
        else deriveSetupOut(&o, o.out, setupOut, MAX_PATH);
        if (!_wcsicmp(setupOut, o.out)) { fail(L"setup output must differ from the exe"); goto done; }
        if (!buildSetup(&o, o.out, setupOut)) goto done;
        if (o.sign && !signFile(&o, setupOut)) goto done;
        if (finalOut) wcsncpy_s(finalOut, cch, setupOut, _TRUNCATE);
    }
    ok = 1;
done:
    free(merged);
    return ok;
}

/* ------------------------------------------------------------------- main -- */
/* GUI-subsystem exe: when run from a terminal, write to the parent console;
 * when stdout is redirected (SciTE output pane, pipes), use it as is. */
static void initStdio(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD t = (h && h != INVALID_HANDLE_VALUE) ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    if (t == FILE_TYPE_PIPE || t == FILE_TYPE_DISK) { g_haveOut = 1; return; }
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE *f;
        _wfreopen_s(&f, L"CONOUT$", L"w", stdout);
        _wfreopen_s(&f, L"CONOUT$", L"w", stderr);
        g_haveOut = 1;
        wprintf(L"\n");
    }
}

int wmain(int argc, wchar_t **argv)
{
    setlocale(LC_CTYPE, "");
    static Opts o; optsDefaults(&o);
    int gui = 0, haveIn = 0;
    for (int i = 1; i < argc; i++)
    {
        const wchar_t *a = argv[i];
        int more = i + 1 < argc;
        if      (!_wcsicmp(a, L"/in")   && more) { wcsncpy_s(o.in,   MAX_PATH, argv[++i], _TRUNCATE); haveIn = 1; }
        else if (!_wcsicmp(a, L"/out")  && more)   wcsncpy_s(o.out,  MAX_PATH, argv[++i], _TRUNCATE);
        else if ((!_wcsicmp(a, L"/bin") || !_wcsicmp(a, L"/base")) && more) wcsncpy_s(o.bin, MAX_PATH, argv[++i], _TRUNCATE);
        else if (!_wcsicmp(a, L"/icon") && more)   wcsncpy_s(o.icon, MAX_PATH, argv[++i], _TRUNCATE);
        else if (!_wcsicmp(a, L"/setup")) o.outputMode = OUT_SETUP;
        else if (!_wcsicmp(a, L"/silent")) o.silent = 1;
        else if (!_wcsicmp(a, L"/gui"))    gui = 1;
        else if ((!_wcsicmp(a, L"/compress") || !_wcsicmp(a, L"/mpress") || !_wcsicmp(a, L"/cp") ||
                  !_wcsicmp(a, L"/resourceid")) && more) i++;   /* Ahk2Exe options, not applicable */
        else if (a[0] != L'/' && !o.in[0]) wcsncpy_s(o.in, MAX_PATH, a, _TRUNCATE);
    }

    if (!haveIn || gui)
    {
        wchar_t init[MAX_PATH] = L"";
        if (o.in[0]) GetFullPathNameW(o.in, MAX_PATH, init, NULL);
        return runGui(init);
    }

    initStdio();
    wchar_t out[MAX_PATH];
    int ok = compileScript(&o, out, MAX_PATH);
    if (!ok)
    {
        if (g_haveOut) { fwprintf(stderr, L"AhkWeb2Exe: %s\n", g_err); fflush(stderr); }
        else if (!o.silent)
        {
            wchar_t m[2200]; _snwprintf_s(m, 2200, _TRUNCATE, L"컴파일 실패:\n%s", g_err);
            MessageBoxW(NULL, m, L"AhkWeb2Exe", MB_ICONERROR);
        }
        return 2;
    }
    return 0;
}

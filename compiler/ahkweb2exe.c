/* ============================================================================
 *  AhkWeb2Exe - AutoHotkey Web script compiler
 *
 *  Produces a standalone .exe by copying the AutoHotkey Web self-contained base
 *  (Unicode 64-bit .bin) and embedding:
 *    - the script as RCDATA ">AHKWEB SCRIPT<" (AHKW1 container, see script_crypt.h),
 *    - optionally a web asset folder, zipped, as RCDATA ">AHKWEB ASSETS<",
 *      when the script contains a directive:  ;@AhkWeb-Assets <folder>
 *    - a custom main icon (real RT_GROUP_ICON/RT_ICON replacement, like Ahk2Exe),
 *    - version information (VS_VERSIONINFO rewritten from base + overrides),
 *    - extra resources (;@Ahk2Exe-AddResource).
 *
 *  Usage (Ahk2Exe-compatible subset):
 *      AhkWeb2Exe                              -> GUI (like Ahk2Exe)
 *      AhkWeb2Exe <script.ahk>                 -> GUI, source prefilled
 *      AhkWeb2Exe /in <script.ahk> [/out <out.exe>] [/bin|/base <base.bin>]
 *                 [/icon <file.ico>] [/silent] [/gui]
 *
 *  Script directives (prefix ;@Ahk2Exe- or ;@AhkWeb-, directives override GUI/CLI):
 *      SetMainIcon <file.ico>          ExeName <path>
 *      SetVersion <1.2.3.4>            SetFileVersion / SetProductVersion
 *      SetDescription / SetCopyright / SetCompanyName / SetProductName
 *      SetOrigFilename / SetInternalName / SetLegalTrademarks / SetComments
 *      SetName <name>                  Set <Property>, <Value>
 *      AddResource [*Type] <file> [, Name]   (.ico -> icon group, e.g. 206/207/208)
 *      Assets <folder> (AhkWeb)        Base <file.bin> (;@AhkWeb-Base only)
 *
 *  Build: see build_compiler.bat (x64 Native Tools, cl /utf-8, GUI subsystem).
 * ============================================================================ */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wchar.h>
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
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t *RES_SCRIPT = L">AHKWEB SCRIPT<";
static const wchar_t *RES_ASSETS = L">AHKWEB ASSETS<";
#define RT_BITMAP_W     MAKEINTRESOURCEW(2)
#define RT_ICON_W       MAKEINTRESOURCEW(3)
#define RT_RCDATA_W     MAKEINTRESOURCEW(10)
#define RT_GROUP_ICON_W MAKEINTRESOURCEW(14)
#define RT_VERSION_W    MAKEINTRESOURCEW(16)
#define IDI_MAIN_GROUP  159   /* engine resource.h: IDI_MAIN (main + tray icon) */
#define LANG_NEUTRAL_ID MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL)

/* ---------------------------------------------------------------- options -- */
#define MAX_VI   24
#define MAX_RES  16
typedef struct { wchar_t key[64]; wchar_t val[256]; } VIPair;
typedef struct {
    wchar_t path[MAX_PATH];
    wchar_t name[64];       /* string name (upper-cased) when !nameIsInt */
    int     nameIsInt;
    WORD    nameInt;
    WORD    type;           /* 0 = auto from extension */
} ResItem;
typedef struct {
    wchar_t in[MAX_PATH], out[MAX_PATH], bin[MAX_PATH], icon[MAX_PATH];
    VIPair  vi[MAX_VI]; int nvi;       /* explicit version overrides */
    ResItem res[MAX_RES]; int nres;
    int     silent;
} Opts;

/* Standard StringFileInfo keys (also what the GUI fills). */
static const wchar_t *VI_STD[] = {
    L"FileDescription", L"FileVersion", L"ProductVersion", L"ProductName",
    L"CompanyName", L"LegalCopyright", L"OriginalFilename", L"InternalName",
    L"LegalTrademarks", L"Comments"
};
#define VI_STD_N (sizeof(VI_STD) / sizeof(VI_STD[0]))

static void viSet(VIPair *arr, int *n, const wchar_t *key, const wchar_t *val)
{
    for (int i = 0; i < *n; i++)
        if (!_wcsicmp(arr[i].key, key)) { wcsncpy_s(arr[i].val, 256, val, _TRUNCATE); return; }
    if (*n >= MAX_VI) return;
    wcsncpy_s(arr[*n].key, 64, key, _TRUNCATE);
    wcsncpy_s(arr[*n].val, 256, val, _TRUNCATE);
    (*n)++;
}

/* ---------------------------------------------------------------- logging -- */
static int     g_haveOut = 0;           /* stdout usable (pipe/file/console) */
static wchar_t g_log[4096];
static wchar_t g_err[1024];

static void info(const wchar_t *fmt, ...)
{
    wchar_t b[1024]; va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(b, 1024, _TRUNCATE, fmt, ap); va_end(ap);
    if (g_haveOut) { wprintf(L"AhkWeb2Exe: %s\n", b); fflush(stdout); }
    if (wcslen(g_log) + wcslen(b) + 2 < 4096) { wcscat_s(g_log, 4096, b); wcscat_s(g_log, 4096, L"\n"); }
}

static int fail(const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(g_err, 1024, _TRUNCATE, fmt, ap); va_end(ap);
    return 0;
}

/* ---------------------------------------------------------------- helpers -- */
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

static int fileExists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void trimW(wchar_t *s)
{
    size_t n = wcslen(s);
    while (n && (s[n-1] == L' ' || s[n-1] == L'\t' || s[n-1] == L'\r')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == L' ' || s[i] == L'\t') i++;
    if (i) memmove(s, s + i, (wcslen(s + i) + 1) * sizeof(wchar_t));
}

static void dirOf(const wchar_t *path, wchar_t *dir, size_t cch)
{
    wcsncpy_s(dir, cch, path, _TRUNCATE);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0; else dir[0] = 0;
}

static void compilerDir(wchar_t *dir, size_t cch)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    dirOf(self, dir, cch);
}

static void defaultBin(wchar_t *out, size_t cch)
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
static void deriveOut(const wchar_t *in, wchar_t *out, size_t cch)
{
    wcsncpy_s(out, cch, in, _TRUNCATE);
    wchar_t *dot = wcsrchr(out, L'.');
    wchar_t *slash = wcsrchr(out, L'\\');
    if (dot && (!slash || dot > slash) && (endsWithI(in, L".ahk") || endsWithI(in, L".ahkw")))
        wcscpy_s(dot, cch - (dot - out), L".exe");
    else
        wcscat_s(out, cch, L".exe");
}

/* Expand %A_ScriptDir% / %A_ScriptName% / %A_ScriptFullPath% (case-insensitive). */
static void expandVars(const wchar_t *src, wchar_t *dst, size_t cch, const wchar_t *scriptPath)
{
    wchar_t dir[MAX_PATH]; dirOf(scriptPath, dir, MAX_PATH);
    size_t dl = wcslen(dir); if (dl && dir[dl-1] == L'\\') dir[dl-1] = 0;
    const wchar_t *name = wcsrchr(scriptPath, L'\\'); name = name ? name + 1 : scriptPath;
    static const wchar_t *VN[] = { L"%A_ScriptDir%", L"%A_ScriptName%", L"%A_ScriptFullPath%" };
    const wchar_t *VV[] = { dir, name, scriptPath };
    size_t o = 0;
    while (*src && o + 1 < cch)
    {
        int hit = 0;
        for (int i = 0; i < 3; i++)
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

/* --- Recursively add a folder's files to the zip, storing paths relative to
 *     the folder root with '/' separators (UTF-8). --------------------------- */
static int zipAddFolder(mz_zip_archive *zip, const wchar_t *root, const wchar_t *sub)
{
    wchar_t pattern[MAX_PATH];
    if (sub && *sub) _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\%s\\*", root, sub);
    else             _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", root);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1; /* empty or missing: not fatal */
    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;

        wchar_t relW[MAX_PATH];
        if (sub && *sub) _snwprintf_s(relW, MAX_PATH, _TRUNCATE, L"%s/%s", sub, fd.cFileName);
        else             _snwprintf_s(relW, MAX_PATH, _TRUNCATE, L"%s", fd.cFileName);
        for (wchar_t *p = relW; *p; ++p) if (*p == L'\\') *p = L'/';

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (!zipAddFolder(zip, root, relW)) { ok = 0; break; }
            continue;
        }

        wchar_t fullW[MAX_PATH];
        _snwprintf_s(fullW, MAX_PATH, _TRUNCATE, L"%s\\%s%s%s", root,
                     (sub && *sub) ? sub : L"", (sub && *sub) ? L"\\" : L"", fd.cFileName);

        DWORD flen = 0;
        unsigned char *data = readFile(fullW, &flen);
        if (!data) { fail(L"cannot read asset %s", fullW); ok = 0; break; }

        char relU8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, relW, -1, relU8, sizeof(relU8), NULL, NULL);

        if (!mz_zip_writer_add_mem(zip, relU8, data, flen, MZ_BEST_COMPRESSION))
        { free(data); fail(L"zip add failed for %s", relW); ok = 0; break; }
        free(data);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
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

static void setVer(Opts *o, const wchar_t *key, const wchar_t *val) { viSet(o->vi, &o->nvi, key, val); }

/* Map Ahk2Exe "Set" property aliases to StringFileInfo keys. Returns 1 if handled. */
static int setProp(Opts *o, const wchar_t *prop, const wchar_t *val)
{
    static const struct { const wchar_t *alias, *key; } AL[] = {
        { L"Description", L"FileDescription" }, { L"Copyright", L"LegalCopyright" },
        { L"OrigFilename", L"OriginalFilename" }, { L"Trademarks", L"LegalTrademarks" },
        { L"LegalTrademarks", L"LegalTrademarks" }, { L"Company", L"CompanyName" },
    };
    if (!_wcsicmp(prop, L"Version"))  { setVer(o, L"FileVersion", val); setVer(o, L"ProductVersion", val); return 1; }
    if (!_wcsicmp(prop, L"Name"))     { setVer(o, L"ProductName", val); setVer(o, L"InternalName", val); return 1; }
    for (size_t i = 0; i < sizeof(AL) / sizeof(AL[0]); i++)
        if (!_wcsicmp(prop, AL[i].alias)) { setVer(o, AL[i].key, val); return 1; }
    for (size_t i = 0; i < VI_STD_N; i++)
        if (!_wcsicmp(prop, VI_STD[i])) { setVer(o, VI_STD[i], val); return 1; }
    if (prop[0]) { setVer(o, prop, val); return 1; }   /* custom property */
    return 0;
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
    if (!name[0])
    {   /* default name: file name upper-cased */
        const wchar_t *fn = wcsrchr(r->path, L'\\'); fn = fn ? fn + 1 : r->path;
        wcsncpy_s(name, MAX_PATH, fn, _TRUNCATE);
    }
    if (name[0] == L'#') memmove(name, name + 1, wcslen(name) * sizeof(wchar_t));
    wchar_t *end = NULL; unsigned long n = wcstoul(name, &end, 10);
    if (name[0] && end && !*end && n > 0 && n < 65536) { r->nameIsInt = 1; r->nameInt = (WORD)n; }
    else { wcsncpy_s(r->name, 64, name, _TRUNCATE); _wcsupr_s(r->name, 64); }
    o->nres++;
}

/* Parse ;@Ahk2Exe-* / ;@AhkWeb-* line directives from the script text.
 * Directives override the incoming options (Ahk2Exe behaviour). */
static void parseDirectives(const wchar_t *text, const wchar_t *scriptPath, Opts *o,
                            wchar_t *assetsDir, size_t assetsCch)
{
    wchar_t scriptDir[MAX_PATH]; dirOf(scriptPath, scriptDir, MAX_PATH);
    const wchar_t *line = text;
    while (*line)
    {
        const wchar_t *eol = line;
        while (*eol && *eol != L'\n') eol++;
        const wchar_t *s = line;
        while (s < eol && (*s == L' ' || *s == L'\t')) s++;

        size_t pl = 0;
        if (eol - s > 2 && s[0] == L';' && s[1] == L'@')
        {
            if (!_wcsnicmp(s + 2, L"Ahk2Exe-", 8)) pl = 10;
            else if (!_wcsnicmp(s + 2, L"AhkWeb-", 7)) pl = 9;
        }
        if (pl)
        {
            int isWeb = (pl == 9);
            wchar_t raw[1024]; size_t rl = (size_t)(eol - (s + pl)); if (rl >= 1024) rl = 1023;
            wmemcpy(raw, s + pl, rl); raw[rl] = 0; trimW(raw);

            wchar_t name[64]; size_t ni = 0;
            const wchar_t *a = raw;
            while (*a && *a != L' ' && *a != L'\t' && ni < 63) name[ni++] = *a++;
            name[ni] = 0;
            wchar_t args[1024]; wcsncpy_s(args, 1024, a, _TRUNCATE);
            /* strip trailing " ;comment" (unescaped) */
            for (wchar_t *c = args; *c; c++)
                if (*c == L';' && c > args && (c[-1] == L' ' || c[-1] == L'\t') && (c - 1 == args || c[-2] != L'`'))
                { *c = 0; break; }
            trimW(args);
            wchar_t ex[1024]; expandVars(args, ex, 1024, scriptPath);

            if (!_wcsicmp(name, L"Assets"))
            {
                wchar_t v[1024]; wcscpy_s(v, 1024, ex); unescape(v);
                if (v[0]) resolvePath(scriptDir, v, assetsDir, assetsCch);
            }
            else if (!_wcsicmp(name, L"SetMainIcon"))
            {
                wchar_t v[1024]; wcscpy_s(v, 1024, ex); unescape(v);
                if (v[0]) resolvePath(scriptDir, v, o->icon, MAX_PATH); else o->icon[0] = 0;
            }
            else if (!_wcsicmp(name, L"ExeName"))
            {
                wchar_t v[1024]; wcscpy_s(v, 1024, ex); unescape(v);
                if (v[0])
                {
                    wchar_t full[MAX_PATH]; resolvePath(scriptDir, v, full, MAX_PATH);
                    size_t L = wcslen(v);
                    if (v[L-1] == L'\\' || v[L-1] == L'/')
                    {   /* directory only: keep script's name */
                        wchar_t d[MAX_PATH]; deriveOut(scriptPath, d, MAX_PATH);
                        const wchar_t *fn = wcsrchr(d, L'\\'); fn = fn ? fn + 1 : d;
                        size_t fl = wcslen(full);
                        if (fl && full[fl-1] != L'\\') wcscat_s(full, MAX_PATH, L"\\");
                        wcscat_s(full, MAX_PATH, fn);
                    }
                    else
                    {
                        const wchar_t *fn = wcsrchr(full, L'\\'); fn = fn ? fn + 1 : full;
                        if (!wcschr(fn, L'.')) wcscat_s(full, MAX_PATH, L".exe");
                    }
                    wcscpy_s(o->out, MAX_PATH, full);
                }
            }
            else if (!_wcsicmp(name, L"Base") || !_wcsicmp(name, L"Bin"))
            {
                if (isWeb)
                {
                    wchar_t v[1024]; wcscpy_s(v, 1024, ex); unescape(v);
                    if (v[0]) resolvePath(scriptDir, v, o->bin, MAX_PATH);
                }
                else info(L"note: ;@Ahk2Exe-%s ignored (stock AutoHotkey base); use ;@AhkWeb-Base", name);
            }
            else if (!_wcsicmp(name, L"SetVersion"))  { unescape(ex); setProp(o, L"Version", ex); }
            else if (!_wcsicmp(name, L"SetName"))     { unescape(ex); setProp(o, L"Name", ex); }
            else if (!_wcsicmp(name, L"Set"))
            {
                wchar_t k[1024], v[1024];
                splitComma(ex, k, v, 1024); unescape(k); unescape(v);
                setProp(o, k, v);
            }
            else if (!_wcsnicmp(name, L"Set", 3) && name[3])
            {   unescape(ex); setProp(o, name + 3, ex); }
            else if (!_wcsicmp(name, L"AddResource"))
                addResourceDirective(o, ex, scriptDir);
            else
                info(L"note: directive ;@%s%s not supported, ignored", isWeb ? L"AhkWeb-" : L"Ahk2Exe-", name);
        }
        line = *eol ? eol + 1 : eol;
    }
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
        for (size_t i = 0; i < VI_STD_N; i++)
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

/* ---------------------------------------------------------------- compile -- */
typedef struct { unsigned char *data; DWORD len; } Blob;

static int compileScript(const Opts *src, wchar_t *outPath, size_t outCch)
{
    Opts o = *src;
    int ok = 0, outCreated = 0;
    unsigned char *script = NULL, *encBuf = NULL;
    wchar_t *wscript = NULL;
    void *zipBuf = NULL; size_t zipSize = 0;
    HMODULE hb = NULL; HANDLE upd = NULL;
    Blob mainIco = {0}, resData[MAX_RES] = {0};
    Buf ver = {0};
    g_err[0] = 0;

    if (!o.in[0]) { fail(L"no script specified (/in)"); goto done; }
    wchar_t inFull[MAX_PATH]; GetFullPathNameW(o.in, MAX_PATH, inFull, NULL);
    wchar_t scriptDir[MAX_PATH]; dirOf(inFull, scriptDir, MAX_PATH);

    /* 1) Read the script; strip a UTF-8 BOM if present. */
    DWORD slen = 0;
    script = readFile(inFull, &slen);
    if (!script) { fail(L"cannot read script: %s", inFull); goto done; }
    unsigned char *sp = script; DWORD sl = slen;
    if (sl >= 3 && sp[0] == 0xEF && sp[1] == 0xBB && sp[2] == 0xBF) { sp += 3; sl -= 3; }

    /* 2) Directives (UTF-8, fall back to ANSI). */
    {
        UINT cp = CP_UTF8;
        int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)sp, (int)sl, NULL, 0);
        if (!wn && sl) { cp = CP_ACP; wn = MultiByteToWideChar(CP_ACP, 0, (const char *)sp, (int)sl, NULL, 0); }
        wscript = (wchar_t *)malloc(((size_t)wn + 1) * sizeof(wchar_t));
        if (!wscript) { fail(L"out of memory"); goto done; }
        if (wn) MultiByteToWideChar(cp, 0, (const char *)sp, (int)sl, wscript, wn);
        wscript[wn] = 0;
    }
    wchar_t assetsDir[MAX_PATH] = L"";
    parseDirectives(wscript, inFull, &o, assetsDir, MAX_PATH);

    if (!o.out[0]) deriveOut(inFull, o.out, MAX_PATH);
    else { wchar_t t[MAX_PATH]; GetFullPathNameW(o.out, MAX_PATH, t, NULL); wcscpy_s(o.out, MAX_PATH, t); }
    if (!o.bin[0]) defaultBin(o.bin, MAX_PATH);
    if (outPath) wcsncpy_s(outPath, outCch, o.out, _TRUNCATE);

    if (!fileExists(o.bin))
    { fail(L"base file not found: %s (build the engine Self-contained|x64 config first)", o.bin); goto done; }
    if (!_wcsicmp(o.out, inFull) || !_wcsicmp(o.out, o.bin)) { fail(L"output path must differ from script/base"); goto done; }

    /* 3) Load and validate icon / resource files BEFORE touching the output. */
    if (o.icon[0])
    {
        mainIco.data = readFile(o.icon, &mainIco.len);
        if (!mainIco.data) { fail(L"cannot read icon: %s", o.icon); goto done; }
        if (!icoValidate(mainIco.data, mainIco.len)) { fail(L"not a valid .ico file: %s", o.icon); goto done; }
    }
    for (int i = 0; i < o.nres; i++)
    {
        resData[i].data = readFile(o.res[i].path, &resData[i].len);
        if (!resData[i].data) { fail(L"AddResource: cannot read %s", o.res[i].path); goto done; }
        if (!o.res[i].type && endsWithI(o.res[i].path, L".ico") && !icoValidate(resData[i].data, resData[i].len))
        { fail(L"AddResource: not a valid .ico file: %s", o.res[i].path); goto done; }
    }

    /* 4) Optional: zip the ;@AhkWeb-Assets folder into memory. */
    if (assetsDir[0])
    {
        DWORD a = GetFileAttributesW(assetsDir);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
        { fail(L"assets folder not found: %s", assetsDir); goto done; }
        mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
        if (!mz_zip_writer_init_heap(&zip, 0, 0)) { fail(L"zip init failed"); goto done; }
        if (!zipAddFolder(&zip, assetsDir, L"")) { mz_zip_writer_end(&zip); if (!g_err[0]) fail(L"failed to add assets"); goto done; }
        if (!mz_zip_writer_finalize_heap_archive(&zip, &zipBuf, &zipSize)) { mz_zip_writer_end(&zip); fail(L"zip finalize failed"); goto done; }
        mz_zip_writer_end(&zip);
        info(L"embedded assets from %s (%zu bytes zipped)", assetsDir, zipSize);
    }

    /* 5) Obfuscate + compress the script into the AHKW1 container (script_crypt.h). */
    DWORD scriptOutLen;
    {
        mz_ulong bound = mz_compressBound(sl ? sl : 1);
        encBuf = (unsigned char *)malloc(AHKW_SCRIPT_HDR_LEN + bound);
        if (!encBuf) { fail(L"out of memory"); goto done; }
        mz_ulong compLen = bound;
        if (mz_compress2(encBuf + AHKW_SCRIPT_HDR_LEN, &compLen, sp, sl, MZ_BEST_COMPRESSION) != MZ_OK)
        { fail(L"script compress failed"); goto done; }
        ahkw_write_header(encBuf, (uint32_t)sl);
        ahkw_xor_keystream(encBuf + AHKW_SCRIPT_HDR_LEN, (uint32_t)compLen, AHKW_SCRIPT_SEED ^ (uint64_t)sl);
        scriptOutLen = AHKW_SCRIPT_HDR_LEN + (DWORD)compLen;
    }

    /* 6) Version info: base strings + explicit overrides (only if anything was set). */
    if (o.nvi)
    {
        VerInfo vi; loadBaseVersion(o.bin, &vi);
        for (int i = 0; i < o.nvi; i++) viSet(vi.s, &vi.n, o.vi[i].key, o.vi[i].val);
        if (!buildVersion(&vi, &ver)) { fail(L"out of memory (version)"); goto done; }
    }

    /* 7) Copy base -> output (creating the destination folder if needed). */
    { wchar_t od[MAX_PATH]; dirOf(o.out, od, MAX_PATH); if (od[0]) SHCreateDirectoryExW(NULL, od, NULL); }
    if (!CopyFileW(o.bin, o.out, FALSE))
    { fail(L"cannot write output %s (err %lu) - is it running?", o.out, GetLastError()); goto done; }
    outCreated = 1;
    SetFileAttributesW(o.out, FILE_ATTRIBUTE_NORMAL);

    hb = LoadLibraryExW(o.bin, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    WORD nextIconId = 0;
    if (hb) EnumResourceNamesW(hb, RT_ICON_W, maxIdCb, (LONG_PTR)&nextIconId);
    nextIconId++;

    /* 8) Embed resources in one update session. */
    upd = BeginUpdateResourceW(o.out, FALSE);
    if (!upd) { fail(L"BeginUpdateResource failed (err %lu)", GetLastError()); goto done; }

    if (!UpdateResourceW(upd, RT_RCDATA_W, RES_SCRIPT, LANG_NEUTRAL_ID, encBuf, scriptOutLen))
    { fail(L"UpdateResource(script) failed"); goto done; }
    if (zipBuf && zipSize &&
        !UpdateResourceW(upd, RT_RCDATA_W, RES_ASSETS, LANG_NEUTRAL_ID, zipBuf, (DWORD)zipSize))
    { fail(L"UpdateResource(assets) failed"); goto done; }

    if (mainIco.data)
    {
        if (!writeIconGroup(upd, hb, MAKEINTRESOURCEW(IDI_MAIN_GROUP), mainIco.data, &nextIconId)) goto done;
        info(L"icon: %s", o.icon);
    }

    for (int i = 0; i < o.nres; i++)
    {
        const ResItem *r = &o.res[i];
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
        else if (endsWithI(r->path, L".manifest")) type = MAKEINTRESOURCEW(24);
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

    if (!EndUpdateResourceW(upd, FALSE)) { upd = NULL; fail(L"EndUpdateResource failed (err %lu)", GetLastError()); goto done; }
    upd = NULL;
    /* Make Explorer pick up the new icon instead of a cached one. */
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, o.out, NULL);
    info(L"wrote %s (script encoded)", o.out);
    ok = 1;

done:
    if (upd) EndUpdateResourceW(upd, TRUE);
    if (hb) FreeLibrary(hb);
    if (!ok && outCreated) DeleteFileW(o.out);
    free(ver.p);
    free(mainIco.data);
    for (int i = 0; i < MAX_RES; i++) free(resData[i].data);
    if (zipBuf) mz_free(zipBuf);
    free(encBuf);
    free(wscript);
    free(script);
    return ok;
}

/* -------------------------------------------------------------------- GUI -- */
enum {
    ID_SRC = 101, ID_SRC_BR, ID_OUT, ID_OUT_BR, ID_ICO, ID_ICO_BR, ID_ICO_DEF, ID_BIN, ID_BIN_BR,
    ID_DESC, ID_VER, ID_PROD, ID_COMP, ID_COPY, ID_PREVIEW, ID_CONVERT, ID_CLOSE, ID_STATUS
};
static const wchar_t *REG_KEY = L"Software\\AutoHotkeyWeb\\AhkWeb2Exe";
static HWND  g_wnd;
static HFONT g_font;
static HICON g_preview;
static double g_scale = 1.0;
static int   g_outAuto = 1, g_settingOut = 0;
static wchar_t g_initSrc[MAX_PATH];

#define S(x) ((int)((x) * g_scale + 0.5))

static void getText(int id, wchar_t *buf, int cch) { GetDlgItemTextW(g_wnd, id, buf, cch); trimW(buf); }

static void regGet(const wchar_t *name, wchar_t *buf, DWORD cch)
{
    DWORD sz = cch * sizeof(wchar_t); buf[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS) buf[0] = 0;
}
static void regSet(const wchar_t *name, const wchar_t *val)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, name, REG_SZ, val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
}

static void updatePreview(void)
{
    wchar_t ico[MAX_PATH], bin[MAX_PATH];
    getText(ID_ICO, ico, MAX_PATH); getText(ID_BIN, bin, MAX_PATH);
    if (!bin[0]) defaultBin(bin, MAX_PATH);
    HICON h = NULL; int sz = S(32);
    if (ico[0]) h = (HICON)LoadImageW(NULL, ico, IMAGE_ICON, sz, sz, LR_LOADFROMFILE);
    else
    {
        HMODULE m = LoadLibraryExW(bin, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (m) { h = (HICON)LoadImageW(m, MAKEINTRESOURCEW(IDI_MAIN_GROUP), IMAGE_ICON, sz, sz, 0); FreeLibrary(m); }
    }
    SendDlgItemMessageW(g_wnd, ID_PREVIEW, STM_SETICON, (WPARAM)h, 0);
    if (g_preview) DestroyIcon(g_preview);
    g_preview = h;
}

static void setStatus(const wchar_t *s) { SetDlgItemTextW(g_wnd, ID_STATUS, s); }

static void setSource(const wchar_t *path)
{
    SetDlgItemTextW(g_wnd, ID_SRC, path);   /* EN_CHANGE derives the output */
}

static int browse(int editId, int save, const wchar_t *filter, const wchar_t *defExt)
{
    wchar_t file[MAX_PATH]; getText(editId, file, MAX_PATH);
    wchar_t dir[MAX_PATH] = L"";
    if (file[0]) { dirOf(file, dir, MAX_PATH); wchar_t *fn = wcsrchr(file, L'\\'); if (fn) memmove(file, fn + 1, (wcslen(fn + 1) + 1) * sizeof(wchar_t)); }
    else if (editId == ID_SRC) regGet(L"LastDir", dir, MAX_PATH);
    OPENFILENAMEW of; memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of); of.hwndOwner = g_wnd;
    of.lpstrFilter = filter; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.lpstrInitialDir = dir[0] ? dir : NULL; of.lpstrDefExt = defExt;
    of.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of))) return 0;
    if (editId == ID_SRC) setSource(file); else SetDlgItemTextW(g_wnd, editId, file);
    return 1;
}

static void doConvert(void)
{
    static Opts o; memset(&o, 0, sizeof(o));
    getText(ID_SRC, o.in, MAX_PATH);
    getText(ID_OUT, o.out, MAX_PATH);
    getText(ID_ICO, o.icon, MAX_PATH);
    getText(ID_BIN, o.bin, MAX_PATH);
    if (!o.in[0]) { MessageBoxW(g_wnd, L"컴파일할 스크립트를 선택하세요.", L"AhkWeb2Exe", MB_ICONWARNING); return; }

    static const struct { int id; const wchar_t *key; } F[] = {
        { ID_DESC, L"FileDescription" }, { ID_PROD, L"ProductName" },
        { ID_COMP, L"CompanyName" }, { ID_COPY, L"LegalCopyright" },
    };
    wchar_t v[256];
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++)
    { getText(F[i].id, v, 256); if (v[0]) viSet(o.vi, &o.nvi, F[i].key, v); }
    getText(ID_VER, v, 256);
    if (v[0]) { viSet(o.vi, &o.nvi, L"FileVersion", v); viSet(o.vi, &o.nvi, L"ProductVersion", v); }

    setStatus(L"컴파일 중...");
    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    g_log[0] = 0;
    wchar_t out[MAX_PATH] = L"";
    int ok = compileScript(&o, out, MAX_PATH);
    SetCursor(old);

    if (ok)
    {
        wchar_t d[MAX_PATH]; dirOf(o.in, d, MAX_PATH); regSet(L"LastDir", d);
        regSet(L"LastIcon", o.icon);
        wchar_t defb[MAX_PATH]; defaultBin(defb, MAX_PATH);
        regSet(L"LastBin", _wcsicmp(o.bin, defb) ? o.bin : L"");
        setStatus(L"컴파일 완료");
        wchar_t m[4096 + MAX_PATH + 64];
        _snwprintf_s(m, sizeof(m) / sizeof(m[0]), _TRUNCATE, L"컴파일 완료:\n%s\n\n%s", out, g_log);
        MessageBoxW(g_wnd, m, L"AhkWeb2Exe", MB_ICONINFORMATION);
    }
    else
    {
        setStatus(L"컴파일 실패");
        wchar_t m[1200]; _snwprintf_s(m, 1200, _TRUNCATE, L"컴파일 실패:\n%s", g_err);
        MessageBoxW(g_wnd, m, L"AhkWeb2Exe", MB_ICONERROR);
    }
}

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, txt,
                             WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}
#define LBL(t, x, y, w)     mk(L"STATIC", t, SS_LEFT, x, (y) + 4, w, 20, -1)
#define EDT(id, x, y, w)    mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 24, id)
#define BTN(t, id, x, y, w) mk(L"BUTTON", t, WS_TABSTOP | BS_PUSHBUTTON, x, (y) - 1, w, 26, id)

static void buildControls(void)
{
    LBL(L"스크립트 (.ahk)", 12, 14, 112);   EDT(ID_SRC, 128, 14, 360);  BTN(L"찾기...", ID_SRC_BR, 494, 14, 82);
    LBL(L"출력 파일 (.exe)", 12, 46, 112);  EDT(ID_OUT, 128, 46, 360);  BTN(L"찾기...", ID_OUT_BR, 494, 46, 82);
    LBL(L"아이콘 (.ico)", 12, 78, 112);     EDT(ID_ICO, 128, 78, 272);  BTN(L"찾기...", ID_ICO_BR, 406, 78, 82);
    BTN(L"기본 아이콘", ID_ICO_DEF, 494, 78, 82);
    mk(L"STATIC", L"", SS_ICON | SS_CENTERIMAGE | SS_SUNKEN, 584, 64, 44, 44, ID_PREVIEW);
    LBL(L"베이스 (.bin)", 12, 110, 112);    EDT(ID_BIN, 128, 110, 360); BTN(L"찾기...", ID_BIN_BR, 494, 110, 82);

    mk(L"BUTTON", L"버전 정보 (선택 · 비우면 기본값 · 스크립트의 ;@Ahk2Exe 지시어가 우선)",
       BS_GROUPBOX, 12, 148, 616, 128, -1);
    LBL(L"파일 설명", 26, 172, 78);   EDT(ID_DESC, 106, 172, 218);
    LBL(L"버전", 338, 172, 60);       EDT(ID_VER, 400, 172, 214);
    LBL(L"제품명", 26, 204, 78);      EDT(ID_PROD, 106, 204, 218);
    LBL(L"회사", 338, 204, 60);       EDT(ID_COMP, 400, 204, 214);
    LBL(L"저작권", 26, 236, 78);      EDT(ID_COPY, 106, 236, 508);

    mk(L"STATIC", L"스크립트나 아이콘 파일을 창에 끌어다 놓을 수 있습니다.", SS_LEFT | SS_ENDELLIPSIS,
       12, 296, 400, 20, ID_STATUS);
    mk(L"BUTTON", L"컴파일", WS_TABSTOP | BS_DEFPUSHBUTTON, 438, 290, 92, 30, ID_CONVERT);
    mk(L"BUTTON", L"닫기", WS_TABSTOP | BS_PUSHBUTTON, 536, 290, 92, 30, ID_CLOSE);

    wchar_t v[MAX_PATH];
    regGet(L"LastIcon", v, MAX_PATH); if (v[0] && fileExists(v)) SetDlgItemTextW(g_wnd, ID_ICO, v);
    regGet(L"LastBin", v, MAX_PATH);
    if (v[0] && fileExists(v)) SetDlgItemTextW(g_wnd, ID_BIN, v);
    else { defaultBin(v, MAX_PATH); SetDlgItemTextW(g_wnd, ID_BIN, v); }
    if (g_initSrc[0]) setSource(g_initSrc);
    updatePreview();
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        g_wnd = h;
        DragAcceptFiles(h, TRUE);
        buildControls();
        SetFocus(GetDlgItem(h, ID_SRC));
        return 0;
    case WM_DROPFILES:
    {
        HDROP d = (HDROP)wp; UINT n = DragQueryFileW(d, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < n; i++)
        {
            wchar_t f[MAX_PATH]; DragQueryFileW(d, i, f, MAX_PATH);
            if (endsWithI(f, L".ico")) SetDlgItemTextW(h, ID_ICO, f);
            else if (endsWithI(f, L".bin")) SetDlgItemTextW(h, ID_BIN, f);
            else if (endsWithI(f, L".exe")) { SetDlgItemTextW(h, ID_OUT, f); }
            else setSource(f);
        }
        DragFinish(d);
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (code == EN_CHANGE)
        {
            if (id == ID_SRC && g_outAuto)
            {
                wchar_t s[MAX_PATH], o[MAX_PATH] = L"";
                getText(ID_SRC, s, MAX_PATH);
                if (s[0]) deriveOut(s, o, MAX_PATH);
                g_settingOut = 1; SetDlgItemTextW(h, ID_OUT, o); g_settingOut = 0;
            }
            else if (id == ID_OUT && !g_settingOut)
            {   wchar_t o[MAX_PATH]; getText(ID_OUT, o, MAX_PATH); g_outAuto = !o[0]; }
            else if (id == ID_ICO || id == ID_BIN) updatePreview();
            return 0;
        }
        switch (id)
        {
        case ID_SRC_BR: browse(ID_SRC, 0, L"AutoHotkey 스크립트 (*.ahk;*.ahkw)\0*.ahk;*.ahkw\0모든 파일 (*.*)\0*.*\0", L"ahk"); break;
        case ID_OUT_BR: if (browse(ID_OUT, 1, L"실행 파일 (*.exe)\0*.exe\0", L"exe")) g_outAuto = 0; break;
        case ID_ICO_BR: browse(ID_ICO, 0, L"아이콘 (*.ico)\0*.ico\0", L"ico"); break;
        case ID_ICO_DEF: SetDlgItemTextW(h, ID_ICO, L""); break;
        case ID_BIN_BR: browse(ID_BIN, 0, L"AutoHotkey Web 베이스 (*.bin)\0*.bin\0모든 파일 (*.*)\0*.*\0", L"bin"); break;
        case ID_CONVERT: doConvert(); break;
        case ID_CLOSE: case IDCANCEL: DestroyWindow(h); break;
        }
        return 0;
    }
    case WM_DESTROY:
        if (g_preview) DestroyIcon(g_preview);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int runGui(void)
{
    SetProcessDPIAware();
    HDC dc = GetDC(NULL); g_scale = GetDeviceCaps(dc, LOGPIXELSY) / 96.0; ReleaseDC(NULL, dc);
    g_font = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");

    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = wndProc; wc.hInstance = hi;
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AhkWeb2ExeWnd";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, S(640), S(332) };
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_CONTROLPARENT);
    HWND w = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"AhkWeb2Exe - AutoHotkey Web 스크립트 컴파일러",
                             style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                             NULL, NULL, hi, NULL);
    if (!w) return 1;
    ShowWindow(w, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { DestroyWindow(w); continue; }
        if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    DeleteObject(g_font);
    return 0;
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
    static Opts o; memset(&o, 0, sizeof(o));
    int gui = 0, haveIn = 0;
    for (int i = 1; i < argc; i++)
    {
        const wchar_t *a = argv[i];
        int more = i + 1 < argc;
        if      (!_wcsicmp(a, L"/in")   && more) { wcsncpy_s(o.in,   MAX_PATH, argv[++i], _TRUNCATE); haveIn = 1; }
        else if (!_wcsicmp(a, L"/out")  && more)   wcsncpy_s(o.out,  MAX_PATH, argv[++i], _TRUNCATE);
        else if ((!_wcsicmp(a, L"/bin") || !_wcsicmp(a, L"/base")) && more) wcsncpy_s(o.bin, MAX_PATH, argv[++i], _TRUNCATE);
        else if (!_wcsicmp(a, L"/icon") && more)   wcsncpy_s(o.icon, MAX_PATH, argv[++i], _TRUNCATE);
        else if (!_wcsicmp(a, L"/silent")) o.silent = 1;
        else if (!_wcsicmp(a, L"/gui"))    gui = 1;
        else if ((!_wcsicmp(a, L"/compress") || !_wcsicmp(a, L"/mpress") || !_wcsicmp(a, L"/cp") ||
                  !_wcsicmp(a, L"/resourceid")) && more) i++;   /* Ahk2Exe options, not applicable */
        else if (a[0] != L'/' && !o.in[0]) wcsncpy_s(o.in, MAX_PATH, a, _TRUNCATE);
    }

    if (!haveIn || gui)
    {
        if (o.in[0]) GetFullPathNameW(o.in, MAX_PATH, g_initSrc, NULL);
        return runGui();
    }

    initStdio();
    int ok = compileScript(&o, NULL, 0);
    if (!ok)
    {
        if (g_haveOut) { fwprintf(stderr, L"AhkWeb2Exe: %s\n", g_err); fflush(stderr); }
        else if (!o.silent)
        {
            wchar_t m[1200]; _snwprintf_s(m, 1200, _TRUNCATE, L"컴파일 실패:\n%s", g_err);
            MessageBoxW(NULL, m, L"AhkWeb2Exe", MB_ICONERROR);
        }
        return 2;
    }
    return 0;
}

// ============================================================================
//  AutoHotkey Web  -  script_webassets.cpp   (compiled WITHOUT the PCH)
//
//  Single-EXE hybrid apps: AhkWeb2Exe can embed a web asset folder (dist/ or
//  www/) as a zip in the RCDATA resource ">AHKWEB ASSETS<". At runtime the
//  engine extracts it once to a per-process temp folder and exposes the path
//  through the built-in variable A_AhkWebAssets, so a script can do:
//
//      assets := A_AhkWebAssets ? A_AhkWebAssets : A_ScriptDir "\www"
//      WebGui, Main:Mount, app.ahkweb, %assets%
//      WebGui, Main:Load,  https://app.ahkweb/index.html
//
//  and behave identically whether run from source or as a compiled EXE.
//  This file is isolated from the project's precompiled header so it can pull
//  in miniz without disturbing the rest of the build.
// ============================================================================
#include <windows.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "miniz.h"

// The resource name written by AhkWeb2Exe (must match the compiler).
static const wchar_t *ASSETS_RES_NAME = L">AHKWEB ASSETS<";

static bool WAMakeDirs(const std::wstring &path)
{
    // Create every directory level in path (which names a folder).
    std::wstring cur;
    for (size_t i = 0; i < path.size(); ++i)
    {
        wchar_t c = path[i];
        cur += c;
        if (c == L'\\' || c == L'/')
            CreateDirectoryW(cur.c_str(), NULL);
    }
    CreateDirectoryW(path.c_str(), NULL);
    return true;
}

// Returns the extracted assets directory, or an empty string if this EXE has no
// embedded assets (e.g. an uncompiled run, or a compile without ;@AhkWeb-Assets).
// The extraction happens once; later calls return the cached path.
const wchar_t *WebAssetsGetDir()
{
    static std::wstring s_dir;   // cached result
    static bool s_done = false;
    if (s_done)
        return s_dir.c_str();
    s_done = true;

    HRSRC hRes = FindResourceW(NULL, ASSETS_RES_NAME, (LPCWSTR)RT_RCDATA);
    if (!hRes)
        return s_dir.c_str(); // no embedded assets -> empty
    DWORD sz = SizeofResource(NULL, hRes);
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!sz || !hData)
        return s_dir.c_str();
    const void *zipMem = LockResource(hData);
    if (!zipMem)
        return s_dir.c_str();

    // Destination: %TEMP%\AhkWeb_<pid>\assets
    wchar_t temp[MAX_PATH]; DWORD tn = GetTempPathW(MAX_PATH, temp);
    if (!tn) return s_dir.c_str();
    std::wstring base = std::wstring(temp) + L"AhkWeb_" + std::to_wstring(GetCurrentProcessId());
    std::wstring dest = base + L"\\assets";
    WAMakeDirs(dest);

    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipMem, sz, 0))
        return s_dir.c_str();

    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        // st.m_filename is UTF-8 with '/' separators.
        int wl = MultiByteToWideChar(CP_UTF8, 0, st.m_filename, -1, NULL, 0);
        std::wstring rel(wl > 0 ? wl - 1 : 0, L'\0');
        if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, st.m_filename, -1, &rel[0], wl);
        for (auto &c : rel) if (c == L'/') c = L'\\';
        std::wstring full = dest + L"\\" + rel;

        if (mz_zip_reader_is_file_a_directory(&zip, i))
        {
            WAMakeDirs(full);
            continue;
        }
        // Ensure the parent directory exists.
        size_t slash = full.find_last_of(L'\\');
        if (slash != std::wstring::npos)
            WAMakeDirs(full.substr(0, slash));

        size_t outSize = 0;
        void *p = mz_zip_reader_extract_to_heap(&zip, i, &outSize, 0);
        if (p)
        {
            HANDLE f = CreateFileW(full.c_str(), GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD wr = 0; SIZE_T off = 0;
                while (off < outSize)
                {
                    DWORD want = (DWORD)((outSize - off > (1u<<20)) ? (1u<<20) : (outSize - off));
                    if (!WriteFile(f, (const BYTE*)p + off, want, &wr, NULL) || wr == 0) break;
                    off += wr;
                }
                CloseHandle(f);
            }
            mz_free(p);
        }
    }
    mz_zip_reader_end(&zip);

    s_dir = dest;
    return s_dir.c_str();
}

// ============================================================================
//  Bundled files (;@AhkWeb-Include) - AhkWeb2Exe embeds any files/folders as a
//  zip in RCDATA ">AHKWEB FILES<" plus a small UTF-16 config ">AHKWEB FILESCFG<":
//      dest=temp|exe|appdata|localappdata|path:<absolute>
//      mode=always|newer|missing
//      cleanup=0|1          (temp destination only)
//      sub=<folder name>    (appdata/localappdata subfolder; default exe name)
//  They are extracted at startup, before the auto-execute section, and the
//  folder is exposed as A_AhkWebFiles (A_ScriptDir when nothing is embedded).
// ============================================================================
static const wchar_t *FILES_RES_NAME = L">AHKWEB FILES<";
static const wchar_t *FILESCFG_RES_NAME = L">AHKWEB FILESCFG<";

static void WADeleteTree(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) WADeleteTree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static std::wstring WATempBase()
{
    wchar_t temp[MAX_PATH]; DWORD tn = GetTempPathW(MAX_PATH, temp);
    if (!tn) return L"";
    return std::wstring(temp) + L"AhkWeb_" + std::to_wstring(GetCurrentProcessId());
}

// Remove %TEMP%\AhkWeb_<pid> folders left behind by processes that no longer run
// (crash, kill, or files that were locked at exit).
static void WASweepStaleTemp()
{
    wchar_t temp[MAX_PATH]; if (!GetTempPathW(MAX_PATH, temp)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(temp) + L"AhkWeb_*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        DWORD pid = wcstoul(fd.cFileName + 7, NULL, 10);
        if (!pid || pid == GetCurrentProcessId()) continue;
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (p) { DWORD w = WaitForSingleObject(p, 0); CloseHandle(p); if (w == WAIT_TIMEOUT) continue; }
        WADeleteTree(std::wstring(temp) + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static bool s_cleanupTemp = false;
static void WACleanupAtExit()
{
    // Assets always live in the per-process temp folder; bundled files only when dest=temp.
    // Anything still locked (e.g. by WebView2) is swept by the next run's WASweepStaleTemp.
    std::wstring base = WATempBase();
    if (base.empty()) return;
    if (s_cleanupTemp) WADeleteTree(base);
    else WADeleteTree(base + L"\\assets"), RemoveDirectoryW(base.c_str());
}

static bool WAWriteFile(const std::wstring &full, const void *p, size_t n, time_t mtime)
{
    size_t slash = full.find_last_of(L'\\');
    if (slash != std::wstring::npos) WAMakeDirs(full.substr(0, slash));
    SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE f = CreateFileW(full.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = true; SIZE_T off = 0;
    while (off < n)
    {
        DWORD want = (DWORD)((n - off > (1u<<20)) ? (1u<<20) : (n - off)), wr = 0;
        if (!WriteFile(f, (const BYTE*)p + off, want, &wr, NULL) || !wr) { ok = false; break; }
        off += wr;
    }
    if (ok && mtime)
    {   // Stamp the zip entry time so "newer" mode can skip unchanged files next run.
        ULONGLONG t = ((ULONGLONG)mtime + 11644473600ULL) * 10000000ULL;
        FILETIME ft; ft.dwLowDateTime = (DWORD)t; ft.dwHighDateTime = (DWORD)(t >> 32);
        SetFileTime(f, NULL, NULL, &ft);
    }
    CloseHandle(f);
    return ok;
}

static bool WAUpToDate(const std::wstring &full, unsigned long long size, time_t mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &a)) return false;
    unsigned long long fsz = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (fsz != size) return false;
    ULONGLONG ft = ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    long long secs = (long long)(ft / 10000000ULL) - 11644473600LL;
    long long d = secs - (long long)mtime; if (d < 0) d = -d;
    return d <= 2;   // zip times have 2-second resolution
}

// Extract into dest; returns false if any file could not be written.
static bool WAExtractZip(const void *zipMem, DWORD sz, const std::wstring &dest, const std::wstring &mode)
{
    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipMem, sz, 0)) return false;
    bool ok = WAMakeDirs(dest);
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n && ok; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        int wl = MultiByteToWideChar(CP_UTF8, 0, st.m_filename, -1, NULL, 0);
        std::wstring rel(wl > 0 ? wl - 1 : 0, L'\0');
        if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, st.m_filename, -1, &rel[0], wl);
        for (auto &c : rel) if (c == L'/') c = L'\\';
        if (rel.find(L"..") != std::wstring::npos || (rel.size() > 1 && rel[1] == L':')) continue; // no escaping dest
        std::wstring full = dest + L"\\" + rel;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) { WAMakeDirs(full); continue; }
        if (mode == L"missing" && GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (mode == L"newer" && WAUpToDate(full, st.m_uncomp_size, st.m_time)) continue;
        size_t outSize = 0;
        void *p = mz_zip_reader_extract_to_heap(&zip, i, &outSize, 0);
        if (!p) { ok = false; break; }
        ok = WAWriteFile(full, p, outSize, st.m_time);
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

static std::wstring WAKnownFolder(int csidl)
{
    wchar_t buf[MAX_PATH] = L"";
    typedef HRESULT (WINAPI *SHGFP)(HWND, int, HANDLE, DWORD, LPWSTR);
    HMODULE sh = LoadLibraryW(L"shell32.dll");
    SHGFP fn = sh ? (SHGFP)GetProcAddress(sh, "SHGetFolderPathW") : NULL;
    if (fn) fn(NULL, csidl, NULL, 0, buf);
    return buf;
}

static std::wstring s_filesDir;
static bool s_filesDone = false;

// Called once at startup (before the auto-execute section). Also sweeps stale temp folders.
void WebFilesExtractAtStartup()
{
    if (s_filesDone) return;
    s_filesDone = true;
    WASweepStaleTemp();
    atexit(WACleanupAtExit);

    HRSRC hRes = FindResourceW(NULL, FILES_RES_NAME, (LPCWSTR)RT_RCDATA);
    if (!hRes) return;
    DWORD sz = SizeofResource(NULL, hRes);
    const void *zipMem = LockResource(LoadResource(NULL, hRes));
    if (!sz || !zipMem) return;

    std::wstring dest = L"temp", mode = L"newer", sub, cleanup = L"1";
    HRSRC hCfg = FindResourceW(NULL, FILESCFG_RES_NAME, (LPCWSTR)RT_RCDATA);
    if (hCfg)
    {
        DWORD cs = SizeofResource(NULL, hCfg);
        const wchar_t *c = (const wchar_t *)LockResource(LoadResource(NULL, hCfg));
        std::wstring cfg = c ? std::wstring(c, cs / sizeof(wchar_t)) : L"";
        size_t pos = 0;
        while (pos < cfg.size())
        {
            size_t e = cfg.find(L'\n', pos); if (e == std::wstring::npos) e = cfg.size();
            std::wstring line = cfg.substr(pos, e - pos); pos = e + 1;
            size_t eq = line.find(L'='); if (eq == std::wstring::npos) continue;
            std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
            while (!v.empty() && (v.back() == L'\r' || v.back() == L'\0')) v.pop_back();
            if (k == L"dest") dest = v; else if (k == L"mode") mode = v;
            else if (k == L"sub") sub = v; else if (k == L"cleanup") cleanup = v;
        }
    }

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring exeDir = exe; exeDir = exeDir.substr(0, exeDir.find_last_of(L'\\'));
    if (sub.empty())
    {
        std::wstring nm = exe; nm = nm.substr(nm.find_last_of(L'\\') + 1);
        size_t dot = nm.find_last_of(L'.'); if (dot != std::wstring::npos) nm.resize(dot);
        sub = nm;
    }
    std::wstring local = WAKnownFolder(0x001c /*CSIDL_LOCAL_APPDATA*/);
    std::wstring target;
    if (dest == L"exe") target = exeDir;
    else if (dest == L"appdata") target = WAKnownFolder(0x001a /*CSIDL_APPDATA*/) + L"\\" + sub;
    else if (dest == L"localappdata") target = local + L"\\" + sub;
    else if (dest.compare(0, 5, L"path:") == 0) target = dest.substr(5);
    else { target = WATempBase() + L"\\files"; s_cleanupTemp = (cleanup != L"0"); }

    if (!WAExtractZip(zipMem, sz, target, mode))
    {   // e.g. exe folder under Program Files without admin rights -> per-user fallback.
        target = local + L"\\" + sub;
        WAExtractZip(zipMem, sz, target, mode);
    }
    s_filesDir = target;
}

// The extraction folder, or "" when nothing is embedded (the caller then uses A_ScriptDir).
const wchar_t *WebFilesGetDir()
{
    if (!s_filesDone) WebFilesExtractAtStartup();
    return s_filesDir.c_str();
}

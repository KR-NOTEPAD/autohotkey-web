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

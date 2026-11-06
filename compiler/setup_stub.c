/* ============================================================================
 *  AhkWebSetup - native setup wizard stub used by AhkWeb2Exe (setup mode)
 *
 *  AhkWeb2Exe copies this exe (AhkWebSetup.bin) and adds RCDATA resources:
 *      ">SETUP CFG<"      UTF-16 key=value lines (name, version, publisher, exe,
 *                         dir, appid, scope=user|machine, desktop, startmenu,
 *                         run, sizekb)
 *      ">SETUP PAYLOAD<"  zip of the files to install (app exe + bundled files)
 *      ">SETUP LICENSE<"  optional UTF-16 license text
 *  plus its icon (group 1) and version info.
 *
 *  Command line:
 *      Setup.exe [/S] [/D=<dir> | /DIR=<dir>]       install (wizard or silent)
 *      uninstall.exe [/S]                           uninstall (copies itself to
 *                                                   %TEMP% and continues there)
 *  Installed: files, uninstall.exe, uninstall.log, shortcuts, and an entry in
 *  "Apps & features" (HKCU or HKLM ...\Uninstall\<appid>).
 *
 *  Build: build_compiler.bat (cl /utf-8 setup_stub.c miniz.c setup_stub.res).
 * ============================================================================ */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "miniz.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define RT_RCDATA_W MAKEINTRESOURCEW(10)
static int g_ko;   /* Korean UI */
#define TR(ko, en) (g_ko ? L##ko : L##en)

/* ------------------------------------------------------------------ config -- */
typedef struct {
    wchar_t name[128], version[64], publisher[128], exe[MAX_PATH], dir[128], appid[160];
    int machine, ask, desktop, startmenu, run;   /* ask: user picks current-user / all-users in the wizard */
    unsigned long sizekb;
} Cfg;
static Cfg      g_cfg;
static wchar_t *g_license;
static const void *g_payload; static DWORD g_payloadSize;
static int      g_silent;
static wchar_t  g_installDir[MAX_PATH];
static wchar_t  g_err[1024];
static HINSTANCE g_hi;

static const void *getRes(const wchar_t *name, DWORD *size)
{
    HRSRC r = FindResourceW(NULL, name, RT_RCDATA_W);
    if (!r) return NULL;
    *size = SizeofResource(NULL, r);
    return LockResource(LoadResource(NULL, r));
}

static void cfgSet(const wchar_t *k, const wchar_t *v)
{
    if (!wcscmp(k, L"name")) wcsncpy_s(g_cfg.name, 128, v, _TRUNCATE);
    else if (!wcscmp(k, L"version")) wcsncpy_s(g_cfg.version, 64, v, _TRUNCATE);
    else if (!wcscmp(k, L"publisher")) wcsncpy_s(g_cfg.publisher, 128, v, _TRUNCATE);
    else if (!wcscmp(k, L"exe")) wcsncpy_s(g_cfg.exe, MAX_PATH, v, _TRUNCATE);
    else if (!wcscmp(k, L"dir")) wcsncpy_s(g_cfg.dir, 128, v, _TRUNCATE);
    else if (!wcscmp(k, L"appid")) wcsncpy_s(g_cfg.appid, 160, v, _TRUNCATE);
    else if (!wcscmp(k, L"scope")) { g_cfg.machine = !wcscmp(v, L"machine"); g_cfg.ask = !wcscmp(v, L"ask"); }
    else if (!wcscmp(k, L"desktop")) g_cfg.desktop = _wtoi(v);
    else if (!wcscmp(k, L"startmenu")) g_cfg.startmenu = _wtoi(v);
    else if (!wcscmp(k, L"run")) g_cfg.run = _wtoi(v);
    else if (!wcscmp(k, L"sizekb")) g_cfg.sizekb = wcstoul(v, NULL, 10);
}

static int loadCfg(void)
{
    DWORD n = 0; const wchar_t *c = (const wchar_t *)getRes(L">SETUP CFG<", &n);
    if (!c) return 0;
    size_t wc = n / sizeof(wchar_t);
    wchar_t *buf = (wchar_t *)calloc(wc + 1, sizeof(wchar_t));
    if (!buf) return 0;
    wmemcpy(buf, c, wc);
    wchar_t *ctx = NULL;
    for (wchar_t *line = wcstok_s(buf, L"\n", &ctx); line; line = wcstok_s(NULL, L"\n", &ctx))
    {
        wchar_t *eq = wcschr(line, L'='); if (!eq) continue;
        *eq = 0; cfgSet(line, eq + 1);
    }
    free(buf);
    DWORD ln = 0; const wchar_t *lic = (const wchar_t *)getRes(L">SETUP LICENSE<", &ln);
    if (lic && ln)
    {   /* normalize to CRLF for the edit control */
        size_t wl = ln / sizeof(wchar_t);
        g_license = (wchar_t *)calloc(wl * 2 + 1, sizeof(wchar_t));
        if (g_license)
        {
            size_t k = 0;
            for (size_t i = 0; i < wl; i++)
            {
                if (lic[i] == L'\n' && (i == 0 || lic[i-1] != L'\r')) g_license[k++] = L'\r';
                g_license[k++] = lic[i];
            }
        }
    }
    g_payload = getRes(L">SETUP PAYLOAD<", &g_payloadSize);
    return g_cfg.exe[0] && g_cfg.appid[0];
}

/* The config as actually installed (scope fixed) - written into uninstall.exe. */
static void cfgSerialize(wchar_t *out, size_t cch)
{
    _snwprintf_s(out, cch, _TRUNCATE,
        L"name=%s\nversion=%s\npublisher=%s\nexe=%s\ndir=%s\nappid=%s\nscope=%s\ndesktop=%d\nstartmenu=%d\nrun=%d\nsizekb=%lu\n",
        g_cfg.name, g_cfg.version, g_cfg.publisher, g_cfg.exe, g_cfg.dir, g_cfg.appid,
        g_cfg.machine ? L"machine" : L"user", g_cfg.desktop, g_cfg.startmenu, g_cfg.run, g_cfg.sizekb);
}

static int uninstallKeyExists(HKEY root)
{
    wchar_t key[300]; HKEY k;
    _snwprintf_s(key, 300, _TRUNCATE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s", g_cfg.appid);
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    RegCloseKey(k); return 1;
}

/* ----------------------------------------------------------------- helpers -- */
static int isAdmin(void)
{
    BOOL r = FALSE; SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY; PSID sid;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid))
    { CheckTokenMembership(NULL, sid, &r); FreeSid(sid); }
    return r;
}

/* Relaunch elevated with the same arguments; returns 1 if launched. */
static int relaunchElevated(const wchar_t *extraArgs)
{
    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    const wchar_t *cl = GetCommandLineW();
    if (*cl == L'"') { cl = wcschr(cl + 1, L'"'); cl = cl ? cl + 1 : L""; }
    else { while (*cl && *cl != L' ') cl++; }
    wchar_t args[2048]; _snwprintf_s(args, 2048, _TRUNCATE, L"%s %s", cl, extraArgs ? extraArgs : L"");
    SHELLEXECUTEINFOW se; memset(&se, 0, sizeof(se));
    se.cbSize = sizeof(se); se.fMask = SEE_MASK_NOCLOSEPROCESS; se.lpVerb = L"runas";
    se.lpFile = self; se.lpParameters = args; se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&se)) return 0;
    if (se.hProcess)
    {   /* silent callers (scripts, deployment tools) expect us to return when it is done */
        if (g_silent) WaitForSingleObject(se.hProcess, INFINITE);
        CloseHandle(se.hProcess);
    }
    return 1;
}

static void folderPath(int csidlUser, int csidlCommon, wchar_t *out)
{
    out[0] = 0;
    SHGetFolderPathW(NULL, g_cfg.machine ? csidlCommon : csidlUser, NULL, 0, out);
}

static void defaultInstallDir(wchar_t *out, size_t cch)
{
    /* an existing install of the same app wins (upgrade in place) */
    HKEY root = g_cfg.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    wchar_t key[300]; _snwprintf_s(key, 300, _TRUNCATE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s", g_cfg.appid);
    DWORD sz = (DWORD)(cch * sizeof(wchar_t));
    if (RegGetValueW(root, key, L"InstallLocation", RRF_RT_REG_SZ, NULL, out, &sz) == ERROR_SUCCESS && out[0]) return;
    wchar_t base[MAX_PATH] = L"";
    if (g_cfg.machine) SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, base);
    else { SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base); wcscat_s(base, MAX_PATH, L"\\Programs"); }
    _snwprintf_s(out, cch, _TRUNCATE, L"%s\\%s", base, g_cfg.dir[0] ? g_cfg.dir : g_cfg.name);
}

static void makeDirs(const wchar_t *path, FILE *log)
{
    wchar_t cur[MAX_PATH]; size_t n = wcslen(path);
    for (size_t i = 0; i <= n && i < MAX_PATH - 1; i++)
    {
        if (path[i] == L'\\' || path[i] == 0)
        {
            wmemcpy(cur, path, i); cur[i] = 0;
            if (i > 2 && GetFileAttributesW(cur) == INVALID_FILE_ATTRIBUTES)
            {
                if (CreateDirectoryW(cur, NULL) && log) fwprintf(log, L"D %s\n", cur);
            }
        }
    }
}

static int createShortcut(const wchar_t *lnk, const wchar_t *target, const wchar_t *workDir)
{
    IShellLinkW *sl = NULL; IPersistFile *pf = NULL; int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return 0;
    IShellLinkW_SetPath(sl, target);
    IShellLinkW_SetWorkingDirectory(sl, workDir);
    IShellLinkW_SetIconLocation(sl, target, 0);
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf)))
    {
        ok = SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE));
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
    return ok;
}

static void regSetSz(HKEY k, const wchar_t *n, const wchar_t *v)
{ RegSetValueExW(k, n, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t))); }
static void regSetDw(HKEY k, const wchar_t *n, DWORD v)
{ RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE *)&v, sizeof(v)); }

/* ------------------------------------------------------------------ install -- */
static HWND  g_wnd;
#define WM_PROGRESS (WM_APP + 1)
#define WM_DONE     (WM_APP + 2)
#define WM_ASKRETRY (WM_APP + 3)

static int writeOut(const wchar_t *full, const void *p, size_t n)
{
    for (;;)
    {
        SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
        HANDLE f = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE)
        {
            DWORD wr = 0; SIZE_T off = 0; int ok = 1;
            while (off < n)
            {
                DWORD want = (DWORD)((n - off > (1u << 20)) ? (1u << 20) : (n - off));
                if (!WriteFile(f, (const BYTE *)p + off, want, &wr, NULL) || !wr) { ok = 0; break; }
                off += wr;
            }
            CloseHandle(f);
            if (ok) return 1;
        }
        /* typically: the app is running -> ask to close it and retry */
        wchar_t m[MAX_PATH + 256];
        _snwprintf_s(m, MAX_PATH + 256, _TRUNCATE,
                     TR("파일을 쓸 수 없습니다:\n%s\n\n프로그램이 실행 중이면 종료한 뒤 [다시 시도]를 누르세요.",
                        "Cannot write file:\n%s\n\nIf the program is running, close it and press Retry."), full);
        int ans = g_silent ? IDCANCEL
                           : (g_wnd ? (int)SendMessageW(g_wnd, WM_ASKRETRY, 0, (LPARAM)m)
                                    : MessageBoxW(NULL, m, g_cfg.name, MB_RETRYCANCEL | MB_ICONWARNING));
        if (ans != IDRETRY) { wcsncpy_s(g_err, 1024, m, _TRUNCATE); return 0; }
    }
}

static int doInstall(void)
{
    int ok = 0;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    wchar_t dir[MAX_PATH]; wcscpy_s(dir, MAX_PATH, g_installDir);
    size_t dl = wcslen(dir); while (dl > 3 && dir[dl-1] == L'\\') dir[--dl] = 0;
    makeDirs(dir, NULL);
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES)
    {
        _snwprintf_s(g_err, 1024, _TRUNCATE, TR("폴더를 만들 수 없습니다: %s", "Cannot create folder: %s"), dir);
        CoUninitialize(); return 0;
    }
    wchar_t logPath[MAX_PATH]; _snwprintf_s(logPath, MAX_PATH, _TRUNCATE, L"%s\\uninstall.log", dir);
    FILE *log = NULL; _wfopen_s(&log, logPath, L"w, ccs=UTF-16LE");
    if (!log) { _snwprintf_s(g_err, 1024, _TRUNCATE, TR("쓰기 권한이 없습니다: %s", "No write access: %s"), dir); CoUninitialize(); return 0; }

    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!g_payload || !mz_zip_reader_init_mem(&zip, g_payload, g_payloadSize, 0))
    { wcscpy_s(g_err, 1024, TR("설치 데이터가 손상되었습니다.", "The setup data is damaged.")); fclose(log); CoUninitialize(); return 0; }
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    unsigned long long total = 0, done = 0;
    for (mz_uint i = 0; i < n; i++) { mz_zip_archive_file_stat st; if (mz_zip_reader_file_stat(&zip, i, &st)) total += st.m_uncomp_size; }
    ok = 1;
    for (mz_uint i = 0; i < n && ok; i++)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        wchar_t rel[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, st.m_filename, -1, rel, MAX_PATH);
        for (wchar_t *p = rel; *p; p++) if (*p == L'/') *p = L'\\';
        if (wcsstr(rel, L"..") || wcschr(rel, L':')) continue;
        wchar_t full[MAX_PATH]; _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, rel);
        if (mz_zip_reader_is_file_a_directory(&zip, i)) { makeDirs(full, log); continue; }
        wchar_t parent[MAX_PATH]; wcscpy_s(parent, MAX_PATH, full); *wcsrchr(parent, L'\\') = 0;
        makeDirs(parent, log);
        size_t sz = 0; void *p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!p) { wcscpy_s(g_err, 1024, TR("압축 해제 실패", "Extraction failed")); ok = 0; break; }
        ok = writeOut(full, p, sz);
        mz_free(p);
        if (ok) fwprintf(log, L"F %s\n", full);
        done += st.m_uncomp_size;
        if (g_wnd) PostMessageW(g_wnd, WM_PROGRESS, (WPARAM)(total ? done * 90 / total : 90), 0);
    }
    mz_zip_reader_end(&zip);

    wchar_t exePath[MAX_PATH]; _snwprintf_s(exePath, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, g_cfg.exe);
    wchar_t unin[MAX_PATH]; _snwprintf_s(unin, MAX_PATH, _TRUNCATE, L"%s\\uninstall.exe", dir);
    if (ok)
    {   /* uninstaller = this exe without the payload */
        wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
        SetFileAttributesW(unin, FILE_ATTRIBUTE_NORMAL);
        if (CopyFileW(self, unin, FALSE))
        {
            HANDLE u = BeginUpdateResourceW(unin, FALSE);
            if (u)
            {
                WORD ln = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
                UpdateResourceW(u, RT_RCDATA_W, L">SETUP PAYLOAD<", ln, NULL, 0);
                UpdateResourceW(u, RT_RCDATA_W, L">SETUP LICENSE<", ln, NULL, 0);
                wchar_t cfg[2048]; cfgSerialize(cfg, 2048);   /* scope as installed, not "ask" */
                UpdateResourceW(u, RT_RCDATA_W, L">SETUP CFG<", ln, cfg, (DWORD)(wcslen(cfg) * sizeof(wchar_t)));
                EndUpdateResourceW(u, FALSE);
            }
        }
        if (g_wnd) PostMessageW(g_wnd, WM_PROGRESS, 94, 0);

        wchar_t f[MAX_PATH], lnk[MAX_PATH];
        if (g_cfg.startmenu)
        {
            folderPath(CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS, f);
            _snwprintf_s(lnk, MAX_PATH, _TRUNCATE, L"%s\\%s.lnk", f, g_cfg.name);
            if (createShortcut(lnk, exePath, dir)) fwprintf(log, L"S %s\n", lnk);
        }
        if (g_cfg.desktop)
        {
            folderPath(CSIDL_DESKTOPDIRECTORY, CSIDL_COMMON_DESKTOPDIRECTORY, f);
            _snwprintf_s(lnk, MAX_PATH, _TRUNCATE, L"%s\\%s.lnk", f, g_cfg.name);
            if (createShortcut(lnk, exePath, dir)) fwprintf(log, L"S %s\n", lnk);
        }

        HKEY root = g_cfg.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, k;
        wchar_t key[300]; _snwprintf_s(key, 300, _TRUNCATE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s", g_cfg.appid);
        if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS)
        {
            wchar_t v[MAX_PATH + 32];
            regSetSz(k, L"DisplayName", g_cfg.name);
            regSetSz(k, L"DisplayVersion", g_cfg.version);
            if (g_cfg.publisher[0]) regSetSz(k, L"Publisher", g_cfg.publisher);
            _snwprintf_s(v, MAX_PATH + 32, _TRUNCATE, L"%s,0", exePath); regSetSz(k, L"DisplayIcon", v);
            regSetSz(k, L"InstallLocation", dir);
            _snwprintf_s(v, MAX_PATH + 32, _TRUNCATE, L"\"%s\"", unin); regSetSz(k, L"UninstallString", v);
            _snwprintf_s(v, MAX_PATH + 32, _TRUNCATE, L"\"%s\" /S", unin); regSetSz(k, L"QuietUninstallString", v);
            regSetDw(k, L"EstimatedSize", g_cfg.sizekb);
            regSetDw(k, L"NoModify", 1); regSetDw(k, L"NoRepair", 1);
            RegCloseKey(k);
        }
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    }
    fclose(log);
    if (g_wnd) PostMessageW(g_wnd, WM_PROGRESS, 100, 0);
    CoUninitialize();
    return ok;
}

/* --------------------------------------------------------------- uninstall -- */
static int deleteWithRetry(const wchar_t *p)
{
    for (;;)
    {
        SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(p) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return 1;
        if (g_silent) return 0;
        wchar_t m[MAX_PATH + 200];
        _snwprintf_s(m, MAX_PATH + 200, _TRUNCATE,
                     TR("파일을 삭제할 수 없습니다:\n%s\n\n프로그램이 실행 중이면 종료한 뒤 [다시 시도]를 누르세요.",
                        "Cannot delete:\n%s\n\nIf the program is running, close it and press Retry."), p);
        if (MessageBoxW(NULL, m, g_cfg.name, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) return 0;
    }
}

static int doUninstall(const wchar_t *dir)
{
    wchar_t logPath[MAX_PATH]; _snwprintf_s(logPath, MAX_PATH, _TRUNCATE, L"%s\\uninstall.log", dir);
    FILE *log = NULL; _wfopen_s(&log, logPath, L"r, ccs=UTF-16LE");
    wchar_t *dirs[1024]; int nd = 0; int ok = 1;
    if (log)
    {
        wchar_t line[MAX_PATH + 8];
        while (fgetws(line, MAX_PATH + 8, log))
        {
            size_t l = wcslen(line); while (l && (line[l-1] == L'\n' || line[l-1] == L'\r')) line[--l] = 0;
            if (l < 3) continue;
            if (line[0] == L'F' || line[0] == L'S') { if (!deleteWithRetry(line + 2) && line[0] == L'F') ok = 0; }
            else if (line[0] == L'D' && nd < 1024) dirs[nd++] = _wcsdup(line + 2);
        }
        fclose(log);
    }
    if (!ok) { for (int i = 0; i < nd; i++) free(dirs[i]); return 0; }
    wchar_t p[MAX_PATH];
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\uninstall.exe", dir);
    SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(p);   /* may still be running (silent mode waits for us): finished by the delayed cleanup in wWinMain */
    DeleteFileW(logPath);
    for (int i = nd - 1; i >= 0; i--) { RemoveDirectoryW(dirs[i]); free(dirs[i]); }
    RemoveDirectoryW(dir);
    HKEY root = g_cfg.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    wchar_t key[300]; _snwprintf_s(key, 300, _TRUNCATE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s", g_cfg.appid);
    RegDeleteKeyW(root, key);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return 1;
}

/* ------------------------------------------------------------------ wizard -- */
enum { P_WELCOME, P_LICENSE, P_FOLDER, P_PROGRESS, P_FINISH, P_COUNT };
enum {
    ID_BACK = 100, ID_NEXT, ID_CANCEL, ID_TITLE, ID_SUB, ID_ICON, ID_TEXT,
    ID_LIC, ID_AGREE, ID_DISAGREE, ID_DIRLBL, ID_DIR, ID_BROWSE, ID_SPACE, ID_DESK, ID_START,
    ID_STATUS, ID_BAR, ID_RUN, ID_SCOPE_U, ID_SCOPE_M
};
static int g_autoInstall;   /* elevated relaunch: go straight to installing */
static int    g_page = P_WELCOME;
static double g_scale = 1.0;
static HFONT  g_font, g_bold, g_big;
static HBRUSH g_white;
static int    g_installOk;
static HWND   g_pages[P_COUNT][8]; static int g_npg[P_COUNT];
#define S(x) ((int)((x) * g_scale + 0.5))

static HWND mk(int page, const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | style, S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id, g_hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    if (page >= 0 && g_npg[page] < 8) g_pages[page][g_npg[page]++] = c;
    else ShowWindow(c, SW_SHOW);
    return c;
}

static void setHeader(const wchar_t *title, const wchar_t *sub)
{
    SetDlgItemTextW(g_wnd, ID_TITLE, title);
    SetDlgItemTextW(g_wnd, ID_SUB, sub);
}

static void updateSpace(void)
{
    wchar_t m[200];
    _snwprintf_s(m, 200, _TRUNCATE, TR("필요한 공간: %.1f MB", "Space required: %.1f MB"), g_cfg.sizekb / 1024.0);
    SetDlgItemTextW(g_wnd, ID_SPACE, m);
}

static void showPage(int p)
{
    if (p == P_LICENSE && !g_license) p = (g_page < P_LICENSE) ? P_FOLDER : P_WELCOME;
    for (int i = 0; i < P_COUNT; i++)
        for (int k = 0; k < g_npg[i]; k++) ShowWindow(g_pages[i][k], i == p ? SW_SHOW : SW_HIDE);
    g_page = p;
    wchar_t t[300];
    switch (p)
    {
    case P_WELCOME:
        _snwprintf_s(t, 300, _TRUNCATE, TR("%s 설치", "%s Setup"), g_cfg.name);
        setHeader(t, TR("설치 마법사에 오신 것을 환영합니다", "Welcome to the setup wizard"));
        break;
    case P_LICENSE: setHeader(TR("사용권 계약", "License Agreement"), TR("계속하기 전에 아래 내용을 읽어 주세요.", "Please read the following before continuing.")); break;
    case P_FOLDER:  setHeader(TR("설치 위치와 옵션", "Install location and options"), TR("설치할 폴더와 바로가기를 선택하세요.", "Choose the folder and shortcuts.")); updateSpace(); break;
    case P_PROGRESS:setHeader(TR("설치 중", "Installing"), TR("잠시 기다려 주세요...", "Please wait...")); break;
    case P_FINISH:
        setHeader(g_installOk ? TR("설치 완료", "Setup complete") : TR("설치 실패", "Setup failed"),
                  g_installOk ? TR("모든 작업이 끝났습니다.", "Everything is done.") : g_err);
        break;
    }
    EnableWindow(GetDlgItem(g_wnd, ID_BACK), p == P_LICENSE || p == P_FOLDER);
    EnableWindow(GetDlgItem(g_wnd, ID_CANCEL), p != P_PROGRESS && p != P_FINISH);
    EnableWindow(GetDlgItem(g_wnd, ID_NEXT), p != P_PROGRESS && (p != P_LICENSE || IsDlgButtonChecked(g_wnd, ID_AGREE)));
    SetDlgItemTextW(g_wnd, ID_NEXT, p == P_FOLDER ? TR("설치", "Install") : p == P_FINISH ? TR("마침", "Finish") : TR("다음 >", "Next >"));
    if (p == P_FINISH)
    {
        ShowWindow(GetDlgItem(g_wnd, ID_RUN), g_installOk ? SW_SHOW : SW_HIDE);
        if (g_installOk) SetDlgItemTextW(g_wnd, ID_TEXT, L"");
    }
}

static DWORD WINAPI installThread(LPVOID p)
{
    (void)p;
    int ok = doInstall();
    PostMessageW(g_wnd, WM_DONE, ok, 0);
    return 0;
}

static void launchApp(void)
{
    wchar_t exe[MAX_PATH]; _snwprintf_s(exe, MAX_PATH, _TRUNCATE, L"%s\\%s", g_installDir, g_cfg.exe);
    /* when elevated, start the app through Explorer so it runs as the normal user */
    if (isAdmin()) ShellExecuteW(NULL, L"open", L"explorer.exe", exe, NULL, SW_SHOWNORMAL);
    else ShellExecuteW(NULL, L"open", exe, NULL, g_installDir, SW_SHOWNORMAL);
}

static void browseFolder(void)
{
    BROWSEINFOW bi; memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_wnd; bi.lpszTitle = TR("설치할 폴더를 선택하세요", "Choose the install folder");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t p[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, p))
    {   /* append the app folder name unless the user picked it already */
        const wchar_t *leaf = wcsrchr(p, L'\\'); leaf = leaf ? leaf + 1 : p;
        const wchar_t *want = g_cfg.dir[0] ? g_cfg.dir : g_cfg.name;
        if (_wcsicmp(leaf, want))
        {
            size_t l = wcslen(p); if (l && p[l-1] != L'\\') wcscat_s(p, MAX_PATH, L"\\");
            wcscat_s(p, MAX_PATH, want);
        }
        SetDlgItemTextW(g_wnd, ID_DIR, p);
    }
    CoTaskMemFree(pidl);
}

static void buildUi(void)
{
    /* header band */
    mk(-1, L"STATIC", L"", SS_LEFT, 0, 20, 14, 400, 22, ID_TITLE, g_bold);
    mk(-1, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, 32, 38, 400, 20, ID_SUB, NULL);
    HWND ic = mk(-1, L"STATIC", L"", SS_ICON | SS_CENTERIMAGE, 0, 446, 12, 48, 48, ID_ICON, NULL);
    HICON hi = (HICON)LoadImageW(g_hi, MAKEINTRESOURCEW(1), IMAGE_ICON, S(48), S(48), 0);
    SendMessageW(ic, STM_SETICON, (WPARAM)hi, 0);
    mk(-1, L"STATIC", L"", SS_ETCHEDHORZ, 0, 0, 70, 512, 2, -1, NULL);
    mk(-1, L"STATIC", L"", SS_ETCHEDHORZ, 0, 0, 316, 512, 2, -1, NULL);
    mk(-1, L"BUTTON", TR("< 뒤로", "< Back"), WS_TABSTOP | BS_PUSHBUTTON, 0, 222, 330, 88, 28, ID_BACK, NULL);
    mk(-1, L"BUTTON", TR("다음 >", "Next >"), WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 314, 330, 88, 28, ID_NEXT, NULL);
    mk(-1, L"BUTTON", TR("취소", "Cancel"), WS_TABSTOP | BS_PUSHBUTTON, 0, 410, 330, 88, 28, ID_CANCEL, NULL);

    wchar_t t[600];
    /* welcome */
    wchar_t pub[200] = L"";
    if (g_cfg.publisher[0]) _snwprintf_s(pub, 200, _TRUNCATE, TR("게시자: %s\n\n", "Publisher: %s\n\n"), g_cfg.publisher);
    _snwprintf_s(t, 600, _TRUNCATE,
        TR("%s %s 을(를) 이 컴퓨터에 설치합니다.\n\n%s계속하려면 [다음]을 누르세요.",
           "This will install %s %s on your computer.\n\n%sClick Next to continue."),
        g_cfg.name, g_cfg.version, pub);
    mk(P_WELCOME, L"STATIC", t, SS_LEFT, 0, 32, 96, 448, 200, ID_TEXT, g_big);
    /* license */
    mk(P_LICENSE, L"EDIT", g_license ? g_license : L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 84, 472, 170, ID_LIC, NULL);
    mk(P_LICENSE, L"BUTTON", TR("동의합니다", "I accept the agreement"), BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 0, 22, 262, 400, 22, ID_AGREE, NULL);
    mk(P_LICENSE, L"BUTTON", TR("동의하지 않습니다", "I do not accept the agreement"), BS_AUTORADIOBUTTON, 0, 22, 286, 400, 22, ID_DISAGREE, NULL);
    CheckDlgButton(g_wnd, ID_DISAGREE, BST_CHECKED);
    /* folder */
    mk(P_FOLDER, L"STATIC", TR("설치 폴더:", "Destination folder:"), SS_LEFT, 0, 22, 90, 460, 20, ID_DIRLBL, NULL);
    mk(P_FOLDER, L"EDIT", g_installDir, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 22, 114, 366, 26, ID_DIR, NULL);
    mk(P_FOLDER, L"BUTTON", TR("찾아보기...", "Browse..."), BS_PUSHBUTTON | WS_TABSTOP, 0, 396, 113, 96, 28, ID_BROWSE, NULL);
    mk(P_FOLDER, L"STATIC", L"", SS_LEFT, 0, 22, 148, 460, 20, ID_SPACE, NULL);
    mk(P_FOLDER, L"BUTTON", TR("바탕화면에 바로가기 만들기", "Create a desktop shortcut"), BS_AUTOCHECKBOX | WS_TABSTOP, 0, 22, 196, 460, 24, ID_DESK, NULL);
    mk(P_FOLDER, L"BUTTON", TR("시작 메뉴에 바로가기 만들기", "Create a Start menu shortcut"), BS_AUTOCHECKBOX | WS_TABSTOP, 0, 22, 224, 460, 24, ID_START, NULL);
    CheckDlgButton(g_wnd, ID_DESK, g_cfg.desktop ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_wnd, ID_START, g_cfg.startmenu ? BST_CHECKED : BST_UNCHECKED);
    if (g_cfg.ask)
    {
        mk(P_FOLDER, L"BUTTON", TR("나만 사용 (현재 사용자, 관리자 권한 불필요)", "Install for me only (no administrator rights)"),
           BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 0, 22, 256, 470, 22, ID_SCOPE_U, NULL);
        mk(P_FOLDER, L"BUTTON", TR("이 PC의 모든 사용자 (관리자 권한 필요)", "Install for all users (administrator rights required)"),
           BS_AUTORADIOBUTTON, 0, 22, 280, 470, 22, ID_SCOPE_M, NULL);
        CheckDlgButton(g_wnd, g_cfg.machine ? ID_SCOPE_M : ID_SCOPE_U, BST_CHECKED);
    }
    /* progress */
    mk(P_PROGRESS, L"STATIC", TR("파일을 복사하는 중...", "Copying files..."), SS_LEFT, 0, 22, 120, 460, 20, ID_STATUS, NULL);
    mk(P_PROGRESS, PROGRESS_CLASSW, L"", 0, 0, 22, 148, 470, 22, ID_BAR, NULL);
    /* finish */
    _snwprintf_s(t, 600, _TRUNCATE, TR("%s 실행", "Launch %s"), g_cfg.name);
    mk(P_FINISH, L"BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 110, 440, 24, ID_RUN, g_big);
    CheckDlgButton(g_wnd, ID_RUN, g_cfg.run ? BST_CHECKED : BST_UNCHECKED);
    showPage(P_WELCOME);
}

static int confirmCancel(void)
{
    return MessageBoxW(g_wnd, TR("설치를 취소하시겠습니까?", "Cancel the installation?"), g_cfg.name,
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        g_wnd = h; buildUi();
        if (g_autoInstall) { showPage(P_PROGRESS); CloseHandle(CreateThread(NULL, 0, installThread, NULL, 0, NULL)); }
        return 0;
    case WM_CTLCOLORSTATIC:
    {
        HWND c = (HWND)lp; int id = GetDlgCtrlID(c);
        if (id == ID_TITLE || id == ID_SUB || id == ID_ICON)
        { SetBkColor((HDC)wp, RGB(255, 255, 255)); return (LRESULT)g_white; }
        break;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r); r.bottom = S(70);
        FillRect(dc, &r, g_white);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ASKRETRY:
        return MessageBoxW(h, (const wchar_t *)lp, g_cfg.name, MB_RETRYCANCEL | MB_ICONWARNING);
    case WM_PROGRESS:
        SendDlgItemMessageW(h, ID_BAR, PBM_SETPOS, wp, 0);
        return 0;
    case WM_DONE:
        g_installOk = (int)wp;
        showPage(P_FINISH);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_AGREE: case ID_DISAGREE:
            EnableWindow(GetDlgItem(h, ID_NEXT), IsDlgButtonChecked(h, ID_AGREE));
            break;
        case ID_BROWSE: browseFolder(); break;
        case ID_SCOPE_U: case ID_SCOPE_M:
            g_cfg.machine = IsDlgButtonChecked(h, ID_SCOPE_M) == BST_CHECKED;
            defaultInstallDir(g_installDir, MAX_PATH);
            SetDlgItemTextW(h, ID_DIR, g_installDir);
            break;
        case ID_BACK:
            if (g_page == P_FOLDER) showPage(g_license ? P_LICENSE : P_WELCOME);
            else if (g_page == P_LICENSE) showPage(P_WELCOME);
            break;
        case ID_NEXT:
            if (g_page == P_WELCOME) showPage(P_LICENSE);
            else if (g_page == P_LICENSE) showPage(P_FOLDER);
            else if (g_page == P_FOLDER)
            {
                GetDlgItemTextW(h, ID_DIR, g_installDir, MAX_PATH);
                if (!g_installDir[0] || g_installDir[1] != L':')
                { MessageBoxW(h, TR("올바른 폴더 경로를 입력하세요.", "Enter a valid folder path."), g_cfg.name, MB_ICONWARNING); break; }
                g_cfg.desktop = IsDlgButtonChecked(h, ID_DESK) == BST_CHECKED;
                g_cfg.startmenu = IsDlgButtonChecked(h, ID_START) == BST_CHECKED;
                if (g_cfg.machine && !isAdmin())
                {   /* all users chosen: continue in an elevated copy that installs right away */
                    wchar_t self[MAX_PATH], args[MAX_PATH + 96];
                    GetModuleFileNameW(NULL, self, MAX_PATH);
                    _snwprintf_s(args, MAX_PATH + 96, _TRUNCATE, L"/elevated-install /ALLUSERS \"/D=%s\"%s%s",
                                 g_installDir, g_cfg.desktop ? L"" : L" /NODESKTOP", g_cfg.startmenu ? L"" : L" /NOSTARTMENU");
                    SHELLEXECUTEINFOW se; memset(&se, 0, sizeof(se));
                    se.cbSize = sizeof(se); se.hwnd = h; se.lpVerb = L"runas"; se.lpFile = self; se.lpParameters = args; se.nShow = SW_SHOWNORMAL;
                    if (ShellExecuteExW(&se)) { g_page = P_FINISH; DestroyWindow(h); }
                    else MessageBoxW(h, TR("모든 사용자용 설치에는 관리자 권한이 필요합니다.\n'나만 사용'을 고르거나 권한 요청을 허용하세요.",
                                           "Installing for all users requires administrator rights.\nChoose 'for me only' or allow the prompt."),
                                     g_cfg.name, MB_ICONWARNING);
                    break;
                }
                showPage(P_PROGRESS);
                CloseHandle(CreateThread(NULL, 0, installThread, NULL, 0, NULL));
            }
            else if (g_page == P_FINISH)
            {
                if (g_installOk && IsDlgButtonChecked(h, ID_RUN) == BST_CHECKED) launchApp();
                DestroyWindow(h);
            }
            break;
        case ID_CANCEL: case IDCANCEL:
            if (g_page == P_FINISH) DestroyWindow(h);
            else if (g_page != P_PROGRESS && confirmCancel()) DestroyWindow(h);
            break;
        }
        return 0;
    case WM_CLOSE:
        if (g_page == P_PROGRESS) return 0;
        if (g_page == P_FINISH || confirmCancel()) DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int runWizard(void)
{
    SetProcessDPIAware();
    HDC dc = GetDC(NULL); g_scale = GetDeviceCaps(dc, LOGPIXELSY) / 96.0; ReleaseDC(NULL, dc);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    const wchar_t *face = g_ko ? L"Malgun Gothic" : L"Segoe UI";
    g_font = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    g_bold = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    g_big  = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    g_white = CreateSolidBrush(RGB(255, 255, 255));

    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = wndProc; wc.hInstance = g_hi;
    wc.hIcon = LoadIconW(g_hi, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AhkWebSetupWnd";
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, S(512), S(372) };
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_CONTROLPARENT);
    wchar_t title[200]; _snwprintf_s(title, 200, _TRUNCATE, TR("%s 설치", "%s Setup"), g_cfg.name);
    int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    HWND wnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, title, style,
                               (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2,
                               w, hgt, NULL, NULL, g_hi, NULL);
    if (!wnd) return 1;
    ShowWindow(wnd, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
        if (!IsDialogMessageW(wnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    return g_installOk ? 0 : 1;
}

/* -------------------------------------------------------------------- main -- */
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, LPWSTR cmdLine, int show)
{
    (void)prev; (void)show;
    g_hi = hi;
    g_ko = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int uninstall = 0, uninstallRun = 0; wchar_t runDir[MAX_PATH] = L"", dirArg[MAX_PATH] = L"";
    int forceMachine = 0, forceUser = 0, noDesk = 0, noStart = 0;
    for (int i = 1; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"/S") || !_wcsicmp(argv[i], L"/SILENT") || !_wcsicmp(argv[i], L"/VERYSILENT")) g_silent = 1;
        else if (!_wcsicmp(argv[i], L"/ALLUSERS")) forceMachine = 1;
        else if (!_wcsicmp(argv[i], L"/CURRENTUSER")) forceUser = 1;
        else if (!_wcsicmp(argv[i], L"/elevated-install")) g_autoInstall = 1;
        else if (!_wcsicmp(argv[i], L"/NODESKTOP")) noDesk = 1;
        else if (!_wcsicmp(argv[i], L"/NOSTARTMENU")) noStart = 1;
        else if (!_wcsicmp(argv[i], L"/uninstall")) uninstall = 1;
        else if (!_wcsicmp(argv[i], L"/uninstall-run") && i + 1 < argc) { uninstallRun = 1; wcsncpy_s(runDir, MAX_PATH, argv[++i], _TRUNCATE); }
        else if (!_wcsnicmp(argv[i], L"/D=", 3)) wcsncpy_s(dirArg, MAX_PATH, argv[i] + 3, _TRUNCATE);
        else if (!_wcsnicmp(argv[i], L"/DIR=", 5)) wcsncpy_s(dirArg, MAX_PATH, argv[i] + 5, _TRUNCATE);
    }
    (void)cmdLine;
    if (!loadCfg())
    {
        MessageBoxW(NULL, TR("설치 정보가 없습니다. AhkWeb2Exe로 만든 설치 파일이 아닙니다.",
                             "No setup data. This file was not built by AhkWeb2Exe."), L"Setup", MB_ICONERROR);
        return 1;
    }

    /* uninstall.exe (no payload): either the user ran it, or it is the temp copy */
    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!g_payload) uninstall = uninstall || !uninstallRun;
    if (uninstall || uninstallRun)
    {
        if (g_cfg.machine && !isAdmin())
        { if (!relaunchElevated(NULL) && !g_silent) MessageBoxW(NULL, TR("관리자 권한이 필요합니다.", "Administrator rights are required."), g_cfg.name, MB_ICONERROR); return 1; }
        if (!uninstallRun)
        {
            wchar_t m[300]; _snwprintf_s(m, 300, _TRUNCATE, TR("%s 을(를) 제거하시겠습니까?", "Remove %s?"), g_cfg.name);
            if (!g_silent && MessageBoxW(NULL, m, g_cfg.name, MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;
            /* continue from a temp copy so uninstall.exe itself can be deleted */
            wchar_t dir[MAX_PATH]; wcscpy_s(dir, MAX_PATH, self); *wcsrchr(dir, L'\\') = 0;
            wchar_t tmp[MAX_PATH], tdir[MAX_PATH]; GetTempPathW(MAX_PATH, tdir);
            _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%sAhkWebUninst_%lu.exe", tdir, GetCurrentProcessId());
            if (CopyFileW(self, tmp, FALSE))
            {
                wchar_t args[MAX_PATH + 64]; _snwprintf_s(args, MAX_PATH + 64, _TRUNCATE, L"/uninstall-run \"%s\"%s", dir, g_silent ? L" /S" : L"");
                SHELLEXECUTEINFOW se; memset(&se, 0, sizeof(se));
                se.cbSize = sizeof(se); se.fMask = SEE_MASK_NOCLOSEPROCESS; se.lpFile = tmp; se.lpParameters = args; se.nShow = SW_SHOWNORMAL;
                if (ShellExecuteExW(&se))
                {
                    if (g_silent && se.hProcess) WaitForSingleObject(se.hProcess, INFINITE);
                    if (se.hProcess) CloseHandle(se.hProcess);
                    return 0;
                }
            }
            wcscpy_s(runDir, MAX_PATH, dir);   /* fallback: in place */
        }
        Sleep(400);   /* let the launching uninstall.exe exit */
        int ok = doUninstall(runDir);
        if (!g_silent)
        {
            wchar_t m[300];
            _snwprintf_s(m, 300, _TRUNCATE, ok ? TR("%s 이(가) 제거되었습니다.", "%s was removed.")
                                                : TR("%s 을(를) 완전히 제거하지 못했습니다.", "%s could not be removed completely."), g_cfg.name);
            MessageBoxW(NULL, m, g_cfg.name, ok ? MB_ICONINFORMATION : MB_ICONWARNING);
        }
        /* Shortly after exit: remove uninstall.exe (its process may still be waiting on us),
         * the now-empty install folder, and this temp copy. */
        {
            wchar_t cmd[MAX_PATH * 3 + 160];
            _snwprintf_s(cmd, MAX_PATH * 3 + 160, _TRUNCATE,
                         L"/c ping 127.0.0.1 -n 3 >nul & del /f /q \"%s\\uninstall.exe\" & rd \"%s\" 2>nul%s%s%s",
                         runDir, runDir, uninstallRun ? L" & del /f /q \"" : L"", uninstallRun ? self : L"", uninstallRun ? L"\"" : L"");
            ShellExecuteW(NULL, L"open", L"cmd.exe", cmd, NULL, SW_HIDE);
        }
        return ok ? 0 : 1;
    }

    /* install */
    if (noDesk) g_cfg.desktop = 0;
    if (noStart) g_cfg.startmenu = 0;
    if (g_cfg.ask)
    {   /* user-chosen scope: flags win, otherwise follow an existing install (default: current user) */
        if (forceMachine) g_cfg.machine = 1;
        else if (forceUser) g_cfg.machine = 0;
        else g_cfg.machine = uninstallKeyExists(HKEY_LOCAL_MACHINE) && !uninstallKeyExists(HKEY_CURRENT_USER);
    }
    else if (forceMachine || forceUser)
        g_cfg.machine = forceMachine;
    /* fixed all-users (or silent /ALLUSERS): elevate now; in "ask" mode the wizard elevates on Install */
    if (g_cfg.machine && !isAdmin() && (!g_cfg.ask || g_silent))
    {
        if (relaunchElevated(NULL)) return 0;
        if (!g_silent) MessageBoxW(NULL, TR("모든 사용자용 설치에는 관리자 권한이 필요합니다.", "Installing for all users requires administrator rights."), g_cfg.name, MB_ICONERROR);
        return 1;
    }
    if (dirArg[0]) wcsncpy_s(g_installDir, MAX_PATH, dirArg, _TRUNCATE);
    else defaultInstallDir(g_installDir, MAX_PATH);

    if (g_silent)
    {
        int ok = doInstall();
        return ok ? 0 : 1;
    }
    return runWizard();
}

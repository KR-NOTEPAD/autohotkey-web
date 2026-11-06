/* ============================================================================
 *  AhkWeb2Exe - GUI (Ahk2Exe-style window, tabbed)
 *    기본 | 버전 정보 | 파일 포함 | 설치 마법사 | 고급
 *  Per-script settings, recent scripts and preferences live under
 *  HKCU\Software\AutoHotkeyWeb\AhkWeb2Exe (signing password DPAPI-encrypted).
 * ============================================================================ */
#include "a2e.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    T_BASIC, T_VERSION, T_FILES, T_SETUP, T_ADV, T_COUNT
};
enum {
    ID_TAB = 100, ID_STATUS, ID_CONVERT, ID_CLOSE,
    /* basic */
    ID_SRC, ID_SRC_BR, ID_OUT, ID_OUT_BR, ID_ICO, ID_ICO_BR, ID_ICO_DEF, ID_PREVIEW, ID_BIN, ID_BIN_BR,
    ID_MODE_EXE, ID_MODE_SETUP, ID_RUNAFTER, ID_OPENDIR, ID_ADMIN,
    /* version */
    ID_DESC, ID_VER, ID_PROD, ID_COMP, ID_COPY, ID_COMMENTS, ID_ORIG,
    /* files */
    ID_LIST, ID_ADDFILE, ID_ADDDIR, ID_REMOVE, ID_DEST, ID_DEST_SET, ID_EXTRACT, ID_EXMODE, ID_KEEP,
    /* setup */
    ID_APPNAME, ID_PUBLISHER, ID_INSTDIR, ID_SCOPE_USER, ID_SCOPE_ALL, ID_LICENSE, ID_LICENSE_BR,
    ID_SC_DESK, ID_SC_START, ID_SC_RUN, ID_SETUPOUT, ID_SETUPOUT_BR,
    /* advanced */
    ID_SUSP, ID_SUSP_BR, ID_PAUSE, ID_PAUSE_BR, ID_PSUSP, ID_PSUSP_BR,
    ID_SIGN, ID_PFX, ID_PFX_BR, ID_PFXPASS, ID_SUBJECT, ID_TS
};

static const wchar_t *REG_KEY = L"Software\\AutoHotkeyWeb\\AhkWeb2Exe";
static HWND   g_wnd, g_tab, g_list;
static HFONT  g_font;
static HICON  g_preview;
static double g_scale = 1.0;
static int    g_outAuto = 1, g_settingOut = 0, g_busy = 0, g_curTab = 0;
static HWND   g_tabCtl[T_COUNT][48]; static int g_ntab[T_COUNT];
static wchar_t g_loadedFor[MAX_PATH];
#define S(x) ((int)((x) * g_scale + 0.5))
#define WM_COMPILED (WM_APP + 10)

/* ---------------------------------------------------------------- helpers -- */
static void getText(int id, wchar_t *buf, int cch) { GetDlgItemTextW(g_wnd, id, buf, cch); trimW(buf); }
static int  checked(int id) { return IsDlgButtonChecked(g_wnd, id) == BST_CHECKED; }
static void check(int id, int on) { CheckDlgButton(g_wnd, id, on ? BST_CHECKED : BST_UNCHECKED); }
static void setStatus(const wchar_t *s) { SetDlgItemTextW(g_wnd, ID_STATUS, s); }

static HWND mk(int tab, const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id)
{
    DWORD ex = (!wcscmp(cls, L"EDIT") || !wcscmp(cls, WC_LISTVIEWW)) ? WS_EX_CLIENTEDGE : 0;
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | style | (tab <= 0 ? WS_VISIBLE : 0),
                             S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (tab >= 0 && g_ntab[tab] < 48) g_tabCtl[tab][g_ntab[tab]++] = c;
    SetWindowPos(c, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    return c;
}
/* tab content area starts at (24, 44) */
#define LBL(t, s, x, y, w)     mk(t, L"STATIC", s, SS_LEFT, x, (y) + 4, w, 20, -1)
#define EDT(t, id, x, y, w)    mk(t, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 24, id)
#define BTN(t, s, id, x, y, w) mk(t, L"BUTTON", s, WS_TABSTOP | BS_PUSHBUTTON, x, (y) - 1, w, 26, id)
#define CHK(t, s, id, x, y, w) mk(t, L"BUTTON", s, WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, 22, id)
#define RAD(t, s, id, x, y, w, grp) mk(t, L"BUTTON", s, WS_TABSTOP | BS_AUTORADIOBUTTON | ((grp) ? WS_GROUP : 0), x, y, w, 22, id)

static void showTab(int t)
{
    for (int i = 0; i < T_COUNT; i++)
        for (int k = 0; k < g_ntab[i]; k++) ShowWindow(g_tabCtl[i][k], i == t ? SW_SHOW : SW_HIDE);
    g_curTab = t;
}

/* ------------------------------------------------------------- registry -- */
static void regGetIn(const wchar_t *sub, const wchar_t *name, wchar_t *buf, DWORD cch)
{
    wchar_t key[300]; _snwprintf_s(key, 300, _TRUNCATE, sub ? L"%s\\%s" : L"%s", REG_KEY, sub);
    DWORD sz = cch * sizeof(wchar_t); buf[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS) buf[0] = 0;
}
static void regSetIn(const wchar_t *sub, const wchar_t *name, const wchar_t *val)
{
    wchar_t key[300]; _snwprintf_s(key, 300, _TRUNCATE, sub ? L"%s\\%s" : L"%s", REG_KEY, sub);
    RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
}
static void regGet(const wchar_t *n, wchar_t *b, DWORD c) { regGetIn(NULL, n, b, c); }
static void regSet(const wchar_t *n, const wchar_t *v) { regSetIn(NULL, n, v); }

/* per-script key: Scripts\<FNV-1a of the lower-cased full path> */
static void scriptKey(const wchar_t *path, wchar_t *out, size_t cch)
{
    unsigned long long h = 1469598103934665603ULL;
    for (const wchar_t *p = path; *p; p++) { h ^= (unsigned)towlower(*p); h *= 1099511628211ULL; }
    _snwprintf_s(out, cch, _TRUNCATE, L"Scripts\\%016llx", h);
}

/* Controls whose text / check state is remembered per script. */
static const int PER_SCRIPT_TEXT[] = {
    ID_OUT, ID_ICO, ID_DESC, ID_VER, ID_PROD, ID_COMP, ID_COPY, ID_COMMENTS, ID_ORIG,
    ID_APPNAME, ID_PUBLISHER, ID_INSTDIR, ID_LICENSE, ID_SETUPOUT,
    ID_SUSP, ID_PAUSE, ID_PSUSP, ID_PFX, ID_SUBJECT, ID_TS
};
static const int PER_SCRIPT_CHECK[] = {
    ID_MODE_SETUP, ID_ADMIN, ID_KEEP, ID_SCOPE_ALL, ID_SC_DESK, ID_SC_START, ID_SC_RUN, ID_SIGN
};
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

static void listClear(void) { ListView_DeleteAllItems(g_list); }
static void listAdd(const wchar_t *src, const wchar_t *dest)
{
    LVITEMW it; memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT; it.iItem = ListView_GetItemCount(g_list); it.pszText = (wchar_t *)src;
    int i = ListView_InsertItem(g_list, &it);
    ListView_SetItemText(g_list, i, 1, (wchar_t *)(dest[0] ? dest : L"(이름 그대로)"));
}
static void listGet(int i, wchar_t *src, wchar_t *dest)
{
    ListView_GetItemText(g_list, i, 0, src, MAX_PATH);
    ListView_GetItemText(g_list, i, 1, dest, MAX_PATH);
    if (dest[0] == L'(') dest[0] = 0;
}

static void savePassword(const wchar_t *sub, const wchar_t *pass)
{
    wchar_t key[300]; _snwprintf_s(key, 300, _TRUNCATE, L"%s\\%s", REG_KEY, sub);
    if (!pass[0]) { RegDeleteKeyValueW(HKEY_CURRENT_USER, key, L"pfxpass"); return; }
    DATA_BLOB in = { (DWORD)((wcslen(pass) + 1) * sizeof(wchar_t)), (BYTE *)pass }, out;
    if (CryptProtectData(&in, L"AhkWeb2Exe", NULL, NULL, NULL, 0, &out))
    {
        RegSetKeyValueW(HKEY_CURRENT_USER, key, L"pfxpass", REG_BINARY, out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
}
static void loadPassword(const wchar_t *sub, wchar_t *pass, DWORD cch)
{
    wchar_t key[300]; _snwprintf_s(key, 300, _TRUNCATE, L"%s\\%s", REG_KEY, sub);
    BYTE buf[2048]; DWORD sz = sizeof(buf); pass[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"pfxpass", RRF_RT_REG_BINARY, NULL, buf, &sz) != ERROR_SUCCESS) return;
    DATA_BLOB in = { sz, buf }, out;
    if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out))
    {
        wcsncpy_s(pass, cch, (const wchar_t *)out.pbData, _TRUNCATE);
        SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData);
    }
}

static void saveScriptSettings(const wchar_t *script)
{
    wchar_t sub[64], v[1024], n[16]; scriptKey(script, sub, 64);
    regSetIn(sub, L"path", script);
    for (size_t i = 0; i < NELEM(PER_SCRIPT_TEXT); i++)
    { getText(PER_SCRIPT_TEXT[i], v, 1024); _snwprintf_s(n, 16, _TRUNCATE, L"t%d", PER_SCRIPT_TEXT[i]); regSetIn(sub, n, v); }
    for (size_t i = 0; i < NELEM(PER_SCRIPT_CHECK); i++)
    { _snwprintf_s(n, 16, _TRUNCATE, L"c%d", PER_SCRIPT_CHECK[i]); regSetIn(sub, n, checked(PER_SCRIPT_CHECK[i]) ? L"1" : L"0"); }
    _snwprintf_s(v, 1024, _TRUNCATE, L"%d", (int)SendDlgItemMessageW(g_wnd, ID_EXTRACT, CB_GETCURSEL, 0, 0)); regSetIn(sub, L"extract", v);
    _snwprintf_s(v, 1024, _TRUNCATE, L"%d", (int)SendDlgItemMessageW(g_wnd, ID_EXMODE, CB_GETCURSEL, 0, 0)); regSetIn(sub, L"exmode", v);
    /* include list as REG_MULTI_SZ "src|dest" */
    int cnt = ListView_GetItemCount(g_list);
    size_t cap = (size_t)(cnt + 1) * (MAX_PATH * 2 + 2) + 2, k = 0;
    wchar_t *ms = (wchar_t *)calloc(cap, sizeof(wchar_t));
    if (ms)
    {
        for (int i = 0; i < cnt; i++)
        {
            wchar_t s[MAX_PATH], d[MAX_PATH]; listGet(i, s, d);
            k += _snwprintf_s(ms + k, cap - k, _TRUNCATE, L"%s|%s", s, d) + 1;
        }
        ms[k++] = 0;
        wchar_t key[300]; _snwprintf_s(key, 300, _TRUNCATE, L"%s\\%s", REG_KEY, sub);
        RegSetKeyValueW(HKEY_CURRENT_USER, key, L"inc", REG_MULTI_SZ, ms, (DWORD)(k * sizeof(wchar_t)));
        free(ms);
    }
    wchar_t pass[128]; GetDlgItemTextW(g_wnd, ID_PFXPASS, pass, 128);
    savePassword(sub, pass);
    SecureZeroMemory(pass, sizeof(pass));
}

static void setDefaultsForNewScript(void)
{
    static const int clear[] = { ID_OUT, ID_DESC, ID_VER, ID_PROD, ID_COMP, ID_COPY, ID_COMMENTS, ID_ORIG,
                                 ID_APPNAME, ID_PUBLISHER, ID_INSTDIR, ID_LICENSE, ID_SETUPOUT,
                                 ID_SUSP, ID_PAUSE, ID_PSUSP, ID_PFX, ID_PFXPASS, ID_SUBJECT };
    for (size_t i = 0; i < NELEM(clear); i++) SetDlgItemTextW(g_wnd, clear[i], L"");
    wchar_t ico[MAX_PATH]; regGet(L"LastIcon", ico, MAX_PATH);
    SetDlgItemTextW(g_wnd, ID_ICO, fileExists(ico) ? ico : L"");
    SetDlgItemTextW(g_wnd, ID_TS, L"http://timestamp.digicert.com");
    check(ID_MODE_EXE, 1); check(ID_MODE_SETUP, 0); check(ID_ADMIN, 0); check(ID_KEEP, 0);
    check(ID_SCOPE_USER, 1); check(ID_SCOPE_ALL, 0); check(ID_SC_DESK, 1); check(ID_SC_START, 1); check(ID_SC_RUN, 1);
    check(ID_SIGN, 0);
    SendDlgItemMessageW(g_wnd, ID_EXTRACT, CB_SETCURSEL, 0, 0);
    SendDlgItemMessageW(g_wnd, ID_EXMODE, CB_SETCURSEL, 1, 0);
    listClear();
    g_outAuto = 1;
}

/* Load remembered settings for this script (or defaults). Returns 1 if found. */
static int loadScriptSettings(const wchar_t *script)
{
    wchar_t sub[64], v[1024], n[16]; scriptKey(script, sub, 64);
    regGetIn(sub, L"path", v, 1024);
    setDefaultsForNewScript();
    if (_wcsicmp(v, script)) return 0;
    for (size_t i = 0; i < NELEM(PER_SCRIPT_TEXT); i++)
    {
        _snwprintf_s(n, 16, _TRUNCATE, L"t%d", PER_SCRIPT_TEXT[i]); regGetIn(sub, n, v, 1024);
        if (PER_SCRIPT_TEXT[i] == ID_OUT) { g_settingOut = 1; SetDlgItemTextW(g_wnd, ID_OUT, v); g_settingOut = 0; g_outAuto = !v[0]; }
        else SetDlgItemTextW(g_wnd, PER_SCRIPT_TEXT[i], v);
    }
    for (size_t i = 0; i < NELEM(PER_SCRIPT_CHECK); i++)
    {
        _snwprintf_s(n, 16, _TRUNCATE, L"c%d", PER_SCRIPT_CHECK[i]); regGetIn(sub, n, v, 1024);
        check(PER_SCRIPT_CHECK[i], v[0] == L'1');
    }
    check(ID_MODE_EXE, !checked(ID_MODE_SETUP));
    check(ID_SCOPE_USER, !checked(ID_SCOPE_ALL));
    regGetIn(sub, L"extract", v, 1024); if (v[0]) SendDlgItemMessageW(g_wnd, ID_EXTRACT, CB_SETCURSEL, _wtoi(v), 0);
    regGetIn(sub, L"exmode", v, 1024);  if (v[0]) SendDlgItemMessageW(g_wnd, ID_EXMODE, CB_SETCURSEL, _wtoi(v), 0);
    wchar_t key[300]; _snwprintf_s(key, 300, _TRUNCATE, L"%s\\%s", REG_KEY, sub);
    DWORD sz = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"inc", RRF_RT_REG_MULTI_SZ, NULL, NULL, &sz) == ERROR_SUCCESS && sz)
    {
        wchar_t *ms = (wchar_t *)malloc(sz + 4);
        if (ms && RegGetValueW(HKEY_CURRENT_USER, key, L"inc", RRF_RT_REG_MULTI_SZ, NULL, ms, &sz) == ERROR_SUCCESS)
            for (wchar_t *p = ms; *p; p += wcslen(p) + 1)
            {
                wchar_t s[MAX_PATH * 2]; wcsncpy_s(s, MAX_PATH * 2, p, _TRUNCATE);
                wchar_t *bar = wcschr(s, L'|'); const wchar_t *d = L"";
                if (bar) { *bar = 0; d = bar + 1; }
                listAdd(s, d);
            }
        free(ms);
    }
    wchar_t pass[128]; loadPassword(sub, pass, 128);
    SetDlgItemTextW(g_wnd, ID_PFXPASS, pass);
    SecureZeroMemory(pass, sizeof(pass));
    return 1;
}

/* recent scripts (MRU, max 10) */
static void mruLoad(void)
{
    SendDlgItemMessageW(g_wnd, ID_SRC, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 10; i++)
    {
        wchar_t n[8], v[MAX_PATH]; _snwprintf_s(n, 8, _TRUNCATE, L"MRU%d", i); regGet(n, v, MAX_PATH);
        if (v[0]) SendDlgItemMessageW(g_wnd, ID_SRC, CB_ADDSTRING, 0, (LPARAM)v);
    }
}
static void mruPush(const wchar_t *path)
{
    wchar_t items[10][MAX_PATH]; int n = 0;
    wcscpy_s(items[n++], MAX_PATH, path);
    for (int i = 0; i < 10 && n < 10; i++)
    {
        wchar_t k[8], v[MAX_PATH]; _snwprintf_s(k, 8, _TRUNCATE, L"MRU%d", i); regGet(k, v, MAX_PATH);
        if (v[0] && _wcsicmp(v, path)) wcscpy_s(items[n++], MAX_PATH, v);
    }
    for (int i = 0; i < 10; i++)
    { wchar_t k[8]; _snwprintf_s(k, 8, _TRUNCATE, L"MRU%d", i); regSet(k, i < n ? items[i] : L""); }
    wchar_t cur[MAX_PATH]; GetDlgItemTextW(g_wnd, ID_SRC, cur, MAX_PATH);
    mruLoad();
    SetDlgItemTextW(g_wnd, ID_SRC, cur);
}

/* ------------------------------------------------------------------ build -- */
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
        if (m) { h = (HICON)LoadImageW(m, MAKEINTRESOURCEW(159), IMAGE_ICON, sz, sz, 0); FreeLibrary(m); }
    }
    SendDlgItemMessageW(g_wnd, ID_PREVIEW, STM_SETICON, (WPARAM)h, 0);
    if (g_preview) DestroyIcon(g_preview);
    g_preview = h;
}

static void onSourceChanged(const wchar_t *path)
{
    if (path[0] && fileExists(path) && _wcsicmp(path, g_loadedFor))
    {
        wcscpy_s(g_loadedFor, MAX_PATH, path);
        if (loadScriptSettings(path)) setStatus(L"이 스크립트의 저장된 설정을 불러왔습니다.");
        else setStatus(L"새 스크립트 - 기본 설정");
        updatePreview();
    }
    if (g_outAuto)
    {
        wchar_t o[MAX_PATH] = L"";
        if (path[0]) deriveOut(path, o, MAX_PATH);
        g_settingOut = 1; SetDlgItemTextW(g_wnd, ID_OUT, o); g_settingOut = 0;
    }
}

static void setSource(const wchar_t *path)
{
    SetDlgItemTextW(g_wnd, ID_SRC, path);
    onSourceChanged(path);
}

static void buildControls(void)
{
    /* The tab control is only the header strip; pages sit on the window background so
     * labels/checkboxes need no custom colouring. */
    g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                            S(12), S(10), S(656), S(28), g_wnd, (HMENU)ID_TAB, GetModuleHandleW(NULL), NULL);
    SendMessageW(g_tab, WM_SETFONT, (WPARAM)g_font, TRUE);
    static const wchar_t *TABS[] = { L"기본", L"버전 정보", L"파일 포함", L"설치 마법사", L"고급" };
    for (int i = 0; i < T_COUNT; i++)
    { TCITEMW ti; ti.mask = TCIF_TEXT; ti.pszText = (wchar_t *)TABS[i]; TabCtrl_InsertItem(g_tab, i, &ti); }

    /* ---- 기본 ---- */
    LBL(T_BASIC, L"스크립트", 28, 50, 110);
    mk(T_BASIC, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL, 140, 50, 410, 300, ID_SRC);
    BTN(T_BASIC, L"찾기...", ID_SRC_BR, 556, 50, 96);
    LBL(T_BASIC, L"출력 파일 (.exe)", 28, 84, 110); EDT(T_BASIC, ID_OUT, 140, 84, 410); BTN(T_BASIC, L"찾기...", ID_OUT_BR, 556, 84, 96);
    LBL(T_BASIC, L"아이콘 (.ico)", 28, 118, 110);   EDT(T_BASIC, ID_ICO, 140, 118, 292);
    BTN(T_BASIC, L"찾기...", ID_ICO_BR, 438, 118, 72); BTN(T_BASIC, L"기본", ID_ICO_DEF, 514, 118, 72);
    mk(T_BASIC, L"STATIC", L"", SS_ICON | SS_CENTERIMAGE | SS_SUNKEN, 600, 108, 44, 44, ID_PREVIEW);
    LBL(T_BASIC, L"베이스 (.bin)", 28, 160, 110);   EDT(T_BASIC, ID_BIN, 140, 160, 410); BTN(T_BASIC, L"찾기...", ID_BIN_BR, 556, 160, 96);
    LBL(T_BASIC, L"출력 방식", 28, 202, 110);
    RAD(T_BASIC, L"단일 EXE", ID_MODE_EXE, 140, 204, 500, 1);
    RAD(T_BASIC, L"EXE + 설치 마법사 (Setup.exe) - '설치 마법사' 탭에서 설정", ID_MODE_SETUP, 140, 230, 500, 0);
    LBL(T_BASIC, L"옵션", 28, 268, 110);
    CHK(T_BASIC, L"컴파일 후 실행", ID_RUNAFTER, 140, 272, 240);
    CHK(T_BASIC, L"출력 폴더 열기", ID_OPENDIR, 400, 272, 240);
    CHK(T_BASIC, L"관리자 권한으로 실행 (UAC 요청)", ID_ADMIN, 140, 298, 400);
    mk(T_BASIC, L"STATIC", L"스크립트 안의 ;@Ahk2Exe- / ;@AhkWeb- 지시어가 이 창의 값보다 우선합니다.\n#Include · Lib 함수 · FileInstall 파일은 자동으로 함께 들어갑니다.",
       SS_LEFT, 28, 352, 620, 44, -1);

    /* ---- 버전 정보 ---- */
    static const struct { const wchar_t *l; int id; } VR[] = {
        { L"파일 설명", ID_DESC }, { L"버전 (1.2.3.4)", ID_VER }, { L"제품명", ID_PROD }, { L"회사", ID_COMP },
        { L"저작권", ID_COPY }, { L"설명 (Comments)", ID_COMMENTS }, { L"원본 파일 이름", ID_ORIG } };
    for (int i = 0; i < 7; i++) { LBL(T_VERSION, VR[i].l, 28, 50 + 34 * i, 110); EDT(T_VERSION, VR[i].id, 140, 50 + 34 * i, 512); }
    mk(T_VERSION, L"STATIC", L"비워 둔 항목은 기본값(공백)을 씁니다. 탐색기 > 속성 > 자세히 탭에 표시됩니다.", SS_LEFT, 28, 300, 620, 40, -1);

    /* ---- 파일 포함 ---- */
    g_list = mk(T_FILES, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL, 28, 50, 624, 196, ID_LIST);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col; memset(&col, 0, sizeof(col)); col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = L"원본 파일 / 폴더"; col.cx = S(390); ListView_InsertColumn(g_list, 0, &col);
    col.pszText = L"대상 경로"; col.cx = S(210); ListView_InsertColumn(g_list, 1, &col);
    BTN(T_FILES, L"파일 추가...", ID_ADDFILE, 28, 256, 100);
    BTN(T_FILES, L"폴더 추가...", ID_ADDDIR, 134, 256, 100);
    BTN(T_FILES, L"제거", ID_REMOVE, 240, 256, 70);
    mk(T_FILES, L"STATIC", L"확장자 제한 없음 · 드래그&드롭 가능\n스크립트에서는 A_AhkWebFiles 로 접근", SS_LEFT, 330, 252, 322, 36, -1);
    LBL(T_FILES, L"대상 경로", 28, 296, 80); EDT(T_FILES, ID_DEST, 112, 296, 380); BTN(T_FILES, L"적용", ID_DEST_SET, 498, 296, 80);
    LBL(T_FILES, L"추출 위치", 28, 332, 80);
    mk(T_FILES, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, 112, 332, 380, 200, ID_EXTRACT);
    LBL(T_FILES, L"추출 방식", 28, 366, 80);
    mk(T_FILES, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, 112, 366, 380, 200, ID_EXMODE);
    CHK(T_FILES, L"임시 폴더의 파일을 종료 후에도 남기기", ID_KEEP, 112, 400, 400);
    static const wchar_t *EX[] = { L"임시 폴더 (종료 시 삭제)", L"EXE 와 같은 폴더", L"AppData\\<EXE 이름>", L"LocalAppData\\<EXE 이름>" };
    for (int i = 0; i < 4; i++) SendDlgItemMessageW(g_wnd, ID_EXTRACT, CB_ADDSTRING, 0, (LPARAM)EX[i]);
    static const wchar_t *EM[] = { L"항상 덮어쓰기", L"바뀐 파일만 (권장)", L"없을 때만" };
    for (int i = 0; i < 3; i++) SendDlgItemMessageW(g_wnd, ID_EXMODE, CB_ADDSTRING, 0, (LPARAM)EM[i]);

    /* ---- 설치 마법사 ---- */
    LBL(T_SETUP, L"앱 이름", 28, 50, 110);        EDT(T_SETUP, ID_APPNAME, 140, 50, 512);
    LBL(T_SETUP, L"게시자", 28, 84, 110);         EDT(T_SETUP, ID_PUBLISHER, 140, 84, 512);
    LBL(T_SETUP, L"설치 폴더 이름", 28, 118, 110); EDT(T_SETUP, ID_INSTDIR, 140, 118, 512);
    LBL(T_SETUP, L"설치 범위", 28, 152, 110);
    RAD(T_SETUP, L"현재 사용자 (관리자 권한 불필요)", ID_SCOPE_USER, 140, 154, 250, 1);
    RAD(T_SETUP, L"모든 사용자 (Program Files, 관리자)", ID_SCOPE_ALL, 400, 154, 252, 0);
    LBL(T_SETUP, L"사용권 계약 (.txt)", 28, 188, 110); EDT(T_SETUP, ID_LICENSE, 140, 188, 410); BTN(T_SETUP, L"찾기...", ID_LICENSE_BR, 556, 188, 96);
    LBL(T_SETUP, L"Setup 파일", 28, 222, 110);    EDT(T_SETUP, ID_SETUPOUT, 140, 222, 410); BTN(T_SETUP, L"찾기...", ID_SETUPOUT_BR, 556, 222, 96);
    LBL(T_SETUP, L"기본 선택", 28, 260, 110);
    CHK(T_SETUP, L"바탕화면 바로가기", ID_SC_DESK, 140, 264, 240);
    CHK(T_SETUP, L"시작 메뉴 바로가기", ID_SC_START, 400, 264, 240);
    CHK(T_SETUP, L"설치 후 바로 실행", ID_SC_RUN, 140, 290, 240);
    mk(T_SETUP, L"STATIC", L"'기본' 탭에서 출력 방식을 'EXE + 설치 마법사'로 고르면 만들어집니다.\n비운 항목은 버전 정보(제품명·회사·버전)나 EXE 이름을 씁니다. '파일 포함' 목록은 설치 폴더에 함께 설치되고,\n제거 프로그램과 '앱 및 기능' 등록, 무인 설치(/S, /D=폴더)를 지원합니다.",
       SS_LEFT, 28, 330, 624, 70, -1);

    /* ---- 고급 ---- */
    LBL(T_ADV, L"트레이 아이콘 (선택, .ico)", 28, 48, 300);
    LBL(T_ADV, L"핫키 일시중지 (Suspend)", 28, 76, 150);   EDT(T_ADV, ID_SUSP, 180, 76, 370);  BTN(T_ADV, L"찾기...", ID_SUSP_BR, 556, 76, 96);
    LBL(T_ADV, L"일시정지 (Pause)", 28, 110, 150);        EDT(T_ADV, ID_PAUSE, 180, 110, 370); BTN(T_ADV, L"찾기...", ID_PAUSE_BR, 556, 110, 96);
    LBL(T_ADV, L"일시정지 + 일시중지", 28, 144, 150);      EDT(T_ADV, ID_PSUSP, 180, 144, 370); BTN(T_ADV, L"찾기...", ID_PSUSP_BR, 556, 144, 96);
    CHK(T_ADV, L"코드 서명 (signtool, Windows SDK 필요)", ID_SIGN, 28, 192, 400);
    LBL(T_ADV, L"인증서 (.pfx)", 28, 222, 150);   EDT(T_ADV, ID_PFX, 180, 222, 370); BTN(T_ADV, L"찾기...", ID_PFX_BR, 556, 222, 96);
    LBL(T_ADV, L"인증서 암호", 28, 256, 150);
    mk(T_ADV, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, 180, 256, 220, 24, ID_PFXPASS);
    LBL(T_ADV, L"또는 인증서 주체 이름", 28, 290, 150); EDT(T_ADV, ID_SUBJECT, 180, 290, 370);
    LBL(T_ADV, L"타임스탬프 URL", 28, 324, 150);   EDT(T_ADV, ID_TS, 180, 324, 370);
    mk(T_ADV, L"STATIC", L"인증서 파일과 주체 이름을 모두 비우면 인증서 저장소에서 자동으로 고릅니다(/a).\n암호는 이 PC에만 암호화(DPAPI)되어 저장됩니다. Setup.exe도 함께 서명됩니다.",
       SS_LEFT, 28, 362, 624, 44, -1);

    /* ---- bottom ---- */
    mk(-1, L"STATIC", L"스크립트를 고르거나 창에 끌어다 놓으세요.", SS_LEFT | SS_ENDELLIPSIS, 14, 466, 440, 20, ID_STATUS);
    mk(-1, L"BUTTON", L"컴파일", WS_TABSTOP | BS_DEFPUSHBUTTON, 472, 460, 96, 32, ID_CONVERT);
    mk(-1, L"BUTTON", L"닫기", WS_TABSTOP | BS_PUSHBUTTON, 574, 460, 94, 32, ID_CLOSE);

    /* preferences */
    wchar_t v[MAX_PATH];
    regGet(L"RunAfter", v, MAX_PATH);  check(ID_RUNAFTER, v[0] == L'1');
    regGet(L"OpenDir", v, MAX_PATH);   check(ID_OPENDIR, v[0] == L'1');
    regGet(L"LastBin", v, MAX_PATH);
    if (v[0] && fileExists(v)) SetDlgItemTextW(g_wnd, ID_BIN, v);
    else { defaultBin(v, MAX_PATH); SetDlgItemTextW(g_wnd, ID_BIN, v); }
    setDefaultsForNewScript();
    mruLoad();
    showTab(T_BASIC);
}

/* ---------------------------------------------------------------- actions -- */
static int browseFile(int editId, int save, const wchar_t *filter, const wchar_t *defExt)
{
    wchar_t file[MAX_PATH]; getText(editId, file, MAX_PATH);
    wchar_t dir[MAX_PATH] = L"";
    if (file[0]) { dirOf(file, dir, MAX_PATH); wchar_t *fn = wcsrchr(file, L'\\'); if (fn) memmove(file, fn + 1, (wcslen(fn + 1) + 1) * sizeof(wchar_t)); }
    else
    {
        wchar_t src[MAX_PATH]; getText(ID_SRC, src, MAX_PATH);
        if (src[0]) dirOf(src, dir, MAX_PATH); else regGet(L"LastDir", dir, MAX_PATH);
    }
    OPENFILENAMEW of; memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of); of.hwndOwner = g_wnd;
    of.lpstrFilter = filter; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.lpstrInitialDir = dir[0] ? dir : NULL; of.lpstrDefExt = defExt;
    of.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of))) return 0;
    if (editId == ID_SRC) setSource(file); else SetDlgItemTextW(g_wnd, editId, file);
    return 1;
}

static void addFilesDialog(void)
{
    wchar_t *buf = (wchar_t *)calloc(32768, sizeof(wchar_t)); if (!buf) return;
    OPENFILENAMEW of; memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of); of.hwndOwner = g_wnd;
    of.lpstrFilter = L"모든 파일 (*.*)\0*.*\0"; of.lpstrFile = buf; of.nMaxFile = 32768;
    of.Flags = OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (GetOpenFileNameW(&of))
    {
        wchar_t *dir = buf, *p = buf + wcslen(buf) + 1;
        if (!*p) listAdd(buf, L"");
        else for (; *p; p += wcslen(p) + 1) { wchar_t f[MAX_PATH]; _snwprintf_s(f, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, p); listAdd(f, L""); }
    }
    free(buf);
}

static void addFolderDialog(void)
{
    BROWSEINFOW bi; memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_wnd; bi.lpszTitle = L"포함할 폴더를 선택하세요 (하위 폴더 포함)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t p[MAX_PATH]; if (SHGetPathFromIDListW(pidl, p)) listAdd(p, L"");
    CoTaskMemFree(pidl);
}

static Opts g_job;

static void gatherOpts(Opts *o)
{
    optsDefaults(o);
    getText(ID_SRC, o->in, MAX_PATH);
    getText(ID_OUT, o->out, MAX_PATH);
    getText(ID_ICO, o->icon, MAX_PATH);
    getText(ID_BIN, o->bin, MAX_PATH);
    getText(ID_SUSP, o->suspendIcon, MAX_PATH);
    getText(ID_PAUSE, o->pauseIcon, MAX_PATH);
    getText(ID_PSUSP, o->pauseSuspendIcon, MAX_PATH);
    o->requireAdmin = checked(ID_ADMIN);
    o->outputMode = checked(ID_MODE_SETUP) ? OUT_SETUP : OUT_EXE;

    static const struct { int id; const wchar_t *key; } F[] = {
        { ID_DESC, L"FileDescription" }, { ID_PROD, L"ProductName" }, { ID_COMP, L"CompanyName" },
        { ID_COPY, L"LegalCopyright" }, { ID_COMMENTS, L"Comments" }, { ID_ORIG, L"OriginalFilename" } };
    wchar_t v[256];
    for (size_t i = 0; i < NELEM(F); i++) { getText(F[i].id, v, 256); if (v[0]) viSet(o->vi, &o->nvi, F[i].key, v); }
    getText(ID_VER, v, 256);
    if (v[0]) { viSet(o->vi, &o->nvi, L"FileVersion", v); viSet(o->vi, &o->nvi, L"ProductVersion", v); }

    int cnt = ListView_GetItemCount(g_list);
    for (int i = 0; i < cnt && o->ninc < MAX_INC; i++)
    { listGet(i, o->inc[o->ninc].src, o->inc[o->ninc].dest); o->ninc++; }
    static const wchar_t *EXV[] = { L"temp", L"exe", L"appdata", L"localappdata" };
    static const wchar_t *EMV[] = { L"always", L"newer", L"missing" };
    int ex = (int)SendDlgItemMessageW(g_wnd, ID_EXTRACT, CB_GETCURSEL, 0, 0); if (ex < 0 || ex > 3) ex = 0;
    int em = (int)SendDlgItemMessageW(g_wnd, ID_EXMODE, CB_GETCURSEL, 0, 0);  if (em < 0 || em > 2) em = 1;
    wcscpy_s(o->extractTo, MAX_PATH, EXV[ex]); wcscpy_s(o->extractMode, 16, EMV[em]);
    o->keepFiles = checked(ID_KEEP);

    getText(ID_APPNAME, o->appName, 128); getText(ID_PUBLISHER, o->publisher, 128); getText(ID_INSTDIR, o->installDir, 128);
    o->perMachine = checked(ID_SCOPE_ALL);
    getText(ID_LICENSE, o->license, MAX_PATH); getText(ID_SETUPOUT, o->setupOut, MAX_PATH);
    o->scDesktop = checked(ID_SC_DESK); o->scStartMenu = checked(ID_SC_START); o->runAfterInstall = checked(ID_SC_RUN);

    o->sign = checked(ID_SIGN);
    getText(ID_PFX, o->signPfx, MAX_PATH); GetDlgItemTextW(g_wnd, ID_PFXPASS, o->signPass, 128);
    getText(ID_SUBJECT, o->signSubject, 128); getText(ID_TS, o->signTs, 256);
}

static DWORD WINAPI compileThread(LPVOID p)
{
    (void)p;
    wchar_t *out = (wchar_t *)calloc(MAX_PATH, sizeof(wchar_t));
    int ok = compileScript(&g_job, out, MAX_PATH);
    SecureZeroMemory(g_job.signPass, sizeof(g_job.signPass));
    PostMessageW(g_wnd, WM_COMPILED, ok, (LPARAM)out);
    return 0;
}

static void setBusy(int b)
{
    g_busy = b;
    EnableWindow(GetDlgItem(g_wnd, ID_CONVERT), !b);
    SetDlgItemTextW(g_wnd, ID_CONVERT, b ? L"컴파일 중..." : L"컴파일");
}

static void doConvert(void)
{
    if (g_busy) return;
    gatherOpts(&g_job);
    if (!g_job.in[0]) { MessageBoxW(g_wnd, L"컴파일할 스크립트를 선택하세요.", L"AhkWeb2Exe", MB_ICONWARNING); return; }
    if (!fileExists(g_job.in)) { MessageBoxW(g_wnd, L"스크립트 파일을 찾을 수 없습니다.", L"AhkWeb2Exe", MB_ICONWARNING); return; }
    g_log[0] = 0;
    setBusy(1);
    setStatus(L"컴파일 중...");
    CloseHandle(CreateThread(NULL, 0, compileThread, NULL, 0, NULL));
}

static void onCompiled(int ok, wchar_t *out)
{
    setBusy(0);
    if (ok)
    {
        wchar_t d[MAX_PATH]; dirOf(g_job.in, d, MAX_PATH); regSet(L"LastDir", d);
        wchar_t ico[MAX_PATH]; getText(ID_ICO, ico, MAX_PATH); if (ico[0]) regSet(L"LastIcon", ico);
        wchar_t defb[MAX_PATH], bin[MAX_PATH]; defaultBin(defb, MAX_PATH); getText(ID_BIN, bin, MAX_PATH);
        regSet(L"LastBin", _wcsicmp(bin, defb) ? bin : L"");
        saveScriptSettings(g_job.in);
        mruPush(g_job.in);
        setStatus(L"컴파일 완료");
        if (checked(ID_OPENDIR))
        {
            wchar_t args[MAX_PATH + 16]; _snwprintf_s(args, MAX_PATH + 16, _TRUNCATE, L"/select,\"%s\"", out);
            ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        }
        if (checked(ID_RUNAFTER) && g_lastExe[0])
        {
            wchar_t ed[MAX_PATH]; dirOf(g_lastExe, ed, MAX_PATH);
            ShellExecuteW(NULL, L"open", g_lastExe, NULL, ed, SW_SHOWNORMAL);
        }
        else
        {
            size_t ml = LOG_CCH + MAX_PATH + 64;
            wchar_t *m = (wchar_t *)malloc(ml * sizeof(wchar_t));
            if (m)
            {
                _snwprintf_s(m, ml, _TRUNCATE, L"컴파일 완료:\n%s\n\n%s", out, g_log);
                MessageBoxW(g_wnd, m, L"AhkWeb2Exe", MB_ICONINFORMATION);
                free(m);
            }
        }
    }
    else
    {
        setStatus(L"컴파일 실패");
        wchar_t m[2200]; _snwprintf_s(m, 2200, _TRUNCATE, L"컴파일 실패:\n%s", g_err);
        MessageBoxW(g_wnd, m, L"AhkWeb2Exe", MB_ICONERROR);
    }
    free(out);
}

/* ---------------------------------------------------------------- wndproc -- */
static wchar_t g_initSrc[MAX_PATH];

static void onDrop(HDROP d)
{
    UINT n = DragQueryFileW(d, 0xFFFFFFFF, NULL, 0);
    int added = 0;
    for (UINT i = 0; i < n; i++)
    {
        wchar_t f[MAX_PATH]; DragQueryFileW(d, i, f, MAX_PATH);
        int isDir = dirExists(f);
        if (!isDir && (endsWithI(f, L".ahk") || endsWithI(f, L".ahkw")) && g_curTab != T_FILES) setSource(f);
        else if (!isDir && endsWithI(f, L".ico") && g_curTab == T_BASIC) SetDlgItemTextW(g_wnd, ID_ICO, f);
        else if (!isDir && endsWithI(f, L".bin") && g_curTab == T_BASIC) SetDlgItemTextW(g_wnd, ID_BIN, f);
        else if (!isDir && endsWithI(f, L".txt") && g_curTab == T_SETUP) SetDlgItemTextW(g_wnd, ID_LICENSE, f);
        else if (!isDir && endsWithI(f, L".pfx") && g_curTab == T_ADV) SetDlgItemTextW(g_wnd, ID_PFX, f);
        else { listAdd(f, L""); added++; }
    }
    DragFinish(d);
    if (added)
    {
        TabCtrl_SetCurSel(g_tab, T_FILES); showTab(T_FILES);
        wchar_t m[100]; _snwprintf_s(m, 100, _TRUNCATE, L"포함 파일 %d개 추가", added); setStatus(m);
    }
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        g_wnd = h;
        DragAcceptFiles(h, TRUE);
        buildControls();
        if (g_initSrc[0]) setSource(g_initSrc);
        updatePreview();
        SetFocus(GetDlgItem(h, ID_SRC));
        return 0;
    case WM_DROPFILES: onDrop((HDROP)wp); return 0;
    case WM_COMPILED:  onCompiled((int)wp, (wchar_t *)lp); return 0;
    case WM_NOTIFY:
    {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == ID_TAB && nh->code == TCN_SELCHANGE) showTab(TabCtrl_GetCurSel(g_tab));
        else if (nh->idFrom == ID_LIST && nh->code == LVN_ITEMCHANGED)
        {
            int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
            if (sel >= 0) { wchar_t s[MAX_PATH], d[MAX_PATH]; listGet(sel, s, d); SetDlgItemTextW(h, ID_DEST, d); }
        }
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_SRC && (code == CBN_EDITCHANGE || code == CBN_SELCHANGE))
        {
            wchar_t s[MAX_PATH] = L"";
            if (code == CBN_SELCHANGE)
            {
                int i = (int)SendDlgItemMessageW(h, ID_SRC, CB_GETCURSEL, 0, 0);
                if (i >= 0) SendDlgItemMessageW(h, ID_SRC, CB_GETLBTEXT, i, (LPARAM)s);
            }
            else getText(ID_SRC, s, MAX_PATH);
            onSourceChanged(s);
            return 0;
        }
        if (code == EN_CHANGE)
        {
            if (id == ID_OUT && !g_settingOut) { wchar_t o[MAX_PATH]; getText(ID_OUT, o, MAX_PATH); g_outAuto = !o[0]; }
            else if (id == ID_ICO || id == ID_BIN) updatePreview();
            return 0;
        }
        switch (id)
        {
        case ID_SRC_BR: browseFile(ID_SRC, 0, L"AutoHotkey 스크립트 (*.ahk;*.ahkw)\0*.ahk;*.ahkw\0모든 파일 (*.*)\0*.*\0", L"ahk"); break;
        case ID_OUT_BR: if (browseFile(ID_OUT, 1, L"실행 파일 (*.exe)\0*.exe\0", L"exe")) g_outAuto = 0; break;
        case ID_ICO_BR: browseFile(ID_ICO, 0, L"아이콘 (*.ico)\0*.ico\0", L"ico"); break;
        case ID_ICO_DEF: SetDlgItemTextW(h, ID_ICO, L""); break;
        case ID_BIN_BR: browseFile(ID_BIN, 0, L"AutoHotkey Web 베이스 (*.bin)\0*.bin\0모든 파일 (*.*)\0*.*\0", L"bin"); break;
        case ID_SUSP_BR:  browseFile(ID_SUSP, 0, L"아이콘 (*.ico)\0*.ico\0", L"ico"); break;
        case ID_PAUSE_BR: browseFile(ID_PAUSE, 0, L"아이콘 (*.ico)\0*.ico\0", L"ico"); break;
        case ID_PSUSP_BR: browseFile(ID_PSUSP, 0, L"아이콘 (*.ico)\0*.ico\0", L"ico"); break;
        case ID_LICENSE_BR: browseFile(ID_LICENSE, 0, L"텍스트 (*.txt)\0*.txt\0모든 파일 (*.*)\0*.*\0", L"txt"); break;
        case ID_SETUPOUT_BR: browseFile(ID_SETUPOUT, 1, L"실행 파일 (*.exe)\0*.exe\0", L"exe"); break;
        case ID_PFX_BR: browseFile(ID_PFX, 0, L"인증서 (*.pfx;*.p12)\0*.pfx;*.p12\0", L"pfx"); break;
        case ID_ADDFILE: addFilesDialog(); break;
        case ID_ADDDIR: addFolderDialog(); break;
        case ID_REMOVE:
        {
            int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
            if (sel >= 0) ListView_DeleteItem(g_list, sel);
            break;
        }
        case ID_DEST_SET:
        {
            int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
            if (sel < 0) { setStatus(L"목록에서 항목을 먼저 선택하세요."); break; }
            wchar_t d[MAX_PATH]; getText(ID_DEST, d, MAX_PATH);
            if (wcsstr(d, L"..") || wcschr(d, L':')) { MessageBoxW(h, L"대상 경로는 상대 경로여야 합니다 (예: data 또는 data\\config.ini).", L"AhkWeb2Exe", MB_ICONWARNING); break; }
            ListView_SetItemText(g_list, sel, 1, d[0] ? d : L"(이름 그대로)");
            break;
        }
        case ID_MODE_SETUP: if (checked(ID_MODE_SETUP)) setStatus(L"'설치 마법사' 탭에서 앱 이름·설치 범위 등을 설정할 수 있습니다."); break;
        case ID_CONVERT: doConvert(); break;
        case ID_CLOSE: case IDCANCEL: if (!g_busy) DestroyWindow(h); break;
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_busy) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        regSet(L"RunAfter", checked(ID_RUNAFTER) ? L"1" : L"0");
        regSet(L"OpenDir", checked(ID_OPENDIR) ? L"1" : L"0");
        if (g_preview) DestroyIcon(g_preview);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int runGui(const wchar_t *initSrc)
{
    if (initSrc) wcsncpy_s(g_initSrc, MAX_PATH, initSrc, _TRUNCATE);
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
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

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rc = { 0, 0, S(680), S(504) };
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_CONTROLPARENT);
    HWND w = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"AhkWeb2Exe - AutoHotkey Web 스크립트 컴파일러",
                             style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                             NULL, NULL, hi, NULL);
    if (!w) return 1;
    ShowWindow(w, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && !g_busy) { DestroyWindow(w); continue; }
        if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    DeleteObject(g_font);
    CoUninitialize();
    return 0;
}

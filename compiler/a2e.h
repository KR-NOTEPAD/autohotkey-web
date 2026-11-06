/* ============================================================================
 *  AhkWeb2Exe - shared declarations (core: ahkweb2exe.c, GUI: a2e_gui.c)
 * ============================================================================ */
#ifndef A2E_H
#define A2E_H
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#define MAX_VI    24
#define MAX_RES   16
#define MAX_INC   64
#define MAX_POST  8
#define LOG_CCH   16384

typedef struct { wchar_t key[64]; wchar_t val[256]; } VIPair;

typedef struct {
    wchar_t path[MAX_PATH];
    wchar_t name[64];       /* string name (upper-cased) when !nameIsInt */
    int     nameIsInt;
    WORD    nameInt;
    WORD    type;           /* 0 = auto from extension */
} ResItem;

/* A file or folder to bundle; dest is a relative folder/file name inside the
 * extraction (or install) folder. Empty dest = item's own name at the root. */
typedef struct { wchar_t src[MAX_PATH]; wchar_t dest[MAX_PATH]; } IncItem;

enum { OUT_EXE = 0, OUT_SETUP = 1 };

typedef struct {
    /* --- basic --- */
    wchar_t in[MAX_PATH], out[MAX_PATH], bin[MAX_PATH], icon[MAX_PATH];
    wchar_t suspendIcon[MAX_PATH], pauseIcon[MAX_PATH], pauseSuspendIcon[MAX_PATH];
    VIPair  vi[MAX_VI]; int nvi;
    ResItem res[MAX_RES]; int nres;
    int     silent;
    int     requireAdmin;              /* manifest requestedExecutionLevel */
    /* --- bundled files --- */
    IncItem inc[MAX_INC]; int ninc;
    wchar_t extractTo[MAX_PATH];       /* temp|exe|appdata|localappdata|path:<abs> */
    wchar_t extractMode[16];           /* always|newer|missing */
    int     keepFiles;                 /* temp: do not delete on exit */
    /* --- output: single exe or setup wizard --- */
    int     outputMode;                /* OUT_EXE / OUT_SETUP */
    wchar_t appName[128], publisher[128], installDir[128];
    int     perMachine;                /* 0 = current user (no admin), 1 = all users, 2 = ask in the wizard */
    wchar_t license[MAX_PATH];
    int     scDesktop, scStartMenu, runAfterInstall;
    wchar_t setupOut[MAX_PATH];
    /* --- signing / post-processing --- */
    int     sign;
    wchar_t signPfx[MAX_PATH], signPass[128], signSubject[128], signTs[256];
    wchar_t postExec[MAX_POST][1024]; int npost;
} Opts;

/* Standard StringFileInfo keys. */
extern const wchar_t *VI_STD[];
extern const int VI_STD_N;

/* logging (core) */
extern int     g_haveOut;
extern wchar_t g_log[LOG_CCH];
extern wchar_t g_err[2048];
void info(const wchar_t *fmt, ...);
int  fail(const wchar_t *fmt, ...);

/* helpers (core) */
void viSet(VIPair *arr, int *n, const wchar_t *key, const wchar_t *val);
const wchar_t *viGet(const VIPair *arr, int n, const wchar_t *key);
int  endsWithI(const wchar_t *s, const wchar_t *suf);
int  fileExists(const wchar_t *p);
int  dirExists(const wchar_t *p);
void trimW(wchar_t *s);
void dirOf(const wchar_t *path, wchar_t *dir, size_t cch);
void compilerDir(wchar_t *dir, size_t cch);
void defaultBin(wchar_t *out, size_t cch);
void deriveOut(const wchar_t *in, wchar_t *out, size_t cch);
void deriveSetupOut(const Opts *o, const wchar_t *exeOut, wchar_t *out, size_t cch);
void optsDefaults(Opts *o);

/* compile: returns 1 on success; finalOut receives the main produced file
 * (the .exe, or the Setup.exe in setup mode). g_lastExe = the produced .exe. */
int  compileScript(const Opts *src, wchar_t *finalOut, size_t cch);
extern wchar_t g_lastExe[MAX_PATH];
/* ;@AhkWeb-Include items written in the script itself (for display). */
int  scriptDirectiveIncludes(const wchar_t *script, IncItem *out, int max);

/* GUI (a2e_gui.c) */
int  runGui(const wchar_t *initSrc);

#endif

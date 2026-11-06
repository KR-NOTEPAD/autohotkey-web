// ============================================================================
//  NOTEPAD.dll  -  notepad_db.cpp
//  [NOTEPAD] 세트 I: SQLite (winsqlite3.dll 런타임 동적 로드).
//
//  방침: Windows 10 1809+ / 11 에 OS 기본 번들된 winsqlite3.dll 을
//        LoadLibrary 로 지연 로드한다. sqlite3.h 를 임베드하지 않으므로
//        필요한 상수/시그니처만 여기서 직접 선언한다(ABI 안정).
//  의존성 0: 추가 배포 파일 없음. (구형 Windows 7 등은 winsqlite3.dll 부재)
//
//  export 규약은 notepad_exports.cpp 와 동일하지만, DB 는 상태(핸들)를
//  유지해야 하므로 이 파일이 export 를 직접 제공한다(핸들을 정수로 AHK 에 전달).
// ============================================================================
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include "cJSON.h"

// notepad_exports.cpp 의 내부 헬퍼와 동일 규약을 쓰기 위해 로컬 복제(파일 독립).
namespace {

std::string wToUtf8_db(const wchar_t* w) {
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring utf8ToW_db(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
// wstring -> out 버퍼 (out==NULL||outCch==0 -> 필요문자수 / 부족 -> 음수 / 성공 -> 문자수)
int writeOut_db(const std::wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}

} // anonymous

// ---- winsqlite3 최소 시그니처 (sqlite3.h 미임베드) --------------------------
// SQLite ABI 상수 (안정, 공개 문서 값).
#define NP_SQLITE_OK        0
#define NP_SQLITE_ROW       100
#define NP_SQLITE_DONE      101
#define NP_SQLITE_INTEGER   1
#define NP_SQLITE_FLOAT     2
#define NP_SQLITE_TEXT      3
#define NP_SQLITE_BLOB      4
#define NP_SQLITE_NULL      5
// open flags
#define NP_SQLITE_OPEN_READWRITE 0x00000002
#define NP_SQLITE_OPEN_CREATE    0x00000004

typedef struct sqlite3      sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef __int64             sqlite_int64;

typedef int   (__cdecl *pfn_open_v2)(const char*, sqlite3**, int, const char*);
typedef int   (__cdecl *pfn_close)(sqlite3*);
typedef int   (__cdecl *pfn_exec)(sqlite3*, const char*,
                                  int (__cdecl*)(void*,int,char**,char**),
                                  void*, char**);
typedef int   (__cdecl *pfn_prepare_v2)(sqlite3*, const char*, int,
                                        sqlite3_stmt**, const char**);
typedef int   (__cdecl *pfn_step)(sqlite3_stmt*);
typedef int   (__cdecl *pfn_finalize)(sqlite3_stmt*);
typedef int   (__cdecl *pfn_column_count)(sqlite3_stmt*);
typedef const char* (__cdecl *pfn_column_name)(sqlite3_stmt*, int);
typedef int   (__cdecl *pfn_column_type)(sqlite3_stmt*, int);
typedef const unsigned char* (__cdecl *pfn_column_text)(sqlite3_stmt*, int);
typedef sqlite_int64 (__cdecl *pfn_column_int64)(sqlite3_stmt*, int);
typedef double (__cdecl *pfn_column_double)(sqlite3_stmt*, int);
typedef const char* (__cdecl *pfn_errmsg)(sqlite3*);
typedef sqlite_int64 (__cdecl *pfn_last_insert_rowid)(sqlite3*);
typedef int   (__cdecl *pfn_changes)(sqlite3*);
typedef void  (__cdecl *pfn_free)(void*);

// ---- 지연 로드 상태 ---------------------------------------------------------
namespace {

struct SqliteApi {
    HMODULE hMod = nullptr;
    pfn_open_v2          open_v2 = nullptr;
    pfn_close            close = nullptr;
    pfn_exec             exec = nullptr;
    pfn_prepare_v2       prepare_v2 = nullptr;
    pfn_step             step = nullptr;
    pfn_finalize         finalize = nullptr;
    pfn_column_count     column_count = nullptr;
    pfn_column_name      column_name = nullptr;
    pfn_column_type      column_type = nullptr;
    pfn_column_text      column_text = nullptr;
    pfn_column_int64     column_int64 = nullptr;
    pfn_column_double    column_double = nullptr;
    pfn_errmsg           errmsg = nullptr;
    pfn_last_insert_rowid last_insert_rowid = nullptr;
    pfn_changes          changes = nullptr;
    pfn_free             sfree = nullptr;
    bool loaded = false;
};

SqliteApi g_api;
std::mutex g_apiMtx;
std::wstring g_lastError; // 마지막 오류 메시지(진단용)

template<typename T>
bool bind(HMODULE h, T& fn, const char* name) {
    fn = reinterpret_cast<T>(GetProcAddress(h, name));
    return fn != nullptr;
}

bool ensureApi() {
    std::lock_guard<std::mutex> lk(g_apiMtx);
    if (g_api.loaded) return true;
    // winsqlite3.dll 은 OS 시스템 DLL. LoadLibrary 로 시스템 경로에서 로드.
    HMODULE h = LoadLibraryW(L"winsqlite3.dll");
    if (!h) { g_lastError = L"winsqlite3.dll not found (Windows 10 1809+ required)"; return false; }
    bool ok = true;
    ok &= bind(h, g_api.open_v2,          "sqlite3_open_v2");
    ok &= bind(h, g_api.close,            "sqlite3_close");
    ok &= bind(h, g_api.exec,             "sqlite3_exec");
    ok &= bind(h, g_api.prepare_v2,       "sqlite3_prepare_v2");
    ok &= bind(h, g_api.step,             "sqlite3_step");
    ok &= bind(h, g_api.finalize,         "sqlite3_finalize");
    ok &= bind(h, g_api.column_count,     "sqlite3_column_count");
    ok &= bind(h, g_api.column_name,      "sqlite3_column_name");
    ok &= bind(h, g_api.column_type,      "sqlite3_column_type");
    ok &= bind(h, g_api.column_text,      "sqlite3_column_text");
    ok &= bind(h, g_api.column_int64,     "sqlite3_column_int64");
    ok &= bind(h, g_api.column_double,    "sqlite3_column_double");
    ok &= bind(h, g_api.errmsg,           "sqlite3_errmsg");
    ok &= bind(h, g_api.last_insert_rowid,"sqlite3_last_insert_rowid");
    ok &= bind(h, g_api.changes,          "sqlite3_changes");
    ok &= bind(h, g_api.sfree,            "sqlite3_free");
    if (!ok) { FreeLibrary(h); g_lastError = L"winsqlite3.dll missing exports"; return false; }
    g_api.hMod = h;
    g_api.loaded = true;
    return true;
}

// AHK 로 넘길 핸들: sqlite3* 포인터를 그대로 __int64 로 캐스팅해 사용.
inline __int64 toHandle(sqlite3* db) { return (__int64)(intptr_t)db; }
inline sqlite3* fromHandle(__int64 h) { return (sqlite3*)(intptr_t)h; }

void setErr(sqlite3* db) {
    if (db && g_api.errmsg) g_lastError = utf8ToW_db(g_api.errmsg(db));
}

} // anonymous

// ============================================================================
//  Exports (세트 I)
// ============================================================================
extern "C" {

// DB 열기(없으면 생성). 성공 시 핸들(>0), 실패 0.
__declspec(dllexport) __int64 __stdcall Notepad_DbOpen(const wchar_t* path) {
    if (!ensureApi()) return 0;
    std::string p = wToUtf8_db(path);
    if (p.empty()) { g_lastError = L"empty path"; return 0; }
    sqlite3* db = nullptr;
    int rc = g_api.open_v2(p.c_str(), &db,
                           NP_SQLITE_OPEN_READWRITE | NP_SQLITE_OPEN_CREATE, nullptr);
    if (rc != NP_SQLITE_OK || !db) {
        if (db) { setErr(db); g_api.close(db); }
        else g_lastError = L"sqlite3_open_v2 failed";
        return 0;
    }
    return toHandle(db);
}

// DB 닫기. 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_DbClose(__int64 handle) {
    if (!g_api.loaded) return 0;
    sqlite3* db = fromHandle(handle);
    if (!db) return 0;
    return g_api.close(db) == NP_SQLITE_OK ? 1 : 0;
}

// 결과를 반환하지 않는 SQL 실행(INSERT/UPDATE/CREATE 등, 여러 문장 가능).
// 성공 1 / 실패 0 (오류 메시지는 Notepad_DbLastError).
__declspec(dllexport) int __stdcall Notepad_DbExec(__int64 handle, const wchar_t* sql) {
    if (!g_api.loaded) return 0;
    sqlite3* db = fromHandle(handle);
    if (!db) return 0;
    std::string s = wToUtf8_db(sql);
    char* err = nullptr;
    int rc = g_api.exec(db, s.c_str(), nullptr, nullptr, &err);
    if (rc != NP_SQLITE_OK) {
        g_lastError = err ? utf8ToW_db(err) : L"exec failed";
        if (err && g_api.sfree) g_api.sfree(err);
        return 0;
    }
    return 1;
}

// SELECT 실행 -> 결과를 JSON 배열(행=객체) 문자열로 반환.
//   예: [{"id":"1","name":"kim"},{"id":"2","name":"lee"}]
//   숫자/텍스트/NULL 모두 문자열로 통일(AHK Note_JsonGet 로 바로 파싱).
//   출력버퍼 규약(2단계 호출): out==NULL||outCch==0 -> 필요문자수, 부족 -> 음수, 오류 -> 0.
// 2단계 호출 중복 쿼리 방지를 위해 (handle+sql) 단일 캐시.
static std::mutex g_qMtx;
static __int64     g_qHandle = 0;
static std::wstring g_qSql;
static std::wstring g_qJson;
static bool        g_qValid = false;

__declspec(dllexport) int __stdcall Notepad_DbQueryJson(__int64 handle,
                                                        const wchar_t* sql,
                                                        wchar_t* out, int outCch) {
    if (!g_api.loaded) return 0;
    sqlite3* db = fromHandle(handle);
    if (!db) return 0;

    std::wstring wsql = sql ? sql : L"";
    {
        std::lock_guard<std::mutex> lk(g_qMtx);
        if (g_qValid && handle == g_qHandle && wsql == g_qSql)
            return writeOut_db(g_qJson, out, outCch); // 캐시 히트(2번째 호출)
    }

    std::string s = wToUtf8_db(sql);
    sqlite3_stmt* stmt = nullptr;
    if (g_api.prepare_v2(db, s.c_str(), -1, &stmt, nullptr) != NP_SQLITE_OK || !stmt) {
        setErr(db);
        return 0;
    }

    cJSON* arr = cJSON_CreateArray();
    int nCol = g_api.column_count(stmt);
    int rc;
    while ((rc = g_api.step(stmt)) == NP_SQLITE_ROW) {
        cJSON* row = cJSON_CreateObject();
        for (int c = 0; c < nCol; ++c) {
            const char* colName = g_api.column_name(stmt, c);
            int t = g_api.column_type(stmt, c);
            if (t == NP_SQLITE_NULL) {
                cJSON_AddNullToObject(row, colName ? colName : "");
            } else {
                // 모든 타입을 텍스트로 통일(sqlite3_column_text 는 형변환 제공).
                const unsigned char* txt = g_api.column_text(stmt, c);
                cJSON_AddStringToObject(row, colName ? colName : "",
                                        txt ? (const char*)txt : "");
            }
        }
        cJSON_AddItemToArray(arr, row);
    }
    g_api.finalize(stmt);

    if (rc != NP_SQLITE_DONE) {
        setErr(db);
        cJSON_Delete(arr);
        return 0;
    }

    char* js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!js) return 0;
    std::wstring wjson = utf8ToW_db(std::string(js));
    cJSON_free(js);

    {
        std::lock_guard<std::mutex> lk(g_qMtx);
        g_qHandle = handle; g_qSql = wsql; g_qJson = wjson; g_qValid = true;
    }
    return writeOut_db(wjson, out, outCch);
}

// 직전 INSERT 로 생성된 rowid. 실패 0.
__declspec(dllexport) __int64 __stdcall Notepad_DbLastInsertId(__int64 handle) {
    if (!g_api.loaded) return 0;
    sqlite3* db = fromHandle(handle);
    if (!db) return 0;
    return (__int64)g_api.last_insert_rowid(db);
}

// 직전 UPDATE/DELETE/INSERT 영향 행 수.
__declspec(dllexport) int __stdcall Notepad_DbChanges(__int64 handle) {
    if (!g_api.loaded) return 0;
    sqlite3* db = fromHandle(handle);
    if (!db) return 0;
    return g_api.changes(db);
}

// 마지막 오류 메시지(진단용).
__declspec(dllexport) int __stdcall Notepad_DbLastError(wchar_t* out, int outCch) {
    return writeOut_db(g_lastError, out, outCch);
}

} // extern "C"

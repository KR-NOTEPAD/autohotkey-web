// ============================================================================
//  NOTEPAD.dll  -  notepad_fs.cpp
//  [NOTEPAD] 세트 T: 파일/폴더 유틸 (복사/이동/휴지통삭제/폴더/목록/특수폴더).
//  의존성 0: kernel32/shell32(기본). 파이썬 shutil/os/pathlib 일부 대응.
//  링크: shell32.lib (특수폴더/휴지통), user32 이미 포함.
// ============================================================================
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <cwctype>
#include "cJSON.h"

#pragma comment(lib, "shell32.lib")

namespace {
std::string fs_wToUtf8(const wchar_t* w) {
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring fs_utf8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
int fs_writeOut(const std::wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}
} // anonymous

extern "C" {

// 파일 복사. overwrite=1 이면 덮어쓰기. 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_FileCopy(const wchar_t* src, const wchar_t* dst,
                                                     int overwrite) {
    return CopyFileW(src, dst, overwrite ? FALSE : TRUE) ? 1 : 0;
}
// 파일/폴더 이동(이름변경). 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_FileMove(const wchar_t* src, const wchar_t* dst) {
    return MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 1 : 0;
}
// 파일/폴더를 휴지통으로 보냄(복구 가능). 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_FileRecycle(const wchar_t* path) {
    if (!path) return 0;
    // SHFileOperation 은 이중 널 종료 문자열 필요.
    std::wstring p = path;
    std::vector<wchar_t> buf(p.begin(), p.end());
    buf.push_back(L'\0'); buf.push_back(L'\0');
    SHFILEOPSTRUCTW op; ZeroMemory(&op, sizeof(op));
    op.wFunc = FO_DELETE;
    op.pFrom = buf.data();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 ? 1 : 0;
}
// 파일 존재 여부. 1=파일존재, 0=없음/폴더.
__declspec(dllexport) int __stdcall Notepad_FileExists(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1;
}
// 폴더 존재 여부. 1=폴더존재, 0=없음/파일.
__declspec(dllexport) int __stdcall Notepad_DirExists(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}
// 파일 크기(바이트). 실패 -1.
__declspec(dllexport) __int64 __stdcall Notepad_FileSize(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return -1;
    LARGE_INTEGER li; li.HighPart = fad.nFileSizeHigh; li.LowPart = fad.nFileSizeLow;
    return (__int64)li.QuadPart;
}
// 폴더 생성(중간 경로 포함). 이미 있으면 성공. 성공 1/0.
__declspec(dllexport) int __stdcall Notepad_DirCreate(const wchar_t* path) {
    if (!path) return 0;
    int r = SHCreateDirectoryExW(nullptr, path, nullptr);
    return (r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS) ? 1 : 0;
}
// 폴더를 휴지통으로(내용 포함). 성공 1/0. (영구삭제 대신 안전한 휴지통 사용)
__declspec(dllexport) int __stdcall Notepad_DirRecycle(const wchar_t* path) {
    return Notepad_FileRecycle(path);
}
// 파일 수정시각을 Unix 초로. 실패 -1.
__declspec(dllexport) __int64 __stdcall Notepad_FileModTime(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return -1;
    ULARGE_INTEGER u; u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    // FILETIME(100ns, 1601 기준) -> Unix 초(1970 기준)
    return (__int64)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}
// 폴더 내 항목 목록을 JSON 배열로. 각 원소 {name, isDir, size}.
//   pattern 예: "*.txt"(빈문자열이면 "*"). 실패 0.
__declspec(dllexport) int __stdcall Notepad_DirList(const wchar_t* dir, const wchar_t* pattern,
                                                    wchar_t* out, int outCch) {
    std::wstring d = dir ? dir : L".";
    std::wstring pat = (pattern && pattern[0]) ? pattern : L"*";
    std::wstring search = d;
    if (!search.empty() && search.back() != L'\\' && search.back() != L'/') search += L"\\";
    search += pat;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    cJSON* arr = cJSON_CreateArray();
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", fs_wToUtf8(fd.cFileName).c_str());
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            cJSON_AddBoolToObject(o, "isDir", isDir ? 1 : 0);
            LARGE_INTEGER li; li.HighPart = fd.nFileSizeHigh; li.LowPart = fd.nFileSizeLow;
            cJSON_AddNumberToObject(o, "size", (double)li.QuadPart);
            cJSON_AddItemToArray(arr, o);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    char* js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!js) return 0;
    std::wstring w = fs_utf8ToW(std::string(js));
    cJSON_free(js);
    return fs_writeOut(w, out, outCch);
}
// 특수 폴더 경로. which: "desktop","documents","appdata","localappdata","temp","windows","programfiles". 실패 "".
__declspec(dllexport) int __stdcall Notepad_SpecialFolder(const wchar_t* which,
                                                          wchar_t* out, int outCch) {
    std::wstring w = which ? which : L"";
    for (auto& c : w) c = (wchar_t)towlower(c);
    int csidl = -1;
    if (w == L"desktop") csidl = CSIDL_DESKTOPDIRECTORY;
    else if (w == L"documents") csidl = CSIDL_PERSONAL;
    else if (w == L"appdata") csidl = CSIDL_APPDATA;
    else if (w == L"localappdata") csidl = CSIDL_LOCAL_APPDATA;
    else if (w == L"programfiles") csidl = CSIDL_PROGRAM_FILES;
    else if (w == L"windows") csidl = CSIDL_WINDOWS;
    if (w == L"temp") {
        wchar_t tmp[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n == 0) return 0;
        std::wstring t(tmp, n);
        if (!t.empty() && (t.back() == L'\\' || t.back() == L'/')) t.pop_back();
        return fs_writeOut(t, out, outCch);
    }
    if (csidl < 0) return 0;
    wchar_t path[MAX_PATH];
    if (SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, path) != S_OK) return 0;
    return fs_writeOut(std::wstring(path), out, outCch);
}
// 고유 임시 파일 경로 생성(파일도 만들어짐). 실패 "".
__declspec(dllexport) int __stdcall Notepad_TempFile(wchar_t* out, int outCch) {
    wchar_t dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, dir) == 0) return 0;
    wchar_t path[MAX_PATH];
    if (GetTempFileNameW(dir, L"NPD", 0, path) == 0) return 0;
    return fs_writeOut(std::wstring(path), out, outCch);
}

} // extern "C"

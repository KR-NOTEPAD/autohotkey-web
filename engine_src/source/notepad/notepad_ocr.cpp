// ============================================================================
//  NOTEPAD.dll  -  notepad_ocr.cpp
//  [NOTEPAD] OCR (두 엔진 내장):
//    (1) 세트 H  : Windows.Media.Ocr (WinRT)  - 가벼움. 영어/숫자 양호, 한글 약함.
//                  → Note_OcrImage / Note_OcrScreen / Note_OcrLanguages
//    (2) 세트 AI : 딥러닝 PaddleOCR ONNX      - EasyOCR급 한국어. (권장)
//                  검출 DBNet(det.onnx) + 인식 한국어 CRNN + (선택)각도분류(cls.onnx).
//                  인식 모델은 PP-OCRv5(rec_korean_v5.onnx + korean_dict_v5.txt,
//                  입력높이 48) 우선, 없으면 구형 v2.0(rec_korean.onnx +
//                  korean_dict.txt, 입력높이 32)로 자동 폴백. opts "v2" 로 강제 가능.
//                  v5 는 한글뿐 아니라 영문/숫자/특수문자/공백까지 함께 인식한다.
//                  → Note_OcrImageAI / Note_OcrScreenAI / Note_OcrAIError
//
//  WinRT 블로킹 async(.get())는 STA 데드락 가능 -> 전용 MTA 워커 스레드에서 실행.
//  AI 엔진은 순수 C++(ONNX Runtime C API + GDI+)라 언어팩/OS 버전 비의존.
//  링크: windowsapp.lib(WinRT), gdiplus.lib(AI), onnxruntime.lib(third_party).
//  /std:c++17 /EHa 필요. AI 실추론은 /DNOTEPAD_ENABLE_ONNX 시 활성(build.bat 자동).
//  핵심 후처리 알고리즘은 notepad_ocr_algo.h 에 분리(단위테스트 검증).
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "notepad_crypto.h"
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <cwctype>
#include <cstdlib>
#include <cstring>

#include "notepad_ocr_algo.h"

#ifdef NOTEPAD_ENABLE_ONNX
#include <onnxruntime_c_api.h>
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(lib, "gdiplus.lib")

using notepad::ocralgo::Comp;

namespace notepad {

// notepad_screen.cpp 에 정의된 화면->PNG 캡처 / 메모리 캡처.
bool captureRegionToPng(int x, int y, int w, int h, const std::string& outPathUtf8);
bool captureBGRA(int x, int y, int w, int h, std::vector<unsigned char>& out);
// notepad_preprocess.cpp: 세트 AJ 품질 향상 레시피를 메모리 버퍼에 적용.
bool runRecipeBGRA(int& w, int& h, std::vector<unsigned char>& px, const std::wstring& recipe, std::wstring& errStep);
int superResBGRA(int& w, int& h, std::vector<unsigned char>& px, const std::wstring& modelPath,
                 int scale, int tile, int fallback, std::wstring& err);

static std::string wToU8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring u8ToW2(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// ============================================================================
//  [세트 H] Windows.Media.Ocr (WinRT) — 기존 엔진(폴백)
// ============================================================================

// 진단: 실패 사유를 %TEMP%\notepad_ocr_err.txt 에 기록
static void logOcrErr(const std::string& msg) {
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return;
    std::wstring path = std::wstring(dir) + L"notepad_ocr_err.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, msg.data(), (DWORD)msg.size(), &wr, nullptr);
    CloseHandle(h);
}

// 공통: MTA 워커 스레드에서 fn 실행 (STA 데드락/아파트먼트 이슈 회피).
template <typename Fn>
static bool runOnMta(Fn&& fn) {
    bool ok = false;
    std::thread t([&]() {
        bool inited = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            inited = true;
            fn(ok);
        } catch (winrt::hresult_error const& e) {
            char buf[64];
            wsprintfA(buf, "hresult=0x%08X | ", (unsigned)e.code());
            logOcrErr(std::string(buf) + wToU8(std::wstring(e.message().c_str())));
        } catch (std::exception const& e) {
            logOcrErr(std::string("std::exception: ") + e.what());
        } catch (...) {
            logOcrErr("unknown/SEH exception (access violation?)");
        }
        if (inited) {
            try { winrt::clear_factory_cache(); } catch (...) {}
            try { winrt::uninit_apartment(); } catch (...) {}
        }
    });
    t.join();
    return ok;
}

static bool recognizeFile(const std::wstring& wpath, const std::string& langTag,
                          std::string& out) {
    using namespace winrt;
    using namespace winrt::Windows::Globalization;
    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Media::Ocr;
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Streams;

    std::string local;
    bool got = runOnMta([&](bool& ok) {
        StorageFile file = StorageFile::GetFileFromPathAsync(hstring(wpath)).get();
        IRandomAccessStream stream = file.OpenAsync(FileAccessMode::Read).get();
        BitmapDecoder decoder = BitmapDecoder::CreateAsync(stream).get();
        SoftwareBitmap bitmap = decoder.GetSoftwareBitmapAsync().get();

        OcrEngine engine{ nullptr };
        if (!langTag.empty()) {
            Language lang{ hstring(u8ToW2(langTag)) };
            if (OcrEngine::IsLanguageSupported(lang))
                engine = OcrEngine::TryCreateFromLanguage(lang);
        }
        if (!engine)
            engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) { logOcrErr("no OcrEngine (language pack missing?)"); return; }

        OcrResult result = engine.RecognizeAsync(bitmap).get();
        local = wToU8(std::wstring(result.Text().c_str()));
        ok = true;
    });
    if (got) out = local;
    return got;
}

bool ocrImageFile(const std::string& imgPathUtf8, const std::string& langTag,
                  std::string& textOut) {
    textOut.clear();
    std::wstring wpath = u8ToW2(imgPathUtf8);
    if (wpath.empty()) return false;
    return recognizeFile(wpath, langTag, textOut);
}

bool ocrScreenRegion(int x, int y, int w, int h, const std::string& langTag,
                     std::string& textOut) {
    textOut.clear();
    wchar_t dir[MAX_PATH], tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return false;
    if (!GetTempFileNameW(dir, L"npo", 0, tmp)) return false;
    std::string tmpUtf8 = wToU8(std::wstring(tmp));
    bool ok = false;
    if (captureRegionToPng(x, y, w, h, tmpUtf8))
        ok = recognizeFile(std::wstring(tmp), langTag, textOut);
    DeleteFileW(tmp);
    return ok;
}

bool ocrAvailableLanguages(std::string& langsOut) {
    using namespace winrt;
    using namespace winrt::Windows::Media::Ocr;
    langsOut.clear();
    std::string local;
    bool got = runOnMta([&](bool& ok) {
        auto langs = OcrEngine::AvailableRecognizerLanguages();
        for (auto const& l : langs) {
            if (!local.empty()) local += ' ';
            local += wToU8(std::wstring(l.LanguageTag().c_str()));
        }
        ok = true;
    });
    if (got) langsOut = local;
    return got;
}

// ============================================================================
//  [세트 AI] 딥러닝 OCR (PaddleOCR ONNX) — 신규 엔진(권장)
// ============================================================================

// ---- 문자열 변환(AI 전용) --------------------------------------------------
static std::string aiW2U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring aiU82W(const char* p, int len) {
    if (!p || len <= 0) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, p, len, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, len, &w[0], n);
    return w;
}

// ---- 진단 로그 + last error ------------------------------------------------
static std::wstring g_ocrAiErr;
static void ocrAiLog(const std::wstring& msg) {
    g_ocrAiErr = msg;
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return;
    std::wstring path = std::wstring(dir) + L"notepad_ocrai_err.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string u8 = aiW2U8(msg);
    DWORD wr = 0; WriteFile(h, u8.data(), (DWORD)u8.size(), &wr, nullptr); CloseHandle(h);
}
std::wstring ocrAILastError() { return g_ocrAiErr; }

// 자기 자신(DLL)의 폴더 경로.
static std::wstring selfDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&selfDir, &self);
    wchar_t path[MAX_PATH]; DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : s.substr(0, slash + 1);
}
static bool fileExistsW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- GDI+ 이미지 (BGRA top-down) -------------------------------------------
struct Img {
    int w = 0, h = 0;
    std::vector<uint8_t> px; // BGRA
    bool valid() const { return w > 0 && h > 0 && px.size() == (size_t)w * h * 4; }
    uint8_t* at(int x, int y) { return px.data() + ((size_t)y * w + x) * 4; }
    const uint8_t* at(int x, int y) const { return px.data() + ((size_t)y * w + x) * 4; }
};

static ULONG_PTR g_aiGpToken = 0; static bool g_aiGpUp = false;
static void aiEnsureGdiplus() {
    if (g_aiGpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_aiGpToken, &in, nullptr) == Gdiplus::Ok) g_aiGpUp = true;
}

static bool aiLoad(const std::wstring& wpath, Img& img) {
    aiEnsureGdiplus();
    Gdiplus::Bitmap bmp(wpath.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;
    Gdiplus::Rect rc(0, 0, w, h); Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    img.w = w; img.h = h; img.px.resize((size_t)w * h * 4);
    for (int r = 0; r < h; ++r)
        memcpy(img.px.data() + (size_t)r * w * 4, (uint8_t*)bd.Scan0 + (size_t)r * bd.Stride, (size_t)w * 4);
    bmp.UnlockBits(&bd);
    return true;
}

static bool aiResize(const Img& src, int nw, int nh, Img& out) {
    if (!src.valid() || nw <= 0 || nh <= 0) return false;
    aiEnsureGdiplus();
    Gdiplus::Bitmap sbmp(src.w, src.h, PixelFormat32bppARGB);
    { Gdiplus::Rect rc(0, 0, src.w, src.h); Gdiplus::BitmapData bd;
      sbmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd);
      for (int r = 0; r < src.h; ++r)
          memcpy((uint8_t*)bd.Scan0 + (size_t)r * bd.Stride, src.px.data() + (size_t)r * src.w * 4, (size_t)src.w * 4);
      sbmp.UnlockBits(&bd); }
    Gdiplus::Bitmap dbmp(nw, nh, PixelFormat32bppARGB);
    { Gdiplus::Graphics g(&dbmp);
      g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
      g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
      g.DrawImage(&sbmp, 0, 0, nw, nh); }
    out.w = nw; out.h = nh; out.px.resize((size_t)nw * nh * 4);
    { Gdiplus::Rect rc(0, 0, nw, nh); Gdiplus::BitmapData bd;
      dbmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd);
      for (int r = 0; r < nh; ++r)
          memcpy(out.px.data() + (size_t)r * nw * 4, (uint8_t*)bd.Scan0 + (size_t)r * bd.Stride, (size_t)nw * 4);
      dbmp.UnlockBits(&bd); }
    return true;
}

// AABB 크롭(원본에서). 좌표 클램프.
static bool aiCrop(const Img& src, int x1, int y1, int x2, int y2, Img& out) {
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 > src.w - 1) x2 = src.w - 1; if (y2 > src.h - 1) y2 = src.h - 1;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (w < 1 || h < 1) return false;
    out.w = w; out.h = h; out.px.resize((size_t)w * h * 4);
    for (int r = 0; r < h; ++r)
        memcpy(out.at(0, r), src.at(x1, y1 + r), (size_t)w * 4);
    return true;
}
// 180° 회전(제자리).
static void aiRotate180(Img& im) {
    size_t n = (size_t)im.w * im.h;
    uint32_t* p = (uint32_t*)im.px.data();
    for (size_t i = 0, j = n - 1; i < j; ++i, --j) std::swap(p[i], p[j]);
}
// 90°/270°(시계방향) 회전 사본. 세로로 쓰인 글자(기둥 차대번호 등)를 가로로 눕혀 인식하기 위함.
static void aiRotateCW(const Img& src, int deg, Img& out) {
    if (deg == 180) { out = src; aiRotate180(out); return; }
    if (deg != 90 && deg != 270) { out = src; return; }
    out.w = src.h; out.h = src.w; out.px.resize((size_t)out.w * out.h * 4);
    for (int y = 0; y < src.h; ++y) for (int x = 0; x < src.w; ++x) {
        int nx, ny;
        if (deg == 90) { nx = src.h - 1 - y; ny = x; } else { nx = y; ny = src.w - 1 - x; }
        memcpy(out.at(nx, ny), src.at(x, y), 4);
    }
}
// 회전된 좌표계의 박스 → 원래(회전 전) 좌표계. W0/H0 = 회전 전 크기.
static void aiUnrotateBox(int deg, int W0, int H0, int& x1, int& y1, int& x2, int& y2) {
    int a1 = x1, b1 = y1, a2 = x2, b2 = y2;
    if (deg == 90)       { x1 = b1; x2 = b2; y1 = H0 - 1 - a2; y2 = H0 - 1 - a1; }
    else if (deg == 270) { y1 = a1; y2 = a2; x1 = W0 - 1 - b2; x2 = W0 - 1 - b1; }
    else if (deg == 180) { x1 = W0 - 1 - a2; x2 = W0 - 1 - a1; y1 = H0 - 1 - b2; y2 = H0 - 1 - b1; }
}

#ifdef NOTEPAD_ENABLE_ONNX
// ---- ONNX Runtime 세션 (det/rec/cls 상주 캐시) -----------------------------
static void aiPreloadOrt() {
    static bool tried = false; if (tried) return; tried = true;
    std::wstring dir = selfDir();
    if (!dir.empty()) LoadLibraryW((dir + L"onnxruntime.dll").c_str());
}
static const OrtApi* g_ort = nullptr;
static bool aiOrtInit() {
    if (g_ort) return true;
    aiPreloadOrt();
    // 지연로드(build.bat /DELAYLOAD): dll 부재 시 OrtGetApiBase 가 SEH 예외 → 미리 확인
    if (!GetModuleHandleW(L"onnxruntime.dll") && !LoadLibraryW(L"onnxruntime.dll")) {
        ocrAiLog(L"onnxruntime.dll 없음 (NOTEPAD.dll 폴더 / 시스템 모두)"); return false;
    }
    const OrtApiBase* base = OrtGetApiBase();
    if (!base) { ocrAiLog(L"OrtGetApiBase null (onnxruntime.dll 없음?)"); return false; }
    for (int v = ORT_API_VERSION; v >= 11; --v) { g_ort = base->GetApi((uint32_t)v); if (g_ort) return true; }
    ocrAiLog(L"호환 ORT API 없음 (onnxruntime.dll 너무 구버전?)");
    return false;
}

struct OcrSessions {
    OrtEnv* env = nullptr;
    OrtSession* det = nullptr;
    OrtSession* rec = nullptr;
    OrtSession* cls = nullptr;
    OrtSessionOptions* so = nullptr;
    std::wstring dir;
    std::wstring recPath;    // 현재 로드된 인식 모델 경로(세대 전환 감지용)
    std::wstring dictPath;   // 인식 모델과 짝이 되는 사전
    int  recH = 32;          // 인식 입력 높이: v2.0=32, PP-OCRv5=48
    bool haveCls = false;
};
static OcrSessions g_ses;

static OrtSession* aiCreateSession(const std::wstring& path) {
    OrtSession* s = nullptr;
    if (g_ort->CreateSession(g_ses.env, path.c_str(), g_ses.so, &s)) return nullptr;
    return s;
}
// 인식 모델 선택: PP-OCRv5(권장, 48px 입력) 가 있으면 우선, 없으면 기존 v2.0(32px).
//  legacy=true 면 강제로 v2.0 사용(opts 에 "v2" 지정 시).
static void aiPickRecModel(const std::wstring& modelDir, bool legacy,
                           std::wstring& recP, std::wstring& dictP, int& recH) {
    std::wstring v5m = modelDir + L"rec_korean_v5.onnx";
    std::wstring v5d = modelDir + L"korean_dict_v5.txt";
    if (!legacy && fileExistsW(v5m) && fileExistsW(v5d)) {
        recP = v5m; dictP = v5d; recH = 48; return;
    }
    recP = modelDir + L"rec_korean.onnx";
    dictP = modelDir + L"korean_dict.txt";
    recH = 32;
}

static bool aiEnsureSessions(const std::wstring& modelDir, bool legacy) {
    std::wstring recP, dictP; int recH = 32;
    aiPickRecModel(modelDir, legacy, recP, dictP, recH);
    if (g_ses.det && g_ses.rec && g_ses.dir == modelDir && g_ses.recPath == recP) return true;
    if (g_ses.det) { g_ort->ReleaseSession(g_ses.det); g_ses.det = nullptr; }
    if (g_ses.rec) { g_ort->ReleaseSession(g_ses.rec); g_ses.rec = nullptr; }
    if (g_ses.cls) { g_ort->ReleaseSession(g_ses.cls); g_ses.cls = nullptr; }
    g_ses.haveCls = false;
    if (!g_ses.env) { if (g_ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "notepad_ocr", &g_ses.env)) { ocrAiLog(L"CreateEnv 실패"); return false; } }
    if (!g_ses.so) {
        if (g_ort->CreateSessionOptions(&g_ses.so)) { ocrAiLog(L"CreateSessionOptions 실패"); return false; }
        g_ort->SetIntraOpNumThreads(g_ses.so, 0);
        g_ort->SetSessionGraphOptimizationLevel(g_ses.so, ORT_ENABLE_ALL);
    }
    std::wstring detP = modelDir + L"det.onnx";
    std::wstring clsP = modelDir + L"cls.onnx";
    if (!fileExistsW(detP)) { ocrAiLog(L"모델 없음: " + detP); return false; }
    if (!fileExistsW(recP)) { ocrAiLog(L"모델 없음: " + recP); return false; }
    g_ses.det = aiCreateSession(detP);
    if (!g_ses.det) { ocrAiLog(L"det 세션 생성 실패: " + detP); return false; }
    g_ses.rec = aiCreateSession(recP);
    if (!g_ses.rec) { ocrAiLog(L"rec 세션 생성 실패: " + recP); return false; }
    if (fileExistsW(clsP)) { g_ses.cls = aiCreateSession(clsP); g_ses.haveCls = (g_ses.cls != nullptr); }
    g_ses.dir = modelDir;
    g_ses.recPath = recP;
    g_ses.dictPath = dictP;
    g_ses.recH = recH;
    return true;
}

// 단일 입력/단일 출력 실행.
static bool aiRun(OrtSession* sess, const std::vector<float>& input,
                  const std::vector<int64_t>& shape, std::vector<float>& out,
                  std::vector<int64_t>& outShape) {
    out.clear(); outShape.clear();
    OrtMemoryInfo* mem = nullptr;
    if (g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem)) return false;
    OrtValue* inVal = nullptr;
    bool ok = false;
    if (!g_ort->CreateTensorWithDataAsOrtValue(mem, (void*)input.data(), input.size() * sizeof(float),
            shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal)) {
        OrtAllocator* alloc = nullptr; g_ort->GetAllocatorWithDefaultOptions(&alloc);
        char* inName = nullptr; char* outName = nullptr;
        g_ort->SessionGetInputName(sess, 0, alloc, &inName);
        g_ort->SessionGetOutputName(sess, 0, alloc, &outName);
        const char* inNames[] = { inName }; const char* outNames[] = { outName };
        OrtValue* outVal = nullptr;
        OrtStatus* st = g_ort->Run(sess, nullptr, inNames, (const OrtValue* const*)&inVal, 1, outNames, 1, &outVal);
        if (st) { ocrAiLog(L"Run 실패"); g_ort->ReleaseStatus(st); }
        else if (outVal) {
            float* od = nullptr;
            if (!g_ort->GetTensorMutableData(outVal, (void**)&od)) {
                OrtTensorTypeAndShapeInfo* info = nullptr; size_t count = 0;
                if (!g_ort->GetTensorTypeAndShape(outVal, &info)) {
                    g_ort->GetTensorShapeElementCount(info, &count);
                    size_t dims = 0;
                    if (!g_ort->GetDimensionsCount(info, &dims) && dims > 0) {
                        std::vector<int64_t> d(dims);
                        if (!g_ort->GetDimensions(info, d.data(), dims)) outShape = d;
                    }
                    g_ort->ReleaseTensorTypeAndShapeInfo(info);
                }
                out.assign(od, od + count);
                ok = true;
            }
            g_ort->ReleaseValue(outVal);
        }
        if (inName) alloc->Free(alloc, inName);
        if (outName) alloc->Free(alloc, outName);
        g_ort->ReleaseValue(inVal);
    }
    g_ort->ReleaseMemoryInfo(mem);
    return ok;
}

// ---- 텐서 빌더 -------------------------------------------------------------
static void aiToTensorNorm(const Img& im, const float mean[3], const float std_[3],
                           std::vector<float>& t) {
    int W = im.w, H = im.h; t.assign((size_t)3 * H * W, 0.f);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const uint8_t* p = im.at(x, y);
        float r = p[2] / 255.f, g = p[1] / 255.f, b = p[0] / 255.f;
        size_t o = (size_t)y * W + x;
        t[0 * H * W + o] = (r - mean[0]) / std_[0];
        t[1 * H * W + o] = (g - mean[1]) / std_[1];
        t[2 * H * W + o] = (b - mean[2]) / std_[2];
    }
}
static void aiToTensor05(const Img& im, std::vector<float>& t) {
    int W = im.w, H = im.h; t.assign((size_t)3 * H * W, 0.f);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const uint8_t* p = im.at(x, y);
        size_t o = (size_t)y * W + x;
        t[0 * H * W + o] = ((p[2] / 255.f) - 0.5f) / 0.5f;
        t[1 * H * W + o] = ((p[1] / 255.f) - 0.5f) / 0.5f;
        t[2 * H * W + o] = ((p[0] / 255.f) - 0.5f) / 0.5f;
    }
}

// ---- 한국어 사전 -----------------------------------------------------------
static std::vector<std::wstring> g_charset;   // 클래스 k -> charset[k-1], 0=blank
static std::wstring g_charsetDir;
static bool aiLoadDict(const std::wstring& path) {
    if (!g_charset.empty() && g_charsetDir == path) return true;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { ocrAiLog(L"사전 없음: " + path); return false; }
    DWORD sz = GetFileSize(h, nullptr);
    std::string raw; raw.resize(sz); DWORD rd = 0;
    ReadFile(h, &raw[0], sz, &rd, nullptr); CloseHandle(h);
    raw.resize(rd);
    size_t start = 0;
    if (raw.size() >= 3 && (uint8_t)raw[0] == 0xEF && (uint8_t)raw[1] == 0xBB && (uint8_t)raw[2] == 0xBF) start = 3;
    std::vector<std::wstring> cs;
    std::string cur;
    auto flush = [&](std::string& s) {
        if (!s.empty() && s.back() == '\r') s.pop_back();
        cs.push_back(aiU82W(s.data(), (int)s.size()));
    };
    for (size_t i = start; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\n') { flush(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) { flush(cur); }
    while (!cs.empty() && cs.back().empty()) cs.pop_back();
    if (cs.empty()) { ocrAiLog(L"사전 비어있음: " + path); return false; }
    g_charset = std::move(cs); g_charsetDir = path;
    return true;
}

// ---- opts 파싱 -------------------------------------------------------------
struct AiOpts {
    int sideLimit = 960;
    float binThr = 0.3f;
    float boxThr = 0.5f;
    double unclipRatio = 1.6;
    bool useCls = false;
    bool dataMode = false;
    bool legacy = false;   // "v2": 구형 korean v2.0 인식 모델 강제
    int recH = 32;         // aiEnsureSessions 이후 모델에 맞게 덮어씀
    // ---- 2026-09 추가 (세트 AJ 연동 + 인식률/속도) ----
    int scalePct = 100;    // "scale=200": 검출 전 확대 %(작은 글자 인식률↑). enhance 만 주면 200.
    bool enhance = false;  // "enhance": ClearType정리+Lanczos확대+언샤프(text 프리셋) 후 OCR
    bool denoise = false;  // "denoise": 사진(카메라) 노이즈 제거(양방향) 추가
    std::wstring allow;    // "allow=0-9A-Z": 허용 문자만 인식(화이트리스트). vin/alnum/digits 단축키
    bool batch = true;     // "nobatch": 인식 배치 추론 끔(진단용)
    bool sr = false;       // "sr": 딥러닝 초해상도(models\\realesrgan_x4.onnx, x4) 후 인식. 카메라 사진·흐린 글자용
    int rot = 0;           // "rot=90|180|270": 이미지를 돌려서 인식(세로 글자). "rot=auto": 0/90/270 중 최적 자동
    bool rotAuto = false;
};
static bool wieq(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }
static AiOpts parseOpts(const std::wstring& s) {
    AiOpts o; std::wstring tok;
    auto handle = [&](std::wstring t) {
        while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
        while (!t.empty() && iswspace(t.back())) t.pop_back();
        if (t.empty()) return;
        std::wstring key = t, val; size_t eq = t.find(L'=');
        if (eq != std::wstring::npos) { key = t.substr(0, eq); val = t.substr(eq + 1); }
        if (wieq(key, L"cls")) o.useCls = true;
        else if (wieq(key, L"data")) o.dataMode = true;
        else if (wieq(key, L"v2") || wieq(key, L"legacy")) o.legacy = true;
        else if (wieq(key, L"side")) { int v = _wtoi(val.c_str()); if (v >= 64) o.sideLimit = v; }
        else if (wieq(key, L"boxthr")) { int v = _wtoi(val.c_str()); if (v > 0 && v < 100) o.boxThr = v / 100.f; }
        else if (wieq(key, L"binthr")) { int v = _wtoi(val.c_str()); if (v > 0 && v < 100) o.binThr = v / 100.f; }
        else if (wieq(key, L"unclip")) { int v = _wtoi(val.c_str()); if (v > 0) o.unclipRatio = v / 100.0; }
        else if (wieq(key, L"scale")) { int v = _wtoi(val.c_str()); if (v >= 50 && v <= 800) o.scalePct = v; }
        else if (wieq(key, L"enhance")) { o.enhance = true; if (o.scalePct == 100) o.scalePct = 200; }
        else if (wieq(key, L"denoise")) o.denoise = true;
        else if (wieq(key, L"nobatch")) o.batch = false;
        else if (wieq(key, L"sr") || wieq(key, L"superres")) o.sr = true;
        else if (wieq(key, L"allow")) o.allow = val;
        else if (wieq(key, L"vin")) { o.allow = L"0-9A-HJ-NPR-Z"; o.useCls = true; }   // 차대번호: I,O,Q 제외 + 180° 뒤집힘 자동보정(cls)
        else if (wieq(key, L"alnum")) o.allow = L"0-9A-Za-z";
        else if (wieq(key, L"digits")) o.allow = L"0-9.,-";
        else if (wieq(key, L"upper")) o.allow = L"0-9A-Z";
        else if (wieq(key, L"rot")) { if (wieq(val, L"auto")) { o.rotAuto = true; o.useCls = true; } else { int v = _wtoi(val.c_str()); if (v == 90 || v == 180 || v == 270) o.rot = v; } }
        else if (wieq(key, L"vertical") || wieq(key, L"세로")) o.rotAuto = true;
    };
    for (wchar_t c : s) { if (c == L' ' || c == L';' || c == L',' || c == L'|') { handle(tok); tok.clear(); } else tok.push_back(c); }
    handle(tok);
    return o;
}

// ---- 각도분류(180° 보정) ---------------------------------------------------
static void aiClsFix(Img& crop) {
    if (!g_ses.haveCls || !g_ses.cls) return;
    Img r; if (!aiResize(crop, 192, 48, r)) return;
    std::vector<float> t; aiToTensor05(r, t);
    std::vector<int64_t> shape = { 1, 3, 48, 192 }, outShape;
    std::vector<float> out;
    if (!aiRun(g_ses.cls, t, shape, out, outShape) || out.size() < 2) return;
    if (out[1] > out[0] && out[1] > 0.9f) aiRotate180(crop);
}

// ---- 허용 문자 마스크(화이트리스트) ------------------------------------------
//  "0-9A-HJ-NPR-Z" 같은 범위 문자열 → 클래스별 허용 여부. 클래스 0(blank)은 항상 허용.
//  허용되지 않은 클래스의 확률을 0 으로 만들어 CTC 디코드가 허용 문자 중에서만 고르게 한다.
//  차대번호(VIN)·숫자만·영숫자만 같은 도메인에서 오인식(예: 8↔B, 0↔O, 1↔I)을 크게 줄인다.
static std::vector<char> g_allowMask; static std::wstring g_allowKey;
static void aiBuildAllowMask(const std::wstring& allow) {
    if (allow.empty()) { g_allowMask.clear(); g_allowKey.clear(); return; }
    std::wstring key = allow + L"|" + g_charsetDir;
    if (key == g_allowKey) return;
    std::vector<char> ok(0x10000, 0);
    for (size_t i = 0; i < allow.size(); ++i) {
        wchar_t a = allow[i];
        if (i + 2 < allow.size() && allow[i + 1] == L'-') { wchar_t b = allow[i + 2]; if (b < a) std::swap(a, b);
            for (wchar_t c = a; c <= b; ++c) ok[c] = 1; i += 2; }
        else ok[a] = 1;
    }
    g_allowMask.assign(g_charset.size() + 1, 0); g_allowMask[0] = 1;
    for (size_t k = 0; k < g_charset.size(); ++k) {
        const std::wstring& ch = g_charset[k];
        g_allowMask[k + 1] = (ch.size() == 1 && ok[(unsigned)ch[0]]) ? 1 : 0;
    }
    g_allowKey = key;
}
static void aiApplyAllowMask(float* out, int T, int C) {
    if (g_allowMask.empty()) return;
    int n = std::min(C, (int)g_allowMask.size());
    for (int t = 0; t < T; ++t) { float* row = out + (size_t)t * C; for (int c = 1; c < n; ++c) if (!g_allowMask[c]) row[c] = 0.0f; }
}

// 디코드 공용: 로짓(T×C) → 텍스트/점수
static void aiDecodeRow(const float* logits, int T, int C, std::wstring& text, double& score) {
    std::vector<int> idx; std::vector<float> pr;
    notepad::ocralgo::ctcGreedyDecode(logits, T, C, idx, pr);
    double ss = 0; text.clear();
    for (size_t i = 0; i < idx.size(); ++i) {
        int k = idx[i];
        if (k - 1 >= 0 && k - 1 < (int)g_charset.size()) text += g_charset[k - 1];
        ss += pr[i];
    }
    score = idx.empty() ? 0.0 : ss / idx.size();
    text = notepad::ocralgo::composeHangul(text);
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
}

// ---- 인식 배치(여러 크롭 -> 텍스트들) ★속도 ---------------------------------
//  같은 폭 버킷(32px 단위)끼리 최대 16장을 [N,3,H,W] 한 텐서로 묶어 1회 추론.
//  짧은 크롭은 오른쪽을 회색(정규화 0)으로 패딩(PaddleOCR 과 동일). 배치가 실패하면
//  (모델이 동적 배치를 지원하지 않는 경우) 자동으로 1장씩 폴백한다.
static bool g_recBatchOk = true;
static bool aiRecognizeBatch(const std::vector<Img>& crops, const AiOpts& o,
                             std::vector<std::wstring>& texts, std::vector<double>& scores) {
    size_t N = crops.size(); texts.assign(N, L""); scores.assign(N, 0.0);
    struct Item { size_t i; int nw; };
    std::vector<Item> items;
    for (size_t i = 0; i < N; ++i) {
        if (crops[i].w < 2 || crops[i].h < 2) continue;
        int nw = (int)std::lround((double)o.recH * crops[i].w / crops[i].h);
        if (nw < 8) nw = 8; if (nw > 2400) nw = 2400;
        items.push_back({ i, nw });
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.nw < b.nw; });
    const int H = o.recH;
    size_t pos = 0;
    while (pos < items.size()) {
        // 버킷: 첫 항목 폭 기준 +25% 이내, 최대 16장
        size_t end = pos + 1; int wMax = items[pos].nw;
        bool useBatch = o.batch && g_recBatchOk;
        if (useBatch)
            while (end < items.size() && end - pos < 16 && items[end].nw <= items[pos].nw * 5 / 4 + 32) { wMax = items[end].nw; ++end; }
        int W = (wMax + 31) / 32 * 32; if (W < 32) W = 32;
        int n = (int)(end - pos);
        std::vector<float> batch((size_t)n * 3 * H * W, 0.0f);
        for (int k = 0; k < n; ++k) {
            const Img& c = crops[items[pos + k].i];
            Img r; if (!aiResize(c, items[pos + k].nw, H, r)) continue;
            std::vector<float> t; aiToTensor05(r, t);   // [3,H,nw]
            float* dst = batch.data() + (size_t)k * 3 * H * W;
            for (int ch = 0; ch < 3; ++ch) for (int y = 0; y < H; ++y)
                memcpy(dst + ((size_t)ch * H + y) * W, t.data() + ((size_t)ch * H + y) * r.w, (size_t)r.w * sizeof(float));
        }
        std::vector<int64_t> shape = { n, 3, H, W }, outShape; std::vector<float> out;
        bool ok = aiRun(g_ses.rec, batch, shape, out, outShape) && outShape.size() >= 3;
        if (ok) {
            int T = (int)outShape[outShape.size() - 2], C = (int)outShape[outShape.size() - 1];
            int NB = (int)(out.size() / ((size_t)T * C));
            if (NB < n) ok = false;
            else {
                for (int k = 0; k < n; ++k) {
                    float* row = out.data() + (size_t)k * T * C;
                    aiApplyAllowMask(row, T, C);
                    aiDecodeRow(row, T, C, texts[items[pos + k].i], scores[items[pos + k].i]);
                }
            }
        }
        if (!ok) {
            if (n > 1) { g_recBatchOk = false; ocrAiLog(L"rec 배치 추론 미지원 → 1장씩 폴백"); continue; }   // 같은 pos 로 재시도(단일)
            // n==1 실패: 건너뜀
        }
        pos = end;
    }
    return true;
}

// ---- 인식(1 크롭 -> 텍스트) ------------------------------------------------
static bool aiRecognize(const Img& crop, const AiOpts& o, std::wstring& text, double& score) {
    text.clear(); score = 0;
    if (crop.w < 2 || crop.h < 2) return false;
    int nw = (int)std::lround((double)o.recH * crop.w / crop.h);
    if (nw < 8) nw = 8; if (nw > 2400) nw = 2400;
    Img r; if (!aiResize(crop, nw, o.recH, r)) return false;
    std::vector<float> t; aiToTensor05(r, t);
    std::vector<int64_t> shape = { 1, 3, o.recH, nw }, outShape;
    std::vector<float> out;
    if (!aiRun(g_ses.rec, t, shape, out, outShape)) return false;
    if (outShape.size() < 3) return false;
    int T = (int)outShape[outShape.size() - 2];
    int C = (int)outShape[outShape.size() - 1];
    if ((size_t)T * C > out.size()) return false;
    aiApplyAllowMask(out.data(), T, C);
    std::vector<int> idx; std::vector<float> pr;
    notepad::ocralgo::ctcGreedyDecode(out.data(), T, C, idx, pr);
    double ss = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
        int k = idx[i];
        if (k - 1 >= 0 && k - 1 < (int)g_charset.size()) text += g_charset[k - 1];
        ss += pr[i];
    }
    score = idx.empty() ? 0.0 : ss / idx.size();
    text = notepad::ocralgo::composeHangul(text);   // 자모 -> 완성형(v5 사전 대응)
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return true;
}

// ---- 검출(이미지 -> 박스들, 원본좌표) --------------------------------------
struct OBox { int x1, y1, x2, y2; double score; };
static bool aiDetect(const Img& orig, const AiOpts& o, std::vector<OBox>& boxes) {
    boxes.clear();
    int W0 = orig.w, H0 = orig.h;
    double s = 1.0;
    int mx = std::max(W0, H0);
    if (mx > o.sideLimit) s = (double)o.sideLimit / mx;
    int nw = std::max(32, (int)std::lround(W0 * s));
    int nh = std::max(32, (int)std::lround(H0 * s));
    nw = (nw + 31) / 32 * 32; nh = (nh + 31) / 32 * 32;
    Img rs; if (!aiResize(orig, nw, nh, rs)) { ocrAiLog(L"검출 리사이즈 실패"); return false; }
    static const float mean[3] = { 0.485f, 0.456f, 0.406f };
    static const float std_[3] = { 0.229f, 0.224f, 0.225f };
    std::vector<float> t; aiToTensorNorm(rs, mean, std_, t);
    std::vector<int64_t> shape = { 1, 3, nh, nw }, outShape;
    std::vector<float> prob;
    if (!aiRun(g_ses.det, t, shape, prob, outShape)) { ocrAiLog(L"검출 추론 실패"); return false; }
    if (outShape.size() < 2) return false;
    int W = (int)outShape[outShape.size() - 1];
    int H = (int)outShape[outShape.size() - 2];
    if ((size_t)W * H > prob.size()) return false;
    double ratioX = (double)W0 / W, ratioY = (double)H0 / H;
    int minSide = 3, minArea = 9;
    auto comps = notepad::ocralgo::connectedComponents(prob, W, H, o.binThr, minSide, minArea);
    for (auto& c : comps) {
        if (c.meanProb() < o.boxThr) continue;
        int x1 = c.x1, y1 = c.y1, x2 = c.x2, y2 = c.y2;
        notepad::ocralgo::unclip(x1, y1, x2, y2, o.unclipRatio, W, H);
        OBox b;
        b.x1 = (int)std::floor(x1 * ratioX); b.y1 = (int)std::floor(y1 * ratioY);
        b.x2 = (int)std::ceil((x2 + 1) * ratioX) - 1; b.y2 = (int)std::ceil((y2 + 1) * ratioY) - 1;
        if (b.x1 < 0) b.x1 = 0; if (b.y1 < 0) b.y1 = 0;
        if (b.x2 > W0 - 1) b.x2 = W0 - 1; if (b.y2 > H0 - 1) b.y2 = H0 - 1;
        if (b.x2 <= b.x1 || b.y2 <= b.y1) continue;
        b.score = c.meanProb();
        boxes.push_back(b);
    }
    notepad::ocralgo::sortReadingOrder(boxes,
        [](const OBox& b) { return b.x1; }, [](const OBox& b) { return b.y1; }, [](const OBox& b) { return b.y2; });
    return true;
}

static std::mutex g_ocrMutex;

// 핵심: 메모리 이미지(BGRA) -> 인식 텍스트. (파일/화면 진입점이 공용으로 사용)
static bool ocrAIImg(Img& orig, const std::wstring& modelDirIn, const std::wstring& optsStr, std::wstring& out) {
    out.clear(); g_ocrAiErr.clear();
    if (!aiOrtInit()) return false;

    std::wstring modelDir = modelDirIn;
    if (modelDir.empty()) modelDir = selfDir() + L"models\\ocr\\";
    else if (modelDir.back() != L'\\' && modelDir.back() != L'/') modelDir += L"\\";

    AiOpts o = parseOpts(optsStr);

    if (!aiEnsureSessions(modelDir, o.legacy)) return false;
    if (!aiLoadDict(g_ses.dictPath)) return false;
    o.recH = g_ses.recH;   // 인식 모델이 요구하는 입력 높이(v2.0=32 / v5=48)
    aiBuildAllowMask(o.allow);

    // ★ 세트 AJ 연동: 검출 전 품질 향상(작은 글자·저대비·카메라 사진). 좌표는 원본 배율로 환산.
    double pre = 1.0;
    if (o.sr) {   // 딥러닝 초해상도(x4) — 이후 scale 은 무시(이미 4배). 모델 없으면 Lanczos x4 폴백.
        int w = orig.w, h = orig.h; std::wstring err;
        int r = superResBGRA(w, h, orig.px, selfDir() + L"models\\realesrgan_x4.onnx", 4, 128, 1, err);
        if (r) { pre = (double)w / std::max(1, orig.w); orig.w = w; orig.h = h; o.scalePct = 100; }
        else ocrAiLog(L"초해상도 실패(무시): " + err);
    }
    if (o.enhance || o.denoise || o.scalePct != 100) {
        std::wstring recipe;
        auto add = [&](const std::wstring& r) { if (!recipe.empty()) recipe += L"|"; recipe += r; };
        if (o.enhance) add(L"declear:30");
        if (o.scalePct != 100) add(L"upscale:" + std::to_wstring(o.scalePct) + L",lanczos");
        if (o.denoise) add(L"bilateral:2,25");
        if (o.enhance) add(L"unsharp:100,1.2,2");
        int w = orig.w, h = orig.h; std::wstring err;
        if (runRecipeBGRA(w, h, orig.px, recipe, err)) { pre *= (double)w / std::max(1, orig.w); orig.w = w; orig.h = h; }
        else ocrAiLog(L"향상 단계 실패(무시하고 원본으로 진행): " + err);
    }

    // ---- 1회 패스: (회전된) 이미지에서 검출 + 배치 인식 ----
    struct Hit { OBox b; std::wstring text; double sc; };
    auto runPass = [&](const Img& im, std::vector<Hit>& hits, double& meanSc, int& nChars) -> bool {
        hits.clear(); meanSc = 0; nChars = 0;
        std::vector<OBox> boxes;
        if (!aiDetect(im, o, boxes)) return false;
        std::vector<Img> crops(boxes.size());
        for (size_t i = 0; i < boxes.size(); ++i) {
            const OBox& b = boxes[i];
            if (!aiCrop(im, b.x1, b.y1, b.x2, b.y2, crops[i])) { crops[i] = Img(); continue; }
            if (o.useCls) aiClsFix(crops[i]);
        }
        std::vector<std::wstring> texts; std::vector<double> scs;
        aiRecognizeBatch(crops, o, texts, scs);   // ★ 속도: 같은 폭 크롭 묶어 1회 추론
        double ss = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (texts[i].empty()) continue;
            hits.push_back({ boxes[i], texts[i], scs[i] });
            ss += scs[i] * (double)texts[i].size(); nChars += (int)texts[i].size();
        }
        meanSc = nChars ? ss / nChars : 0.0;
        return true;
    };

    // ---- 회전 처리: rot=90/180/270 고정 또는 rot=auto(0 → 부족하면 90/270 시도해 최적 채택) ----
    std::vector<Hit> hits; double meanSc = 0; int nChars = 0; int usedRot = 0;
    const int W0 = orig.w, H0 = orig.h;   // (향상/확대 후) 회전 전 크기
    if (o.rot != 0 && !o.rotAuto) {
        Img r; aiRotateCW(orig, o.rot, r); usedRot = o.rot;
        if (!runPass(r, hits, meanSc, nChars)) return false;
    } else {
        if (!runPass(orig, hits, meanSc, nChars)) return false;
        if (o.rotAuto && (nChars < 4 || meanSc < 0.80)) {
            double bestQ = nChars * meanSc;
            for (int deg : { 90, 270 }) {
                Img r; aiRotateCW(orig, deg, r);
                std::vector<Hit> h2; double m2 = 0; int n2 = 0;
                if (!runPass(r, h2, m2, n2)) continue;
                double q = n2 * m2;
                if (q > bestQ * 1.15 && n2 >= 2) { bestQ = q; hits = std::move(h2); meanSc = m2; nChars = n2; usedRot = deg; }
            }
        }
    }

    // 같은 줄에 있는 박스는 한 줄로 이어 붙인다(검출이 한 줄을 여러 조각으로
    // 나눠도 원문 줄 구조가 유지되도록). data 모드는 박스=1줄 규칙을 지킨다.
    double medH = 16.0;
    if (!hits.empty()) {
        std::vector<int> hs; hs.reserve(hits.size());
        for (auto& h : hits) hs.push_back(h.b.y2 - h.b.y1 + 1);
        std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
        medH = (double)hs[hs.size() / 2];
        if (medH < 1) medH = 1;
    }
    const double rowH = medH * 0.7;
    bool havePrev = false; long prevRow = 0; int prevX2 = 0;

    std::wstring result;
    for (auto& h : hits) {
        const OBox& b = h.b;
        long row = (long)std::floor(((b.y1 + b.y2) * 0.5) / rowH);
        if (!result.empty()) {
            if (!o.dataMode && havePrev && row == prevRow) {
                // 같은 줄: 가로 간격이 있으면 공백 하나, 붙어 있으면 그대로 연결
                result += (b.x1 - prevX2 > (int)(medH * 0.25)) ? L" " : L"";
            } else {
                result += L"\n";
            }
        }
        havePrev = true; prevRow = row; prevX2 = b.x2;
        if (o.dataMode) {
            // 좌표: 회전 되돌림 → 확대 배율 환산 → 원본 이미지 좌표
            int x1 = b.x1, y1 = b.y1, x2 = b.x2, y2 = b.y2;
            aiUnrotateBox(usedRot, W0, H0, x1, y1, x2, y2);
            wchar_t hdr[96];
            int ox = (int)std::lround(x1 / pre), oy = (int)std::lround(y1 / pre);
            int ow = (int)std::lround((x2 - x1 + 1) / pre), oh = (int)std::lround((y2 - y1 + 1) / pre);
            wsprintfW(hdr, L"%d,%d,%d,%d,", ox, oy, ow, oh);
            result += hdr;
            int pct = (int)(h.sc * 100 + 0.5); wchar_t sb[16]; wsprintfW(sb, L"%d\t", pct);
            result += sb;
        }
        result += h.text;
    }
    out = result;
    return true;
}

// 이미지 파일 -> 인식 텍스트. modelDir 비우면 DLL폴더\models\ocr\. 실패 false.
bool ocrAIImage(const std::wstring& imgPath, const std::wstring& modelDirIn,
                const std::wstring& optsStr, std::wstring& out) {
    std::lock_guard<std::mutex> lk(g_ocrMutex);
    out.clear(); g_ocrAiErr.clear();
    Img orig;
    if (!aiLoad(imgPath, orig)) { ocrAiLog(L"이미지 로드 실패: " + imgPath); return false; }
    return ocrAIImg(orig, modelDirIn, optsStr, out);
}

// 화면 영역 -> 인식 텍스트. (★ 속도: 임시 PNG 없이 메모리 캡처 직결)
bool ocrAIScreen(int x, int y, int w, int h, const std::wstring& modelDir,
                 const std::wstring& optsStr, std::wstring& out) {
    std::lock_guard<std::mutex> lk(g_ocrMutex);
    out.clear(); g_ocrAiErr.clear();
    if (w <= 0 || h <= 0) { ocrAiLog(L"영역 크기 오류"); return false; }
    Img orig; orig.w = w; orig.h = h;
    if (!captureBGRA(x, y, w, h, orig.px) || !orig.valid()) { ocrAiLog(L"화면 캡처 실패"); return false; }
    return ocrAIImg(orig, modelDir, optsStr, out);
}

#else  // ------- ONNX 미정의: AI 스텁 -------
bool ocrAIImage(const std::wstring&, const std::wstring&, const std::wstring&, std::wstring& out) {
    out.clear(); ocrAiLog(L"AI OCR 비활성(ONNX 미빌드: /DNOTEPAD_ENABLE_ONNX 필요)"); return false;
}
bool ocrAIScreen(int, int, int, int, const std::wstring&, const std::wstring&, std::wstring& out) {
    out.clear(); ocrAiLog(L"AI OCR 비활성(ONNX 미빌드: /DNOTEPAD_ENABLE_ONNX 필요)"); return false;
}
#endif

} // namespace notepad

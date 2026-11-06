// ============================================================================
//  NOTEPAD.dll  -  notepad_exports2.cpp
//  [NOTEPAD] 세트 AA~AE export 인터페이스 (AHK v1 64비트 DllCall).
//  규약은 notepad_exports.cpp 와 동일: 문자열 UTF-16, 출력버퍼 2단계 호출,
//  성공=문자수/실패=0, bool형 함수는 1/0. def 에 이름 등록 필수.
// ============================================================================
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

using std::wstring;

#ifndef NOTEPAD_API
#define NOTEPAD_API extern "C" __declspec(dllexport)
#endif

// ---- 출력 헬퍼(2단계 버퍼 규약, exports.cpp 와 동일 로직) --------------------
static int writeOut2(const wstring& s, wchar_t* out, int outCch) {
    int need = (int)s.size();
    if (out == nullptr || outCch == 0) return need;
    if (outCch < need + 1) return -need;
    memcpy(out, s.data(), s.size() * sizeof(wchar_t));
    out[s.size()] = L'\0';
    return need;
}
static wstring argW(const wchar_t* p) { return p ? wstring(p) : wstring(); }

// ============================================================================
//  namespace notepad 내부 로직 선언 (각 .cpp 에 정의됨)
// ============================================================================
namespace notepad {
// AA 전처리
bool preprocessFile(const std::wstring& inPath, const std::wstring& outPath,
                    const std::wstring& recipe, int quality, std::wstring& errStep);
// AB ONNX
bool onnxInferCsv(const std::wstring& modelPath, const std::wstring& inputCsv,
                  const std::wstring& shapeCsv, std::wstring& outCsv);
bool argmaxCsv(const std::wstring& logitsCsv, int& idx, double& score);
// AB+ 통짜 객체탐지
bool detectObjectsFile(const std::wstring& modelPath, const std::wstring& imgPath,
                       double confThr, double iouThr, int side, std::wstring& outText);
bool detectObjectsScreen(const std::wstring& modelPath, int x, int y, int w, int h,
                         double confThr, double iouThr, int side, std::wstring& outText);
// AC 고전 ML
bool matchTemplateMultiScale(const std::wstring& needlePath, int x1, int y1, int x2, int y2,
                             int scaleMinPct, int scaleMaxPct, int scaleStepPct, int step,
                             int& fx, int& fy, double& fscore, int& fscalePct);
bool detectColorBBox(int x1, int y1, int x2, int y2, uint32_t rgb, int tol, int minPixels,
                     int& bx, int& by, int& bw, int& bh, int& count);
bool knnClassify(const std::wstring& trainData, const std::wstring& query,
                 std::wstring& labelOut, double& distOut);
// AD 오버레이
bool selectRegionModal(int& ox, int& oy, int& ow, int& oh);
// AH 드래그 캡처 -> 폴더 저장 (학습샘플 수집)
bool captureRegionToFolder(const std::string& saveDirUtf8, const std::string& prefixUtf8,
                           const std::string& extUtf8, int quality, std::string& outPathUtf8);
bool overlayBox(int id, int x, int y, int w, int h, uint32_t argb, int thick);
bool overlayText(int id, int x, int y, const std::wstring& text, uint32_t fgArgb, uint32_t bgArgb, int fontPx);
bool overlayIcon(int id, int x, int y, const std::wstring& pngPath, int w, int h);
bool overlayClear(int id);
bool showCrosshair(int durationMs);
// AE-1 입력/시스템
int setDpiAwareV2();
std::wstring enumMonitorsJson();
int hookStart(); int hookStop(); std::wstring hookStats();
int clipboardImageToPng(const std::wstring& outPng);
int pngToClipboardImage(const std::wstring& inPng);
int mouseMoveAbs(int x, int y);
int mouseButton(int btn, int down);
int mouseWheel(int delta);
int mouseDragSmooth(int x1, int y1, int x2, int y2, int steps, int stepDelayMs);
std::wstring watchFolderOnce(const std::wstring& dir, int subtree, int timeoutMs);
// AE-2 네트워크/캡처
int downloadAsyncStart(const std::wstring& url, const std::wstring& outPath);
std::wstring downloadAsyncStatus(int id);
int downloadAsyncFree(int id);
int wsConnect(const std::wstring& url);
int wsSend(int id, const std::wstring& text);
std::wstring wsReceive(int id);
int wsClose(int id);
// AK HTTP 잡 풀 (notepad_net2.cpp)
int httpJobStart(const std::wstring& method, const std::wstring& url, const std::wstring& headers,
                 const std::wstring& body, int timeoutMs, int maxRetry);
std::wstring httpJobStatus(int id);
std::wstring httpJobResult(int id);
std::wstring httpJobHeaders(int id);
std::wstring httpJobError(int id);
int httpJobFree(int id);
int httpJobWaitAny(int timeoutMs);
int httpJobSetConcurrency(int n);
int httpJobSetMinGap(int ms);
int httpJobCancelAll();
std::wstring httpJobPoolInfo();
int captureFrames(int x, int y, int w, int h, const std::wstring& outDir,
                  const std::wstring& prefix, int count, int fps);
// AF 실전 빈칸
int windowSetTopmost(HWND hwnd, int on);
int windowSetOpacity(HWND hwnd, int alpha);
int windowSetClickThrough(HWND hwnd, int on);
bool windowGetRect(HWND hwnd, std::wstring& out);
int windowMove(HWND hwnd, int x, int y, int w, int h);
HWND windowFromCursor();
long long perfCounter();
long long perfFrequency();
long long perfElapsedUs(long long start, long long end);
void rgbToHsv(uint32_t rgb, int& h, int& s, int& v);
uint32_t hsvToRgb(int h, int s, int v);
int colorDistance(uint32_t a, uint32_t b);
std::wstring iniRead(const std::wstring& path, const std::wstring& section, const std::wstring& key, const std::wstring& def);
int iniWrite(const std::wstring& path, const std::wstring& section, const std::wstring& key, const std::wstring& val);
int iniDelete(const std::wstring& path, const std::wstring& section, const std::wstring& key);
std::wstring iniKeys(const std::wstring& path, const std::wstring& section);
std::wstring detectEncoding(const std::wstring& path);
int convertToUtf8(const std::wstring& inPath, const std::wstring& outPath);
// AG 신경망 학습 분류기 (전이학습 + C++ MLP)
int nnTrain(const std::wstring& backbone, const std::wstring& dir, const std::wstring& modelPath,
            int epochs, int hidden, int augLevel, std::wstring& err);
std::wstring nnClassifyImage(const std::wstring& modelPath, const std::wstring& imgPath, int topK);
std::wstring nnClassifyScreen(const std::wstring& modelPath, int x, int y, int w, int h, int topK);
std::wstring nnDetectScreen(const std::wstring& modelPath, int rx, int ry, int rw, int rh,
                            int win, int stepPct, int minScorePct);
std::wstring nnDetectScreenFast(const std::wstring& modelPath, int rx, int ry, int rw, int rh,
                                int win, int stepPct, int minScorePct, int edgeKeepPct);
std::wstring nnDetectScreenMulti(const std::wstring& modelPath, int rx, int ry, int rw, int rh,
                                 int winMin, int winMax, int nScales, int aspectMode,
                                 int stepPct, int minScorePct, int edgeKeepPct);
std::wstring nnModelInfo(const std::wstring& modelPath);
std::wstring nnDetectImage(const std::wstring& modelPath, const std::wstring& imgPath,
                           int winMin, int winMax, int nScales, int aspectMode,
                           int stepPct, int minScorePct, int edgeKeepPct);
int nnExpandTrainSet(const std::wstring& srcDir, const std::wstring& dstDir,
                     const std::wstring& opts, int enhScalePct, const std::wstring& enhPreset, int quality, std::wstring& err);
int nnSetStrict(int pct);
int nnSetTrainNorm(int on);
std::wstring nnDiagScreen(const std::wstring& modelPath, int x, int y, int w, int h);
std::wstring nnNearestScreen(const std::wstring& modelPath, int x, int y, int w, int h, const std::wstring& trainDir, int topK);
std::wstring nnNearestImage(const std::wstring& modelPath, const std::wstring& imgPath, const std::wstring& trainDir, int topK);
std::wstring nnSimilarRegion(const std::wstring& modelPath, const std::wstring& queryImg, const std::wstring& targetImg, int topK);
std::wstring nnAuditTrainSet(const std::wstring& dir, std::wstring& err);
std::wstring nnCheckSample(const std::wstring& imgPath, const std::wstring& dir, const std::wstring& cls, std::wstring& err);
std::wstring nnCrossVal(const std::wstring& backboneIn, const std::wstring& dir, int k, int epochs, int hidden, int augLevel, std::wstring& err);
int nnImageMaskRect(const std::wstring& inPath, const std::wstring& outPath, int x, int y, int w, int h, int mode, int quality);
// AG-b 특징 기반 분류기 (의존성 0, onnxruntime 불필요)
int trainFolder(const std::wstring& dir, const std::wstring& modelPath, int augLevel);
std::wstring classifyImage(const std::wstring& modelPath, const std::wstring& imgPath, int topK);
std::wstring classifyScreen(const std::wstring& modelPath, int x, int y, int w, int h, int topK);
std::wstring detectLearnedScreen(const std::wstring& modelPath, int rx, int ry, int rw, int rh,
                                 int win, int stepPct, double minScore, int topK);
std::wstring modelInfo(const std::wstring& modelPath);
// AJ 이미지 품질 향상 (notepad_preprocess.cpp)
bool enhanceFile(const std::wstring& inPath, const std::wstring& outPath, int scalePct,
                 const std::wstring& opts, int quality, std::wstring& usedRecipe, std::wstring& errStep);
int inpaintFile(const std::wstring& inPath, const std::wstring& outPath, int mode, uint32_t rgb, int tol, int quality);
std::wstring imageQuality(const std::wstring& inPath);
int superResFile(const std::wstring& inPath, const std::wstring& outPath, const std::wstring& modelPath,
                 int scale, int tile, int fallback, int quality, std::wstring& err);
int enhanceFolder(const std::wstring& srcDir, const std::wstring& dstDir, int scalePct,
                  const std::wstring& opts, int quality);
// AI OCR (세트 AI): 딥러닝 PaddleOCR ONNX
bool ocrAIImage(const std::wstring& imgPath, const std::wstring& modelDir, const std::wstring& opts, std::wstring& out);
bool ocrAIScreen(int x, int y, int w, int h, const std::wstring& modelDir, const std::wstring& opts, std::wstring& out);
std::wstring ocrAILastError();
} // namespace notepad

// ============================================================================
//  [세트 AA] 이미지 전처리
// ============================================================================
// 파이프라인 레시피로 전처리. 성공 1 / 실패 0. 실패 시 Notepad_PreprocessLastError.
static wstring g_ppErr;
NOTEPAD_API int __stdcall Notepad_Preprocess(const wchar_t* inPath, const wchar_t* outPath,
                                             const wchar_t* recipe, int quality) {
    g_ppErr.clear();
    return notepad::preprocessFile(argW(inPath), argW(outPath), argW(recipe), quality, g_ppErr) ? 1 : 0;
}
NOTEPAD_API int __stdcall Notepad_PreprocessLastError(wchar_t* out, int outCch) {
    return writeOut2(g_ppErr, out, outCch);
}

// ============================================================================
//  [세트 AB] ONNX 딥러닝 추론 (Windows ML)
// ============================================================================
// 범용 추론: 입력 float CSV + shape CSV -> 출력 float CSV. 실패 "".
NOTEPAD_API int __stdcall Notepad_OnnxInfer(const wchar_t* modelPath, const wchar_t* inputCsv,
                                            const wchar_t* shapeCsv, wchar_t* out, int outCch) {
    wstring res;
    if (!notepad::onnxInferCsv(argW(modelPath), argW(inputCsv), argW(shapeCsv), res)) return 0;
    return writeOut2(res, out, outCch);
}
// 로짓 CSV -> "index,score" (top-1). 추론 후처리 헬퍼.
NOTEPAD_API int __stdcall Notepad_Argmax(const wchar_t* logitsCsv, wchar_t* out, int outCch) {
    int idx; double score;
    if (!notepad::argmaxCsv(argW(logitsCsv), idx, score)) return 0;
    wchar_t buf[64]; wsprintfW(buf, L"%d,", idx);
    wstring s = buf; s += std::to_wstring(score);
    return writeOut2(s, out, outCch);
}
// 통짜 객체탐지(이미지 파일). 반환: "className,x,y,w,h,score" 개행구분 / "".
//   confThr/iouThr 는 정수 %로 받음(예 conf 25 = 0.25, iou 45 = 0.45). side 보통 640.
NOTEPAD_API int __stdcall Notepad_DetectObjects(const wchar_t* modelPath, const wchar_t* imgPath,
        int confPct, int iouPct, int side, wchar_t* out, int outCch) {
    wstring res;
    double conf = (confPct > 0 ? confPct : 25) / 100.0;
    double iou = (iouPct > 0 ? iouPct : 45) / 100.0;
    if (!notepad::detectObjectsFile(argW(modelPath), argW(imgPath), conf, iou, side, res)) return 0;
    if (res.empty()) return 0;
    return writeOut2(res, out, outCch);
}
// 통짜 객체탐지(화면 영역). 좌표는 화면 절대좌표로 반환.
NOTEPAD_API int __stdcall Notepad_DetectObjectsScreen(const wchar_t* modelPath,
        int x, int y, int w, int h, int confPct, int iouPct, int side, wchar_t* out, int outCch) {
    wstring res;
    double conf = (confPct > 0 ? confPct : 25) / 100.0;
    double iou = (iouPct > 0 ? iouPct : 45) / 100.0;
    if (!notepad::detectObjectsScreen(argW(modelPath), x, y, w, h, conf, iou, side, res)) return 0;
    if (res.empty()) return 0;
    return writeOut2(res, out, outCch);
}

// ============================================================================
//  [세트 AC] 고전 ML / 강건 매칭
// ============================================================================
// 멀티스케일 NCC 템플릿매칭. 반환 "x,y,score,scalePct" / "".
NOTEPAD_API int __stdcall Notepad_MatchTemplate(const wchar_t* needlePath,
        int x1, int y1, int x2, int y2,
        int scaleMinPct, int scaleMaxPct, int scaleStepPct, int step,
        wchar_t* out, int outCch) {
    int fx, fy, fscale; double fscore;
    if (!notepad::matchTemplateMultiScale(argW(needlePath), x1, y1, x2, y2,
            scaleMinPct, scaleMaxPct, scaleStepPct, step, fx, fy, fscore, fscale)) return 0;
    wchar_t buf[96]; wsprintfW(buf, L"%d,%d,", fx, fy);
    wstring s = buf; s += std::to_wstring((int)(fscore * 100) / 100.0);
    wchar_t buf2[32]; wsprintfW(buf2, L",%d", fscale); s += buf2;
    return writeOut2(s, out, outCch);
}
// 색 기반 물체탐지. 반환 "x,y,w,h,count" / "".
NOTEPAD_API int __stdcall Notepad_DetectColor(int x1, int y1, int x2, int y2,
        int colorRgb, int tol, int minPixels, wchar_t* out, int outCch) {
    int bx, by, bw, bh, cnt;
    if (!notepad::detectColorBBox(x1, y1, x2, y2, (uint32_t)colorRgb, tol, minPixels, bx, by, bw, bh, cnt)) return 0;
    wchar_t buf[128]; wsprintfW(buf, L"%d,%d,%d,%d,%d", bx, by, bw, bh, cnt);
    return writeOut2(buf, out, outCch);
}
// k-NN 분류. 반환 "label,distance" / "".
NOTEPAD_API int __stdcall Notepad_KnnClassify(const wchar_t* trainData, const wchar_t* query,
                                              wchar_t* out, int outCch) {
    wstring label; double dist;
    if (!notepad::knnClassify(argW(trainData), argW(query), label, dist)) return 0;
    wstring s = label; s += L","; s += std::to_wstring(dist);
    return writeOut2(s, out, outCch);
}

// ============================================================================
//  [세트 AD] 오버레이 / 영역선택
// ============================================================================
// 드래그로 화면 영역 선택. 반환 "x,y,w,h" / "" (취소/빈선택).
//   ★ 이 함수는 대화형(모달 드래그) 이므로 "부수효과 있는 호출"이다.
//     AHK 의 2단계 규약(길이질의 -> 본호출)으로 호출되면 DllCall 이 2번 나가는데,
//     그대로 두면 드래그 창이 2번 떠서 "두 번 드래그해야 인식" 증상이 난다.
//     -> 첫 호출에서 얻은 결과를 캐시해두고, 이어지는 본호출은 드래그 없이
//        캐시를 그대로 돌려준다(버퍼에 담아 소비하면 캐시 해제).
//     AHK 쪽에서 고정버퍼로 1번만 호출해도 정상 동작한다(캐시가 바로 소비됨).
static std::wstring g_selCache;          // 아직 소비되지 않은 선택 결과
static bool         g_selPending = false;
static DWORD        g_selTick    = 0;    // 캐시 생성 시각(오래된 캐시 방지)
static const DWORD  SEL_CACHE_MS = 30000;

NOTEPAD_API int __stdcall Notepad_SelectRegion(wchar_t* out, int outCch) {
    // 오래 남은 캐시는 폐기(길이질의 후 본호출이 오지 않은 비정상 경로 방어).
    if (g_selPending && (GetTickCount() - g_selTick) > SEL_CACHE_MS) {
        g_selPending = false; g_selCache.clear();
    }
    if (!g_selPending) {
        int x, y, w, h;
        if (!notepad::selectRegionModal(x, y, w, h)) {
            g_selPending = false; g_selCache.clear();
            return 0;                     // 취소/빈선택 -> AHK 는 여기서 종료
        }
        wchar_t buf[64]; wsprintfW(buf, L"%d,%d,%d,%d", x, y, w, h);
        g_selCache = buf; g_selPending = true; g_selTick = GetTickCount();
    }
    // 길이질의(out==null / outCch==0)면 캐시를 유지한 채 길이만 알려준다.
    if (out == nullptr || outCch == 0) return (int)g_selCache.size();
    int r = writeOut2(g_selCache, out, outCch);
    if (r >= 0) { g_selPending = false; g_selCache.clear(); }  // 정상 전달 -> 소비
    return r;                                                  // r<0(버퍼부족)이면 캐시 유지
}
// 박스 오버레이. argb 0xAARRGGBB. 성공 1.
NOTEPAD_API int __stdcall Notepad_OverlayBox(int id, int x, int y, int w, int h, int argb, int thick) {
    return notepad::overlayBox(id, x, y, w, h, (uint32_t)argb, thick) ? 1 : 0;
}
// 텍스트 라벨 오버레이. 성공 1.
NOTEPAD_API int __stdcall Notepad_OverlayText(int id, int x, int y, const wchar_t* text,
                                              int fgArgb, int bgArgb, int fontPx) {
    return notepad::overlayText(id, x, y, argW(text), (uint32_t)fgArgb, (uint32_t)bgArgb, fontPx) ? 1 : 0;
}
// PNG 아이콘 오버레이. w/h<=0 이면 원본 크기. 성공 1.
NOTEPAD_API int __stdcall Notepad_OverlayIcon(int id, int x, int y, const wchar_t* pngPath, int w, int h) {
    return notepad::overlayIcon(id, x, y, argW(pngPath), w, h) ? 1 : 0;
}
// 오버레이 제거. id<0 이면 전체. 성공 1.
NOTEPAD_API int __stdcall Notepad_OverlayClear(int id) {
    return notepad::overlayClear(id) ? 1 : 0;
}
// 크로스헤어 HUD durationMs 동안. 성공 1.
NOTEPAD_API int __stdcall Notepad_ShowCrosshair(int durationMs) {
    return notepad::showCrosshair(durationMs) ? 1 : 0;
}

// ============================================================================
//  [세트 AH] 드래그 캡처 -> 카테고리 폴더에 자동번호 저장 (학습샘플 수집)
// ============================================================================
// UTF-16 -> UTF-8 (이 export 전용 로컬 헬퍼)
static std::string wToUtf8_ah(const wchar_t* w) {
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
// 드래그로 화면 영역을 선택해 saveDir 에 <prefix><다음번호>.png 로 즉시 저장.
//   saveDir : 카테고리 폴더(없으면 자동 생성). prefix : 파일명 접두(""이면 번호만).
//   ext     : "png" 권장(무손실). quality : 예약(현재 png 고정).
//   out/outCch : 저장경로를 받을 버퍼(넉넉히 1024 권장). 취소·실패 시 out[0]='\0'.
//   반환    : 저장한 경로 문자수(>0) / 취소·실패 시 0.
//   ★ 이 함수는 "단일 호출"이다. 드래그 캡처는 1회 동작이므로 길이질의 2단계 규약을
//     쓰지 않는다. AHK 는 넉넉한 고정버퍼를 넘겨 한 번만 호출하면 된다.
NOTEPAD_API int __stdcall Notepad_CaptureRegionToFolder(const wchar_t* saveDir,
        const wchar_t* prefix, const wchar_t* ext, int quality, wchar_t* out, int outCch) {
    if (out && outCch > 0) out[0] = L'\0';
    std::string saveDirU8 = wToUtf8_ah(saveDir);
    std::string prefixU8  = wToUtf8_ah(prefix);
    std::string extU8     = wToUtf8_ah(ext);
    std::string savedU8;
    if (!notepad::captureRegionToFolder(saveDirU8, prefixU8, extU8, quality, savedU8))
        return 0;   // 취소/실패
    // UTF-8 -> UTF-16
    std::wstring wsaved;
    int n = MultiByteToWideChar(CP_UTF8, 0, savedU8.data(), (int)savedU8.size(), nullptr, 0);
    if (n > 0) { wsaved.resize((size_t)n); MultiByteToWideChar(CP_UTF8, 0, savedU8.data(), (int)savedU8.size(), &wsaved[0], n); }
    // 버퍼에 복사(넘치면 잘라서 담되 반환값은 실제 길이).
    if (out && outCch > 0) {
        int cch = (int)wsaved.size();
        int copy = (cch < outCch - 1) ? cch : (outCch - 1);
        if (copy > 0) memcpy(out, wsaved.data(), (size_t)copy * sizeof(wchar_t));
        out[copy] = L'\0';
    }
    return (int)wsaved.size();
}

// ============================================================================
//  [세트 AE-1] 입력/시스템 차별기능
// ============================================================================
NOTEPAD_API int __stdcall Notepad_SetDpiAware() { return notepad::setDpiAwareV2(); }
NOTEPAD_API int __stdcall Notepad_MonitorsJson(wchar_t* out, int outCch) {
    return writeOut2(notepad::enumMonitorsJson(), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_HookStart() { return notepad::hookStart(); }
NOTEPAD_API int __stdcall Notepad_HookStop() { return notepad::hookStop(); }
NOTEPAD_API int __stdcall Notepad_HookStats(wchar_t* out, int outCch) {
    return writeOut2(notepad::hookStats(), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_ClipboardImageToPng(const wchar_t* outPng) {
    return notepad::clipboardImageToPng(argW(outPng));
}
NOTEPAD_API int __stdcall Notepad_PngToClipboardImage(const wchar_t* inPng) {
    return notepad::pngToClipboardImage(argW(inPng));
}
NOTEPAD_API int __stdcall Notepad_MouseMove(int x, int y) { return notepad::mouseMoveAbs(x, y); }
NOTEPAD_API int __stdcall Notepad_MouseButton(int btn, int down) { return notepad::mouseButton(btn, down); }
NOTEPAD_API int __stdcall Notepad_MouseWheel(int delta) { return notepad::mouseWheel(delta); }
NOTEPAD_API int __stdcall Notepad_MouseDrag(int x1, int y1, int x2, int y2, int steps, int stepDelayMs) {
    return notepad::mouseDragSmooth(x1, y1, x2, y2, steps, stepDelayMs);
}
NOTEPAD_API int __stdcall Notepad_WatchFolder(const wchar_t* dir, int subtree, int timeoutMs,
                                              wchar_t* out, int outCch) {
    wstring r = notepad::watchFolderOnce(argW(dir), subtree, timeoutMs);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}

// ============================================================================
//  [세트 AE-2] 비동기 다운로드 / WebSocket / 연속 프레임 캡처
// ============================================================================
NOTEPAD_API int __stdcall Notepad_DownloadStart(const wchar_t* url, const wchar_t* outPath) {
    return notepad::downloadAsyncStart(argW(url), argW(outPath));
}
NOTEPAD_API int __stdcall Notepad_DownloadStatus(int id, wchar_t* out, int outCch) {
    wstring r = notepad::downloadAsyncStatus(id);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_DownloadFree(int id) { return notepad::downloadAsyncFree(id); }
NOTEPAD_API int __stdcall Notepad_WsConnect(const wchar_t* url) { return notepad::wsConnect(argW(url)); }
NOTEPAD_API int __stdcall Notepad_WsSend(int id, const wchar_t* text) { return notepad::wsSend(id, argW(text)); }
NOTEPAD_API int __stdcall Notepad_WsReceive(int id, wchar_t* out, int outCch) {
    wstring r = notepad::wsReceive(id);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_WsClose(int id) { return notepad::wsClose(id); }
NOTEPAD_API int __stdcall Notepad_CaptureFrames(int x, int y, int w, int h,
        const wchar_t* outDir, const wchar_t* prefix, int count, int fps) {
    return notepad::captureFrames(x, y, w, h, argW(outDir), argW(prefix), count, fps);
}

// ============================================================================
//  [세트 AF] AHK v1 실전 빈칸
// ============================================================================
// 창 제어(hwnd 는 AHK 의 ahk_id, 정수). Int64 로 받아 HWND 로 캐스팅.
NOTEPAD_API int __stdcall Notepad_WinSetTopmost(__int64 hwnd, int on) {
    return notepad::windowSetTopmost((HWND)hwnd, on);
}
NOTEPAD_API int __stdcall Notepad_WinSetOpacity(__int64 hwnd, int alpha) {
    return notepad::windowSetOpacity((HWND)hwnd, alpha);
}
NOTEPAD_API int __stdcall Notepad_WinSetClickThrough(__int64 hwnd, int on) {
    return notepad::windowSetClickThrough((HWND)hwnd, on);
}
NOTEPAD_API int __stdcall Notepad_WinGetRect(__int64 hwnd, wchar_t* out, int outCch) {
    wstring r; if (!notepad::windowGetRect((HWND)hwnd, r)) return 0;
    return writeOut2(r, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_WinMove(__int64 hwnd, int x, int y, int w, int h) {
    return notepad::windowMove((HWND)hwnd, x, y, w, h);
}
NOTEPAD_API __int64 __stdcall Notepad_WinFromCursor() {
    return (__int64)notepad::windowFromCursor();
}
// 고해상도 타이머
NOTEPAD_API __int64 __stdcall Notepad_PerfCounter() { return notepad::perfCounter(); }
NOTEPAD_API __int64 __stdcall Notepad_PerfFrequency() { return notepad::perfFrequency(); }
NOTEPAD_API __int64 __stdcall Notepad_PerfElapsedUs(__int64 start, __int64 end) {
    return notepad::perfElapsedUs(start, end);
}
// 색상 변환/거리
NOTEPAD_API int __stdcall Notepad_RgbToHsv(int rgb, wchar_t* out, int outCch) {
    int h, s, v; notepad::rgbToHsv((uint32_t)rgb, h, s, v);
    wchar_t buf[48]; wsprintfW(buf, L"%d,%d,%d", h, s, v);
    return writeOut2(buf, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_HsvToRgb(int h, int s, int v) {
    return (int)notepad::hsvToRgb(h, s, v);
}
NOTEPAD_API int __stdcall Notepad_ColorDistance(int a, int b) {
    return notepad::colorDistance((uint32_t)a, (uint32_t)b);
}
// INI
NOTEPAD_API int __stdcall Notepad_IniRead(const wchar_t* path, const wchar_t* section,
        const wchar_t* key, const wchar_t* def, wchar_t* out, int outCch) {
    return writeOut2(notepad::iniRead(argW(path), argW(section), argW(key), argW(def)), out, outCch);
}
NOTEPAD_API int __stdcall Notepad_IniWrite(const wchar_t* path, const wchar_t* section,
        const wchar_t* key, const wchar_t* val) {
    return notepad::iniWrite(argW(path), argW(section), argW(key), argW(val));
}
NOTEPAD_API int __stdcall Notepad_IniDelete(const wchar_t* path, const wchar_t* section, const wchar_t* key) {
    return notepad::iniDelete(argW(path), argW(section), argW(key));
}
NOTEPAD_API int __stdcall Notepad_IniKeys(const wchar_t* path, const wchar_t* section, wchar_t* out, int outCch) {
    wstring r = notepad::iniKeys(argW(path), argW(section));
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 인코딩 감지/변환
NOTEPAD_API int __stdcall Notepad_DetectEncoding(const wchar_t* path, wchar_t* out, int outCch) {
    wstring r = notepad::detectEncoding(argW(path));
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_ConvertToUtf8(const wchar_t* inPath, const wchar_t* outPath) {
    return notepad::convertToUtf8(argW(inPath), argW(outPath));
}

// ============================================================================
//  [세트 AG] 신경망 학습 분류기 (전이학습 MobileNet + C++ MLP)
// ============================================================================
static wstring g_nnErr;
// 폴더 학습. dir 하위폴더명=클래스. backbone=mobilenetv2.onnx. -> modelPath(.nn).
//   epochs(기본 200), hidden(은닉 뉴런수, 기본 64). 반환: 학습 이미지 수(0=실패).
NOTEPAD_API int __stdcall Notepad_NNTrain(const wchar_t* backbone, const wchar_t* dir,
        const wchar_t* modelPath, int epochs, int hidden, int augLevel) {
    g_nnErr.clear();
    return notepad::nnTrain(argW(backbone), argW(dir), argW(modelPath), epochs, hidden, augLevel, g_nnErr);
}
NOTEPAD_API int __stdcall Notepad_NNLastError(wchar_t* out, int outCch) {
    return writeOut2(g_nnErr, out, outCch);
}
// 이미지 분류. 반환: "label1,prob1;label2,prob2;..." (prob 0~100, softmax) / "".
NOTEPAD_API int __stdcall Notepad_NNClassify(const wchar_t* modelPath, const wchar_t* imgPath,
        int topK, wchar_t* out, int outCch) {
    wstring r = notepad::nnClassifyImage(argW(modelPath), argW(imgPath), topK);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 화면 영역 분류. 반환 동일.
NOTEPAD_API int __stdcall Notepad_NNClassifyScreen(const wchar_t* modelPath,
        int x, int y, int w, int h, int topK, wchar_t* out, int outCch) {
    wstring r = notepad::nnClassifyScreen(argW(modelPath), x, y, w, h, topK);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 신경망 슬라이딩 위치탐지. 화면영역을 격자로 훑어 학습된 것들의 위치를 찾는다.
//   반환: "label,x,y,w,h,score" 개행구분 (Note_ParseDetections 로 파싱) / "".
//   win=셀크기px, stepPct=이동간격%, minScorePct=채택 임계 확률.
NOTEPAD_API int __stdcall Notepad_NNDetectScreen(const wchar_t* modelPath,
        int rx, int ry, int rw, int rh, int win, int stepPct, int minScorePct,
        wchar_t* out, int outCch) {
    wstring r = notepad::nnDetectScreen(argW(modelPath), rx, ry, rw, rh, win, stepPct, minScorePct);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 신경망 위치탐지(하이브리드/고속): 후보영역 제안 → 학습모델 판정. 반환 동일.
//   edgeKeepPct=엣지밀도 상위 몇 %를 후보로 볼지(기본 30).
NOTEPAD_API int __stdcall Notepad_NNDetectScreenFast(const wchar_t* modelPath,
        int rx, int ry, int rw, int rh, int win, int stepPct, int minScorePct, int edgeKeepPct,
        wchar_t* out, int outCch) {
    wstring r = notepad::nnDetectScreenFast(argW(modelPath), rx, ry, rw, rh, win, stepPct, minScorePct, edgeKeepPct);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 신경망 위치탐지(다중 스케일+비율): 정확도 강화. winMin~winMax nScales단계, aspectMode.
NOTEPAD_API int __stdcall Notepad_NNDetectScreenMulti(const wchar_t* modelPath,
        int rx, int ry, int rw, int rh, int winMin, int winMax, int nScales, int aspectMode,
        int stepPct, int minScorePct, int edgeKeepPct, wchar_t* out, int outCch) {
    wstring r = notepad::nnDetectScreenMulti(argW(modelPath), rx, ry, rw, rh,
                    winMin, winMax, nScales, aspectMode, stepPct, minScorePct, edgeKeepPct);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 모델 정보. 반환: "label0;label1;..." / "".
NOTEPAD_API int __stdcall Notepad_NNModelInfo(const wchar_t* modelPath, wchar_t* out, int outCch) {
    wstring r = notepad::nnModelInfo(argW(modelPath));
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// ★ 이미지 파일 안에서 대상 탐지: "label,x,y,w,h,score" 개행구분(원본 좌표). winMin<=0 = 자동 창 크기.
//   전체 사진을 분류기에 통째로 넣지 말고 이걸로 "어디에 무엇이" 있는지 찾은 뒤 그 영역을 쓴다.
NOTEPAD_API int __stdcall Notepad_NNDetectImage(const wchar_t* modelPath, const wchar_t* imgPath,
        int winMin, int winMax, int nScales, int aspectMode, int stepPct, int minScorePct, int edgeKeepPct,
        wchar_t* out, int outCch) {
    wstring r = notepad::nnDetectImage(argW(modelPath), argW(imgPath), winMin, winMax, nScales, aspectMode, stepPct, minScorePct, edgeKeepPct);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// ★ 학습셋 확장 저장: 원본마다 (품질향상+) 증강 변형을 실제 파일로 dstDir 에 저장(기본 PNG, opts "fmt=jpg" 면 JPEG quality).
//   파일명 "<stem>__vN.<ext>" 로 같은 원본끼리 묶여 오픈셋 게이트가 유지된다.
//   저장 폴더로 학습할 땐 augLevel=0 권장(이미 증강됨). 반환: 저장 파일 수(0=실패).
NOTEPAD_API int __stdcall Notepad_NNExpandTrainSet(const wchar_t* srcDir, const wchar_t* dstDir,
        const wchar_t* opts, int enhScalePct, const wchar_t* enhPreset, int quality) {
    g_nnErr.clear();
    return notepad::nnExpandTrainSet(argW(srcDir), argW(dstDir), argW(opts), enhScalePct, argW(enhPreset), quality, g_nnErr);
}
// ★ 오픈셋 엄격도(0=게이트 끔, 50=기본, 100=최대 거절). 반환: 이전 값.
//   학습 분포 밖 입력(엉뚱한 배경)을 "__unknown__" 으로 돌리는 강도. NPNN2 모델부터 동작.
NOTEPAD_API int __stdcall Notepad_NNSetStrict(int pct) {
    return notepad::nnSetStrict(pct);
}
// ★ 학습 시 대비 정규화 on/off (기본 1). 밝기 자체가 단서인 카메라 사진 데이터면 0 으로 실험.
NOTEPAD_API int __stdcall Notepad_NNSetTrainNorm(int on) {
    return notepad::nnSetTrainNorm(on);
}
// ★ 진단: 화면 영역 → "label,prob,sim,thr,gate,featdim,norm". 왜 채택/거절됐는지 확인용.
NOTEPAD_API int __stdcall Notepad_NNDiagScreen(const wchar_t* modelPath,
        int x, int y, int w, int h, wchar_t* out, int outCch) {
    wstring r = notepad::nnDiagScreen(argW(modelPath), x, y, w, h);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// ★ 오탐 추적: 화면 영역/이미지 파일과 가장 닮은 학습 샘플 topK → "경로<TAB>라벨<TAB>유사도%" 개행 구분.
//   오탐을 만든 원인 샘플(잘못 라벨링·너무 닮은 배경)을 찾아 지우거나 예외로 뺄 때. trainDir = 하위폴더가 클래스인 학습 폴더.
NOTEPAD_API int __stdcall Notepad_NNNearestScreen(const wchar_t* modelPath,
        int x, int y, int w, int h, const wchar_t* trainDir, int topK, wchar_t* out, int outCch) {
    wstring r = notepad::nnNearestScreen(argW(modelPath), x, y, w, h, argW(trainDir), topK);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_NNNearestImage(const wchar_t* modelPath, const wchar_t* imgPath,
        const wchar_t* trainDir, int topK, wchar_t* out, int outCch) {
    wstring r = notepad::nnNearestImage(argW(modelPath), argW(imgPath), argW(trainDir), topK);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// ★ 유사 영역: 질의 이미지(오탐 박스)와 가장 닮은 "부분"을 대상 사진 안에서 찾는다 → "x,y,w,h,sim" 개행(원본 좌표).
//   마지막 줄은 사진 전체 "0,0,W,H,sim". 닮은 학습 샘플의 어느 부분이 닮았는지 박스로 보여줄 때.
NOTEPAD_API int __stdcall Notepad_NNSimilarRegion(const wchar_t* modelPath, const wchar_t* queryImg,
        const wchar_t* targetImg, int topK, wchar_t* out, int outCch) {
    wstring r = notepad::nnSimilarRegion(argW(modelPath), argW(queryImg), argW(targetImg), topK);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// ★ 데이터셋 감사(2026-09-05): 학습 폴더의 클래스 분리도·라벨 오류 의심·근사중복·크기 이탈·저품질·클래스 통계를 줄 단위 텍스트로.
//   "key=필드\t필드..." 형식(classes/count/stat/proto/suspect/dup/outlier/lowq). 실패 0(사유 Notepad_NNLastError).
NOTEPAD_API int __stdcall Notepad_NNAuditTrainSet(const wchar_t* dir, wchar_t* out, int outCch) {
    g_nnErr.clear();
    wstring r = notepad::nnAuditTrainSet(argW(dir), g_nnErr);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// ★ 수집 샘플 검사: 새 사진 1장 vs 학습 폴더 → "w\th\t밝기\t대비\t선명도\t같은클래스최대%\t경로\t다른클래스최대%\t라벨\t경로\t클래스장수\t중앙W\t중앙H".
NOTEPAD_API int __stdcall Notepad_NNCheckSample(const wchar_t* imgPath, const wchar_t* dir, const wchar_t* cls, wchar_t* out, int outCch) {
    g_nnErr.clear();
    wstring r = notepad::nnCheckSample(argW(imgPath), argW(dir), argW(cls), g_nnErr);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// ★ k-fold 교차검증: 원본 단위로 나눠 학습/평가 → 클래스별 정답률·혼동행렬·점수 범위. 수 초~수십 초.
NOTEPAD_API int __stdcall Notepad_NNCrossVal(const wchar_t* backbone, const wchar_t* dir, int k, int epochs, int hidden, int augLevel,
        wchar_t* out, int outCch) {
    g_nnErr.clear();
    wstring r = notepad::nnCrossVal(argW(backbone), argW(dir), k, epochs, hidden, augLevel, g_nnErr);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// ★ 영역 마스킹: 사진의 (x,y,w,h) 만 주변색(mode 0)/모자이크(mode 1)로 가려 outPath 로 저장(확장자로 포맷). 성공 1.
NOTEPAD_API int __stdcall Notepad_ImageMaskRect(const wchar_t* inPath, const wchar_t* outPath, int x, int y, int w, int h, int mode, int quality) {
    return notepad::nnImageMaskRect(argW(inPath), argW(outPath), x, y, w, h, mode, quality);
}

// ============================================================================
//  [세트 AG-b] 특징 기반 분류기 (의존성 0, onnxruntime 불필요, 빠름, 위치탐지)
// ============================================================================
// 폴더 학습: dir 하위폴더명=카테고리. -> modelPath(.dat). 반환: 학습 샘플 수(0=실패).
NOTEPAD_API int __stdcall Notepad_TrainFolder(const wchar_t* dir, const wchar_t* modelPath, int augLevel) {
    int n = notepad::trainFolder(argW(dir), argW(modelPath), augLevel);
    return n < 0 ? 0 : n;
}
// 이미지 분류. 반환: "label1,score1;label2,score2;..." (score 0~100, 유사도%) / "".
NOTEPAD_API int __stdcall Notepad_Classify(const wchar_t* modelPath, const wchar_t* imgPath,
        int topK, wchar_t* out, int outCch) {
    wstring r = notepad::classifyImage(argW(modelPath), argW(imgPath), topK);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_ClassifyScreen(const wchar_t* modelPath,
        int x, int y, int w, int h, int topK, wchar_t* out, int outCch) {
    wstring r = notepad::classifyScreen(argW(modelPath), x, y, w, h, topK);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 슬라이딩 위치 탐지. 반환: "label,x,y,w,h,score" 개행구분 / "".
NOTEPAD_API int __stdcall Notepad_DetectLearned(const wchar_t* modelPath,
        int rx, int ry, int rw, int rh, int win, int stepPct, int minScorePct,
        wchar_t* out, int outCch) {
    wstring r = notepad::detectLearnedScreen(argW(modelPath), rx, ry, rw, rh, win, stepPct, (double)minScorePct, 1);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 모델 정보. 반환: "label:count;..." / "".
NOTEPAD_API int __stdcall Notepad_ModelInfo(const wchar_t* modelPath, wchar_t* out, int outCch) {
    wstring r = notepad::modelInfo(argW(modelPath));
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}

// ============================================================================
//  [세트 AI] AI OCR (딥러닝 PaddleOCR ONNX) — EasyOCR급 한국어 인식
// ============================================================================
// 이미지 파일 -> 인식 텍스트(줄바꿈 구분). modelDir 비우면 DLL폴더\models\ocr\.
//   opts: "data"=좌표+점수 포함("x,y,w,h,score<TAB>텍스트"), "cls"=각도보정,
//         "side=960","boxthr=50","binthr=30","unclip=160" 등. 실패 "".
NOTEPAD_API int __stdcall Notepad_OcrImageAI(const wchar_t* imgPath, const wchar_t* modelDir,
        const wchar_t* opts, wchar_t* out, int outCch) {
    wstring res;
    if (!notepad::ocrAIImage(argW(imgPath), argW(modelDir), argW(opts), res)) return 0;
    return writeOut2(res, out, outCch);
}
// 화면 영역 -> 인식 텍스트. 좌표는 화면 절대좌표.
NOTEPAD_API int __stdcall Notepad_OcrScreenAI(int x, int y, int w, int h,
        const wchar_t* modelDir, const wchar_t* opts, wchar_t* out, int outCch) {
    wstring res;
    if (!notepad::ocrAIScreen(x, y, w, h, argW(modelDir), argW(opts), res)) return 0;
    return writeOut2(res, out, outCch);
}
// 직전 AI OCR 실패 사유(진단). 성공 시 "".
NOTEPAD_API int __stdcall Notepad_OcrAIError(wchar_t* out, int outCch) {
    return writeOut2(notepad::ocrAILastError(), out, outCch);
}

// ============================================================================
//  [세트 AJ] 이미지 품질 향상 (의존성 0 + 선택적 ONNX 초해상도)
// ============================================================================
static wstring g_enhRecipe, g_enhErr;
// 원클릭 향상. scalePct=확대 배율%(100=유지), opts="auto"|"text"|"icon"|플래그 조합. 성공 1/0.
NOTEPAD_API int __stdcall Notepad_ImageEnhance(const wchar_t* inPath, const wchar_t* outPath,
        int scalePct, const wchar_t* opts, int quality) {
    g_enhRecipe.clear(); g_enhErr.clear();
    bool ok = notepad::enhanceFile(argW(inPath), argW(outPath), scalePct, argW(opts), quality, g_enhRecipe, g_enhErr);
    return ok ? 1 : 0;
}
// 직전 향상에 실제 적용된 레시피(진단/재현용) 또는 실패 단계.
NOTEPAD_API int __stdcall Notepad_ImageEnhanceInfo(wchar_t* out, int outCch) {
    wstring s = g_enhErr.empty() ? g_enhRecipe : (L"error:" + g_enhErr + L"|" + g_enhRecipe);
    return writeOut2(s, out, outCch);
}
// 손실 픽셀 복원. mode 0=auto(고립 이상치) 1=alpha(투명) 2=color(rgb±tol). 반환: 복원 픽셀 수(-1 실패).
NOTEPAD_API int __stdcall Notepad_ImageInpaint(const wchar_t* inPath, const wchar_t* outPath,
        int mode, unsigned int rgb, int tol, int quality) {
    return notepad::inpaintFile(argW(inPath), argW(outPath), mode, (uint32_t)rgb, tol, quality);
}
// 품질 지표 "w=..;h=..;sharp=..;noise=..;contrast=.." / "".
NOTEPAD_API int __stdcall Notepad_ImageQuality(const wchar_t* inPath, wchar_t* out, int outCch) {
    wstring r = notepad::imageQuality(argW(inPath));
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 딥러닝 초해상도(ONNX). 반환 1=DL, 2=Lanczos 폴백, 0=실패(사유는 Notepad_ImageEnhanceInfo).
NOTEPAD_API int __stdcall Notepad_ImageSuperRes(const wchar_t* inPath, const wchar_t* outPath,
        const wchar_t* modelPath, int scale, int tile, int fallback, int quality) {
    g_enhRecipe.clear(); g_enhErr.clear();
    int r = notepad::superResFile(argW(inPath), argW(outPath), argW(modelPath), scale, tile, fallback, quality, g_enhErr);
    g_enhRecipe = (r == 1) ? L"superres:onnx" : (r == 2) ? L"superres:fallback-lanczos" : L"";
    return r;
}
// 폴더 일괄 향상(하위폴더 구조 유지). 반환: 처리 장수.
NOTEPAD_API int __stdcall Notepad_ImageEnhanceFolder(const wchar_t* srcDir, const wchar_t* dstDir,
        int scalePct, const wchar_t* opts, int quality) {
    return notepad::enhanceFolder(argW(srcDir), argW(dstDir), scalePct, argW(opts), quality);
}

// ============================================================================
//  [세트 AK] HTTP 잡 풀 - 병렬 요청/재시도/타임아웃/동시수 제한 (2026-09-07)
//  규약: Start -> id(>0). WaitAny 로 완료 id 회수 -> Status/Result/Error -> Free.
// ============================================================================
// 잡 등록. method GET/POST/PUT/DELETE, headers "K: V\r\nK2: V2", body 문자열(UTF-8 전송).
// timeoutMs 연결/송수신 타임아웃, maxRetry 재시도 횟수(0~10). 반환 id(>0), 실패 0.
NOTEPAD_API int __stdcall Notepad_HttpJobStart(const wchar_t* method, const wchar_t* url,
        const wchar_t* headers, const wchar_t* body, int timeoutMs, int maxRetry) {
    return notepad::httpJobStart(argW(method), argW(url), argW(headers), argW(body), timeoutMs, maxRetry);
}
// "state,httpStatus,tries,errCode,elapsedMs" (state 0대기 1실행 2완료 3실패). 없는 id "".
NOTEPAD_API int __stdcall Notepad_HttpJobStatus(int id, wchar_t* out, int outCch) {
    wstring r = notepad::httpJobStatus(id);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 응답 본문(완료 후). 빈 본문/미완료 "".
NOTEPAD_API int __stdcall Notepad_HttpJobResult(int id, wchar_t* out, int outCch) {
    wstring r = notepad::httpJobResult(id);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 응답 헤더 원문(CRLF 구분).
NOTEPAD_API int __stdcall Notepad_HttpJobHeaders(int id, wchar_t* out, int outCch) {
    wstring r = notepad::httpJobHeaders(id);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
// 실패 사유 텍스트("[코드] 메시지"). 없으면 "".
NOTEPAD_API int __stdcall Notepad_HttpJobError(int id, wchar_t* out, int outCch) {
    wstring r = notepad::httpJobError(id);
    if (r.empty()) return 0;
    return writeOut2(r, out, outCch);
}
NOTEPAD_API int __stdcall Notepad_HttpJobFree(int id) { return notepad::httpJobFree(id); }
// 완료된(아직 회수 안 한) 잡 id 하나. timeoutMs 안에 없으면 0. 미완료 잡이 없으면 즉시 0.
NOTEPAD_API int __stdcall Notepad_HttpJobWaitAny(int timeoutMs) { return notepad::httpJobWaitAny(timeoutMs); }
// 동시수(1~32) 설정 + 취소/강등/게이트 초기화. 새 배치 전에 호출. 반환 적용값.
NOTEPAD_API int __stdcall Notepad_HttpJobSetConcurrency(int n) { return notepad::httpJobSetConcurrency(n); }
// 요청 시작 최소 간격(ms). 0=제한 없음.
NOTEPAD_API int __stdcall Notepad_HttpJobSetMinGap(int ms) { return notepad::httpJobSetMinGap(ms); }
// 대기 잡 즉시 실패 처리 + 실행 중 잡 재시도 중단. 반환 취소된 대기 잡 수.
NOTEPAD_API int __stdcall Notepad_HttpJobCancelAll() { return notepad::httpJobCancelAll(); }
// "maxConc,active,queued,pending,downshifts,gateWaitMs,minGapMs"
NOTEPAD_API int __stdcall Notepad_HttpJobPoolInfo(wchar_t* out, int outCch) {
    return writeOut2(notepad::httpJobPoolInfo(), out, outCch);
}

// ============================================================================
//  NOTEPAD.dll  -  notepad_onnx.cpp
//  [NOTEPAD] 세트 AB: 딥러닝 추론 (ONNX). Windows ML(OS 내장 ONNX Runtime) 사용.
//  링크: windowsapp.lib (build.bat 에 이미 있음). 의존성 0 - 세트 H(OCR)와 동일 구조.
//
//  배경: 2025-09 Windows ML 정식출시(GA). OS 가 유지하는 ONNX Runtime 을 통해
//        CPU/GPU(DirectML)/NPU 로 .onnx 모델을 실행. 앱은 런타임을 번들하지 않는다.
//        요구: Windows App SDK 지원 OS(Win10 1809+/11). NPU/특정가속은 Win11 24H2+.
//
//  설계: 무거운 텐서 파싱을 DLL 이 다 떠안지 않는다. 이 세트는 두 층으로 제공:
//    (1) 범용 추론기 - 모델 로드 후 float 입력배열 -> float 출력배열 (콤마문자열).
//        전처리(리사이즈/정규화)는 세트 AA 로, 후처리(NMS/argmax)는 AHK 또는
//        세트 AC 로 조합. 어떤 모델이든 태울 수 있는 최소 인터페이스.
//    (2) 이미지 분류 헬퍼 - 이미지 파일 + 입력크기 -> top-1 클래스 인덱스/점수.
//
//  이 파일은 Windows App SDK 의 Microsoft.Windows.AI.MachineLearning /
//  onnxruntime.dll 헤더가 대상 PC 에 있을 때 실제 추론이 활성화된다. 헤더 부재
//  환경에서도 DLL 빌드가 깨지지 않도록 NOTEPAD_ENABLE_ONNX 매크로로 감싼다.
//  (build.bat 에 /DNOTEPAD_ENABLE_ONNX 를 주고, ORT 헤더 include 경로를 추가하면
//   실추론이 켜진다. 미정의 시 스텁이 "" 를 반환하고 사유를 로그에 남긴다.)
// ============================================================================
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>

namespace notepad {

// 진단 로그(%TEMP%\notepad_onnx_err.txt) - OCR 와 동일 패턴
static void logOnnxErr(const std::string& msg) {
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return;
    std::wstring path = std::wstring(dir) + L"notepad_onnx_err.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, msg.data(), (DWORD)msg.size(), &wr, nullptr); CloseHandle(h);
}

static std::vector<float> parseFloats(const std::wstring& csv) {
    std::vector<float> v; std::wstring cur;
    for (wchar_t c : csv) { if (c == L',' || c == L' ' || c == L'\n' || c == L'\t') { if (!cur.empty()) { v.push_back((float)_wtof(cur.c_str())); cur.clear(); } } else cur.push_back(c); }
    if (!cur.empty()) v.push_back((float)_wtof(cur.c_str()));
    return v;
}
static std::wstring floatsToCsv(const std::vector<float>& v) {
    std::wstringstream ss; ss.precision(7);
    for (size_t i = 0; i < v.size(); ++i) { if (i) ss << L','; ss << v[i]; }
    return ss.str();
}

// ====================================================================
//  실추론 (ORT C API). NOTEPAD_ENABLE_ONNX 정의 시 활성화.
//  onnxruntime.dll 은 Windows ML 이 시스템에 배치(또는 App SDK 로 획득).
// ====================================================================
#ifdef NOTEPAD_ENABLE_ONNX
} // namespace notepad (헤더는 전역에서)
#include <onnxruntime_c_api.h>
namespace notepad {

// NOTEPAD.dll 이 있는 폴더의 onnxruntime.dll 을 먼저 명시적으로 로드한다.
// (시스템 PATH 에 구버전 onnxruntime.dll 이 있으면 그게 먼저 잡혀 API 버전이
//  안 맞을 수 있으므로, 우리가 배포한 최신 dll 을 우선 로드해 그 문제를 없앤다.)
extern "C" IMAGE_DOS_HEADER __ImageBase;
static void preloadLocalOrt() {
    static bool tried = false;
    if (tried) return; tried = true;
    wchar_t path[MAX_PATH];
    HMODULE self = (HMODULE)&__ImageBase;
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    // 파일명 자리에 onnxruntime.dll 로 치환
    for (int i = (int)n - 1; i >= 0; --i) {
        if (path[i] == L'\\' || path[i] == L'/') { path[i + 1] = 0; break; }
    }
    std::wstring dll = std::wstring(path) + L"onnxruntime.dll";
    LoadLibraryW(dll.c_str()); // 실패해도 무방(시스템 dll 폴백)
}

static const OrtApi* g_ort = nullptr;
static bool ortInit() {
    if (g_ort) return true;
    preloadLocalOrt();
    // 지연로드(build.bat /DELAYLOAD) 이므로, dll 이 아예 없으면 OrtGetApiBase 호출이 SEH 예외가 된다 → 미리 확인.
    if (!GetModuleHandleW(L"onnxruntime.dll") && !LoadLibraryW(L"onnxruntime.dll")) {
        logOnnxErr("onnxruntime.dll not found (NOTEPAD.dll 폴더 / 시스템 모두 없음)"); return false;
    }
    const OrtApiBase* base = OrtGetApiBase();
    if (!base) { logOnnxErr("OrtGetApiBase null (onnxruntime.dll missing?)"); return false; }
    // 헤더의 ORT_API_VERSION(빌드 시점 버전)을 먼저 시도하되, 시스템에 더 낮은
    // 버전의 onnxruntime.dll 이 먼저 로드될 수 있으므로 버전을 낮춰가며 재시도한다.
    // ORT API 는 하위호환이라 낮은 버전을 요청하면 신·구 dll 모두에서 동작한다.
    for (int v = ORT_API_VERSION; v >= 11; --v) {
        g_ort = base->GetApi((uint32_t)v);
        if (g_ort) {
            if (v != ORT_API_VERSION) {
                char buf[96];
                wsprintfA(buf, "ORT API fallback: using v%d (header was v%d)", v, ORT_API_VERSION);
                logOnnxErr(buf); // 진단용(성공이어도 기록)
            }
            return true;
        }
    }
    logOnnxErr("no compatible ORT API (onnxruntime.dll too old?)");
    return false;
}

// 다른 모듈(세트 AO 텍스트 분류 등)이 같은 ORT API 핸들을 재사용하도록 공개.
// 초기화(로컬 onnxruntime.dll 선로드 + 버전 폴백)는 ortInit() 이 담당.
const OrtApi* ortApiHandle() { return ortInit() ? g_ort : nullptr; }

// 범용: 모델 + float 입력 -> float 출력(+출력 shape). 입력 shape 는 모델 1번째
//   입력에 맞춰 호출자가 flat 배열로 준다. 단일 입력/단일 출력 가정.
bool onnxRunFloatsEx(const std::wstring& modelPath, const std::vector<float>& input,
                     const std::vector<int64_t>& shape, std::vector<float>& output,
                     std::vector<int64_t>& outShape) {
    outShape.clear();
    if (!ortInit()) return false;
    OrtEnv* env = nullptr; OrtSession* sess = nullptr; OrtSessionOptions* so = nullptr;
    bool ok = false;
    do {
        if (g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "notepad", &env)) break;
        if (g_ort->CreateSessionOptions(&so)) break;
        g_ort->SetIntraOpNumThreads(so, 0); // 0=자동(코어수)
        g_ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
        if (g_ort->CreateSession(env, modelPath.c_str(), so, &sess)) { logOnnxErr("CreateSession failed"); break; }

        OrtMemoryInfo* mem = nullptr;
        if (g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem)) break;

        OrtValue* inVal = nullptr;
        if (g_ort->CreateTensorWithDataAsOrtValue(mem, (void*)input.data(),
                input.size() * sizeof(float), shape.data(), shape.size(),
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal)) { g_ort->ReleaseMemoryInfo(mem); break; }

        // 입력/출력 이름
        OrtAllocator* alloc = nullptr; g_ort->GetAllocatorWithDefaultOptions(&alloc);
        char* inName = nullptr; char* outName = nullptr;
        g_ort->SessionGetInputName(sess, 0, alloc, &inName);
        g_ort->SessionGetOutputName(sess, 0, alloc, &outName);
        const char* inNames[] = { inName }; const char* outNames[] = { outName };

        OrtValue* outVal = nullptr;
        OrtStatus* st = g_ort->Run(sess, nullptr, inNames, (const OrtValue* const*)&inVal, 1, outNames, 1, &outVal);
        if (st) { logOnnxErr("Run failed"); g_ort->ReleaseStatus(st); }
        else if (outVal) {
            float* outData = nullptr;
            if (!g_ort->GetTensorMutableData(outVal, (void**)&outData)) {
                OrtTensorTypeAndShapeInfo* info = nullptr;
                size_t count = 0;
                if (!g_ort->GetTensorTypeAndShape(outVal, &info)) {
                    g_ort->GetTensorShapeElementCount(info, &count);
                    size_t dims = 0;
                    if (!g_ort->GetDimensionsCount(info, &dims) && dims > 0) {
                        std::vector<int64_t> d(dims);
                        if (!g_ort->GetDimensions(info, d.data(), dims))
                            outShape = d;
                    }
                    g_ort->ReleaseTensorTypeAndShapeInfo(info);
                }
                output.assign(outData, outData + count);
                ok = true;
            }
            g_ort->ReleaseValue(outVal);
        }
        if (inName) alloc->Free(alloc, inName);
        if (outName) alloc->Free(alloc, outName);
        g_ort->ReleaseValue(inVal);
        g_ort->ReleaseMemoryInfo(mem);
    } while (0);
    if (sess) g_ort->ReleaseSession(sess);
    if (so) g_ort->ReleaseSessionOptions(so);
    if (env) g_ort->ReleaseEnv(env);
    return ok;
}

// ============================================================================
//  ★ 세션 캐시 추론 (속도 핵심)
//  onnxRunFloatsEx 는 호출마다 모델을 디스크에서 로드하고 세션(그래프 최적화)을
//  새로 만들었다 파괴한다 → 셀마다 반복하면 추론보다 이 준비비용이 훨씬 크다.
//  아래는 같은 모델의 세션을 static 으로 1회만 만들어 재사용한다(단일 스레드 가정).
//  경로가 바뀌면 이전 세션을 정리하고 새로 만든다. 배치 입력([N,3,224,224])도 그대로.
// ============================================================================
static OrtEnv* g_cEnv = nullptr;
static OrtSession* g_cSess = nullptr;
static OrtSessionOptions* g_cSo = nullptr;
static std::wstring g_cPath;

static bool ortEnsureSession(const std::wstring& modelPath) {
    if (g_cSess && g_cPath == modelPath) return true;
    if (g_cSess) { g_ort->ReleaseSession(g_cSess); g_cSess = nullptr; }
    if (g_cSo) { g_ort->ReleaseSessionOptions(g_cSo); g_cSo = nullptr; }
    if (!g_cEnv) { if (g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "notepad", &g_cEnv)) return false; }
    if (g_ort->CreateSessionOptions(&g_cSo)) return false;
    g_ort->SetIntraOpNumThreads(g_cSo, 0);
    g_ort->SetSessionGraphOptimizationLevel(g_cSo, ORT_ENABLE_ALL);
    if (g_ort->CreateSession(g_cEnv, modelPath.c_str(), g_cSo, &g_cSess)) {
        logOnnxErr("cached CreateSession failed"); g_cSess = nullptr; return false;
    }
    g_cPath = modelPath;
    return true;
}

bool onnxRunFloatsCached(const std::wstring& modelPath, const std::vector<float>& input,
                         const std::vector<int64_t>& shape, std::vector<float>& output,
                         std::vector<int64_t>& outShape) {
    outShape.clear();
    if (!ortInit()) return false;
    if (!ortEnsureSession(modelPath)) return false;
    OrtMemoryInfo* mem = nullptr;
    if (g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem)) return false;
    OrtValue* inVal = nullptr;
    bool ok = false;
    if (!g_ort->CreateTensorWithDataAsOrtValue(mem, (void*)input.data(),
            input.size() * sizeof(float), shape.data(), shape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal)) {
        OrtAllocator* alloc = nullptr; g_ort->GetAllocatorWithDefaultOptions(&alloc);
        char* inName = nullptr; char* outName = nullptr;
        g_ort->SessionGetInputName(g_cSess, 0, alloc, &inName);
        g_ort->SessionGetOutputName(g_cSess, 0, alloc, &outName);
        const char* inNames[] = { inName }; const char* outNames[] = { outName };
        OrtValue* outVal = nullptr;
        OrtStatus* st = g_ort->Run(g_cSess, nullptr, inNames, (const OrtValue* const*)&inVal, 1, outNames, 1, &outVal);
        if (st) { logOnnxErr("cached Run failed"); g_ort->ReleaseStatus(st); }
        else if (outVal) {
            float* outData = nullptr;
            if (!g_ort->GetTensorMutableData(outVal, (void**)&outData)) {
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
                output.assign(outData, outData + count);
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

#else  // NOTEPAD_ENABLE_ONNX 미정의: 스텁(빌드는 되지만 실행 시 "" + 로그)

bool onnxRunFloatsEx(const std::wstring&, const std::vector<float>&,
                     const std::vector<int64_t>&, std::vector<float>&,
                     std::vector<int64_t>&) {
    logOnnxErr("ONNX disabled: rebuild with /DNOTEPAD_ENABLE_ONNX and ORT headers "
               "(Windows ML / onnxruntime). This stub returns empty.");
    return false;
}
bool onnxRunFloatsCached(const std::wstring&, const std::vector<float>&,
                         const std::vector<int64_t>&, std::vector<float>&,
                         std::vector<int64_t>&) {
    logOnnxErr("ONNX disabled (cached stub).");
    return false;
}

#endif

// 하위호환/범용 래퍼: 출력 shape 이 필요없을 때.
bool onnxRunFloats(const std::wstring& modelPath, const std::vector<float>& input,
                   const std::vector<int64_t>& shape, std::vector<float>& output) {
    std::vector<int64_t> tmp;
    return onnxRunFloatsEx(modelPath, input, shape, output, tmp);
}

// ---- exports 에서 호출하는 래핑 진입점 --------------------------------------

// 범용 추론: 입력 float CSV + shape CSV -> 출력 float CSV. 실패 "".
bool onnxInferCsv(const std::wstring& modelPath, const std::wstring& inputCsv,
                  const std::wstring& shapeCsv, std::wstring& outCsv) {
    std::vector<float> in = parseFloats(inputCsv);
    std::vector<float> shapeF = parseFloats(shapeCsv);
    std::vector<int64_t> shape; for (float f : shapeF) shape.push_back((int64_t)f);
    if (in.empty() || shape.empty()) return false;
    std::vector<float> out;
    if (!onnxRunFloats(modelPath, in, shape, out)) return false;
    outCsv = floatsToCsv(out);
    return true;
}

// top-1: 출력 로짓 CSV 에서 최대 인덱스/값. (분류 후처리 헬퍼, 추론과 독립적으로도 씀)
bool argmaxCsv(const std::wstring& logitsCsv, int& idx, double& score) {
    std::vector<float> v = parseFloats(logitsCsv);
    if (v.empty()) return false;
    int bi = 0; float bv = v[0];
    for (int i = 1; i < (int)v.size(); ++i) if (v[i] > bv) { bv = v[i]; bi = i; }
    idx = bi; score = bv; return true;
}

} // namespace notepad

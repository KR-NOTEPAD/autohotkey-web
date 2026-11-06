// ============================================================================
//  NOTEPAD.dll  -  notepad_nn.cpp
//  [NOTEPAD] 세트 AG: 신경망 학습 분류기 (전이학습 + C++ MLP, DLL 내부 학습).
//  링크: gdiplus.lib (이미 포함). ONNX 추론은 notepad_onnx.cpp 재사용.
//
//  진짜 신경망을 DLL 안에서 학습한다:
//   1) 특징추출(전이학습): 사전학습 MobileNetV2(models\mobilenetv2.onnx)로 각
//      이미지를 1000차원 딥러닝 특징 벡터로 변환. ImageNet 의미공간의 특징이라
//      수제 특징보다 훨씬 표현력이 높다.
//   2) 분류헤드 학습: 그 특징 위에 작은 신경망(MLP: 입력→은닉(ReLU)→소프트맥스)을
//      C++로 순전파+역전파+경사하강(SGD+모멘텀) 직접 구현해 학습. Python·GPU 불필요.
//   3) 저장/추론: 가중치를 .nn 파일로 저장. 추론은 특징추출→MLP→"이름,확률%".
//
//  ONNX 실추론이 켜져 있어야 특징추출이 동작한다(build.bat 이 third_party 감지).
//  스텁이면 학습/추론이 0 을 반환하고 %TEMP%\notepad_onnx_err.txt 에 사유.
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <random>
#include <cstdint>
#include <cstdlib>

#pragma comment(lib, "gdiplus.lib")

namespace notepad {

// notepad_onnx.cpp
bool onnxRunFloatsEx(const std::wstring& modelPath, const std::vector<float>& input,
                     const std::vector<int64_t>& shape, std::vector<float>& output,
                     std::vector<int64_t>& outShape);
bool onnxRunFloatsCached(const std::wstring& modelPath, const std::vector<float>& input,
                         const std::vector<int64_t>& shape, std::vector<float>& output,
                         std::vector<int64_t>& outShape);   // 세션 캐시(반복 추론 고속)
// notepad_screen.cpp
bool captureRegionToPng(int x, int y, int w, int h, const std::string& outPngUtf8);
bool captureBGRA(int x, int y, int w, int h, std::vector<unsigned char>& out);   // 메모리 캡처(고속)
// notepad_preprocess.cpp (세트 AJ 품질향상)
bool enhanceFile(const std::wstring& inPath, const std::wstring& outPath, int scalePct,
                 const std::wstring& opts, int quality, std::wstring& usedRecipe, std::wstring& errStep);

// ---- 공용 ----
static ULONG_PTR g_nnGpToken = 0; static bool g_nnGpUp = false;
static void nnEnsureGdiplus() {
    if (g_nnGpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_nnGpToken, &in, nullptr) == Gdiplus::Ok) g_nnGpUp = true;
}
static std::wstring nnU8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}
static std::string nnWToU8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}

struct NImg { int w = 0, h = 0; std::vector<unsigned char> px; }; // BGRA
// ★ EXIF Orientation 적용(2026-09-05): 폰 사진(카톡·전산 JPG)은 픽셀이 눕혀진 채 저장되고 태그로만 "세워서 보여라"가
//   적혀 있다. GDI+ 는 태그를 무시하므로 그대로 쓰면 학습/판정이 옆으로 누운 사진을 보게 되고, 화면 캡처(세워진 상태)와
//   어긋난다. 여기서 태그대로 돌려 "사람이 보는 방향"으로 통일한다.
static void nnApplyExif(Gdiplus::Bitmap& bmp) {
    UINT sz = bmp.GetPropertyItemSize(PropertyTagOrientation);
    if (sz == 0) return;
    std::vector<unsigned char> buf(sz);
    Gdiplus::PropertyItem* pi = (Gdiplus::PropertyItem*)buf.data();
    if (bmp.GetPropertyItem(PropertyTagOrientation, sz, pi) != Gdiplus::Ok) return;
    if (pi->type != PropertyTagTypeShort || !pi->value) return;
    unsigned short o = *(unsigned short*)pi->value;
    Gdiplus::RotateFlipType rf = Gdiplus::RotateNoneFlipNone; bool apply = true;
    switch (o) {
        case 2: rf = Gdiplus::RotateNoneFlipX; break;   case 3: rf = Gdiplus::Rotate180FlipNone; break;
        case 4: rf = Gdiplus::RotateNoneFlipY; break;   case 5: rf = Gdiplus::Rotate90FlipX; break;
        case 6: rf = Gdiplus::Rotate90FlipNone; break;  case 7: rf = Gdiplus::Rotate270FlipX; break;
        case 8: rf = Gdiplus::Rotate270FlipNone; break; default: apply = false; break;
    }
    if (apply) { bmp.RotateFlip(rf); bmp.RemovePropertyItem(PropertyTagOrientation); }
}
static bool nnLoad(const std::wstring& wpath, NImg& img) {
    nnEnsureGdiplus();
    Gdiplus::Bitmap bmp(wpath.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    nnApplyExif(bmp);
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;
    Gdiplus::Rect rc(0, 0, w, h); Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    img.w = w; img.h = h; img.px.resize((size_t)w * h * 4);
    for (int r = 0; r < h; ++r) memcpy(img.px.data() + (size_t)r * w * 4, (unsigned char*)bd.Scan0 + (size_t)r * bd.Stride, (size_t)w * 4);
    bmp.UnlockBits(&bd);
    return true;
}

// NImg(BGRA) → PNG 파일 저장. 성공 true.
static bool nnSavePng(const NImg& im, const std::wstring& path) {
    nnEnsureGdiplus();
    if (im.w <= 0 || im.h <= 0) return false;
    Gdiplus::Bitmap bmp(im.w, im.h, PixelFormat32bppARGB);
    Gdiplus::Rect rc(0, 0, im.w, im.h); Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    for (int r = 0; r < im.h; ++r)
        memcpy((unsigned char*)bd.Scan0 + (size_t)r * bd.Stride, im.px.data() + (size_t)r * im.w * 4, (size_t)im.w * 4);
    bmp.UnlockBits(&bd);
    // PNG 인코더 CLSID
    UINT num = 0, size = 0; Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<unsigned char> buf(size);
    Gdiplus::ImageCodecInfo* enc = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, enc);
    CLSID clsid{}; bool found = false;
    for (UINT j = 0; j < num; ++j) if (wcscmp(enc[j].MimeType, L"image/png") == 0) { clsid = enc[j].Clsid; found = true; break; }
    if (!found) return false;
    return bmp.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}
// ★ NImg → 파일 저장, 확장자로 포맷 결정: .jpg/.jpeg = JPEG(quality 1~100, 알파 버림), 그 외 = PNG(무손실).
//   nnExpandTrainSet 이 "fmt=jpg" 옵션일 때 사용 — 판정 사진이 전산 JPG 면 학습셋도 같은 질감으로 맞추기 위함.
static bool nnSaveImg(const NImg& im, const std::wstring& path, int quality) {
    size_t d = path.find_last_of(L'.');
    std::wstring e = (d == std::wstring::npos) ? L"" : path.substr(d + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    if (e != L"jpg" && e != L"jpeg") return nnSavePng(im, path);
    nnEnsureGdiplus();
    if (im.w <= 0 || im.h <= 0) return false;
    Gdiplus::Bitmap bmp(im.w, im.h, PixelFormat32bppRGB);   // JPEG 는 알파 없음 → RGB 로
    Gdiplus::Rect rc(0, 0, im.w, im.h); Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppRGB, &bd) != Gdiplus::Ok) return false;
    for (int r = 0; r < im.h; ++r)
        memcpy((unsigned char*)bd.Scan0 + (size_t)r * bd.Stride, im.px.data() + (size_t)r * im.w * 4, (size_t)im.w * 4);
    bmp.UnlockBits(&bd);
    UINT num = 0, size = 0; Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<unsigned char> buf(size);
    Gdiplus::ImageCodecInfo* enc = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, enc);
    CLSID clsid{}; bool found = false;
    for (UINT j = 0; j < num; ++j) if (wcscmp(enc[j].MimeType, L"image/jpeg") == 0) { clsid = enc[j].Clsid; found = true; break; }
    if (!found) return false;
    Gdiplus::EncoderParameters ep; ep.Count = 1; ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
    ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong; ep.Parameter[0].NumberOfValues = 1;
    ULONG qq = (ULONG)(quality < 1 ? 90 : (quality > 100 ? 100 : quality)); ep.Parameter[0].Value = &qq;
    return bmp.Save(path.c_str(), &clsid, &ep) == Gdiplus::Ok;
}

// ---- 전이학습 특징추출 (MobileNetV2 -> 1280-d 임베딩 / 구형 1000-d 로짓) ----
//  ★ v2(2026-09): 백본 기본값을 models\mobilenetv2_embed.onnx(GAP 임베딩 1280-d)로 바꿈.
//    ImageNet 1000 로짓은 "1000개 물체 중 무엇에 가깝나"로 압축된 값이라 차대번호/UI 같은
//    ImageNet 에 없는 대상은 정보가 뭉개진다. 분류기 직전 임베딩은 훨씬 더 잘 구분된다.
//    (make_embed_model.py 로 생성. 없으면 mobilenetv2.onnx 로짓으로 자동 폴백 = 구형 동작)
//  특징 차원은 백본 출력에서 자동으로 정해진다(FEAT_DIM 고정 아님).
static const int FEAT_DIM_LOGITS = 1000;
static std::wstring g_backbone;   // 특징추출 백본 경로(모델과 함께 저장/사용)
// ★ 대비 정규화 스위치(학습·추론 공통). .nn 의 norm= 값으로 로드 시 설정된다.
//   구형 NPNN1 모델은 0(정규화 없음)으로 기존 동작 유지.
static int g_featNorm = 1;

// ★ 대비(밝기) 정규화: 휘도 2%~98% 백분위를 0~1 로 선형 스트레치(채널 공통 계수).
//   "어둡다/밝다" 자체가 특징으로 잡히는 것을 줄인다. 어두운 차대번호 사진과
//   어두운 아무 배경이 "둘 다 어둡다"는 이유로 가까워지던 문제를 완화.
//   균일한 배경은 스트레치 후 노이즈 텍스처가 되어 오히려 대상과 더 멀어진다.
//   학습·추론 양쪽에 똑같이 적용되어야 하므로 toMobilenetInput 안에서만 수행.
static void nnContrastParams(const NImg& im, float& lo, float& hi) {
    lo = 0.0f; hi = 1.0f;
    int N = im.w * im.h; if (N <= 0) return;
    unsigned int hist[256] = { 0 };
    for (int i = 0; i < N; ++i) {
        const unsigned char* p = im.px.data() + (size_t)i * 4; int a = p[3];
        int b = (p[0] * a + 255 * (255 - a)) / 255, g = (p[1] * a + 255 * (255 - a)) / 255, r = (p[2] * a + 255 * (255 - a)) / 255;
        hist[(r * 299 + g * 587 + b * 114) / 1000]++;
    }
    unsigned int c = 0; int l = 0, h = 255;
    unsigned int t2 = (unsigned int)(N * 0.02), t98 = (unsigned int)(N * 0.98);
    for (int i = 0; i < 256; ++i) { c += hist[i]; if (c > t2) { l = i; break; } }
    c = 0;
    for (int i = 0; i < 256; ++i) { c += hist[i]; if (c >= t98) { h = i; break; } }
    if (h - l < 24) { int mid = (h + l) / 2; l = mid - 12; h = mid + 12; }   // 거의 단색: 과도한 증폭 방지
    lo = l / 255.0f; hi = h / 255.0f;
}

// 이미지 -> MobileNetV2 입력 텐서(1x3x224x224, ImageNet 정규화, RGB, NCHW)
//  ★ 개선(인식률): (1) 비율 유지 레터박스 — 가로로 긴 버튼을 정사각으로 찌그러뜨리지 않고
//     테두리 평균색으로 패딩(학습·추론 모두 동일 규칙이라 형태 특징이 보존된다).
//     (2) 최근접 대신 이중선형(확대)/영역평균(축소) 리샘플 — 12×17px 같은 초소형 캡처를
//     224 로 늘릴 때 생기던 계단 블록을 없애 백본 특징이 안정된다.
//     (3) 알파는 흰색에 평탄화. ※ 기존 .nn 은 재학습 권장(특징 분포가 조금 달라짐).
static void toMobilenetInput(const NImg& im, std::vector<float>& chw) {
    const int S = 224;
    chw.assign(3 * S * S, 0.0f);
    const float mean[3] = { 0.485f, 0.456f, 0.406f };
    const float std_[3] = { 0.229f, 0.224f, 0.225f };
    if (im.w <= 0 || im.h <= 0) return;
    // 테두리 평균색(패딩색)
    long br = 0, bg = 0, bb = 0, bn = 0;
    auto addEdge = [&](int x, int y) { const unsigned char* p = im.px.data() + ((size_t)y * im.w + x) * 4; int a = p[3];
        bb += (p[0] * a + 255 * (255 - a)) / 255; bg += (p[1] * a + 255 * (255 - a)) / 255; br += (p[2] * a + 255 * (255 - a)) / 255; ++bn; };
    for (int x = 0; x < im.w; ++x) { addEdge(x, 0); if (im.h > 1) addEdge(x, im.h - 1); }
    for (int y = 1; y < im.h - 1; ++y) { addEdge(0, y); if (im.w > 1) addEdge(im.w - 1, y); }
    float padR = bn ? br / (float)bn / 255.0f : 0.5f, padG = bn ? bg / (float)bn / 255.0f : 0.5f, padB = bn ? bb / (float)bn / 255.0f : 0.5f;
    // ★ 대비 정규화 계수(g_featNorm=1 일 때). 패딩색에도 같은 계수를 적용해 경계 불연속 방지.
    float cLo = 0.0f, cHi = 1.0f;
    if (g_featNorm) nnContrastParams(im, cLo, cHi);
    const float cScale = 1.0f / std::max(1e-3f, cHi - cLo);
    auto stretch = [&](float v) { v = (v - cLo) * cScale; return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); };
    // 레터박스 배치
    double sc = std::min((double)S / im.w, (double)S / im.h);
    int nw = std::max(1, (int)std::lround(im.w * sc)), nh = std::max(1, (int)std::lround(im.h * sc));
    int ox = (S - nw) / 2, oy = (S - nh) / 2;
    bool upscale = sc >= 1.0;
    auto fetch = [&](int sx, int sy, float* rgb) {   // 원본 픽셀(알파 평탄화) 0~1
        sx = std::max(0, std::min(im.w - 1, sx)); sy = std::max(0, std::min(im.h - 1, sy));
        const unsigned char* p = im.px.data() + ((size_t)sy * im.w + sx) * 4; float a = p[3] / 255.0f;
        rgb[0] = (p[2] / 255.0f) * a + (1 - a); rgb[1] = (p[1] / 255.0f) * a + (1 - a); rgb[2] = (p[0] / 255.0f) * a + (1 - a);
    };
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            float rgb[3];
            int lx = x - ox, ly = y - oy;
            if (lx < 0 || ly < 0 || lx >= nw || ly >= nh) { rgb[0] = padR; rgb[1] = padG; rgb[2] = padB; }
            else if (upscale) {   // 이중선형
                float fx = (lx + 0.5f) * im.w / nw - 0.5f, fy = (ly + 0.5f) * im.h / nh - 0.5f;
                int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy); float tx = fx - x0, ty = fy - y0;
                float c00[3], c10[3], c01[3], c11[3];
                fetch(x0, y0, c00); fetch(x0 + 1, y0, c10); fetch(x0, y0 + 1, c01); fetch(x0 + 1, y0 + 1, c11);
                for (int c = 0; c < 3; ++c) rgb[c] = (c00[c] * (1 - tx) + c10[c] * tx) * (1 - ty) + (c01[c] * (1 - tx) + c11[c] * tx) * ty;
            } else {              // 영역 평균(박스 필터) — 축소 시 앨리어싱 방지
                int sx0 = lx * im.w / nw, sx1 = std::max(sx0 + 1, (lx + 1) * im.w / nw);
                int sy0 = ly * im.h / nh, sy1 = std::max(sy0 + 1, (ly + 1) * im.h / nh);
                float acc[3] = { 0,0,0 }; int n = 0;
                for (int sy = sy0; sy < sy1; ++sy) for (int sx = sx0; sx < sx1; ++sx) { float c[3]; fetch(sx, sy, c); acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]; ++n; }
                for (int c = 0; c < 3; ++c) rgb[c] = acc[c] / (n ? n : 1);
            }
            size_t base = (size_t)y * S + x;
            if (g_featNorm) { rgb[0] = stretch(rgb[0]); rgb[1] = stretch(rgb[1]); rgb[2] = stretch(rgb[2]); }
            chw[0 * S * S + base] = (rgb[0] - mean[0]) / std_[0];
            chw[1 * S * S + base] = (rgb[1] - mean[1]) / std_[1];
            chw[2 * S * S + base] = (rgb[2] - mean[2]) / std_[2];
        }
    }
}

// 이미지 -> 특징벡터(1000-d). 백본으로 추론. 실패 false.
// 로짓(FEAT_DIM) -> 안정화(평균 제거) + L2 정규화 → 특징벡터.
//  ★ 임베딩(1280-d, GAP 출력, 비음수)은 제곱근 파워정규화 후 평균제거+L2 — 소수 채널이
//    폭주(burstiness)하는 것을 눌러 코사인 유사도가 훨씬 안정된다(검색/전이학습 표준).
//    로짓(1000-d)은 기존대로 평균제거+L2 (구형 .nn 호환).
static void normalizeLogits(const float* logits, int dim, std::vector<float>& feat) {
    std::vector<float> t(dim);
    const bool embed = (dim != FEAT_DIM_LOGITS);
    for (int i = 0; i < dim; ++i) t[i] = embed ? (logits[i] >= 0 ? std::sqrt(logits[i]) : -std::sqrt(-logits[i])) : logits[i];
    double m = 0; for (int i = 0; i < dim; ++i) m += t[i]; m /= (dim > 0 ? dim : 1);
    double nrm = 0;
    for (int i = 0; i < dim; ++i) { t[i] = (float)(t[i] - m); nrm += (double)t[i] * t[i]; }
    nrm = std::sqrt(nrm); if (nrm < 1e-9) nrm = 1;
    feat.assign(dim, 0.0f);
    for (int i = 0; i < dim; ++i) feat[i] = (float)(t[i] / nrm);
}
static bool extractFeat(const std::wstring& backbone, const NImg& im, std::vector<float>& feat) {
    std::vector<float> chw; toMobilenetInput(im, chw);
    std::vector<int64_t> inShape = { 1, 3, 224, 224 };
    std::vector<int64_t> outShape;
    std::vector<float> out;
    if (!onnxRunFloatsCached(backbone, chw, inShape, out, outShape)) return false;   // 세션 캐시
    if (out.empty()) return false;
    normalizeLogits(out.data(), (int)out.size(), feat);
    return true;
}
// ★ 배치 특징추출: 여러 셀을 [N,3,224,224] 한 번의 추론으로 처리(반복 오버헤드 제거).
//   feats[i] = ims[i] 의 특징(실패한 이미지는 빈 벡터). 반환: 전체 성공 여부.
static bool extractFeatBatch(const std::wstring& backbone, const std::vector<NImg>& ims,
                             std::vector<std::vector<float>>& feats) {
    feats.assign(ims.size(), std::vector<float>());
    if (ims.empty()) return true;
    const int S = 224;
    const size_t per = (size_t)3 * S * S;
    // 메모리 보호: 한 번에 너무 많으면 청크로 나눈다(셀당 ~0.6MB 입력).
    const int CHUNK = 48;
    for (size_t base = 0; base < ims.size(); base += CHUNK) {
        int n = (int)std::min((size_t)CHUNK, ims.size() - base);
        std::vector<float> batch((size_t)n * per);
        for (int i = 0; i < n; ++i) {
            std::vector<float> chw; toMobilenetInput(ims[base + i], chw);
            memcpy(batch.data() + (size_t)i * per, chw.data(), per * sizeof(float));
        }
        std::vector<int64_t> inShape = { n, 3, S, S };
        std::vector<int64_t> outShape; std::vector<float> out;
        if (!onnxRunFloatsCached(backbone, batch, inShape, out, outShape)) return false;
        if (out.empty() || (int)(out.size() % n) != 0) return false;   // 출력 [n,DIM]
        const int DIM = (int)(out.size() / n);
        for (int i = 0; i < n; ++i)
            normalizeLogits(out.data() + (size_t)i * DIM, DIM, feats[base + i]);
    }
    return true;
}
static bool extractFeatFile(const std::wstring& backbone, const std::wstring& path, std::vector<float>& feat) {
    NImg im; if (!nnLoad(path, im)) return false;
    return extractFeat(backbone, im, feat);
}

// ============================================================================
//  신경망 (MLP): 입력(1000) -> 은닉(H, ReLU) -> 출력(C, softmax)
//  순전파 + 역전파 + SGD(모멘텀). 순수 C++.
// ============================================================================
struct MLP {
    int in = FEAT_DIM_LOGITS, hid = 64, out = 0;
    std::vector<float> W1, b1;   // in x hid, hid
    std::vector<float> W2, b2;   // hid x out, out
    std::vector<float> vW1, vb1, vW2, vb2; // 모멘텀

    // ★ 오픈셋(open-set) 게이트 데이터 — NPNN2 부터 .nn 에 함께 저장.
    //   softmax 는 "학습한 클래스 중 무엇에 가장 가깝나"만 답하는 닫힌 집합이라,
    //   전혀 관계없는 입력(검은 배경 등)도 어느 한 클래스로 90%+ 가 나온다.
    //   그래서 학습 특징의 클래스별 프로토타입(평균)과 대표 샘플(exemplar)을 기억해 두고,
    //   추론 시 예측 클래스의 학습 분포와의 코사인 유사도가 임계(thr) 미만이면 점수를
    //   깎고 "__unknown__" 으로 돌린다. (학습 분포 밖 = 모르는 것)
    std::vector<std::vector<float>> proto;   // [out][in] L2 정규화 프로토타입
    std::vector<float> thr;                  // [out] 클래스별 유사도 임계(0~1)
    std::vector<std::vector<float>> ex;      // 대표 샘플 특징(L2 정규화)
    std::vector<int> exLabel;                // 대표 샘플 라벨
    bool hasGate() const { return !proto.empty() && thr.size() == proto.size(); }

    void init(int classes, int hidden, int inDim) {
        out = classes; hid = hidden; in = inDim > 0 ? inDim : in;
        std::mt19937 rng(12345);
        auto he = [&](int fan) { std::normal_distribution<float> d(0.0f, std::sqrt(2.0f / fan)); return d(rng); };
        W1.resize((size_t)in * hid); for (auto& w : W1) w = he(in);
        b1.assign(hid, 0.0f);
        W2.resize((size_t)hid * out); for (auto& w : W2) w = he(hid);
        b2.assign(out, 0.0f);
        vW1.assign(W1.size(), 0.0f); vb1.assign(hid, 0.0f);
        vW2.assign(W2.size(), 0.0f); vb2.assign(out, 0.0f);
    }
    // 순전파. h(ReLU), y(softmax) 채움.
    void forward(const std::vector<float>& x, std::vector<float>& h, std::vector<float>& y) const {
        h.assign(hid, 0.0f);
        // 안전장치: 백본 차원(1280/1000)과 모델 입력 차원이 다르면(백본 파일 불일치) 겹치는 부분만 사용
        const int n = std::min(in, (int)x.size());
        for (int j = 0; j < hid; ++j) {
            float s = b1[j];
            const float* w = &W1[(size_t)j * in]; // W1 저장: [hid][in]
            for (int i = 0; i < n; ++i) s += w[i] * x[i];
            h[j] = s > 0 ? s : 0; // ReLU
        }
        y.assign(out, 0.0f);
        float mx = -1e30f;
        for (int k = 0; k < out; ++k) {
            float s = b2[k];
            const float* w = &W2[(size_t)k * hid];
            for (int j = 0; j < hid; ++j) s += w[j] * h[j];
            y[k] = s; if (s > mx) mx = s;
        }
        float sum = 0; for (int k = 0; k < out; ++k) { y[k] = std::exp(y[k] - mx); sum += y[k]; }
        for (int k = 0; k < out; ++k) y[k] /= (sum > 0 ? sum : 1);
    }
    // 한 샘플 역전파 + 갱신(SGD+모멘텀). label = 정답 클래스. 반환: 손실.
    float trainStep(const std::vector<float>& x, int label, float lr, float mom, float l2) {
        std::vector<float> h, y; forward(x, h, y);
        float loss = -std::log(std::max(1e-7f, y[label]));
        // 출력 그래디언트 dy = y - onehot
        std::vector<float> dy(out);
        for (int k = 0; k < out; ++k) dy[k] = y[k] - (k == label ? 1.0f : 0.0f);
        // W2, b2 grad + 은닉 grad
        std::vector<float> dh(hid, 0.0f);
        for (int k = 0; k < out; ++k) {
            float g = dy[k];
            float* w = &W2[(size_t)k * hid];
            float* vw = &vW2[(size_t)k * hid];
            for (int j = 0; j < hid; ++j) {
                float grad = g * h[j] + l2 * w[j];
                vw[j] = mom * vw[j] - lr * grad;
                dh[j] += g * w[j];
                w[j] += vw[j];
            }
            vb2[k] = mom * vb2[k] - lr * g; b2[k] += vb2[k];
        }
        // ReLU 미분
        for (int j = 0; j < hid; ++j) if (h[j] <= 0) dh[j] = 0;
        // W1, b1 grad
        for (int j = 0; j < hid; ++j) {
            float g = dh[j];
            float* w = &W1[(size_t)j * in];
            float* vw = &vW1[(size_t)j * in];
            for (int i = 0; i < in; ++i) {
                float grad = g * x[i] + l2 * w[i];
                vw[i] = mom * vw[i] - lr * grad;
                w[i] += vw[i];
            }
            vb1[j] = mom * vb1[j] - lr * g; b1[j] += vb1[j];
        }
        return loss;
    }
    int predict(const std::vector<float>& x, std::vector<float>& probOut) const {
        std::vector<float> h; forward(x, h, probOut);
        int best = 0; for (int k = 1; k < out; ++k) if (probOut[k] > probOut[best]) best = k;
        return best;
    }
};

// ★ 오픈셋 엄격도(0~100). 50=학습 통계 그대로, 100=임계 +0.10(더 잘 거절), 0=게이트 끔.
//   Notepad_NNSetStrict 로 조절. 라벨링/탐지 시 오탐이 많으면 올리고, 정답을 거절하면 내린다.
static int g_nnStrict = 50;
static float nnStrictShift() { return (g_nnStrict - 50) * 0.002f; }   // -0.10 ~ +0.10
// ★ 학습 시 대비 정규화 사용 여부(기본 1). 카메라 사진처럼 "밝기 자체"가 클래스 단서인 데이터면
//   0 으로 끄고 학습할 수 있다(.nn 에 norm=0 저장 → 추론도 동일). Notepad_NNSetTrainNorm 로 설정.
static int g_trainNorm = 1;

static float nnCos(const std::vector<float>& a, const std::vector<float>& b) {
    size_t n = std::min(a.size(), b.size()); double s = 0;
    for (size_t i = 0; i < n; ++i) s += (double)a[i] * b[i];
    return (float)s;   // 두 벡터 모두 L2 정규화됨
}

// ★ 게이트 적용 점수 계산 (분류·탐지 공통).
//   1) softmax 확률 prob, 예측 best.
//   2) gate ∈[0,1] : 예측 클래스의 대표 샘플/프로토타입과의 최대 코사인 유사도 sim 이
//      thr 이상이면 1, thr-0.15 이하면 0, 사이는 선형. (게이트 데이터 없으면 1)
//   3) 반환 score = prob[best] * gate. unknownOut = 1-gate (호출자가 "__unknown__" 표시에 사용).
//   __bg__ 예측은 게이트를 적용하지 않는다(배경은 원래 다양하므로).
static int nnScoreGated(const MLP& net, const std::vector<std::wstring>& labels, const std::vector<float>& feat,
                        std::vector<float>& prob, float& score, float& gateOut, float& simOut) {
    int best = net.predict(feat, prob);
    score = prob.empty() ? 0.0f : prob[best]; gateOut = 1.0f; simOut = 1.0f;
    if (g_nnStrict <= 0 || !net.hasGate() || best >= (int)net.proto.size()) return best;
    if (best < (int)labels.size() && labels[best] == L"__bg__") return best;
    float sim = nnCos(feat, net.proto[best]);
    for (size_t i = 0; i < net.ex.size(); ++i)
        if (net.exLabel[i] == best) { float s = nnCos(feat, net.ex[i]); if (s > sim) sim = s; }
    float thr = net.thr[best] + nnStrictShift();
    const float width = 0.15f;
    float gate = (sim - (thr - width)) / width;
    gate = gate < 0 ? 0.0f : (gate > 1 ? 1.0f : gate);
    simOut = sim; gateOut = gate; score = score * gate;
    for (auto& p : prob) p *= gate;   // 호출자가 prob 를 그대로 쓰는 경우를 위해 일괄 축소
    return best;
}

// ============================================================================
//  학습 데이터 증강 (augmentation)
//  이미지 1장에서 크기/밝기 변형 사본을 만들어 학습 샘플을 늘린다.
//  특히 "크기 축소+배경패딩" 변형은, 화면에서 객체가 셀(win)보다 작게 보일 때를
//  미리 학습시켜 탐지 성공률을 크게 올린다(탐지 실패의 최대 원인=크기 불일치).
//  변형 순서(유용도순): 0=원본, 1=축소0.75+패딩, 2=확대1.3+크롭, 3=밝게, 4=어둡게.
// ============================================================================
static void nnMakeVariant(const NImg& src, int v, NImg& dst) {
    dst.w = src.w; dst.h = src.h; dst.px.resize((size_t)src.w * src.h * 4);
    if (v == 0) { dst.px = src.px; return; }
    auto sample = [&](int sx, int sy, unsigned char* out4) {
        sx = std::max(0, std::min(src.w - 1, sx)); sy = std::max(0, std::min(src.h - 1, sy));
        memcpy(out4, src.px.data() + ((size_t)sy * src.w + sx) * 4, 4);
    };
    if (v == 1) {   // 축소 0.75 + 배경(평균색) 패딩
        long b = 0, g = 0, r = 0, a = 0; int N = src.w * src.h;
        for (int i = 0; i < N; ++i) { const unsigned char* p = src.px.data() + (size_t)i * 4; b += p[0]; g += p[1]; r += p[2]; a += p[3]; }
        unsigned char avg[4] = { (unsigned char)(b / N),(unsigned char)(g / N),(unsigned char)(r / N),(unsigned char)(a / N) };
        for (int i = 0; i < N; ++i) memcpy(dst.px.data() + (size_t)i * 4, avg, 4);
        int sw = src.w * 3 / 4, sh = src.h * 3 / 4, ox = (src.w - sw) / 2, oy = (src.h - sh) / 2;
        for (int y = 0; y < sh; ++y) for (int x = 0; x < sw; ++x) {
            unsigned char px[4]; sample(x * src.w / sw, y * src.h / sh, px);
            memcpy(dst.px.data() + ((size_t)(oy + y) * src.w + (ox + x)) * 4, px, 4);
        }
        return;
    }
    if (v == 2) {   // 확대 1.3 + 중앙 크롭(줌인)
        int cw = src.w * 10 / 13, ch = src.h * 10 / 13, cx0 = (src.w - cw) / 2, cy0 = (src.h - ch) / 2;
        for (int y = 0; y < src.h; ++y) for (int x = 0; x < src.w; ++x) {
            unsigned char px[4]; sample(cx0 + x * cw / src.w, cy0 + y * ch / src.h, px);
            memcpy(dst.px.data() + ((size_t)y * src.w + x) * 4, px, 4);
        }
        return;
    }
    // v==5/6: ★ 회전 ±6° (기울어진 차대번호/사진에 강해짐). 빈 모서리는 테두리 평균색.
    //   역방향 매핑 + 이중선형. 학습 이미지가 조금 기울어 찍혀도, 탐지 셀이 대상과 살짝 어긋나도 잡히게.
    if (v == 5 || v == 6) {
        const double deg = (v == 5) ? 6.0 : -6.0, rad = deg * 3.14159265358979 / 180.0;
        const double cs = std::cos(rad), sn = std::sin(rad), cx = (src.w - 1) / 2.0, cy = (src.h - 1) / 2.0;
        long b = 0, g = 0, r = 0, n = 0;
        for (int x = 0; x < src.w; ++x) { const unsigned char* p = src.px.data() + (size_t)x * 4; b += p[0]; g += p[1]; r += p[2]; ++n;
            p = src.px.data() + ((size_t)(src.h - 1) * src.w + x) * 4; b += p[0]; g += p[1]; r += p[2]; ++n; }
        unsigned char pad[4] = { (unsigned char)(b / n), (unsigned char)(g / n), (unsigned char)(r / n), 255 };
        for (int y = 0; y < src.h; ++y) for (int x = 0; x < src.w; ++x) {
            double dx = x - cx, dy = y - cy;
            double sx = cs * dx + sn * dy + cx, sy = -sn * dx + cs * dy + cy;   // 역회전
            unsigned char* o = dst.px.data() + ((size_t)y * src.w + x) * 4;
            if (sx < 0 || sy < 0 || sx > src.w - 1 || sy > src.h - 1) { memcpy(o, pad, 4); continue; }
            int x0 = (int)sx, y0 = (int)sy, x1 = std::min(x0 + 1, src.w - 1), y1 = std::min(y0 + 1, src.h - 1);
            float tx = (float)(sx - x0), ty = (float)(sy - y0);
            const unsigned char* p00 = src.px.data() + ((size_t)y0 * src.w + x0) * 4; const unsigned char* p10 = src.px.data() + ((size_t)y0 * src.w + x1) * 4;
            const unsigned char* p01 = src.px.data() + ((size_t)y1 * src.w + x0) * 4; const unsigned char* p11 = src.px.data() + ((size_t)y1 * src.w + x1) * 4;
            for (int c = 0; c < 4; ++c) {
                float val = (p00[c] * (1 - tx) + p10[c] * tx) * (1 - ty) + (p01[c] * (1 - tx) + p11[c] * tx) * ty;
                o[c] = (unsigned char)(val + 0.5f);
            }
        }
        return;
    }
    // v==3/4: 밝기 조절
    float f = (v == 3) ? 1.2f : 0.8f;
    for (int i = 0; i < src.w * src.h; ++i) {
        const unsigned char* p = src.px.data() + (size_t)i * 4;
        unsigned char* o = dst.px.data() + (size_t)i * 4;
        for (int c = 0; c < 3; ++c) { int val = (int)(p[c] * f); o[c] = (unsigned char)(val > 255 ? 255 : val); }
        o[3] = p[3];
    }
}

// 단일 클래스 학습용 "배경(negative)" 변형 생성.
//   객체 구조를 뭉개 "이건 객체가 아님"을 학습시킨다.
//   0=평균색 단색, 1=강한 블러(8x8 축소→확대), 2=좌우/상하 뒤섞기.
static void nnMakeBg(const NImg& src, int v, NImg& dst) {
    dst.w = src.w; dst.h = src.h; dst.px.resize((size_t)src.w * src.h * 4);
    if (v == 0) {   // 평균색 단색(빈 배경 대표)
        long b=0,g=0,r=0; int N=src.w*src.h;
        for (int i=0;i<N;++i){const unsigned char* p=src.px.data()+(size_t)i*4;b+=p[0];g+=p[1];r+=p[2];}
        unsigned char c[4]={(unsigned char)(b/N),(unsigned char)(g/N),(unsigned char)(r/N),255};
        for (int i=0;i<N;++i) memcpy(dst.px.data()+(size_t)i*4,c,4);
        return;
    }
    if (v == 1) {   // 8x8 로 축소했다 확대 = 강한 블러(구조 소실)
        const int B=8; unsigned char smallbuf[B*B*4];   // 주의: 'small' 은 Windows 매크로라 사용 금지
        for (int y=0;y<B;++y) for (int x=0;x<B;++x) {
            int sx=std::min(src.w-1,x*src.w/B), sy=std::min(src.h-1,y*src.h/B);
            memcpy(smallbuf+((size_t)y*B+x)*4, src.px.data()+((size_t)sy*src.w+sx)*4, 4);
        }
        for (int y=0;y<src.h;++y) for (int x=0;x<src.w;++x) {
            int sx=x*B/src.w, sy=y*B/src.h;
            memcpy(dst.px.data()+((size_t)y*src.w+x)*4, smallbuf+((size_t)sy*B+sx)*4, 4);
        }
        return;
    }
    // v==2: ★ 변경(2026-09) — 예전 "상하좌우 뒤집기"는 특징 수준에서 원본과 거의 같아
    //   "배경"이라 가르치면 오히려 모델을 혼란시켰다(차대번호 뒤집어도 차대번호처럼 보임).
    //   대신 원본의 평균 밝기/편차를 가진 "랜덤 노이즈 텍스처" = 구조 없는 배경 대표.
    {
        long b=0,g=0,r=0; int N=src.w*src.h;
        for (int i=0;i<N;++i){const unsigned char* p=src.px.data()+(size_t)i*4;b+=p[0];g+=p[1];r+=p[2];}
        int mb=(int)(b/N), mg=(int)(g/N), mr=(int)(r/N);
        std::mt19937 rng(4242u + (unsigned)N);
        std::uniform_int_distribution<int> d(-40, 40);
        for (int i=0;i<N;++i){ int n=d(rng); unsigned char* o=dst.px.data()+(size_t)i*4;
            o[0]=(unsigned char)std::max(0,std::min(255,mb+n)); o[1]=(unsigned char)std::max(0,std::min(255,mg+n));
            o[2]=(unsigned char)std::max(0,std::min(255,mr+n)); o[3]=255; }
    }
}

// ============================================================================
//  학습 데이터 수집 (폴더 순회, 특징추출)
// ============================================================================
static bool isImgName(const std::wstring& n) {
    size_t d = n.find_last_of(L'.'); if (d == std::wstring::npos) return false;
    std::wstring e = n.substr(d + 1); for (auto& c : e) c = (wchar_t)towlower(c);
    return e == L"png" || e == L"jpg" || e == L"jpeg" || e == L"bmp" || e == L"gif";
}

// dir 하위폴더명=라벨. 특징X와 라벨y 수집. labels 에 라벨명(인덱스순).
//   augLevel = 이미지당 추가 증강 변형 수(0~4). 원본+augLevel개 학습.
static bool collectData(const std::wstring& backbone, const std::wstring& dir,
                        std::vector<std::vector<float>>& X, std::vector<int>& Y,
                        std::vector<std::wstring>& labels, int augLevel,
                        std::vector<int>* SRC = nullptr,     // SRC: 샘플별 원본 이미지 id(증강 사본은 같은 id)
                        std::vector<int>* ORIG = nullptr) {  // ORIG: 1이면 진짜 원본(v0), 0이면 증강/전처리 사본
    if (augLevel < 0) augLevel = 0; if (augLevel > 6) augLevel = 6;   // 5/6 = 회전 ±6° 추가
    int srcId = 0;
    std::vector<std::pair<std::wstring, int>> groupMap;   // 파일명 그룹키 → srcId (nnExpandTrainSet 산물 묶기)
    auto scan = [&](const std::wstring& folder, int labelIdx) {
        std::wstring pat = folder + L"\\*";
        WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
        if (hf == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = fd.cFileName;
            if (!isImgName(name)) continue;
            NImg im;
            if (!nnLoad(folder + L"\\" + name, im)) continue;
            // ★ 같은 원본에서 나온 미리-증강 파일("<stem>__vN.png")은 하나의 원본(srcId)으로 묶는다.
            //   그래야 오픈셋 게이트가 회전본을 '다른 원본'으로 오인하지 않는다. "__" 없으면 파일마다 별개.
            size_t us = name.find(L"__");
            if (us != std::wstring::npos) {
                std::wstring key = folder + L"|" + name.substr(0, us);
                int gid = -1;
                for (auto& p : groupMap) if (p.first == key) { gid = p.second; break; }
                if (gid < 0) { gid = ++srcId; groupMap.push_back({ key, gid }); }
                // "__v0" 를 담은 파일만 진짜 원본으로 표시(게이트 통계용). 나머지 __xxx 는 사본.
                int isOrig = (name.find(L"__v0") != std::wstring::npos) ? 1 : 0;
                {
                    std::vector<float> feat0;
                    if (extractFeat(backbone, im, feat0)) { X.push_back(std::move(feat0)); Y.push_back(labelIdx); if (SRC) SRC->push_back(gid); if (ORIG) ORIG->push_back(isOrig); }
                }
                // 미리-증강 파일은 그 자체가 변형이므로 추가 증강은 하지 않는다(파일이 이미 증강본).
                continue;
            }
            ++srcId;
            for (int v = 0; v <= augLevel; ++v) {   // v=0 원본 + 증강
                NImg var; nnMakeVariant(im, v, var);
                std::vector<float> feat;
                if (extractFeat(backbone, var, feat)) { X.push_back(std::move(feat)); Y.push_back(labelIdx); if (SRC) SRC->push_back(srcId); if (ORIG) ORIG->push_back(v == 0 ? 1 : 0); }
            }
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    };
    // 하위폴더 = 클래스
    std::wstring pat = dir + L"\\*";
    WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        int idx = (int)labels.size(); labels.push_back(name);
        scan(dir + L"\\" + name, idx);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return !X.empty();
}

// ============================================================================
//  모델 저장/로드 (.nn 텍스트)
// ============================================================================
static bool saveNN(const std::wstring& path, const MLP& net, const std::vector<std::wstring>& labels,
                   const std::wstring& backbone) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string out = "NPNN2\n";
    out += "backbone=" + nnWToU8(backbone) + "\n";
    out += "norm=" + std::to_string(g_featNorm) + "\n";
    out += "in=" + std::to_string(net.in) + " hid=" + std::to_string(net.hid) + " out=" + std::to_string(net.out) + "\n";
    out += "labels=";
    for (size_t i = 0; i < labels.size(); ++i) { if (i) out += "\t"; out += nnWToU8(labels[i]); }
    out += "\n";
    char buf[24];
    auto dumpv = [&](const char* name, const std::vector<float>& v) {
        out += name; out += "=";
        for (size_t i = 0; i < v.size(); ++i) { if (i) out += ' '; snprintf(buf, sizeof(buf), "%.6g", v[i]); out += buf; }
        out += "\n";
    };
    dumpv("W1", net.W1); dumpv("b1", net.b1); dumpv("W2", net.W2); dumpv("b2", net.b2);
    // ★ 오픈셋 게이트 데이터(NPNN2): 클래스별 프로토타입/임계 + 대표 샘플.
    if (net.hasGate()) {
        dumpv("thr", net.thr);
        auto dumpc = [&](const char* name, int idx, const std::vector<float>& v) {   // 짧은 포맷(용량)
            out += name; out += "="; out += std::to_string(idx); out += ":";
            for (size_t i = 0; i < v.size(); ++i) { if (i) out += ' '; snprintf(buf, sizeof(buf), "%.4g", v[i]); out += buf; }
            out += "\n";
        };
        for (size_t c = 0; c < net.proto.size(); ++c) dumpc("proto", (int)c, net.proto[c]);
        for (size_t i = 0; i < net.ex.size(); ++i) dumpc("ex", net.exLabel[i], net.ex[i]);
    }
    DWORD w; WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr); CloseHandle(h);
    return true;
}

static std::vector<std::string> splitLines(const std::string& data) {
    std::vector<std::string> lines; size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) { lines.push_back(data.substr(pos)); break; }
        std::string ln = data.substr(pos, nl - pos);
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        lines.push_back(ln); pos = nl + 1;
    }
    return lines;
}
static void parseFloatsInto(const std::string& s, std::vector<float>& v) {
    std::string cur; for (char c : s) { if (c == ' ') { if (!cur.empty()) { v.push_back((float)atof(cur.c_str())); cur.clear(); } } else cur.push_back(c); }
    if (!cur.empty()) v.push_back((float)atof(cur.c_str()));
}
static std::wstring nnU8ToW2(const std::string& s) { return nnU8ToW(s); }

// ★ 범용성: .nn 에 저장된 backbone 절대경로가 이 PC 에 없으면(다른 PC 로 복사 등)
//   NOTEPAD.dll 이 있는 폴더\models\mobilenetv2.onnx 로 자동 폴백한다.
extern "C" IMAGE_DOS_HEADER __ImageBase;
static std::wstring nnDllDir() {
    wchar_t path[MAX_PATH]; DWORD n = GetModuleFileNameW((HMODULE)&__ImageBase, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring p(path, n); size_t k = p.find_last_of(L"\\/");
    return (k == std::wstring::npos) ? L"" : p.substr(0, k + 1);
}
static bool nnFileExists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
// ★ 학습 시 백본 선택: 지정 경로가 있으면 그대로. 없거나(빈 문자열) 구형 기본 mobilenetv2.onnx 를
//   가리키면 같은 폴더의 mobilenetv2_embed.onnx(임베딩)가 있을 때 그것을 우선 사용한다.
static std::wstring resolveBackboneForTrain(const std::wstring& given) {
    std::wstring base = given;
    if (base.empty()) base = nnDllDir() + L"models\\mobilenetv2.onnx";
    size_t k = base.find_last_of(L"\\/");
    std::wstring dir = (k == std::wstring::npos) ? L"" : base.substr(0, k + 1);
    std::wstring name = (k == std::wstring::npos) ? base : base.substr(k + 1);
    if (name == L"mobilenetv2.onnx") {
        std::wstring emb = dir + L"mobilenetv2_embed.onnx";
        if (nnFileExists(emb)) return emb;
        emb = nnDllDir() + L"models\\mobilenetv2_embed.onnx";
        if (nnFileExists(emb)) return emb;
    }
    if (nnFileExists(base)) return base;
    std::wstring cand = nnDllDir() + L"models\\" + name;
    if (nnFileExists(cand)) return cand;
    return base;
}
// 추론 시: .nn 에 저장된 백본 경로 → 없으면 DLL\models\<같은 파일명> → mobilenetv2.onnx
static std::wstring resolveBackbone(const std::wstring& stored) {
    if (nnFileExists(stored)) return stored;
    std::wstring name = L"mobilenetv2.onnx";
    if (!stored.empty()) { size_t k = stored.find_last_of(L"\\/"); if (k != std::wstring::npos) name = stored.substr(k + 1); }
    std::wstring cand = nnDllDir() + L"models\\" + name;
    if (nnFileExists(cand)) return cand;
    cand = nnDllDir() + L"models\\mobilenetv2.onnx";
    if (nnFileExists(cand)) return cand;
    return stored;
}

static bool loadNNRaw(const std::wstring& path, MLP& net, std::vector<std::wstring>& labels, std::wstring& backbone);
// ★ 속도: 같은 .nn 을 매 호출마다 텍스트 파싱하지 않도록 (경로+수정시각) 캐시.
struct NNCacheEntry { std::wstring path; FILETIME mtime{}; MLP net; std::vector<std::wstring> labels; std::wstring backbone; int norm = 0; };
static std::vector<NNCacheEntry> g_nnCache;
static int g_loadNorm = 0;   // loadNNRaw 가 파싱한 norm= 값
static bool loadNN(const std::wstring& path, MLP& net, std::vector<std::wstring>& labels, std::wstring& backbone) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    bool haveFa = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) != 0;
    if (haveFa) {
        for (auto& e : g_nnCache)
            if (e.path == path && CompareFileTime(&e.mtime, &fa.ftLastWriteTime) == 0) {
                net = e.net; labels = e.labels; backbone = e.backbone; g_featNorm = e.norm; return true;
            }
    }
    g_loadNorm = 0;
    if (!loadNNRaw(path, net, labels, backbone)) return false;
    backbone = resolveBackbone(backbone);
    g_featNorm = g_loadNorm;   // 모델이 학습된 방식 그대로 전처리(구형=0, 신형=1)
    if (haveFa) {
        if (g_nnCache.size() >= 8) g_nnCache.erase(g_nnCache.begin());
        NNCacheEntry e; e.path = path; e.mtime = fa.ftLastWriteTime; e.net = net; e.labels = labels; e.backbone = backbone; e.norm = g_featNorm;
        g_nnCache.push_back(std::move(e));
    }
    return true;
}
static bool loadNNRaw(const std::wstring& path, MLP& net, std::vector<std::wstring>& labels, std::wstring& backbone) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string data; std::vector<char> buf(1 << 16); DWORD rd;
    while (ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd) data.append(buf.data(), rd);
    CloseHandle(h);
    auto lines = splitLines(data);
    if (lines.empty() || (lines[0].rfind("NPNN1", 0) != 0 && lines[0].rfind("NPNN2", 0) != 0)) return false;
    net.proto.clear(); net.thr.clear(); net.ex.clear(); net.exLabel.clear();
    auto parseIdxVec = [&](const std::string& body, int& idx, std::vector<float>& v) {   // "<idx>:<floats>"
        size_t c = body.find(':'); if (c == std::string::npos) return false;
        idx = atoi(body.substr(0, c).c_str()); v.clear(); parseFloatsInto(body.substr(c + 1), v); return !v.empty();
    };
    for (auto& ln : lines) {
        if (ln.rfind("backbone=", 0) == 0) backbone = nnU8ToW2(ln.substr(9));
        else if (ln.rfind("norm=", 0) == 0) g_loadNorm = atoi(ln.c_str() + 5);
        else if (ln.rfind("thr=", 0) == 0) { net.thr.clear(); parseFloatsInto(ln.substr(4), net.thr); }
        else if (ln.rfind("proto=", 0) == 0) {
            int idx; std::vector<float> v;
            if (parseIdxVec(ln.substr(6), idx, v)) { if ((int)net.proto.size() <= idx) net.proto.resize(idx + 1); net.proto[idx] = std::move(v); }
        } else if (ln.rfind("ex=", 0) == 0) {
            int idx; std::vector<float> v;
            if (parseIdxVec(ln.substr(3), idx, v)) { net.ex.push_back(std::move(v)); net.exLabel.push_back(idx); }
        } else if (ln.rfind("in=", 0) == 0) {
            sscanf(ln.c_str(), "in=%d hid=%d out=%d", &net.in, &net.hid, &net.out);
        } else if (ln.rfind("labels=", 0) == 0) {
            std::string rest = ln.substr(7); std::string cur;
            labels.clear();
            for (char c : rest) { if (c == '\t') { labels.push_back(nnU8ToW2(cur)); cur.clear(); } else cur.push_back(c); }
            if (!cur.empty()) labels.push_back(nnU8ToW2(cur));
        } else if (ln.rfind("W1=", 0) == 0) { net.W1.clear(); parseFloatsInto(ln.substr(3), net.W1); }
        else if (ln.rfind("b1=", 0) == 0) { net.b1.clear(); parseFloatsInto(ln.substr(3), net.b1); }
        else if (ln.rfind("W2=", 0) == 0) { net.W2.clear(); parseFloatsInto(ln.substr(3), net.W2); }
        else if (ln.rfind("b2=", 0) == 0) { net.b2.clear(); parseFloatsInto(ln.substr(3), net.b2); }
    }
    return net.out > 0 && !net.W1.empty() && !net.W2.empty();
}

// ============================================================================
//  공개 API
// ============================================================================

static void nnFitModel(const std::vector<std::vector<float>>& X, const std::vector<int>& Y,
                       const std::vector<int>& SRC, const std::vector<int>& ORIG,
                       const std::vector<std::wstring>& labels, int epochs, int hidden, MLP& net);
// 학습: dir(하위폴더=클래스) -> modelPath(.nn). epochs/hidden 지정 가능.
//   반환: 학습 이미지 수(0=실패). backbone 은 mobilenetv2.onnx 경로.
int nnTrain(const std::wstring& backboneIn, const std::wstring& dir, const std::wstring& modelPath,
            int epochs, int hidden, int augLevel, std::wstring& err) {
    std::vector<std::vector<float>> X; std::vector<int> Y; std::vector<std::wstring> labels; std::vector<int> SRC, ORIG;
    const std::wstring backbone = resolveBackboneForTrain(backboneIn);   // 임베딩 백본 우선, 없으면 로짓
    g_featNorm = g_trainNorm;   // 대비 정규화(기본 ON). .nn 에 norm= 로 저장 → 추론도 동일 규칙
    if (!collectData(backbone, dir, X, Y, labels, augLevel, &SRC, &ORIG)) { err = L"no training images or feature extraction failed (ONNX enabled? backbone path?)"; return 0; }

    // ★ 단일 클래스 학습: 카테고리가 1개면 "__bg__"(배경) negative 클래스를 자동 생성.
    //   객체 이미지를 뭉갠 변형을 "객체 아님"으로 학습 → 객체 vs 배경 2클래스가 되어
    //   빈/다른 화면은 낮은 점수가 나온다(탐지 임계가 의미를 가짐). 클래스≥2면 생략.
    if (labels.size() == 1) {
        int bgIdx = 1; std::wstring only = labels[0];
        std::wstring folder = dir + L"\\" + only;
        std::wstring pat = folder + L"\\*";
        WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
        int added = 0;
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring name = fd.cFileName; if (!isImgName(name)) continue;
                NImg im; if (!nnLoad(folder + L"\\" + name, im)) continue;
                for (int v = 0; v < 3; ++v) {
                    NImg bg; nnMakeBg(im, v, bg);
                    std::vector<float> feat;
                    if (extractFeat(backbone, bg, feat)) { X.push_back(std::move(feat)); Y.push_back(bgIdx); SRC.push_back(-1); ORIG.push_back(0); added++; }
                }
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        if (added == 0) { err = L"single-class: failed to synthesize background samples"; return 0; }
        labels.push_back(L"__bg__");
    }
    if (labels.size() < 2) { err = L"need at least 1 category folder with images"; return 0; }

    MLP net;
    nnFitModel(X, Y, SRC, ORIG, labels, epochs, hidden, net);
    if (!saveNN(modelPath, net, labels, backbone)) { err = L"save failed"; return 0; }
    return (int)X.size();
}

// ★ 특징(X,Y) + 원본 그룹(SRC,ORIG) → 오픈셋 게이트 통계 + MLP 학습. nnTrain 과 교차검증(nnCrossVal)이 공용.
static void nnFitModel(const std::vector<std::vector<float>>& X, const std::vector<int>& Y,
                       const std::vector<int>& SRC, const std::vector<int>& ORIG,
                       const std::vector<std::wstring>& labels, int epochs, int hidden, MLP& net) {
    const int inDim = (int)X[0].size();
    net.init((int)labels.size(), hidden > 0 ? hidden : 64, inDim);
    if (epochs <= 0) epochs = 200;

    // ★ 오픈셋 게이트 통계 (학습 전, 특징만으로 계산)
    //   proto[c] = 클래스 c 특징 평균(L2 정규화).
    //   thr[c]   = "같은 클래스의 다른 원본 이미지"와의 최대 코사인 유사도 분포에서
    //              평균 - 2σ (하한 0.30, 상한 0.90). 증강 사본끼리는 서로 제외(같은 SRC)해야
    //              임계가 비현실적으로 높아지지 않는다. 원본이 1장뿐이면 proto 유사도 - 0.15.
    //   ex       = 클래스별 대표 샘플(원본 v=0 우선, 최대 총 240개 균등 샘플링).
    {
        const int C = (int)labels.size();
        net.proto.assign(C, std::vector<float>(inDim, 0.0f));
        std::vector<int> cnt(C, 0);
        for (size_t i = 0; i < X.size(); ++i) { for (int d = 0; d < inDim; ++d) net.proto[Y[i]][d] += X[i][d]; cnt[Y[i]]++; }
        for (int c = 0; c < C; ++c) {
            double n2 = 0; for (int d = 0; d < inDim; ++d) n2 += (double)net.proto[c][d] * net.proto[c][d];
            n2 = std::sqrt(n2); if (n2 < 1e-9) n2 = 1;
            for (int d = 0; d < inDim; ++d) net.proto[c][d] = (float)(net.proto[c][d] / n2);
        }
        // ★ 게이트 통계는 '진짜 원본(ORIG==1)'끼리만 쓴다. 회전·그레이스케일·엣지 등 증강/전처리
        //   사본이 섞이면 임계가 왜곡되므로(비슷한 사본끼리 유사도 폭주), 원본만으로 분포를 잡는다.
        net.thr.assign(C, 0.5f);
        for (int c = 0; c < C; ++c) {
            std::vector<float> sims;
            for (size_t i = 0; i < X.size(); ++i) {
                if (Y[i] != c || ORIG[i] != 1) continue;
                float best = -2.0f; bool any = false;
                for (size_t j = 0; j < X.size(); ++j) {
                    if (j == i || Y[j] != c || ORIG[j] != 1) continue;
                    if (SRC[i] >= 0 && SRC[i] == SRC[j]) continue;   // 같은 원본 제외
                    any = true; float s = nnCos(X[i], X[j]); if (s > best) best = s;
                }
                if (!any) best = nnCos(X[i], net.proto[c]) - 0.15f;
                sims.push_back(best);
            }
            if (sims.empty()) continue;
            double m = 0; for (float s : sims) m += s; m /= sims.size();
            double v = 0; for (float s : sims) v += (s - m) * (s - m); v = std::sqrt(v / sims.size());
            // 원본 장수가 적으면 분포 추정이 낙관적(σ 과소)이라 여유를 더 준다.
            int nOrig = (int)sims.size();
            double spread = std::max(2.0 * v, 0.06);
            double extra = nOrig <= 3 ? 0.10 : (nOrig <= 6 ? 0.05 : (nOrig <= 10 ? 0.02 : 0.0));
            float t = (float)(m - spread - extra);
            if (t < 0.30f) t = 0.30f; if (t > 0.90f) t = 0.90f;
            net.thr[c] = t;
        }
        // 대표 샘플: 진짜 원본(ORIG==1)만, 그룹(SRC)당 1개, 총 240개 상한
        const int MAX_EX = 240;
        std::vector<size_t> origIdx;
        {
            std::vector<int> seen;
            for (size_t i = 0; i < X.size(); ++i) {
                if (SRC[i] < 0 || ORIG[i] != 1) continue;   // 배경·사본 제외
                bool first = std::find(seen.begin(), seen.end(), SRC[i]) == seen.end();
                if (first) { origIdx.push_back(i); seen.push_back(SRC[i]); }
            }
        }
        size_t stride = origIdx.size() > (size_t)MAX_EX ? (origIdx.size() + MAX_EX - 1) / MAX_EX : 1;
        for (size_t k = 0; k < origIdx.size(); k += stride) { net.ex.push_back(X[origIdx[k]]); net.exLabel.push_back(Y[origIdx[k]]); }
    }

    // ★ 클래스 불균형 보정: 샘플이 적은 클래스의 학습률을 키워(최대 3배) softmax 가 큰 클래스로
    //   쏠리는 것을 막는다(예: 차대번호 27장 vs 근접사진 6장 → 근접이 늘 밀리던 문제).
    //   w_c = N/(C·n_c) 를 [0.5, 3.0] 으로 클램프.
    std::vector<float> cw(labels.size(), 1.0f);
    {
        std::vector<int> cnt(labels.size(), 0); for (int y : Y) cnt[y]++;
        for (size_t c = 0; c < labels.size(); ++c) {
            float w = cnt[c] > 0 ? (float)X.size() / ((float)labels.size() * cnt[c]) : 1.0f;
            cw[c] = w < 0.5f ? 0.5f : (w > 3.0f ? 3.0f : w);
        }
    }
    // 학습(미니배치=1, 셔플, lr 감쇠)
    std::vector<int> idx(X.size()); for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
    std::mt19937 rng(777);
    float lr0 = 0.05f, mom = 0.9f, l2 = 1e-4f;
    for (int e = 0; e < epochs; ++e) {
        std::shuffle(idx.begin(), idx.end(), rng);
        float lr = lr0 * (1.0f - 0.9f * e / epochs); // 선형 감쇠
        for (int id : idx) net.trainStep(X[id], Y[id], lr * cw[Y[id]], mom, l2);
    }
}

// ============================================================================
//  전처리 연산 (광도/색/특수) — 학습셋 확장에서 "추가 사본"을 만든다.
//  기하 변형(축소/확대/회전)은 nnMakeVariant 가 담당. 여기는 픽셀값 변환만.
//  ※ 모두 크기(w,h)는 유지 → 원본과 같은 화면 좌표계, 위치탐지에도 안전.
// ============================================================================
static inline unsigned char clamp8(int v) { return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
static int nnLum(const unsigned char* p) { return (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000; }   // BGRA

// 그레이스케일(휘도). 색에 의존하지 않게 학습 → 조명/색 변화에 강건.
static void opGray(const NImg& s, NImg& d) {
    d.w = s.w; d.h = s.h; d.px.resize(s.px.size());
    for (int i = 0; i < s.w * s.h; ++i) { const unsigned char* p = s.px.data() + (size_t)i * 4; unsigned char* o = d.px.data() + (size_t)i * 4;
        unsigned char g = (unsigned char)nnLum(p); o[0] = o[1] = o[2] = g; o[3] = p[3]; }
}
// 대비 정규화(휘도 2~98% 스트레치, 채널 공통 계수) — "어둡다/밝다" 편향 제거.
static void opContrast(const NImg& s, NImg& d) {
    float lo, hi; nnContrastParams(s, lo, hi); float sc = 1.0f / std::max(1e-3f, hi - lo);
    d.w = s.w; d.h = s.h; d.px.resize(s.px.size());
    for (int i = 0; i < s.w * s.h; ++i) { const unsigned char* p = s.px.data() + (size_t)i * 4; unsigned char* o = d.px.data() + (size_t)i * 4;
        for (int c = 0; c < 3; ++c) { float val = ((p[c] / 255.0f) - lo) * sc; o[c] = clamp8((int)(val * 255.0f + 0.5f)); } o[3] = p[3]; }
}
// 히스토그램 평활화(휘도 CDF). 국소 대비를 키움(글자/엣지 또렷).
static void opHistEq(const NImg& s, NImg& d) {
    int N = s.w * s.h; if (N <= 0) { d = s; return; }
    unsigned int h[256] = { 0 }; for (int i = 0; i < N; ++i) h[nnLum(s.px.data() + (size_t)i * 4)]++;
    unsigned int cdf[256]; unsigned int acc = 0, cmin = 0; for (int i = 0; i < 256; ++i) { acc += h[i]; cdf[i] = acc; if (!cmin && acc) cmin = acc; }
    unsigned char lut[256]; for (int i = 0; i < 256; ++i) lut[i] = (unsigned char)((cdf[i] - cmin) * 255.0 / std::max(1u, (unsigned)N - cmin));
    d.w = s.w; d.h = s.h; d.px.resize(s.px.size());
    for (int i = 0; i < N; ++i) { const unsigned char* p = s.px.data() + (size_t)i * 4; unsigned char* o = d.px.data() + (size_t)i * 4;
        int L = nnLum(p); int nl = lut[L]; float k = L > 0 ? (float)nl / L : 1.0f;
        for (int c = 0; c < 3; ++c) o[c] = clamp8((int)(p[c] * k + 0.5f)); o[3] = p[3]; }
}
// 감마 보정(g<1 밝게, g>1 어둡게). 노출 차이 흉내.
static void opGamma(const NImg& s, NImg& d, float g) {
    unsigned char lut[256]; for (int i = 0; i < 256; ++i) lut[i] = clamp8((int)(255.0 * pow(i / 255.0, g) + 0.5));
    d.w = s.w; d.h = s.h; d.px.resize(s.px.size());
    for (int i = 0; i < s.w * s.h; ++i) { const unsigned char* p = s.px.data() + (size_t)i * 4; unsigned char* o = d.px.data() + (size_t)i * 4;
        o[0] = lut[p[0]]; o[1] = lut[p[1]]; o[2] = lut[p[2]]; o[3] = p[3]; }
}
// 3x3 박스 블러(내부용).
static void nnBox3(const NImg& s, NImg& d) {
    d.w = s.w; d.h = s.h; d.px.resize(s.px.size());
    for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) {
        int acc[3] = { 0,0,0 }, n = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) { int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= s.w || yy >= s.h) continue;
            const unsigned char* p = s.px.data() + ((size_t)yy * s.w + xx) * 4; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; ++n; }
        unsigned char* o = d.px.data() + ((size_t)y * s.w + x) * 4; for (int c = 0; c < 3; ++c) o[c] = (unsigned char)(acc[c] / n); o[3] = s.px[((size_t)y * s.w + x) * 4 + 3]; }
}
// 노이즈 제거(박스 블러). 캡처 노이즈/JPEG 블록 완화.
static void opDenoise(const NImg& s, NImg& d) { nnBox3(s, d); }
// 샤픈(언샤프 마스크): 원본 + amt*(원본-블러). 흐린 캡처 또렷하게.
static void opSharpen(const NImg& s, NImg& d) {
    NImg b; nnBox3(s, b); const float amt = 0.8f;
    d.w = s.w; d.h = s.h; d.px.resize(s.px.size());
    for (int i = 0; i < s.w * s.h; ++i) { const unsigned char* p = s.px.data() + (size_t)i * 4; const unsigned char* q = b.px.data() + (size_t)i * 4; unsigned char* o = d.px.data() + (size_t)i * 4;
        for (int c = 0; c < 3; ++c) o[c] = clamp8((int)(p[c] + amt * (p[c] - q[c]) + 0.5f)); o[3] = p[3]; }
}
// 엣지(소벨 크기, 흰 엣지/검은 배경). 템플릿매칭·형태 기반 특징.
static void opEdge(const NImg& s, NImg& d) {
    d.w = s.w; d.h = s.h; d.px.resize(s.px.size());
    for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) {
        int gx = 0, gy = 0;
        auto L = [&](int xx, int yy) { xx = xx < 0 ? 0 : (xx >= s.w ? s.w - 1 : xx); yy = yy < 0 ? 0 : (yy >= s.h ? s.h - 1 : yy); return nnLum(s.px.data() + ((size_t)yy * s.w + xx) * 4); };
        gx = -L(x-1,y-1) - 2*L(x-1,y) - L(x-1,y+1) + L(x+1,y-1) + 2*L(x+1,y) + L(x+1,y+1);
        gy = -L(x-1,y-1) - 2*L(x,y-1) - L(x+1,y-1) + L(x-1,y+1) + 2*L(x,y+1) + L(x+1,y+1);
        int m = (int)(sqrt((double)gx * gx + (double)gy * gy)); m = m > 255 ? 255 : m;
        unsigned char* o = d.px.data() + ((size_t)y * s.w + x) * 4; o[0] = o[1] = o[2] = (unsigned char)m; o[3] = 255; }
}
// 이진화(Otsu). 글자/기호를 흑백으로 — 템플릿매칭 스타일.
static void opBinar(const NImg& s, NImg& d) {
    int N = s.w * s.h; if (N <= 0) { d = s; return; }
    unsigned int h[256] = { 0 }; for (int i = 0; i < N; ++i) h[nnLum(s.px.data() + (size_t)i * 4)]++;
    long total = N; double sum = 0; for (int i = 0; i < 256; ++i) sum += (double)i * h[i];
    double sumB = 0; long wB = 0; double maxV = 0; int thr = 128;
    for (int i = 0; i < 256; ++i) { wB += h[i]; if (!wB) continue; long wF = total - wB; if (!wF) break; sumB += (double)i * h[i];
        double mB = sumB / wB, mF = (sum - sumB) / wF, between = (double)wB * wF * (mB - mF) * (mB - mF); if (between > maxV) { maxV = between; thr = i; } }
    d.w = s.w; d.h = s.h; d.px.resize(s.px.size());
    for (int i = 0; i < N; ++i) { unsigned char* o = d.px.data() + (size_t)i * 4; unsigned char g = nnLum(s.px.data() + (size_t)i * 4) > thr ? 255 : 0; o[0] = o[1] = o[2] = g; o[3] = 255; }
}
// 색 반전.
static void opInvert(const NImg& s, NImg& d) {
    d.w = s.w; d.h = s.h; d.px.resize(s.px.size());
    for (int i = 0; i < s.w * s.h; ++i) { const unsigned char* p = s.px.data() + (size_t)i * 4; unsigned char* o = d.px.data() + (size_t)i * 4;
        o[0] = 255 - p[0]; o[1] = 255 - p[1]; o[2] = 255 - p[2]; o[3] = p[3]; }
}
// 좌우 기울임(shear). 살짝 비스듬한 촬영에 강건. k>0 아래쪽이 오른쪽으로.
static void opShear(const NImg& s, NImg& d, float k) {
    d.w = s.w; d.h = s.h; d.px.assign(s.px.size(), 0);
    long b = 0, g = 0, r = 0; for (int x = 0; x < s.w; ++x) { const unsigned char* p = s.px.data() + (size_t)x * 4; b += p[0]; g += p[1]; r += p[2]; }
    unsigned char pad[4] = { (unsigned char)(b / s.w), (unsigned char)(g / s.w), (unsigned char)(r / s.w), 255 };
    for (int y = 0; y < s.h; ++y) { float sh = k * (y - s.h / 2.0f);
        for (int x = 0; x < s.w; ++x) { int sx = (int)(x - sh + 0.5f); unsigned char* o = d.px.data() + ((size_t)y * s.w + x) * 4;
            if (sx < 0 || sx >= s.w) memcpy(o, pad, 4); else memcpy(o, s.px.data() + ((size_t)y * s.w + sx) * 4, 4); } }
}

// ★ JPEG 재압축 사본: 학습은 PNG(무손실) 크롭인데 실제 판정 사진은 JPG 인 경우, 압축 흔적(8×8 블록·링잉·
//   색 뭉개짐)이 미세 텍스처를 바꿔 분포가 어긋난다. 품질 q 로 JPEG 인코딩→디코딩한 사본을 학습에 넣어
//   압축 흔적에 익숙하게 만든다(GDI+ 로 임시파일 경유, 의존성 0).
static bool opJpeg(const NImg& s, NImg& d, int q) {
    nnEnsureGdiplus();
    Gdiplus::Bitmap bmp(s.w, s.h, PixelFormat32bppARGB);
    Gdiplus::Rect rc(0, 0, s.w, s.h); Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    for (int r = 0; r < s.h; ++r) memcpy((unsigned char*)bd.Scan0 + (size_t)r * bd.Stride, s.px.data() + (size_t)r * s.w * 4, (size_t)s.w * 4);
    bmp.UnlockBits(&bd);
    UINT num = 0, size = 0; Gdiplus::GetImageEncodersSize(&num, &size); if (!size) return false;
    std::vector<unsigned char> buf(size); Gdiplus::ImageCodecInfo* enc = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, enc);
    CLSID clsid{}; bool found = false;
    for (UINT j = 0; j < num; ++j) if (wcscmp(enc[j].MimeType, L"image/jpeg") == 0) { clsid = enc[j].Clsid; found = true; break; }
    if (!found) return false;
    wchar_t tmpDir[MAX_PATH]; GetTempPathW(MAX_PATH, tmpDir);
    std::wstring tmp = std::wstring(tmpDir) + L"np_jpegaug.jpg";
    Gdiplus::EncoderParameters ep; ep.Count = 1; ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
    ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong; ep.Parameter[0].NumberOfValues = 1;
    ULONG qq = (ULONG)(q < 1 ? 1 : (q > 100 ? 100 : q)); ep.Parameter[0].Value = &qq;
    if (bmp.Save(tmp.c_str(), &clsid, &ep) != Gdiplus::Ok) return false;
    bool ok = nnLoad(tmp, d);
    DeleteFileW(tmp.c_str());
    return ok;
}

// 이름별 전처리 사본 생성. 반환 false = 해당 op 없음(건너뜀).
static bool nnApplyPreOp(const NImg& base, const std::wstring& op, NImg& out) {
    if (op == L"jpeg")     { return opJpeg(base, out, 60); }      // JPEG 압축 흉내(품질 60)
    if (op == L"jpeglow")  { return opJpeg(base, out, 35); }      // 강한 압축(카톡·메신저 전송본 수준)
    if (op == L"scale") { nnMakeVariant(base, 1, out); return true; }
    if (op == L"zoom")  { nnMakeVariant(base, 2, out); return true; }
    if (op == L"bright"){ nnMakeVariant(base, 3, out); return true; }
    if (op == L"dark")  { nnMakeVariant(base, 4, out); return true; }
    if (op == L"rotp")  { nnMakeVariant(base, 5, out); return true; }
    if (op == L"rotn")  { nnMakeVariant(base, 6, out); return true; }
    if (op == L"gray")     { opGray(base, out); return true; }
    if (op == L"contrast") { opContrast(base, out); return true; }
    if (op == L"histeq")   { opHistEq(base, out); return true; }
    if (op == L"gammaup")  { opGamma(base, out, 0.7f); return true; }
    if (op == L"gammadn")  { opGamma(base, out, 1.4f); return true; }
    if (op == L"denoise")  { opDenoise(base, out); return true; }
    if (op == L"sharpen")  { opSharpen(base, out); return true; }
    if (op == L"edge")     { opEdge(base, out); return true; }
    if (op == L"binar")    { opBinar(base, out); return true; }
    if (op == L"invert")   { opInvert(base, out); return true; }
    if (op == L"shearp")   { opShear(base, out, 0.18f); return true; }
    if (op == L"shearn")   { opShear(base, out, -0.18f); return true; }
    return false;
}

// ★ 학습셋 확장(파일로 저장): 원본마다 원본(선택적 품질향상)+선택한 전처리 사본을 실제 PNG 로 저장.
//   opts: 공백구분 전처리 이름들(위 nnApplyPreOp 참조). 예 "gray contrast denoise sharpen rotp rotn".
//   파일명 "<원본>__v0.png"(원본), "<원본>__<op>.png"(사본) → collectData 가 "__" 앞부분으로 원본을
//   묶고 __v0 만 진짜 원본으로 취급하므로, 사본을 아무리 넣어도 오픈셋 게이트가 왜곡되지 않는다.
//   enhScalePct>0 이면 base 에 세트 AJ 품질향상(enhPreset)을 먼저 적용(모든 사본의 바탕이 됨).
//   ★ opts 에 "fmt=jpg" 를 넣으면 모든 산출물을 JPEG(quality) 로 저장(.jpg) — 판정 사진이 전산 JPG 일 때 학습 포맷 일치용.
//     기본/"fmt=png" 은 무손실 PNG. (collectData 의 __v0/__op 묶음은 확장자와 무관하게 동작)
//   저장 폴더로 학습할 땐 augLevel=0(이미 사본이 파일로 있음). 반환: 저장 파일 수(0=실패).
int nnExpandTrainSet(const std::wstring& srcDir, const std::wstring& dstDir,
                     const std::wstring& opts, int enhScalePct, const std::wstring& enhPreset, int quality, std::wstring& err) {
    // opts 를 토큰으로. ★ "fmt=jpg"/"fmt=png" 토큰은 저장 포맷(기본 png, jpg 는 quality 로 인코딩) — 전처리 op 목록에서는 제외.
    std::wstring ext = L"png";
    std::vector<std::wstring> ops; { std::wstring cur; for (wchar_t c : opts) { if (c == L' ' || c == L',') { if (!cur.empty()) { ops.push_back(cur); cur.clear(); } } else cur.push_back(c); } if (!cur.empty()) ops.push_back(cur); }
    for (size_t i = 0; i < ops.size(); ) {
        if (ops[i].rfind(L"fmt=", 0) == 0) {
            std::wstring v = ops[i].substr(4); for (auto& c : v) c = (wchar_t)towlower(c);
            ext = (v == L"jpg" || v == L"jpeg") ? L"jpg" : L"png";
            ops.erase(ops.begin() + i);
        } else ++i;
    }
    CreateDirectoryW(dstDir.c_str(), nullptr);
    std::wstring tmpEnh = dstDir + L"\\__enh_tmp.png";
    int total = 0;
    // 하위폴더 = 카테고리
    std::wstring pat = srcDir + L"\\*";
    WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) { err = L"src dir not found"; return 0; }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring cat = fd.cFileName;
        if (cat == L"." || cat == L"..") continue;
        std::wstring srcCat = srcDir + L"\\" + cat, dstCat = dstDir + L"\\" + cat;
        CreateDirectoryW(dstCat.c_str(), nullptr);
        WIN32_FIND_DATAW f2; HANDLE h2 = FindFirstFileW((srcCat + L"\\*").c_str(), &f2);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        do {
            if (f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = f2.cFileName;
            if (!isImgName(name)) continue;
            std::wstring stem = name; size_t dot = stem.find_last_of(L'.'); if (dot != std::wstring::npos) stem = stem.substr(0, dot);
            size_t us = stem.find(L"__"); if (us != std::wstring::npos) stem = stem.substr(0, us);   // 재실행 시 그룹 유지
            NImg base;
            if (enhScalePct > 0) {
                std::wstring used, es;
                if (enhanceFile(srcCat + L"\\" + name, tmpEnh, enhScalePct, enhPreset, quality, used, es) && nnLoad(tmpEnh, base)) {}
                else if (!nnLoad(srcCat + L"\\" + name, base)) continue;
            } else if (!nnLoad(srcCat + L"\\" + name, base)) continue;
            // 원본(v0) — 게이트의 '진짜 원본'
            if (nnSaveImg(base, dstCat + L"\\" + stem + L"__v0." + ext, quality)) ++total;
            // 선택한 전처리 사본들
            for (auto& op : ops) { NImg var; if (nnApplyPreOp(base, op, var)) { if (nnSaveImg(var, dstCat + L"\\" + stem + L"__" + op + L"." + ext, quality)) ++total; } }
        } while (FindNextFileW(h2, &f2));
        FindClose(h2);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    DeleteFileW(tmpEnh.c_str());
    if (total == 0) err = L"no images expanded";
    return total;
}

// 특징벡터 -> "label1,prob1;label2,prob2;..." (prob 0~100, softmax 확률). topK.
static std::wstring classifyFeatNN(const MLP& net, const std::vector<std::wstring>& labels,
                                   const std::vector<float>& feat, int topK) {
    std::vector<float> prob; float score, gate, sim;
    nnScoreGated(net, labels, feat, prob, score, gate, sim);   // prob 는 gate 만큼 축소됨
    std::vector<std::pair<int, float>> order;
    for (int k = 0; k < (int)prob.size(); ++k) order.push_back({ k, prob[k] });
    // ★ 오픈셋: 게이트가 깎은 만큼(1-gate)을 "__unknown__" 가상 클래스로 보여준다.
    //   → 학습 분포 밖 입력은 "__unknown__,85;차대번호,12" 처럼 나온다(1등 = __unknown__).
    if (gate < 0.999f) order.push_back({ -1, 1.0f - gate });
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (topK <= 0) topK = 3;
    std::wstring out;
    for (int i = 0; i < (int)order.size() && i < topK; ++i) {
        wchar_t buf[32]; swprintf(buf, 32, L",%.1f", order[i].second * 100.0f);
        if (!out.empty()) out += L";";
        out += (order[i].first < 0) ? L"__unknown__" : labels[order[i].first]; out += buf;
    }
    return out;
}

// 이미지 분류
std::wstring nnClassifyImage(const std::wstring& modelPath, const std::wstring& imgPath, int topK) {
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    std::vector<float> feat;
    if (!extractFeatFile(backbone, imgPath, feat)) return L"";
    return classifyFeatNN(net, labels, feat, topK);
}

// 화면 영역 분류 (★ 속도: 임시 PNG 저장/로드 없이 메모리 캡처 → 바로 특징추출)
std::wstring nnClassifyScreen(const std::wstring& modelPath, int x, int y, int w, int h, int topK) {
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    if (w <= 0 || h <= 0) return L"";
    NImg im; im.w = w; im.h = h;
    if (!captureBGRA(x, y, w, h, im.px) || (int)im.px.size() < (size_t)w * h * 4) return L"";
    std::vector<float> feat;
    if (!extractFeat(backbone, im, feat)) return L"";
    return classifyFeatNN(net, labels, feat, topK);
}

// ============================================================================
//  신경망 위치탐지 (슬라이딩 윈도우)
//  화면영역(rx,ry,rw,rh)을 win 크기 셀로 격자 스캔한다. 각 셀을 캡처→특징추출
//  →MLP 분류해, 1등 확률이 minScorePct 이상이면 그 셀을 후보로 채택한다.
//  같은 라벨끼리 크게 겹치는 후보는 간단한 NMS 로 최고점만 남긴다.
//  반환: "label,x,y,w,h,score" 개행구분 (특징기반 DetectLearned 와 동일 포맷,
//        AHK Note_ParseDetections 로 그대로 파싱). 없으면 "".
//   win        : 셀(윈도우) 크기 px (학습 이미지 크기와 비슷하게)
//   stepPct    : 이동 간격 % (win 대비. 50이면 절반씩 이동. 작을수록 촘촘·느림)
//   minScorePct: 채택 임계 확률(0~100)
//  ※ 셀마다 MobileNet 특징추출(ONNX)이 돌므로, 넓은영역+작은win 은 느리다.
//    빠른 위치탐지가 필요하면 특징기반 detectLearnedScreen 을 쓰는 편이 낫다.
// ============================================================================
struct NNDet { int labelIdx; int x, y, w, h; float score; };

static float nnIoU(const NNDet& a, const NNDet& b) {
    int ix1 = std::max(a.x, b.x), iy1 = std::max(a.y, b.y);
    int ix2 = std::min(a.x + a.w, b.x + b.w), iy2 = std::min(a.y + a.h, b.y + b.h);
    int iw = ix2 - ix1, ih = iy2 - iy1;
    if (iw <= 0 || ih <= 0) return 0.0f;
    float inter = (float)iw * ih;
    float uni = (float)a.w * a.h + (float)b.w * b.h - inter;
    return uni > 0 ? inter / uni : 0.0f;
}

std::wstring nnDetectScreen(const std::wstring& modelPath, int rx, int ry, int rw, int rh,
                            int win, int stepPct, int minScorePct) {
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    if (win <= 0) win = 96;
    if (stepPct <= 0) stepPct = 50;
    int step = std::max(1, win * stepPct / 100);
    float thr = minScorePct <= 0 ? 0.6f : (float)minScorePct / 100.0f;

    // ★ 속도개선: 탐지영역 전체를 메모리로 딱 1회 캡처하고, 각 셀은 그 버퍼에서
    //   잘라내 곧바로 특징추출한다. (기존: 셀마다 개별 캡처+PNG 저장+PNG 로드 → 제거)
    std::vector<unsigned char> scene;   // BGRA, top-down, rw x rh
    if (!captureBGRA(rx, ry, rw, rh, scene)) return L"";
    if ((int)scene.size() < (size_t)rw * rh * 4) return L"";

    // 전 격자 셀을 모아 배치로 한 번에 특징추출(세션 캐시 + 배치 = 고속).
    std::vector<NImg> cCells; std::vector<std::pair<int,int>> cPos;
    for (int cy = 0; cy + win <= rh; cy += step)
        for (int cx = 0; cx + win <= rw; cx += step) {
            NImg cell; cell.w = win; cell.h = win; cell.px.resize((size_t)win * win * 4);
            for (int yy = 0; yy < win; ++yy) {
                const unsigned char* srcRow = scene.data() + ((size_t)(cy + yy) * rw + cx) * 4;
                memcpy(cell.px.data() + (size_t)yy * win * 4, srcRow, (size_t)win * 4);
            }
            cCells.push_back(std::move(cell)); cPos.push_back({ cx, cy });
        }
    std::vector<std::vector<float>> feats;
    extractFeatBatch(backbone, cCells, feats);
    std::vector<NNDet> cands;
    for (size_t i = 0; i < feats.size(); ++i) {
        if (feats[i].empty()) continue;
        std::vector<float> prob; float sc, gate, sim;
        int b = nnScoreGated(net, labels, feats[i], prob, sc, gate, sim);   // ★ 오픈셋 게이트 적용 점수
        if (sc >= thr && labels[b] != L"__bg__")   // 배경 클래스는 탐지에서 제외
            cands.push_back({ b, rx + cPos[i].first, ry + cPos[i].second, win, win, sc });
    }

    // NMS: 점수 내림차순, 같은 라벨끼리 IoU>0.3 겹치면 억제
    std::sort(cands.begin(), cands.end(), [](const NNDet& a, const NNDet& b) { return a.score > b.score; });
    std::vector<NNDet> kept;
    for (const auto& c : cands) {
        bool sup = false;
        for (const auto& k : kept)
            if (k.labelIdx == c.labelIdx && nnIoU(k, c) > 0.3f) { sup = true; break; }
        if (!sup) kept.push_back(c);
    }

    std::wstring out;
    for (const auto& d : kept) {
        wchar_t buf[128];
        swprintf(buf, 128, L",%d,%d,%d,%d,%.1f", d.x, d.y, d.w, d.h, d.score * 100.0f);
        if (!out.empty()) out += L"\n";
        out += labels[d.labelIdx]; out += buf;
    }
    return out;
}

// ============================================================================
//  신경망 위치탐지 (하이브리드: 후보영역 제안 → 학습모델 정밀판정)  ★고속★
// ============================================================================
//  기존 nnDetectScreen 은 모든 격자 셀마다 MobileNet 추론을 돌려 정확하지만 느리다.
//  이 함수는 그 전에 "객체가 있을 만한 후보 위치"만 값싼 방법(엣지 밀도)으로 추려,
//  후보 셀에서만 신경망을 돌린다. 배경(엣지 거의 없음)은 아예 추론하지 않으므로
//  추론 횟수가 수백 → 수십으로 줄어 훨씬 빠르다.
//   - 원리: 균일한 배경은 그래디언트(엣지)가 거의 없다. 학습 대상(버튼/아이콘/UI)은
//     테두리·글자·무늬로 엣지가 많다. 그래서 "엣지 밀도 상위 셀"만 후보로 본다.
//   - 트레이드오프: 엣지가 거의 없는 밋밋한 객체는 후보에서 놓칠 수 있다.
//     정확도 최우선이면 기존 nnDetectScreen(전수 슬라이딩)을 써라.
//   파라미터는 nnDetectScreen 과 동일 + edgeKeepPct(후보로 남길 상위 비율%, 기본 30).
std::wstring nnDetectScreenFast(const std::wstring& modelPath, int rx, int ry, int rw, int rh,
                                int win, int stepPct, int minScorePct, int edgeKeepPct) {
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    if (win <= 0) win = 96;
    if (stepPct <= 0) stepPct = 50;
    int step = std::max(1, win * stepPct / 100);
    float thr = minScorePct <= 0 ? 0.6f : (float)minScorePct / 100.0f;
    if (edgeKeepPct <= 0 || edgeKeepPct > 100) edgeKeepPct = 30;

    std::vector<unsigned char> scene;   // BGRA, top-down, rw x rh
    if (!captureBGRA(rx, ry, rw, rh, scene)) return L"";
    if ((int)scene.size() < (size_t)rw * rh * 4) return L"";

    // 1) 그레이스케일 + 셀별 엣지밀도(그래디언트 크기 합) 계산.
    //    scene 를 grey[rw*rh] 로 변환.
    std::vector<float> grey((size_t)rw * rh);
    for (int i = 0; i < rw * rh; ++i) {
        const unsigned char* p = scene.data() + (size_t)i * 4;   // BGRA
        grey[i] = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000.0f;
    }
    // 각 셀 위치(격자)의 엣지밀도. 셀 내부를 성기게(4px 간격) 샘플링해 |dx|+|dy| 합.
    struct Cand { int cx, cy; float edge; };
    std::vector<Cand> cells;
    for (int cy = 0; cy + win <= rh; cy += step)
        for (int cx = 0; cx + win <= rw; cx += step) {
            float e = 0;
            for (int yy = 2; yy < win - 2; yy += 4)
                for (int xx = 2; xx < win - 2; xx += 4) {
                    int gx = cx + xx, gy = cy + yy;
                    float dx = grey[(size_t)gy * rw + (gx + 1)] - grey[(size_t)gy * rw + (gx - 1)];
                    float dy = grey[(size_t)(gy + 1) * rw + gx] - grey[(size_t)(gy - 1) * rw + gx];
                    e += std::abs(dx) + std::abs(dy);
                }
            cells.push_back({ cx, cy, e });
        }
    if (cells.empty()) return L"";

    // 2) 엣지밀도 상위 edgeKeepPct% 셀만 후보로 남긴다(배경 제거).
    std::vector<float> edges; edges.reserve(cells.size());
    for (auto& c : cells) edges.push_back(c.edge);
    std::sort(edges.begin(), edges.end());
    int keepIdx = (int)(edges.size() * (100 - edgeKeepPct) / 100);
    if (keepIdx < 0) keepIdx = 0; if (keepIdx >= (int)edges.size()) keepIdx = (int)edges.size() - 1;
    float edgeThr = edges[keepIdx];

    // 3) 후보 셀들을 모아 배치로 한 번에 특징추출(세션 캐시 + 배치 = 고속).
    std::vector<NImg> cCells; std::vector<std::pair<int,int>> cPos;
    for (auto& c : cells) {
        if (c.edge < edgeThr) continue;   // 배경 셀 건너뜀(추론 안 함)
        NImg cell; cell.w = win; cell.h = win; cell.px.resize((size_t)win * win * 4);
        for (int yy = 0; yy < win; ++yy) {
            const unsigned char* srcRow = scene.data() + ((size_t)(c.cy + yy) * rw + c.cx) * 4;
            memcpy(cell.px.data() + (size_t)yy * win * 4, srcRow, (size_t)win * 4);
        }
        cCells.push_back(std::move(cell)); cPos.push_back({ c.cx, c.cy });
    }
    std::vector<std::vector<float>> feats;
    extractFeatBatch(backbone, cCells, feats);
    std::vector<NNDet> cands;
    for (size_t i = 0; i < feats.size(); ++i) {
        if (feats[i].empty()) continue;
        std::vector<float> prob; float sc, gate, sim;
        int best = nnScoreGated(net, labels, feats[i], prob, sc, gate, sim);   // ★ 오픈셋 게이트
        if (sc >= thr && labels[best] != L"__bg__")   // 배경 클래스는 탐지에서 제외
            cands.push_back({ best, rx + cPos[i].first, ry + cPos[i].second, win, win, sc });
    }

    // NMS
    std::sort(cands.begin(), cands.end(), [](const NNDet& a, const NNDet& b) { return a.score > b.score; });
    std::vector<NNDet> kept;
    for (const auto& c : cands) {
        bool sup = false;
        for (const auto& k : kept)
            if (k.labelIdx == c.labelIdx && nnIoU(k, c) > 0.3f) { sup = true; break; }
        if (!sup) kept.push_back(c);
    }
    std::wstring out;
    for (const auto& d : kept) {
        wchar_t buf[128];
        swprintf(buf, 128, L",%d,%d,%d,%d,%.1f", d.x, d.y, d.w, d.h, d.score * 100.0f);
        if (!out.empty()) out += L"\n";
        out += labels[d.labelIdx]; out += buf;
    }
    return out;
}

// ============================================================================
//  신경망 위치탐지 (다중 스케일 + 비율) — 정확도 강화판
//  여러 셀 크기(winMin~winMax, nScales단계)와 비율(정사각/가로/세로)을 동시에 훑어
//  크기·비율이 제각각인 객체(예: 세로 기둥 차대번호 vs 가로 유리 차대번호)를 잡는다.
//  장면은 1회만 캡처하고, 모든 형태의 후보 셀을 한 배치로 특징추출(고속).
//   aspectMode: 0=정사각, 1=+가로(1.6:1), 2=+세로(1:1.6), 3=+가로+세로.
//   나머지 파라미터는 Fast 와 동일. 반환/파싱 동일(Note_ParseDetections).
// ============================================================================
// ★ 공용: 이미 메모리에 있는 장면(BGRA, rw×rh)에서 다중스케일 탐지. 화면/이미지파일 양쪽이 사용.
//   rx,ry = 결과 좌표에 더할 오프셋(화면이면 영역 원점, 이미지면 0).
static std::wstring nnDetectMultiScene(const MLP& net, const std::vector<std::wstring>& labels, const std::wstring& backbone,
                                       const std::vector<unsigned char>& scene, int rw, int rh, int rx, int ry,
                                       int winMin, int winMax, int nScales, int aspectMode,
                                       int stepPct, int minScorePct, int edgeKeepPct) {
    if (winMin <= 0) winMin = 64;
    if (winMax < winMin) winMax = winMin;
    if (nScales < 1) nScales = 1;
    if (stepPct <= 0) stepPct = 50;
    if (edgeKeepPct <= 0 || edgeKeepPct > 100) edgeKeepPct = 30;
    float thr = minScorePct <= 0 ? 0.6f : (float)minScorePct / 100.0f;
    if ((int)scene.size() < (size_t)rw * rh * 4) return L"";
    std::vector<float> grey((size_t)rw * rh);
    for (int i = 0; i < rw * rh; ++i) {
        const unsigned char* p = scene.data() + (size_t)i * 4;
        grey[i] = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000.0f;
    }

    // 탐색할 (w,h) 형태 목록 구성: 스케일 × 비율.
    std::vector<std::pair<int,int>> shapes;
    for (int s = 0; s < nScales; ++s) {
        int base = (nScales <= 1) ? winMin : (winMin + (winMax - winMin) * s / (nScales - 1));
        if (base < 16) base = 16;
        shapes.push_back({ base, base });                                    // 정사각
        if (aspectMode == 1 || aspectMode == 3) shapes.push_back({ (int)(base * 1.6), (int)(base * 0.625) }); // 가로
        if (aspectMode == 2 || aspectMode == 3) shapes.push_back({ (int)(base * 0.625), (int)(base * 1.6) }); // 세로
    }

    // 모든 형태에서 후보 셀 수집(엣지 상위 edgeKeepPct%). 전역 후보 수 상한으로 폭주 방지.
    struct Cand { int cx, cy, w, h; };
    std::vector<Cand> cands;
    const int MAX_CANDS = 500;
    for (auto& sh : shapes) {
        int ww = sh.first, hh = sh.second;
        if (ww < 8 || hh < 8 || ww > rw || hh > rh) continue;
        int sx = std::max(1, ww * stepPct / 100), sy = std::max(1, hh * stepPct / 100);
        struct EC { int cx, cy; float e; };
        std::vector<EC> ecs;
        for (int cy = 0; cy + hh <= rh; cy += sy)
            for (int cx = 0; cx + ww <= rw; cx += sx) {
                float e = 0;
                for (int yy = 2; yy < hh - 2; yy += 4)
                    for (int xx = 2; xx < ww - 2; xx += 4) {
                        int gx = cx + xx, gy = cy + yy;
                        float dx = grey[(size_t)gy * rw + (gx + 1)] - grey[(size_t)gy * rw + (gx - 1)];
                        float dy = grey[(size_t)(gy + 1) * rw + gx] - grey[(size_t)(gy - 1) * rw + gx];
                        e += std::abs(dx) + std::abs(dy);
                    }
                ecs.push_back({ cx, cy, e });
            }
        if (ecs.empty()) continue;
        std::vector<float> es; for (auto& c : ecs) es.push_back(c.e);
        std::sort(es.begin(), es.end());
        int ki = (int)(es.size() * (100 - edgeKeepPct) / 100);
        if (ki < 0) ki = 0; if (ki >= (int)es.size()) ki = (int)es.size() - 1;
        float et = es[ki];
        for (auto& c : ecs)
            if (c.e >= et) cands.push_back({ c.cx, c.cy, ww, hh });
    }
    if (cands.empty()) return L"";
    // 상한 초과 시 앞쪽부터 자름(대략 균등).
    if ((int)cands.size() > MAX_CANDS) cands.resize(MAX_CANDS);

    // 후보 셀들을 NImg 로 잘라 배치 특징추출.
    std::vector<NImg> cCells; cCells.reserve(cands.size());
    for (auto& c : cands) {
        NImg cell; cell.w = c.w; cell.h = c.h; cell.px.resize((size_t)c.w * c.h * 4);
        for (int yy = 0; yy < c.h; ++yy) {
            const unsigned char* srcRow = scene.data() + ((size_t)(c.cy + yy) * rw + c.cx) * 4;
            memcpy(cell.px.data() + (size_t)yy * c.w * 4, srcRow, (size_t)c.w * 4);
        }
        cCells.push_back(std::move(cell));
    }
    std::vector<std::vector<float>> feats;
    extractFeatBatch(backbone, cCells, feats);

    std::vector<NNDet> dets;
    for (size_t i = 0; i < feats.size(); ++i) {
        if (feats[i].empty()) continue;
        std::vector<float> prob; float sc, gate, sim;
        int best = nnScoreGated(net, labels, feats[i], prob, sc, gate, sim);   // ★ 오픈셋 게이트
        if (sc >= thr && labels[best] != L"__bg__")
            dets.push_back({ best, rx + cands[i].cx, ry + cands[i].cy, cands[i].w, cands[i].h, sc });
    }

    // 전역 NMS(스케일·비율 교차): 같은 라벨끼리 IoU>0.3 겹치면 최고점만.
    std::sort(dets.begin(), dets.end(), [](const NNDet& a, const NNDet& b) { return a.score > b.score; });
    std::vector<NNDet> kept;
    for (const auto& c : dets) {
        bool sup = false;
        for (const auto& k : kept)
            if (k.labelIdx == c.labelIdx && nnIoU(k, c) > 0.3f) { sup = true; break; }
        if (!sup) kept.push_back(c);
    }
    std::wstring out;
    for (const auto& d : kept) {
        wchar_t buf[128];
        swprintf(buf, 128, L",%d,%d,%d,%d,%.1f", d.x, d.y, d.w, d.h, d.score * 100.0f);
        if (!out.empty()) out += L"\n";
        out += labels[d.labelIdx]; out += buf;
    }
    return out;
}

// 화면 영역 다중스케일 탐지(공용 helper 호출).
std::wstring nnDetectScreenMulti(const std::wstring& modelPath, int rx, int ry, int rw, int rh,
                                 int winMin, int winMax, int nScales, int aspectMode,
                                 int stepPct, int minScorePct, int edgeKeepPct) {
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    std::vector<unsigned char> scene;
    if (!captureBGRA(rx, ry, rw, rh, scene)) return L"";
    return nnDetectMultiScene(net, labels, backbone, scene, rw, rh, rx, ry,
                              winMin, winMax, nScales, aspectMode, stepPct, minScorePct, edgeKeepPct);
}

// ★ 이미지 파일 안에서 대상 찾기 (사진 → 어디에 무엇이 있나). "전체 사진을 분류기에 통째로 넣으면
//   대상이 작은 일부라 안 맞는" 문제의 정답 도구. 큰 사진은 긴 변 1000px 로 축소해 탐색(좌표는 원본 기준 환산).
//   winMin<=0 이면 사진 크기에 맞춰 자동(짧은 변의 1/8 ~ 1/2, 4단계). 반환 형식은 다른 탐지와 동일.
std::wstring nnDetectImage(const std::wstring& modelPath, const std::wstring& imgPath,
                           int winMin, int winMax, int nScales, int aspectMode,
                           int stepPct, int minScorePct, int edgeKeepPct) {
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    NImg im; if (!nnLoad(imgPath, im)) return L"";
    // 큰 사진 축소(속도). scale = 원본/축소본.
    const int MAXSIDE = 1000; double scale = 1.0;
    NImg work;
    if (std::max(im.w, im.h) > MAXSIDE) {
        scale = (double)std::max(im.w, im.h) / MAXSIDE;
        work.w = (int)(im.w / scale), work.h = (int)(im.h / scale); work.px.resize((size_t)work.w * work.h * 4);
        for (int y = 0; y < work.h; ++y) for (int x = 0; x < work.w; ++x) {   // 영역평균 축소
            int sx0 = (int)(x * scale), sy0 = (int)(y * scale), sx1 = std::min(im.w, (int)((x + 1) * scale)), sy1 = std::min(im.h, (int)((y + 1) * scale));
            int acc[4] = { 0,0,0,0 }, n = 0;
            for (int sy = sy0; sy < std::max(sy0 + 1, sy1); ++sy) for (int sx = sx0; sx < std::max(sx0 + 1, sx1); ++sx) { const unsigned char* p = im.px.data() + ((size_t)std::min(sy, im.h - 1) * im.w + std::min(sx, im.w - 1)) * 4; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; ++n; }
            unsigned char* o = work.px.data() + ((size_t)y * work.w + x) * 4; for (int c = 0; c < 4; ++c) o[c] = (unsigned char)(acc[c] / n);
        }
    } else work = im;
    int minSide = std::min(work.w, work.h);
    if (winMin <= 0) { winMin = std::max(48, minSide / 8); winMax = std::max(winMin, minSide / 2); if (nScales < 1) nScales = 4; }
    if (aspectMode < 0) aspectMode = 3;
    std::wstring r = nnDetectMultiScene(net, labels, backbone, work.px, work.w, work.h, 0, 0,
                                        winMin, winMax, nScales, aspectMode, stepPct, minScorePct, edgeKeepPct);
    if (scale == 1.0 || r.empty()) return r;
    // 좌표를 원본 크기로 환산
    std::wstring out; size_t pos = 0;
    while (pos <= r.size()) {
        size_t nl = r.find(L'\n', pos); std::wstring ln = r.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        if (!ln.empty()) {
            // "label,x,y,w,h,score"
            size_t c1 = ln.find(L','); std::wstring label = ln.substr(0, c1);
            int x, y, w, h; float sc; swscanf(ln.c_str() + c1 + 1, L"%d,%d,%d,%d,%f", &x, &y, &w, &h, &sc);
            wchar_t buf[160]; swprintf(buf, 160, L"%s,%d,%d,%d,%d,%.1f", label.c_str(), (int)(x * scale), (int)(y * scale), (int)(w * scale), (int)(h * scale), sc);
            if (!out.empty()) out += L"\n"; out += buf;
        }
        if (nl == std::wstring::npos) break; pos = nl + 1;
    }
    return out;
}

// 모델 정보 "label0;label1;..." + 클래스 수
std::wstring nnModelInfo(const std::wstring& modelPath) {
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    std::wstring out;
    for (size_t i = 0; i < labels.size(); ++i) { if (i) out += L";"; out += labels[i]; }
    return out;
}

// ★ 학습 시 대비 정규화 on/off. 반환: 이전 값.
int nnSetTrainNorm(int on) { int prev = g_trainNorm; g_trainNorm = on ? 1 : 0; return prev; }

// ★ 오픈셋 엄격도 설정(0=게이트 끔, 50=기본, 100=최대). 반환: 이전 값.
int nnSetStrict(int pct) {
    int prev = g_nnStrict;
    g_nnStrict = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    return prev;
}

// ★ 진단: 화면 영역 1개에 대해 "label,prob,sim,thr,gate,featdim,norm" 을 돌려준다.
//   라벨링 툴에서 왜 거절/채택됐는지 보여주는 용도(임계 튜닝 근거).
std::wstring nnDiagScreen(const std::wstring& modelPath, int x, int y, int w, int h) {
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    if (w <= 0 || h <= 0) return L"";
    NImg im; im.w = w; im.h = h;
    if (!captureBGRA(x, y, w, h, im.px) || (int)im.px.size() < (size_t)w * h * 4) return L"";
    std::vector<float> feat;
    if (!extractFeat(backbone, im, feat)) return L"";
    std::vector<float> prob; int best = net.predict(feat, prob);
    float rawProb = prob.empty() ? 0.0f : prob[best];
    float sc, gate, sim; nnScoreGated(net, labels, feat, prob, sc, gate, sim);
    float thr = (net.hasGate() && best < (int)net.thr.size()) ? net.thr[best] + nnStrictShift() : 0.0f;
    wchar_t buf[256];
    swprintf(buf, 256, L"%s,%.1f,%.3f,%.3f,%.2f,%d,%d", labels[best].c_str(), rawProb * 100.0f, sim, thr, gate, (int)feat.size(), g_featNorm);
    return buf;
}

// ============================================================================
//  ★ 오탐 추적 (2026-09-05): "이 오탐은 어느 학습 샘플 때문인가"
//  판정 영역의 특징과 trainDir(하위폴더=클래스) 안 모든 학습 이미지의 특징을 같은 백본·같은 정규화로
//  뽑아 코사인 유사도 상위 topK 를 돌려준다. 유사도가 높은 샘플이 오탐의 원인(잘못 라벨링됐거나
//  배경이 너무 닮은 샘플)이므로, 그 파일을 지우거나 예외(_excluded)로 빼고 재학습하면 오탐이 사라진다.
//  반환: "경로<TAB>라벨<TAB>유사도%" 개행 구분(유사도 내림차순) / "".
//  ※ AHK _Note_CallStr 는 길이질의 + 본호출 2번 부르므로, 같은 요청은 몇 초간 캐시해 특징추출을 한 번만 한다.
// ============================================================================
struct NNNearHit { std::wstring path, label; float sim; };
static std::wstring g_nearKey, g_nearOut; static DWORD g_nearTick = 0;
static std::wstring nnNearestFeat(const std::wstring& backbone, const std::vector<float>& feat,
                                  const std::wstring& trainDir, int topK) {
    std::vector<NNNearHit> hits;
    WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW((trainDir + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return L"";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring cat = fd.cFileName;
        if (cat == L"." || cat == L"..") continue;
        std::wstring cdir = trainDir + L"\\" + cat;
        WIN32_FIND_DATAW f2; HANDLE h2 = FindFirstFileW((cdir + L"\\*").c_str(), &f2);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        do {
            if (f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = f2.cFileName;
            if (!isImgName(n)) continue;
            // 한 장씩 처리(고해상도 사진을 한꺼번에 메모리에 올리지 않음)
            std::vector<float> f;
            if (!extractFeatFile(backbone, cdir + L"\\" + n, f) || f.size() != feat.size()) continue;
            double s = 0; for (size_t k = 0; k < feat.size(); ++k) s += (double)feat[k] * f[k];
            hits.push_back({ cdir + L"\\" + n, cat, (float)s });
        } while (FindNextFileW(h2, &f2));
        FindClose(h2);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    if (hits.empty()) return L"";
    std::sort(hits.begin(), hits.end(), [](const NNNearHit& a, const NNNearHit& b) { return a.sim > b.sim; });
    if (topK <= 0) topK = 5;
    std::wstring out;
    for (int i = 0; i < (int)hits.size() && i < topK; ++i) {
        wchar_t buf[32]; swprintf(buf, 32, L"%.1f", hits[i].sim * 100.0f);
        out += hits[i].path + L"\t" + hits[i].label + L"\t" + buf + L"\n";
    }
    return out;
}
static bool nnNearCached(const std::wstring& key, std::wstring& out) {
    if (key == g_nearKey && GetTickCount() - g_nearTick < 8000) { out = g_nearOut; return true; }
    return false;
}
static void nnNearStore(const std::wstring& key, const std::wstring& out) { g_nearKey = key; g_nearOut = out; g_nearTick = GetTickCount(); }

// 화면 영역(x,y,w,h) → 가장 닮은 학습 샘플 topK
std::wstring nnNearestScreen(const std::wstring& modelPath, int x, int y, int w, int h,
                             const std::wstring& trainDir, int topK) {
    wchar_t kb[64]; swprintf(kb, 64, L"|S|%d,%d,%d,%d|%d", x, y, w, h, topK);
    std::wstring key = modelPath + kb + trainDir, out;
    if (nnNearCached(key, out)) return out;
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    if (w <= 0 || h <= 0) return L"";
    NImg im; im.w = w; im.h = h;
    if (!captureBGRA(x, y, w, h, im.px) || (int)im.px.size() < (size_t)w * h * 4) return L"";
    std::vector<float> feat;
    if (!extractFeat(backbone, im, feat)) return L"";
    out = nnNearestFeat(backbone, feat, trainDir, topK);
    nnNearStore(key, out);
    return out;
}
// 이미지 파일 → 가장 닮은 학습 샘플 topK
std::wstring nnNearestImage(const std::wstring& modelPath, const std::wstring& imgPath,
                            const std::wstring& trainDir, int topK) {
    wchar_t kb[32]; swprintf(kb, 32, L"|I|%d|", topK);
    std::wstring key = modelPath + kb + imgPath + L"|" + trainDir, out;
    if (nnNearCached(key, out)) return out;
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    std::vector<float> feat;
    if (!extractFeatFile(backbone, imgPath, feat)) return L"";
    out = nnNearestFeat(backbone, feat, trainDir, topK);
    nnNearStore(key, out);
    return out;
}

// ============================================================================
//  ★ 유사 영역 찾기 (2026-09-05): "닮은 학습 샘플의 어느 부분이 닮았나"
//  질의 이미지(오탐 박스 크롭)의 특징과, 대상 사진을 다중 스케일 슬라이딩 창으로 훑은 각 창의 특징을 비교해
//  가장 닮은 창 topK 를 돌려준다. 학습 샘플이 사진 전체라도 "이 부분 때문에 닮았다"를 박스로 보여줄 수 있다.
//  반환: "x,y,w,h,sim" 개행(원본 좌표, 유사도 내림차순). 마지막 줄은 사진 전체 "0,0,W,H,sim". 실패 "".
//  긴 변 800px 로 축소해 탐색(창 ~200~400개, 배치 추론). 같은 요청은 몇 초간 캐시(AHK 2단계 호출 대비).
// ============================================================================
static std::wstring g_simKey, g_simOut; static DWORD g_simTick = 0;
std::wstring nnSimilarRegion(const std::wstring& modelPath, const std::wstring& queryImg,
                             const std::wstring& targetImg, int topK) {
    wchar_t kb[32]; swprintf(kb, 32, L"|R|%d|", topK);
    std::wstring key = modelPath + kb + queryImg + L"|" + targetImg;
    { WIN32_FILE_ATTRIBUTE_DATA fa{}; if (GetFileAttributesExW(targetImg.c_str(), GetFileExInfoStandard, &fa)) { wchar_t b[64]; swprintf(b, 64, L"|%lu|%lu", fa.ftLastWriteTime.dwLowDateTime, fa.nFileSizeLow); key += b; } }   // 대상 파일이 바뀌면(부분 마스킹) 캐시 무효
    if (key == g_simKey && GetTickCount() - g_simTick < 8000) return g_simOut;
    MLP net; std::vector<std::wstring> labels; std::wstring backbone;
    if (!loadNN(modelPath, net, labels, backbone)) return L"";
    NImg q; if (!nnLoad(queryImg, q)) return L"";
    std::vector<float> fq; if (!extractFeat(backbone, q, fq)) return L"";
    NImg im; if (!nnLoad(targetImg, im)) return L"";
    const int MAXSIDE = 800; double scale = 1.0; NImg work;
    if (std::max(im.w, im.h) > MAXSIDE) {
        scale = (double)std::max(im.w, im.h) / MAXSIDE;
        work.w = (int)(im.w / scale), work.h = (int)(im.h / scale); work.px.resize((size_t)work.w * work.h * 4);
        for (int y = 0; y < work.h; ++y) for (int x = 0; x < work.w; ++x) {
            int sx0 = (int)(x * scale), sy0 = (int)(y * scale), sx1 = std::min(im.w, (int)((x + 1) * scale)), sy1 = std::min(im.h, (int)((y + 1) * scale));
            int acc[4] = { 0,0,0,0 }, n = 0;
            for (int sy = sy0; sy < std::max(sy0 + 1, sy1); ++sy) for (int sx = sx0; sx < std::max(sx0 + 1, sx1); ++sx) { const unsigned char* p = im.px.data() + ((size_t)std::min(sy, im.h - 1) * im.w + std::min(sx, im.w - 1)) * 4; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; ++n; }
            unsigned char* o = work.px.data() + ((size_t)y * work.w + x) * 4; for (int c = 0; c < 4; ++c) o[c] = (unsigned char)(acc[c] / n);
        }
    } else work = im;
    struct Cand { int x, y, w, h; float sim; };
    std::vector<Cand> cands;
    auto cosv = [&](const std::vector<float>& f) { if (f.size() != fq.size()) return -1.0f; double s = 0; for (size_t k = 0; k < f.size(); ++k) s += (double)fq[k] * f[k]; return (float)s; };
    auto cropTo = [&](int x, int y, int w, int h, NImg& o) { o.w = w; o.h = h; o.px.resize((size_t)w * h * 4); for (int r = 0; r < h; ++r) memcpy(o.px.data() + (size_t)r * w * 4, work.px.data() + ((size_t)(y + r) * work.w + x) * 4, (size_t)w * 4); };
    double ar = (q.h > 0) ? (double)q.w / q.h : 1.0; if (ar < 0.25) ar = 0.25; if (ar > 4.0) ar = 4.0;
    int minSide = std::min(work.w, work.h);
    const double FR[] = { 1.0 / 6, 1.0 / 4, 0.4, 0.6, 0.85 };
    for (double fr : FR) {
        double base = minSide * fr; int w, h;
        if (ar >= 1.0) { h = (int)base; w = (int)(base * ar); } else { w = (int)base; h = (int)(base / ar); }
        if (w < 32 || h < 32 || w > work.w || h > work.h) continue;
        int step = std::max(8, std::min(w, h) / 2);
        // 창 수 상한(~400): 넘으면 간격을 키운다
        for (;;) { long nx = (work.w - w) / step + 1, ny = (work.h - h) / step + 1; if (nx * ny <= 400) break; step = (int)(step * 1.4) + 1; }
        std::vector<NImg> cells; std::vector<Cand> pos;
        for (int y = 0; ; y += step) { if (y + h > work.h) { if (y == 0 || y - step + h >= work.h) break; y = work.h - h; }
            for (int x = 0; ; x += step) { if (x + w > work.w) { if (x == 0 || x - step + w >= work.w) break; x = work.w - w; }
                NImg c; cropTo(x, y, w, h, c); cells.push_back(std::move(c)); pos.push_back({ x, y, w, h, 0 });
                if (x == work.w - w) break; }
            if (y == work.h - h) break; }
        std::vector<std::vector<float>> feats;
        if (!extractFeatBatch(backbone, cells, feats)) continue;
        for (size_t i = 0; i < pos.size(); ++i) { pos[i].sim = cosv(feats[i]); if (pos[i].sim >= 0) cands.push_back(pos[i]); }
    }
    std::vector<float> fw; float wholeSim = 0; if (extractFeat(backbone, work, fw)) wholeSim = cosv(fw);
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.sim > b.sim; });
    // NMS(겹침 40% 이상은 최고만)
    std::vector<Cand> keep;
    for (auto& c : cands) {
        bool dup = false;
        for (auto& k : keep) { int ix1 = std::max(c.x, k.x), iy1 = std::max(c.y, k.y), ix2 = std::min(c.x + c.w, k.x + k.w), iy2 = std::min(c.y + c.h, k.y + k.h);
            int iw = ix2 - ix1, ih = iy2 - iy1; if (iw <= 0 || ih <= 0) continue; float inter = (float)iw * ih, uni = (float)c.w * c.h + (float)k.w * k.h - inter; if (uni > 0 && inter / uni > 0.4f) { dup = true; break; } }
        if (!dup) keep.push_back(c);
        if ((int)keep.size() >= (topK <= 0 ? 3 : topK)) break;
    }
    std::wstring out; wchar_t buf[128];
    for (auto& k : keep) { swprintf(buf, 128, L"%d,%d,%d,%d,%.1f\n", (int)(k.x * scale), (int)(k.y * scale), (int)(k.w * scale), (int)(k.h * scale), k.sim * 100.0f); out += buf; }
    swprintf(buf, 128, L"0,0,%d,%d,%.1f\n", im.w, im.h, wholeSim * 100.0f); out += buf;
    g_simKey = key; g_simOut = out; g_simTick = GetTickCount();
    return out;
}

// ============================================================================
//  ★ 데이터셋 감사 / 수집 샘플 검사 / k-fold 교차검증 / 영역 마스킹 (2026-09-05)
//  "수집·학습의 필수/보완 사항을 도구가 대신 잡아준다":
//   nnAuditTrainSet : 학습 폴더 전체 특징을 1회 뽑아(캐시) 클래스 분리도·라벨 오류 의심·근사중복·
//                     크기 이탈·저품질·클래스별 밝기/대비 통계를 텍스트로.
//   nnCheckSample   : 새 샘플 1장을 폴더와 비교(같은 클래스 최대 유사/다른 클래스 최대 유사)+품질 지표.
//   nnCrossVal      : 원본 그룹 단위 k-fold. 클래스별 정답률·혼동행렬·정답 최저점/오답 최고점.
//   nnImageMaskRect : 사진의 일부 영역만 주변색/모자이크로 가려 저장(오탐 원인 부분만 제거).
//  ※ AHK _Note_CallStr 는 길이질의+본호출 2번 부르므로 비싼 결과는 몇 초간 캐시한다.
// ============================================================================
struct NNFileFeat {
    std::wstring path, label; FILETIME mtime{}; unsigned long long size = 0;
    std::vector<float> feat; int w = 0, h = 0; float lum = 0, con = 0, blur = 0; bool orig = true;
};
static std::vector<NNFileFeat> g_ffCache;      // 경로 기준 특징 캐시(mtime/size 바뀌면 재계산)
static int g_ffNorm = -1; static std::wstring g_ffBackbone;

// 휘도 평균/표준편차(대비) + 라플라시안 분산(선명도, 클수록 또렷). 큰 사진은 ~256² 로 샘플링해 크기 무관.
static void nnImgStats(const NImg& im, float& lum, float& con, float& blur) {
    lum = con = blur = 0; int N = im.w * im.h; if (N <= 0) return;
    int step = std::max(1, (int)std::sqrt((double)N / 65536.0));
    int lw = (im.w + step - 1) / step, lh = (im.h + step - 1) / step;
    std::vector<int> L((size_t)lw * lh); double s = 0, s2 = 0; long n = 0;
    for (int y = 0, yy = 0; y < im.h; y += step, ++yy) for (int x = 0, xx = 0; x < im.w; x += step, ++xx) {
        int v = nnLum(im.px.data() + ((size_t)y * im.w + x) * 4); L[(size_t)yy * lw + xx] = v; s += v; s2 += (double)v * v; ++n; }
    lum = (float)(s / n); con = (float)std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n)));
    double ls = 0, ls2 = 0; long ln = 0;
    for (int y = 1; y < lh - 1; ++y) for (int x = 1; x < lw - 1; ++x) {
        int c = L[(size_t)y * lw + x];
        int lap = 4 * c - L[(size_t)y * lw + x - 1] - L[(size_t)y * lw + x + 1] - L[(size_t)(y - 1) * lw + x] - L[(size_t)(y + 1) * lw + x];
        ls += lap; ls2 += (double)lap * lap; ++ln; }
    blur = ln > 0 ? (float)(ls2 / ln - (ls / ln) * (ls / ln)) : 0;
}
// 폴더(하위폴더=클래스) 이미지의 특징을 캐시에서 채워 인덱스 목록으로. 새 파일/바뀐 파일만 추출.
static bool nnScanFolderFeats(const std::wstring& backbone, const std::wstring& dir, std::vector<int>& out, std::wstring& err) {
    if (g_ffNorm != g_featNorm || g_ffBackbone != backbone) { g_ffCache.clear(); g_ffNorm = g_featNorm; g_ffBackbone = backbone; }
    out.clear();
    WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) { err = L"dir not found"; return false; }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring cat = fd.cFileName; if (cat == L"." || cat == L"..") continue;
        std::wstring cdir = dir + L"\\" + cat;
        WIN32_FIND_DATAW f2; HANDLE h2 = FindFirstFileW((cdir + L"\\*").c_str(), &f2);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        do {
            if (f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = f2.cFileName; if (!isImgName(n)) continue;
            std::wstring p = cdir + L"\\" + n;
            unsigned long long sz = ((unsigned long long)f2.nFileSizeHigh << 32) | f2.nFileSizeLow;
            int idx = -1;
            for (size_t i = 0; i < g_ffCache.size(); ++i) if (g_ffCache[i].path == p) { idx = (int)i; break; }
            if (idx < 0) { g_ffCache.push_back(NNFileFeat()); idx = (int)g_ffCache.size() - 1; g_ffCache[idx].path = p; }
            NNFileFeat& e = g_ffCache[idx];
            if (!e.feat.empty() && (CompareFileTime(&e.mtime, &f2.ftLastWriteTime) != 0 || e.size != sz)) e.feat.clear();
            if (e.feat.empty()) {
                NImg im; if (!nnLoad(p, im)) continue;
                std::vector<float> f; if (!extractFeat(backbone, im, f)) continue;
                e.feat = std::move(f); e.w = im.w; e.h = im.h; nnImgStats(im, e.lum, e.con, e.blur);
                e.mtime = f2.ftLastWriteTime; e.size = sz;
            }
            e.label = cat;
            size_t us = n.find(L"__"); e.orig = (us == std::wstring::npos) || (n.find(L"__v0") != std::wstring::npos);
            out.push_back(idx);
        } while (FindNextFileW(h2, &f2));
        FindClose(h2);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    if (out.empty()) { err = L"no images in dir"; return false; }
    return true;
}
static std::wstring nnFmt1(double v) { wchar_t b[32]; swprintf(b, 32, L"%.1f", v); return b; }
static std::wstring g_auditKey, g_auditOut; static DWORD g_auditTick = 0;

std::wstring nnAuditTrainSet(const std::wstring& dir, std::wstring& err) {
    std::wstring key = L"A|" + dir;
    if (key == g_auditKey && GetTickCount() - g_auditTick < 5000) return g_auditOut;
    const std::wstring backbone = resolveBackboneForTrain(L"");
    g_featNorm = g_trainNorm;
    std::vector<int> ix; if (!nnScanFolderFeats(backbone, dir, ix, err)) return L"";
    std::vector<int> O; for (int i : ix) if (g_ffCache[i].orig) O.push_back(i);
    if (O.empty()) O = ix;
    if (O.size() > 3000) O.resize(3000);
    std::vector<std::wstring> labels; std::vector<int> lab(O.size());
    for (size_t k = 0; k < O.size(); ++k) {
        const std::wstring& l = g_ffCache[O[k]].label; int id = -1;
        for (size_t c = 0; c < labels.size(); ++c) if (labels[c] == l) { id = (int)c; break; }
        if (id < 0) { id = (int)labels.size(); labels.push_back(l); }
        lab[k] = id;
    }
    const int C = (int)labels.size(); const size_t N = O.size();
    // 샘플별 같은 클래스 최대 유사 / 다른 클래스 최대 유사, 근사중복 쌍
    std::vector<float> own(N, -2.0f), oth(N, -2.0f); std::vector<int> othIdx(N, -1);
    std::wstring dupLines; int nDup = 0;
    for (size_t i = 0; i < N; ++i) {
        const auto& fi = g_ffCache[O[i]].feat;
        for (size_t j = i + 1; j < N; ++j) {
            float s = nnCos(fi, g_ffCache[O[j]].feat);
            if (lab[i] == lab[j]) { if (s > own[i]) own[i] = s; if (s > own[j]) own[j] = s; }
            else { if (s > oth[i]) { oth[i] = s; othIdx[i] = (int)j; } if (s > oth[j]) { oth[j] = s; othIdx[j] = (int)i; } }
            if (s >= 0.97f && nDup < 200) {
                dupLines += L"dup=" + g_ffCache[O[i]].path + L"\t" + g_ffCache[O[j]].path + L"\t" + nnFmt1(s * 100) + L"\t" + labels[lab[i]] + L"\t" + labels[lab[j]] + L"\n"; ++nDup;
            }
        }
    }
    // 클래스 프로토타입/통계
    const int D = (int)g_ffCache[O[0]].feat.size();
    std::vector<std::vector<double>> proto(C, std::vector<double>(D, 0.0)); std::vector<int> cnt(C, 0);
    std::vector<double> lumS(C, 0), lumS2(C, 0), conS(C, 0), blurS(C, 0), cohS(C, 0); std::vector<int> cohN(C, 0);
    std::vector<std::vector<long long>> areas(C); std::vector<std::vector<int>> ws(C), hs(C);
    for (size_t k = 0; k < N; ++k) {
        const NNFileFeat& e = g_ffCache[O[k]]; int c = lab[k];
        for (int d = 0; d < D && d < (int)e.feat.size(); ++d) proto[c][d] += e.feat[d];
        cnt[c]++; lumS[c] += e.lum; lumS2[c] += (double)e.lum * e.lum; conS[c] += e.con; blurS[c] += e.blur;
        if (own[k] > -1.5f) { cohS[c] += own[k]; cohN[c]++; }
        areas[c].push_back((long long)e.w * e.h); ws[c].push_back(e.w); hs[c].push_back(e.h);
    }
    for (int c = 0; c < C; ++c) { double n2 = 0; for (int d = 0; d < D; ++d) n2 += proto[c][d] * proto[c][d]; n2 = std::sqrt(n2); if (n2 < 1e-9) n2 = 1; for (int d = 0; d < D; ++d) proto[c][d] /= n2; }
    std::wstring out;
    out += L"classes="; for (int c = 0; c < C; ++c) { if (c) out += L"\t"; out += labels[c]; } out += L"\n";
    out += L"count=";   for (int c = 0; c < C; ++c) { if (c) out += L"\t"; out += std::to_wstring(cnt[c]); } out += L"\n";
    std::vector<long long> medArea(C, 0);
    for (int c = 0; c < C; ++c) {
        auto a = areas[c]; std::sort(a.begin(), a.end()); medArea[c] = a.empty() ? 0 : a[a.size() / 2];
        auto w = ws[c]; std::sort(w.begin(), w.end()); auto h = hs[c]; std::sort(h.begin(), h.end());
        double n = std::max(1, cnt[c]); double lm = lumS[c] / n, lsd = std::sqrt(std::max(0.0, lumS2[c] / n - lm * lm));
        out += L"stat=" + labels[c] + L"\t" + std::to_wstring(cnt[c]) + L"\t" + std::to_wstring(w.empty() ? 0 : w[w.size() / 2]) + L"\t" + std::to_wstring(h.empty() ? 0 : h[h.size() / 2])
             + L"\t" + nnFmt1(lm) + L"\t" + nnFmt1(lsd) + L"\t" + nnFmt1(conS[c] / n) + L"\t" + nnFmt1(blurS[c] / n)
             + L"\t" + (cohN[c] ? nnFmt1(cohS[c] / cohN[c] * 100) : L"-") + L"\n";
    }
    for (int a = 0; a < C; ++a) for (int b = a + 1; b < C; ++b) {
        double s = 0; for (int d = 0; d < D; ++d) s += proto[a][d] * proto[b][d];
        out += L"proto=" + labels[a] + L"\t" + labels[b] + L"\t" + nnFmt1(s * 100) + L"\n";
    }
    // 라벨 오류 의심: 다른 클래스 샘플이 같은 클래스 어떤 샘플보다 더 닮음(여유 3%p, 최소 45%). 같은 클래스가 1장뿐이면 85% 이상일 때만.
    int nSus = 0;
    for (size_t k = 0; k < N && nSus < 200; ++k) {
        if (othIdx[k] < 0) continue;
        bool sus = (own[k] > -1.5f) ? (oth[k] >= 0.45f && oth[k] > own[k] + 0.03f) : (oth[k] >= 0.85f);
        if (!sus) continue;
        const NNFileFeat& e = g_ffCache[O[k]]; const NNFileFeat& o = g_ffCache[O[othIdx[k]]];
        out += L"suspect=" + e.path + L"\t" + labels[lab[k]] + L"\t" + (own[k] > -1.5f ? nnFmt1(own[k] * 100) : L"-") + L"\t" + o.label + L"\t" + nnFmt1(oth[k] * 100) + L"\t" + o.path + L"\n"; ++nSus;
    }
    out += dupLines;
    // 크기 이탈(클래스 중앙값 면적의 16배 초과/1/16 미만 — 해상도 차이(750×1000 vs 3000×3000)는 정상, 96×96 크롭 vs 전체 사진 같은 구도 차이만) · 저품질(선명도<30 또는 대비<12)
    int nOut = 0, nLow = 0;
    for (size_t k = 0; k < N; ++k) {
        const NNFileFeat& e = g_ffCache[O[k]]; int c = lab[k];
        long long a = (long long)e.w * e.h;
        if (medArea[c] > 0 && cnt[c] >= 3 && nOut < 200 && (a > medArea[c] * 16 || a * 16 < medArea[c])) {
            out += L"outlier=" + e.path + L"\t" + labels[c] + L"\t" + std::to_wstring(e.w) + L"\t" + std::to_wstring(e.h) + L"\t" + nnFmt1((double)a / medArea[c]) + L"\n"; ++nOut; }
        if (nLow < 200 && (e.blur < 30.0f || e.con < 12.0f)) {
            out += L"lowq=" + e.path + L"\t" + labels[c] + L"\t" + nnFmt1(e.blur) + L"\t" + nnFmt1(e.con) + L"\n"; ++nLow; }
    }
    g_auditKey = key; g_auditOut = out; g_auditTick = GetTickCount();
    return out;
}

// 새 샘플 1장 검사. 반환(탭 구분 1줄):
//   w h lum con blur ownMax% ownPath otherMax% otherLabel otherPath clsN clsMedW clsMedH   (폴더에 비교 대상이 없으면 own/other 는 -1)
std::wstring nnCheckSample(const std::wstring& imgPath, const std::wstring& dir, const std::wstring& cls, std::wstring& err) {
    std::wstring key = L"C|" + imgPath + L"|" + dir + L"|" + cls;
    { WIN32_FILE_ATTRIBUTE_DATA fa{}; if (GetFileAttributesExW(imgPath.c_str(), GetFileExInfoStandard, &fa)) { wchar_t b[64]; swprintf(b, 64, L"|%lu|%lu|%lu", fa.ftLastWriteTime.dwLowDateTime, fa.nFileSizeLow, fa.ftLastWriteTime.dwHighDateTime); key += b; } }   // 같은 경로라도 파일이 바뀌면(마스킹 등) 캐시 무효
    if (key == g_auditKey && GetTickCount() - g_auditTick < 5000) return g_auditOut;
    const std::wstring backbone = resolveBackboneForTrain(L"");
    g_featNorm = g_trainNorm;
    NImg im; if (!nnLoad(imgPath, im)) { err = L"load failed"; return L""; }
    float lum, con, blur; nnImgStats(im, lum, con, blur);
    std::vector<float> f; bool haveF = extractFeat(backbone, im, f);
    std::vector<int> ix; std::wstring e2; bool haveDir = nnScanFolderFeats(backbone, dir, ix, e2);
    float own = -1, oth = -1; std::wstring ownP, othL, othP; int clsN = 0; std::vector<int> cw, ch;
    if (haveF && haveDir) for (int i : ix) {
        const NNFileFeat& e = g_ffCache[i];
        if (e.path == imgPath) continue;
        bool same = (e.label == cls);
        if (same && e.orig) { clsN++; cw.push_back(e.w); ch.push_back(e.h); }
        float s = nnCos(f, e.feat);
        if (same) { if (s > own) { own = s; ownP = e.path; } }
        else if (s > oth) { oth = s; othL = e.label; othP = e.path; }
    }
    std::sort(cw.begin(), cw.end()); std::sort(ch.begin(), ch.end());
    std::wstring out = std::to_wstring(im.w) + L"\t" + std::to_wstring(im.h) + L"\t" + nnFmt1(lum) + L"\t" + nnFmt1(con) + L"\t" + nnFmt1(blur)
        + L"\t" + (own >= 0 ? nnFmt1(own * 100) : L"-1") + L"\t" + ownP + L"\t" + (oth >= 0 ? nnFmt1(oth * 100) : L"-1") + L"\t" + othL + L"\t" + othP
        + L"\t" + std::to_wstring(clsN) + L"\t" + std::to_wstring(cw.empty() ? 0 : cw[cw.size() / 2]) + L"\t" + std::to_wstring(ch.empty() ? 0 : ch[ch.size() / 2]);
    g_auditKey = key; g_auditOut = out; g_auditTick = GetTickCount();
    return out;
}

// k-fold 교차검증. 원본(SRC 그룹) 단위로 나눠 같은 원본의 증강본이 학습/평가에 갈라지지 않게 한다. 평가는 진짜 원본(ORIG==1)만.
// 반환: "n=<평가수>\tk=<k>\tms=<ms>" / "acc=<클래스>\t<정답>\t<총>\t<정답최저점%>\t<오답최고점%>" / "conf=<정답라벨>\t<예측라벨|__unknown__>\t<수>" / "overall=<정답>\t<총>"
static std::wstring g_cvKey, g_cvOut; static DWORD g_cvTick = 0;
std::wstring nnCrossVal(const std::wstring& backboneIn, const std::wstring& dir, int k, int epochs, int hidden, int augLevel, std::wstring& err) {
    wchar_t kb[64]; swprintf(kb, 64, L"|%d|%d|%d|%d|%d|%d", k, epochs, hidden, augLevel, g_nnStrict, g_trainNorm);
    std::wstring key = L"V|" + dir + kb;
    if (key == g_cvKey && GetTickCount() - g_cvTick < 30000) return g_cvOut;
    DWORD t0 = GetTickCount();
    std::vector<std::vector<float>> X; std::vector<int> Y; std::vector<std::wstring> labels; std::vector<int> SRC, ORIG;
    const std::wstring backbone = resolveBackboneForTrain(backboneIn);
    g_featNorm = g_trainNorm;
    if (!collectData(backbone, dir, X, Y, labels, augLevel, &SRC, &ORIG)) { err = L"no training images or feature extraction failed"; return L""; }
    const int C = (int)labels.size();
    if (C < 2) { err = L"need 2+ classes for cross-validation"; return L""; }
    std::vector<int> groups; for (int g : SRC) if (g >= 0 && std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
    if (groups.size() < 2) { err = L"need 2+ source images"; return L""; }
    if (k < 2) k = 5; if (k > 10) k = 10; if ((int)groups.size() < k) k = (int)groups.size();
    std::mt19937 rng(4242); std::shuffle(groups.begin(), groups.end(), rng);
    // 클래스별로 고르게 섞이도록: 그룹을 (클래스, 순서)로 정렬 후 순환 배정
    std::vector<int> gLabel(groups.size()); for (size_t i = 0; i < groups.size(); ++i) { gLabel[i] = 0; for (size_t s = 0; s < SRC.size(); ++s) if (SRC[s] == groups[i]) { gLabel[i] = Y[s]; break; } }
    std::vector<int> order(groups.size()); for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return gLabel[a] < gLabel[b]; });
    std::vector<int> foldOf(groups.size()); for (size_t i = 0; i < order.size(); ++i) foldOf[order[i]] = (int)(i % k);
    auto foldOfSrc = [&](int src) { for (size_t i = 0; i < groups.size(); ++i) if (groups[i] == src) return foldOf[i]; return -1; };
    std::vector<int> sf(X.size()); for (size_t s = 0; s < X.size(); ++s) sf[s] = foldOfSrc(SRC[s]);
    std::vector<std::vector<int>> conf(C, std::vector<int>(C + 1, 0));   // 마지막 열 = __unknown__
    std::vector<int> corr(C, 0), tot(C, 0); std::vector<float> minCorrect(C, 999.0f), maxWrongAs(C, 0.0f);
    for (int f = 0; f < k; ++f) {
        std::vector<std::vector<float>> tX; std::vector<int> tY, tS, tO;
        for (size_t s = 0; s < X.size(); ++s) if (sf[s] != f) { tX.push_back(X[s]); tY.push_back(Y[s]); tS.push_back(SRC[s]); tO.push_back(ORIG[s]); }
        if (tX.empty()) continue;
        std::vector<int> seen(C, 0); for (int y : tY) seen[y] = 1; bool allCls = true; for (int c = 0; c < C; ++c) if (!seen[c]) allCls = false;
        if (!allCls) continue;   // 이 fold 의 학습셋에 빠진 클래스가 있으면(장수 극소) 건너뜀
        MLP net; nnFitModel(tX, tY, tS, tO, labels, epochs, hidden, net);
        for (size_t s = 0; s < X.size(); ++s) {
            if (sf[s] != f || ORIG[s] != 1) continue;
            std::vector<float> prob; float sc, gate, sim;
            int best = nnScoreGated(net, labels, X[s], prob, sc, gate, sim);
            bool unknown = (gate < 0.999f) && ((1.0f - gate) > prob[best]);
            int y = Y[s]; tot[y]++;
            int pred = unknown ? C : best;
            conf[y][pred]++;
            bool ok = (pred == y) || (unknown && labels[y] == L"__bg__");
            if (ok) { corr[y]++; if (!unknown && sc * 100 < minCorrect[y]) minCorrect[y] = sc * 100; }
            else if (!unknown && sc * 100 > maxWrongAs[best]) maxWrongAs[best] = sc * 100;
        }
    }
    int tc = 0, tn = 0; for (int c = 0; c < C; ++c) { tc += corr[c]; tn += tot[c]; }
    std::wstring out = L"n=" + std::to_wstring(tn) + L"\tk=" + std::to_wstring(k) + L"\tms=" + std::to_wstring(GetTickCount() - t0) + L"\n";
    for (int c = 0; c < C; ++c)
        out += L"acc=" + labels[c] + L"\t" + std::to_wstring(corr[c]) + L"\t" + std::to_wstring(tot[c]) + L"\t" + (minCorrect[c] > 500 ? L"-" : nnFmt1(minCorrect[c])) + L"\t" + nnFmt1(maxWrongAs[c]) + L"\n";
    for (int y = 0; y < C; ++y) for (int p = 0; p <= C; ++p) if (conf[y][p])
        out += L"conf=" + labels[y] + L"\t" + (p == C ? std::wstring(L"__unknown__") : labels[p]) + L"\t" + std::to_wstring(conf[y][p]) + L"\n";
    out += L"overall=" + std::to_wstring(tc) + L"\t" + std::to_wstring(tn) + L"\n";
    g_cvKey = key; g_cvOut = out; g_cvTick = GetTickCount();
    return out;
}

// 사진의 영역(x,y,w,h)만 가려 저장. mode 0 = 영역 둘레(테두리 띠)의 평균색으로 채움(가장 자연스러움), 1 = 굵은 모자이크(블록 = 영역/8).
// 오탐 추적에서 "닮은 부분"만 지우고 나머지(정상 부분)는 학습에 남길 때. 반환 1 성공.
int nnImageMaskRect(const std::wstring& inPath, const std::wstring& outPath, int x, int y, int w, int h, int mode, int quality) {
    NImg im; if (!nnLoad(inPath, im)) return 0;
    int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(im.w, x + w), y1 = std::min(im.h, y + h);
    if (x1 <= x0 || y1 <= y0) return 0;
    if (mode == 1) {
        int bs = std::max(4, std::min(x1 - x0, y1 - y0) / 8);
        for (int by = y0; by < y1; by += bs) for (int bx = x0; bx < x1; bx += bs) {
            int ex = std::min(x1, bx + bs), ey = std::min(y1, by + bs); long acc[3] = { 0,0,0 }; int n = 0;
            for (int yy = by; yy < ey; ++yy) for (int xx = bx; xx < ex; ++xx) { const unsigned char* p = im.px.data() + ((size_t)yy * im.w + xx) * 4; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; ++n; }
            for (int yy = by; yy < ey; ++yy) for (int xx = bx; xx < ex; ++xx) { unsigned char* p = im.px.data() + ((size_t)yy * im.w + xx) * 4; p[0] = (unsigned char)(acc[0] / n); p[1] = (unsigned char)(acc[1] / n); p[2] = (unsigned char)(acc[2] / n); }
        }
    } else {
        int ring = std::max(4, std::min(x1 - x0, y1 - y0) / 4);
        long acc[3] = { 0,0,0 }; long n = 0;
        for (int yy = std::max(0, y0 - ring); yy < std::min(im.h, y1 + ring); ++yy) for (int xx = std::max(0, x0 - ring); xx < std::min(im.w, x1 + ring); ++xx) {
            if (xx >= x0 && xx < x1 && yy >= y0 && yy < y1) continue;
            const unsigned char* p = im.px.data() + ((size_t)yy * im.w + xx) * 4; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; ++n; }
        if (n == 0) { for (int i = 0; i < im.w * im.h; ++i) { const unsigned char* p = im.px.data() + (size_t)i * 4; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; ++n; } }
        unsigned char fill[3] = { (unsigned char)(acc[0] / n), (unsigned char)(acc[1] / n), (unsigned char)(acc[2] / n) };
        for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) { unsigned char* p = im.px.data() + ((size_t)yy * im.w + xx) * 4; p[0] = fill[0]; p[1] = fill[1]; p[2] = fill[2]; }
    }
    return nnSaveImg(im, outPath, quality) ? 1 : 0;
}

} // namespace notepad

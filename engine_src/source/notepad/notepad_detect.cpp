// ============================================================================
//  NOTEPAD.dll  -  notepad_detect.cpp
//  [NOTEPAD] 세트 AB+: 통짜 객체탐지 (이미지/화면영역 -> 박스 목록).
//  링크: gdiplus.lib (이미 포함). ONNX 추론은 notepad_onnx.cpp 재사용.
//
//  Note_OnnxInfer 는 float CSV 를 주고받아 YOLO 처럼 입력이 큰 모델엔 비현실적이다.
//  이 파일은 그 간극을 메운다: 이미지 파일(또는 화면 영역)을 주면 DLL 내부에서
//    1) letterbox 리사이즈(640x640, 비율 유지+패딩)
//    2) NCHW float 정규화(0~1) 텐서 생성
//    3) ONNX 추론(onnxRunFloatsEx)
//    4) YOLOv8 출력 파싱(1x84x8400 등) + 신뢰도 필터 + NMS
//    5) 원본 좌표로 역변환
//  까지 다 하고 "클래스,x,y,w,h,점수" 목록만 돌려준다. AHK 는 한 줄 호출.
//
//  지원: YOLOv8/YOLOv5 계열(출력 [1,84,8400] 또는 [1,8400,84]). 입력 640 기본.
//  클래스 이름은 COCO 80 기본 내장(모델이 그 순서면 이름까지 반환).
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

namespace notepad {

// notepad_onnx.cpp
bool onnxRunFloatsEx(const std::wstring& modelPath, const std::vector<float>& input,
                     const std::vector<int64_t>& shape, std::vector<float>& output,
                     std::vector<int64_t>& outShape);
// notepad_screen.cpp
bool captureRegionToPng(int x, int y, int w, int h, const std::string& outPngUtf8);

// COCO 80 클래스(YOLOv8 기본 학습셋 순서)
static const char* COCO80[80] = {
 "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat",
 "traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat",
 "dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack",
 "umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball",
 "kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket",
 "bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
 "sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair",
 "couch","potted plant","bed","dining table","toilet","tv","laptop","mouse",
 "remote","keyboard","cell phone","microwave","oven","toaster","sink",
 "refrigerator","book","clock","vase","scissors","teddy bear","hair drier",
 "toothbrush" };

static ULONG_PTR g_dtGpToken = 0; static bool g_dtGpUp = false;
static void dtEnsureGdiplus() {
    if (g_dtGpUp) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_dtGpToken, &in, nullptr) == Gdiplus::Ok) g_dtGpUp = true;
}
static std::wstring dtU8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string dtWToU8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

struct DImg { int w = 0, h = 0; std::vector<unsigned char> px; }; // BGRA
static bool dtLoad(const std::wstring& wpath, DImg& img) {
    dtEnsureGdiplus();
    Gdiplus::Bitmap bmp(wpath.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;
    Gdiplus::Rect rc(0, 0, w, h); Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    img.w = w; img.h = h; img.px.resize((size_t)w * h * 4);
    for (int r = 0; r < h; ++r)
        memcpy(img.px.data() + (size_t)r * w * 4, (unsigned char*)bd.Scan0 + (size_t)r * bd.Stride, (size_t)w * 4);
    bmp.UnlockBits(&bd);
    return true;
}

// letterbox: 원본을 비율유지로 side x side 에 맞추고 회색(114) 패딩.
//   scale, padX, padY 를 돌려줘 나중에 좌표 역변환에 쓴다.
static void letterbox(const DImg& src, int side, std::vector<float>& chw,
                      double& scale, int& padX, int& padY) {
    scale = std::min((double)side / src.w, (double)side / src.h);
    int nw = (int)std::round(src.w * scale), nh = (int)std::round(src.h * scale);
    padX = (side - nw) / 2; padY = (side - nh) / 2;

    // 리사이즈(nearest, 빠름) -> 패딩 캔버스에 배치 -> NCHW float(0~1), RGB 순
    chw.assign((size_t)3 * side * side, 114.0f / 255.0f); // 패딩 기본값
    for (int y = 0; y < nh; ++y) {
        int sy = std::min((int)(y / scale), src.h - 1);
        for (int x = 0; x < nw; ++x) {
            int sx = std::min((int)(x / scale), src.w - 1);
            const unsigned char* p = src.px.data() + ((size_t)sy * src.w + sx) * 4; // BGRA
            int dy = y + padY, dx = x + padX;
            size_t base = (size_t)dy * side + dx;
            chw[0 * side * side + base] = p[2] / 255.0f; // R
            chw[1 * side * side + base] = p[1] / 255.0f; // G
            chw[2 * side * side + base] = p[0] / 255.0f; // B
        }
    }
}

struct Det { int cls; float score; float x, y, w, h; };

static float iou(const Det& a, const Det& b) {
    float ax2 = a.x + a.w, ay2 = a.y + a.h, bx2 = b.x + b.w, by2 = b.y + b.h;
    float ix1 = std::max(a.x, b.x), iy1 = std::max(a.y, b.y);
    float ix2 = std::min(ax2, bx2), iy2 = std::min(ay2, by2);
    float iw = std::max(0.f, ix2 - ix1), ih = std::max(0.f, iy2 - iy1);
    float inter = iw * ih, uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0 ? inter / uni : 0;
}

// YOLOv8 출력 파싱. out 은 flat, shape 은 [1,C,N] 또는 [1,N,C] (C=4+numClasses).
static void parseYolo(const std::vector<float>& out, const std::vector<int64_t>& shape,
                      float confThr, std::vector<Det>& dets) {
    if (shape.size() != 3) return;
    int C, N; bool clsMajor; // clsMajor=true 이면 [1,C,N]
    if (shape[1] < shape[2]) { C = (int)shape[1]; N = (int)shape[2]; clsMajor = true; }
    else { C = (int)shape[2]; N = (int)shape[1]; clsMajor = false; }
    int numCls = C - 4;
    if (numCls <= 0) return;
    auto val = [&](int c, int n) -> float {
        return clsMajor ? out[(size_t)c * N + n] : out[(size_t)n * C + c];
    };
    for (int n = 0; n < N; ++n) {
        // 최고 클래스
        int bestC = -1; float bestS = 0;
        for (int c = 0; c < numCls; ++c) {
            float s = val(4 + c, n);
            if (s > bestS) { bestS = s; bestC = c; }
        }
        if (bestC < 0 || bestS < confThr) continue;
        // YOLOv8: cx,cy,w,h (letterbox 640 좌표)
        float cx = val(0, n), cy = val(1, n), bw = val(2, n), bh = val(3, n);
        Det d; d.cls = bestC; d.score = bestS;
        d.x = cx - bw / 2; d.y = cy - bh / 2; d.w = bw; d.h = bh;
        dets.push_back(d);
    }
}

static void nms(std::vector<Det>& dets, float iouThr) {
    std::sort(dets.begin(), dets.end(), [](const Det& a, const Det& b) { return a.score > b.score; });
    std::vector<bool> removed(dets.size(), false);
    for (size_t i = 0; i < dets.size(); ++i) {
        if (removed[i]) continue;
        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (removed[j] || dets[j].cls != dets[i].cls) continue;
            if (iou(dets[i], dets[j]) > iouThr) removed[j] = true;
        }
    }
    std::vector<Det> keep;
    for (size_t i = 0; i < dets.size(); ++i) if (!removed[i]) keep.push_back(dets[i]);
    dets = keep;
}

// 공개 진입점: 이미지 파일 -> 탐지 결과 문자열.
//   반환(성공): 각 줄 "className,x,y,w,h,score" 개행구분(원본 픽셀 좌표).
//   confThr(0~1, 예 0.25), iouThr(예 0.45), side(입력크기, 보통 640).
bool detectObjectsFile(const std::wstring& modelPath, const std::wstring& imgPath,
                       double confThr, double iouThr, int side, std::wstring& outText) {
    outText.clear();
    if (side <= 0) side = 640;
    DImg img;
    if (!dtLoad(imgPath, img)) return false;

    std::vector<float> chw; double scale; int padX, padY;
    letterbox(img, side, chw, scale, padX, padY);

    std::vector<int64_t> inShape = { 1, 3, side, side };
    std::vector<float> out; std::vector<int64_t> outShape;
    if (!onnxRunFloatsEx(modelPath, chw, inShape, out, outShape)) return false;
    if (out.empty() || outShape.size() != 3) return false;

    std::vector<Det> dets;
    parseYolo(out, outShape, (float)confThr, dets);
    nms(dets, (float)iouThr);

    // letterbox 좌표 -> 원본 좌표 역변환
    std::wstring res;
    wchar_t buf[256];
    for (auto& d : dets) {
        float ox = (d.x - padX) / (float)scale;
        float oy = (d.y - padY) / (float)scale;
        float ow = d.w / (float)scale;
        float oh = d.h / (float)scale;
        if (ox < 0) ox = 0; if (oy < 0) oy = 0;
        std::string name = (d.cls >= 0 && d.cls < 80) ? COCO80[d.cls] : ("class" + std::to_string(d.cls));
        std::wstring wname = dtU8ToW(name);
        swprintf(buf, 256, L"%s,%d,%d,%d,%d,%.3f", wname.c_str(),
                 (int)std::round(ox), (int)std::round(oy),
                 (int)std::round(ow), (int)std::round(oh), d.score);
        if (!res.empty()) res += L"\n";
        res += buf;
    }
    outText = res;
    return true;
}

// 화면 영역 -> 탐지(임시 PNG 경유). 좌표는 화면 절대좌표로 보정.
bool detectObjectsScreen(const std::wstring& modelPath, int x, int y, int w, int h,
                         double confThr, double iouThr, int side, std::wstring& outText) {
    outText.clear();
    wchar_t dir[MAX_PATH], tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return false;
    if (!GetTempFileNameW(dir, L"npd", 0, tmp)) return false;
    std::wstring wpng = std::wstring(tmp) + L".png";
    bool ok = captureRegionToPng(x, y, w, h, dtWToU8(wpng)) &&
              detectObjectsFile(modelPath, wpng, confThr, iouThr, side, outText);
    DeleteFileW(tmp); DeleteFileW(wpng.c_str());
    if (!ok) return false;
    // 영역 좌표 -> 화면 절대좌표 보정
    if (!outText.empty()) {
        std::wstring fixed; std::wstring line;
        auto flush = [&](const std::wstring& ln) {
            if (ln.empty()) return;
            // "name,x,y,w,h,score" 에서 x,y 에 (x,y) 더하기
            std::vector<std::wstring> f; std::wstring cur;
            for (wchar_t c : ln) { if (c == L',') { f.push_back(cur); cur.clear(); } else cur.push_back(c); }
            f.push_back(cur);
            if (f.size() == 6) {
                int bx = _wtoi(f[1].c_str()) + x, by = _wtoi(f[2].c_str()) + y;
                wchar_t b2[256];
                swprintf(b2, 256, L"%s,%d,%d,%s,%s,%s", f[0].c_str(), bx, by, f[3].c_str(), f[4].c_str(), f[5].c_str());
                if (!fixed.empty()) fixed += L"\n";
                fixed += b2;
            }
        };
        for (wchar_t c : outText) { if (c == L'\n') { flush(line); line.clear(); } else line.push_back(c); }
        flush(line);
        outText = fixed;
    }
    return true;
}

} // namespace notepad

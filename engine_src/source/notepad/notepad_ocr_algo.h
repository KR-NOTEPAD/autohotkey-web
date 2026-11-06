// ============================================================================
//  NOTEPAD.dll  -  notepad_ocr_algo.h
//  [NOTEPAD] 세트 AI: AI OCR 순수 알고리즘 (플랫폼 독립, 의존성 0).
//  DBNet 후처리(연결요소 박스/언클립/읽기순 정렬) + CRNN CTC 그리디 디코딩.
//  Windows/ORT/GDI+ 와 분리해 리눅스 g++ 로도 그대로 단위테스트 가능하게 함.
//  (notepad_ocr_ai.cpp 와 tests/ocr_algo_test.cpp 가 동일 파일을 include)
// ============================================================================
#ifndef NOTEPAD_OCR_ALGO_H
#define NOTEPAD_OCR_ALGO_H

#include <vector>
#include <string>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace notepad {
namespace ocralgo {

// ---- 검출 박스(정수 AABB + 점수) -------------------------------------------
struct Comp {
    int x1, y1, x2, y2;   // inclusive bbox (검출 특징맵 좌표계)
    int count;            // 구성 픽셀 수
    double sumProb;       // 구성 픽셀 prob 합
    double meanProb() const { return count > 0 ? sumProb / count : 0.0; }
    int w() const { return x2 - x1 + 1; }
    int h() const { return y2 - y1 + 1; }
};

// ---- 8방향 연결요소 라벨링 --------------------------------------------------
//  prob: [H*W] float 확률맵(0~1). thr 이상 픽셀을 텍스트로 보고 blob 을 만든다.
//  minSide/minArea 로 잡음 제거. 반환: 각 blob 의 bbox + 픽셀수 + prob 합.
inline std::vector<Comp> connectedComponents(const std::vector<float>& prob,
        int W, int H, float thr, int minSide, int minArea) {
    std::vector<Comp> comps;
    if (W <= 0 || H <= 0 || (int)prob.size() < W * H) return comps;
    std::vector<uint8_t> vis((size_t)W * H, 0);
    std::vector<int> stack;
    stack.reserve(1024);
    const int dx[8] = { 1,-1,0,0, 1,1,-1,-1 };
    const int dy[8] = { 0,0,1,-1, 1,-1,1,-1 };
    for (int sy = 0; sy < H; ++sy) {
        for (int sx = 0; sx < W; ++sx) {
            int si = sy * W + sx;
            if (vis[si] || prob[si] < thr) continue;
            // flood fill
            Comp c; c.x1 = c.x2 = sx; c.y1 = c.y2 = sy; c.count = 0; c.sumProb = 0.0;
            stack.clear(); stack.push_back(si); vis[si] = 1;
            while (!stack.empty()) {
                int idx = stack.back(); stack.pop_back();
                int cx = idx % W, cy = idx / W;
                c.count++; c.sumProb += prob[idx];
                if (cx < c.x1) c.x1 = cx; if (cx > c.x2) c.x2 = cx;
                if (cy < c.y1) c.y1 = cy; if (cy > c.y2) c.y2 = cy;
                for (int k = 0; k < 8; ++k) {
                    int nx = cx + dx[k], ny = cy + dy[k];
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    int ni = ny * W + nx;
                    if (vis[ni] || prob[ni] < thr) continue;
                    vis[ni] = 1; stack.push_back(ni);
                }
            }
            if (c.w() < minSide || c.h() < minSide) continue;
            if (c.count < minArea) continue;
            comps.push_back(c);
        }
    }
    return comps;
}

// ---- 언클립(박스 확장) ------------------------------------------------------
//  DBNet 확률맵은 축소(shrink)되어 있어, 원 글자 경계까지 되살리려면 박스를
//  바깥으로 넓힌다. PaddleOCR 방식: d = area*ratio/perimeter 만큼 각 변 확장.
//  좌표는 [0,maxW-1]/[0,maxH-1] 로 클램프.
inline void unclip(int& x1, int& y1, int& x2, int& y2,
                   double ratio, int maxW, int maxH) {
    double w = x2 - x1 + 1, h = y2 - y1 + 1;
    double area = w * h, peri = 2.0 * (w + h);
    if (peri <= 0) return;
    double d = area * ratio / peri;
    int di = (int)std::lround(d);
    x1 -= di; y1 -= di; x2 += di; y2 += di;
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 > maxW - 1) x2 = maxW - 1;
    if (y2 > maxH - 1) y2 = maxH - 1;
}

// ---- 읽기순 정렬(위->아래, 같은 줄은 왼->오) --------------------------------
//  각 박스의 y중심을 줄 높이로 버킷화해 (행,열) 순으로 안정 정렬.
template <typename BoxT, typename GetX1, typename GetY1, typename GetY2>
inline void sortReadingOrder(std::vector<BoxT>& boxes,
        GetX1 gx1, GetY1 gy1, GetY2 gy2) {
    int n = (int)boxes.size();
    if (n <= 1) return;
    std::vector<int> heights;
    heights.reserve(n);
    for (auto& b : boxes) heights.push_back(gy2(b) - gy1(b) + 1);
    std::vector<int> hs = heights;
    std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
    double medH = (double)hs[hs.size() / 2];
    if (medH < 1) medH = 1;
    double rowH = medH * 0.7; if (rowH < 1) rowH = 1;
    std::stable_sort(boxes.begin(), boxes.end(), [&](const BoxT& a, const BoxT& b) {
        double ayc = (gy1(a) + gy2(a)) * 0.5, byc = (gy1(b) + gy2(b)) * 0.5;
        long ra = (long)std::floor(ayc / rowH), rb = (long)std::floor(byc / rowH);
        if (ra != rb) return ra < rb;
        return gx1(a) < gx1(b);
    });
}

// ---- CTC 그리디 디코딩 ------------------------------------------------------
//  logits: [T*C] (행=timestep). blank=0. 규칙: 연속 동일 인덱스 축약 후 blank 제거.
//  반환: 비blank 클래스 인덱스열(outIdx, 1-based 클래스) + 각 스텝 확률(outProb).
//  문자 매핑(charset[idx-1])은 호출부에서 수행.
inline void ctcGreedyDecode(const float* logits, int T, int C,
        std::vector<int>& outIdx, std::vector<float>& outProb) {
    outIdx.clear(); outProb.clear();
    if (!logits || T <= 0 || C <= 0) return;
    int prev = 0; // blank
    for (int t = 0; t < T; ++t) {
        const float* row = logits + (size_t)t * C;
        int best = 0; float bv = row[0];
        for (int c = 1; c < C; ++c) if (row[c] > bv) { bv = row[c]; best = c; }
        if (best != 0 && best != prev) { outIdx.push_back(best); outProb.push_back(bv); }
        prev = best;
    }
}

// ---- 한글 자모 -> 완성형 음절 조합(NFC) -------------------------------------
//  PP-OCRv5 한국어 사전에는 첫가끝 자모(U+1100~)가 포함돼 있어, 디코딩 결과에
//  자모가 섞여 나올 수 있다. L(+V)(+T) 를 만나면 완성형(U+AC00~)으로 합친다.
//  자모가 없으면 입력을 그대로 돌려준다(무해).
inline std::wstring composeHangul(const std::wstring& s) {
    auto isL = [](wchar_t c) { return c >= 0x1100 && c <= 0x1112; };
    auto isV = [](wchar_t c) { return c >= 0x1161 && c <= 0x1175; };
    auto isT = [](wchar_t c) { return c >= 0x11A8 && c <= 0x11C2; };
    bool any = false;
    for (wchar_t c : s) if (isL(c) || isV(c) || isT(c)) { any = true; break; }
    if (!any) return s;
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (isL(c) && i + 1 < s.size() && isV(s[i + 1])) {
            int L = c - 0x1100, V = s[i + 1] - 0x1161, T = 0;
            size_t used = 1;
            if (i + 2 < s.size() && isT(s[i + 2])) { T = s[i + 2] - 0x11A7; used = 2; }
            out.push_back((wchar_t)(0xAC00 + (L * 21 + V) * 28 + T));
            i += used;
            continue;
        }
        // 이미 완성형인데 종성이 비어 있고 뒤에 종성 자모가 오는 경우
        if (c >= 0xAC00 && c <= 0xD7A3 && ((c - 0xAC00) % 28) == 0 &&
            i + 1 < s.size() && isT(s[i + 1])) {
            out.push_back((wchar_t)(c + (s[i + 1] - 0x11A7)));
            ++i;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

} // namespace ocralgo
} // namespace notepad
#endif // NOTEPAD_OCR_ALGO_H

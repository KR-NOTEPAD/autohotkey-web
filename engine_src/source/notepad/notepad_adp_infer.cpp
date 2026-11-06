// ============================================================================
//  NOTEPAD.dll  -  notepad_adp_infer.cpp
//  [NOTEPAD] 세트 AL-2: ADP 추론통계 — 추정 / 가설검정 / 상관 / 회귀 / 분산분석 / 전처리
//
//  공부 포인트:
//   * 모든 신뢰구간은 "추정량 ± 임계값 × 표준오차" 한 구조다. 무엇이 추정량이고 표준오차가
//     어디서 오는지(√(s²/n), √(p(1-p)/n), Fisher z …)만 바뀐다.
//   * 모든 검정은 "통계량 = (관측 − 귀무가설값) / 표준오차" 를 구하고, 그 통계량이 따르는
//     분포(t, z, χ², F)에서 꼬리확률(p-value)을 읽는다. alt 는 어느 꼬리를 읽을지 정한다.
//   * 회귀는 최소제곱 = 선형대수(QR 분해). 정규방정식 (XᵀX)⁻¹Xᵀy 를 직접 풀면 조건수가
//     제곱되어 불안정하므로 Householder QR 을 쓴다.
// ============================================================================
#include "notepad_adp.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>
#include <map>

namespace adp {

static const double NaN = std::numeric_limits<double>::quiet_NaN();
static const double INF = std::numeric_limits<double>::infinity();

// ---- 공통: 꼬리확률 -------------------------------------------------------
// alt 0 양측 / 1 우측(H1: >) / -1 좌측(H1: <)
static double pFromT(double t, double df, int alt) {
    if (alt > 0) return tSf(t, df);
    if (alt < 0) return tSf(-t, df);
    return std::min(1.0, 2 * tSf(std::fabs(t), df));
}
static double pFromZ(double z, int alt) {
    if (alt > 0) return normCdf(-z);
    if (alt < 0) return normCdf(z);
    return std::min(1.0, 2 * normCdf(-std::fabs(z)));
}
static bool confOk(double conf) { return conf > 0 && conf < 1; }
static bool altOk(int alt) { return alt == 0 || alt == 1 || alt == -1; }
// 양측이면 α/2, 단측이면 α 에 해당하는 임계값 계수
static double tCrit(double conf, double df, int alt) { return alt == 0 ? tQuantile(1 - (1 - conf) / 2, df) : tQuantile(conf, df); }
static double zCrit(double conf, int alt)            { return alt == 0 ? normQuantile(1 - (1 - conf) / 2) : normQuantile(conf); }
// CI 구간: 양측 [est±c·se], 우측 [est−c·se, +∞), 좌측 (−∞, est+c·se]
static void ciFrom(double est, double c, double se, int alt, double& lo, double& hi) {
    if (alt == 0) { lo = est - c * se; hi = est + c * se; }
    else if (alt > 0) { lo = est - c * se; hi = INF; }
    else { lo = -INF; hi = est + c * se; }
}

// ============================================================================
//  4. 추정
// ============================================================================
// [원리] x̄ ± t_{n−1, 1−α/2} · s/√n.  σ 를 모르므로 s 로 대신하고, 그 불확실성을 t 분포가 보정한다.
int meanCI(const double* x, int n, double conf, double* out) {
    if (!x || !out) return ERR_NULL; if (n < 2) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    if (!confOk(conf)) return ERR_PARAM;
    double m = mean(x, n), sd; stddev(x, n, 1, sd);
    double se = sd / std::sqrt((double)n), df = n - 1, c = tQuantile(1 - (1 - conf) / 2, df);
    out[0] = m; out[1] = m - c * se; out[2] = m + c * se; out[3] = se; out[4] = df; return OK;
}
// [원리] Wald: p̂ ± z√(p̂(1−p̂)/n) — n 작거나 p̂ 극단이면 실제 포함확률이 낮다.
//        Wilson: z² 을 포함한 이차식 해 — 작은 n 에서도 안정. 실무 기본 권장.
//        Agresti-Coull: 성공 z²/2, 실패 z²/2 를 더한 뒤 Wald.
int proportionCI(int k, int n, double conf, int method, double* out) {
    if (!out) return ERR_NULL; if (n < 1 || k < 0 || k > n) return ERR_SIZE; if (!confOk(conf)) return ERR_PARAM;
    double p = (double)k / n, z = normQuantile(1 - (1 - conf) / 2);
    out[0] = p; out[3] = std::sqrt(p * (1 - p) / n);
    if (method == 0) { out[1] = std::max(0.0, p - z * out[3]); out[2] = std::min(1.0, p + z * out[3]); }
    else if (method == 1) {
        double z2 = z * z, den = 1 + z2 / n, c = (p + z2 / (2 * n)) / den, h = z * std::sqrt(p * (1 - p) / n + z2 / (4.0 * n * n)) / den;
        out[1] = std::max(0.0, c - h); out[2] = std::min(1.0, c + h);
    } else if (method == 2) {
        double nt = n + z * z, pt = (k + z * z / 2) / nt, h = z * std::sqrt(pt * (1 - pt) / nt);
        out[1] = std::max(0.0, pt - h); out[2] = std::min(1.0, pt + h);
    } else return ERR_PARAM;
    return OK;
}
// [원리] (n−1)s²/σ² ~ χ²_{n−1}  →  σ² ∈ [(n−1)s²/χ²_{1−α/2}, (n−1)s²/χ²_{α/2}]. 정규성 가정에 민감.
int varianceCI(const double* x, int n, double conf, double* out) {
    if (!x || !out) return ERR_NULL; if (n < 2) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    if (!confOk(conf)) return ERR_PARAM;
    double v; variance(x, n, 1, v); double df = n - 1, a = 1 - conf;
    out[0] = v; out[1] = df * v / chi2Quantile(1 - a / 2, df); out[2] = df * v / chi2Quantile(a / 2, df);
    out[3] = std::sqrt(v); out[4] = std::sqrt(out[1]); out[5] = std::sqrt(out[2]); return OK;
}
static void twoSampleSe(const double* x, int nx, const double* y, int ny, int equalVar, double& se, double& df, double& sp) {
    double vx, vy; variance(x, nx, 1, vx); variance(y, ny, 1, vy);
    sp = std::sqrt(((nx - 1) * vx + (ny - 1) * vy) / (nx + ny - 2));
    if (equalVar) { se = sp * std::sqrt(1.0 / nx + 1.0 / ny); df = nx + ny - 2; }
    else {   // Welch–Satterthwaite 자유도
        double a = vx / nx, b = vy / ny; se = std::sqrt(a + b);
        df = (a + b) * (a + b) / (a * a / (nx - 1) + b * b / (ny - 1));
    }
}
int diffMeansCI(const double* x, int nx, const double* y, int ny, double conf, int equalVar, double* out) {
    if (!x || !y || !out) return ERR_NULL; if (nx < 2 || ny < 2) return ERR_SIZE;
    if (!allFinite(x, nx) || !allFinite(y, ny)) return ERR_NAN; if (!confOk(conf)) return ERR_PARAM;
    double se, df, sp; twoSampleSe(x, nx, y, ny, equalVar, se, df, sp);
    if (!(se > 0)) return ERR_DEGENERATE;
    double d = mean(x, nx) - mean(y, ny), c = tQuantile(1 - (1 - conf) / 2, df);
    out[0] = d; out[1] = d - c * se; out[2] = d + c * se; out[3] = se; out[4] = df; return OK;
}
int diffPropsCI(int k1, int n1, int k2, int n2, double conf, double* out) {
    if (!out) return ERR_NULL; if (n1 < 1 || n2 < 1 || k1 < 0 || k2 < 0 || k1 > n1 || k2 > n2) return ERR_SIZE;
    if (!confOk(conf)) return ERR_PARAM;
    double p1 = (double)k1 / n1, p2 = (double)k2 / n2, se = std::sqrt(p1 * (1 - p1) / n1 + p2 * (1 - p2) / n2);
    double z = normQuantile(1 - (1 - conf) / 2);
    out[0] = p1 - p2; out[1] = out[0] - z * se; out[2] = out[0] + z * se; out[3] = se; return OK;
}
// [원리] 오차한계 E = z·σ/√n  →  n = (zσ/E)².  올림해서 정수.
int sampleSizeMean(double sigma, double E, double conf, double& n) {
    if (sigma <= 0 || E <= 0 || !confOk(conf)) return ERR_PARAM;
    double z = normQuantile(1 - (1 - conf) / 2); n = std::ceil(z * sigma / E * (z * sigma / E)); return OK;
}
int sampleSizeProportion(double p, double E, double conf, double& n) {
    if (p < 0 || p > 1 || E <= 0 || !confOk(conf)) return ERR_PARAM;
    double z = normQuantile(1 - (1 - conf) / 2); n = std::ceil(z * z * p * (1 - p) / (E * E)); return OK;
}
// [원리] 두 집단 평균차 δ 를 검출력 1−β 로 잡으려면 집단당 n = 2σ²(z_{1−α/2}+z_{1−β})²/δ².
int sampleSizeTwoGroups(double delta, double sigma, double alpha, double power, double& n) {
    if (delta == 0 || sigma <= 0 || !confOk(alpha) || !confOk(power)) return ERR_PARAM;
    double za = normQuantile(1 - alpha / 2), zb = normQuantile(power);
    n = std::ceil(2 * sigma * sigma * (za + zb) * (za + zb) / (delta * delta)); return OK;
}

// ============================================================================
//  5. 가설검정
// ============================================================================
// [원리] t = (x̄ − μ₀)/(s/√n), df = n−1.  효과크기 Cohen d = (x̄ − μ₀)/s (n 에 무관한 "차이의 크기").
int tTestOne(const double* x, int n, double mu0, int alt, double conf, double* out) {
    if (!x || !out) return ERR_NULL; if (n < 2) return ERR_SIZE; if (!allFinite(x, n) || !std::isfinite(mu0)) return ERR_NAN;
    if (!altOk(alt) || !confOk(conf)) return ERR_PARAM;
    double m = mean(x, n), sd; stddev(x, n, 1, sd);
    if (!(sd > 0)) return ERR_DEGENERATE;
    double se = sd / std::sqrt((double)n), df = n - 1, t = (m - mu0) / se;
    out[0] = t; out[1] = df; out[2] = pFromT(t, df, alt);
    ciFrom(m, tCrit(conf, df, alt), se, alt, out[3], out[4]);
    out[5] = (m - mu0) / sd; out[6] = m; out[7] = se; return OK;
}
// [원리] 합동분산 t: 두 모분산이 같다고 가정(sp² 로 합침). Welch: 가정하지 않음(자유도만 줄어듦).
//        실무 기본은 Welch — 등분산 검정 후 선택하는 2단계 절차는 오히려 오류율을 높인다.
int tTestTwo(const double* x, int nx, const double* y, int ny, int equalVar, int alt, double conf, double* out) {
    if (!x || !y || !out) return ERR_NULL; if (nx < 2 || ny < 2) return ERR_SIZE;
    if (!allFinite(x, nx) || !allFinite(y, ny)) return ERR_NAN; if (!altOk(alt) || !confOk(conf)) return ERR_PARAM;
    double se, df, sp; twoSampleSe(x, nx, y, ny, equalVar, se, df, sp);
    if (!(se > 0) || !(sp > 0)) return ERR_DEGENERATE;
    double d = mean(x, nx) - mean(y, ny), t = d / se;
    out[0] = t; out[1] = df; out[2] = pFromT(t, df, alt);
    ciFrom(d, tCrit(conf, df, alt), se, alt, out[3], out[4]);
    out[5] = d / sp; out[6] = d; out[7] = se; return OK;
}
// [원리] 같은 대상의 전/후 차이 d = x−y 를 일표본 t 로. 개체 간 변동이 제거되어 검정력이 높다.
int tTestPaired(const double* x, const double* y, int n, int alt, double conf, double* out) {
    if (!x || !y) return ERR_NULL; if (n < 2) return ERR_SIZE;
    std::vector<double> d(n); for (int i = 0; i < n; ++i) d[i] = x[i] - y[i];
    return tTestOne(d.data(), n, 0.0, alt, conf, out);
}
// [원리] χ² = (n−1)s²/σ₀² ~ χ²_{n−1}. 정규성에 매우 민감(왜도 있으면 오류율 급증).
int varTestChi(const double* x, int n, double sigma0Sq, int alt, double conf, double* out) {
    if (!x || !out) return ERR_NULL; if (n < 2) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    if (sigma0Sq <= 0 || !altOk(alt) || !confOk(conf)) return ERR_PARAM;
    double v; variance(x, n, 1, v); double df = n - 1, s = df * v / sigma0Sq, a = 1 - conf;
    out[0] = s; out[1] = df;
    double cdf = chi2Cdf(s, df);
    out[2] = alt > 0 ? chi2Sf(s, df) : alt < 0 ? cdf : 2 * std::min(cdf, chi2Sf(s, df));
    if (alt == 0) { out[3] = df * v / chi2Quantile(1 - a / 2, df); out[4] = df * v / chi2Quantile(a / 2, df); }
    else if (alt > 0) { out[3] = df * v / chi2Quantile(conf, df); out[4] = INF; }
    else { out[3] = 0; out[4] = df * v / chi2Quantile(1 - conf, df); }
    out[5] = v; return OK;
}
// [원리] F = s₁²/s₂² ~ F(n₁−1, n₂−1). 양측 p = 2·min(P(F<f), P(F>f)).
int varTestF(const double* x, int nx, const double* y, int ny, int alt, double conf, double* out) {
    if (!x || !y || !out) return ERR_NULL; if (nx < 2 || ny < 2) return ERR_SIZE;
    if (!allFinite(x, nx) || !allFinite(y, ny)) return ERR_NAN; if (!altOk(alt) || !confOk(conf)) return ERR_PARAM;
    double vx, vy; variance(x, nx, 1, vx); variance(y, ny, 1, vy);
    if (!(vy > 0) || !(vx > 0)) return ERR_DEGENERATE;
    double f = vx / vy, d1 = nx - 1, d2 = ny - 1, cdf = fCdf(f, d1, d2), a = 1 - conf;
    out[0] = f; out[1] = d1; out[2] = d2;
    out[3] = alt > 0 ? fSf(f, d1, d2) : alt < 0 ? cdf : 2 * std::min(cdf, fSf(f, d1, d2));
    out[4] = f;
    if (alt == 0) { out[5] = f / fQuantile(1 - a / 2, d1, d2); out[6] = f / fQuantile(a / 2, d1, d2); }
    else if (alt > 0) { out[5] = f / fQuantile(conf, d1, d2); out[6] = INF; }
    else { out[5] = 0; out[6] = f / fQuantile(1 - conf, d1, d2); }
    return OK;
}
// [원리] z = (p̂ − p₀)/√(p₀(1−p₀)/n). 표준오차에 p₀(귀무가설 값)를 쓰는 점이 CI 와 다르다.
//        연속성 보정: 이산(이항)을 연속(정규)으로 근사할 때 |p̂−p₀| 에서 0.5/n 을 뺀다.
int propTestOne(int k, int n, double p0, int alt, int correct, double conf, double* out) {
    if (!out) return ERR_NULL; if (n < 1 || k < 0 || k > n) return ERR_SIZE;
    if (p0 <= 0 || p0 >= 1 || !altOk(alt) || !confOk(conf)) return ERR_PARAM;
    double p = (double)k / n, se0 = std::sqrt(p0 * (1 - p0) / n), d = p - p0;
    if (correct) { double c = 0.5 / n; if (std::fabs(d) <= c) d = 0; else d = d > 0 ? d - c : d + c; }
    double z = d / se0;
    out[0] = z; out[1] = pFromZ(z, alt); out[2] = p; out[3] = std::sqrt(p * (1 - p) / n);
    double ci[4]; proportionCI(k, n, alt == 0 ? conf : 1 - 2 * (1 - conf), 1, ci);   // Wilson
    if (alt == 0) { out[4] = ci[1]; out[5] = ci[2]; } else if (alt > 0) { out[4] = ci[1]; out[5] = 1; } else { out[4] = 0; out[5] = ci[2]; }
    return OK;
}
// [원리] H0: p₁=p₂ 아래에서는 공통 p̄ = (k₁+k₂)/(n₁+n₂) 로 표준오차를 만든다(합동). CI 는 각자 p̂ 로(비합동).
int propTestTwo(int k1, int n1, int k2, int n2, int alt, int correct, double conf, double* out) {
    if (!out) return ERR_NULL; if (n1 < 1 || n2 < 1 || k1 < 0 || k2 < 0 || k1 > n1 || k2 > n2) return ERR_SIZE;
    if (!altOk(alt) || !confOk(conf)) return ERR_PARAM;
    double p1 = (double)k1 / n1, p2 = (double)k2 / n2, pp = (double)(k1 + k2) / (n1 + n2);
    double se0 = std::sqrt(pp * (1 - pp) * (1.0 / n1 + 1.0 / n2));
    if (!(se0 > 0)) return ERR_DEGENERATE;
    double d = p1 - p2;
    if (correct) { double c = 0.5 * (1.0 / n1 + 1.0 / n2); if (std::fabs(d) <= c) d = 0; else d = d > 0 ? d - c : d + c; }
    double z = d / se0, se = std::sqrt(p1 * (1 - p1) / n1 + p2 * (1 - p2) / n2);
    out[0] = z; out[1] = pFromZ(z, alt); out[2] = p1; out[3] = p2; out[4] = p1 - p2;
    ciFrom(p1 - p2, zCrit(conf, alt), se, alt, out[5], out[6]);
    if (alt > 0) out[6] = std::min(out[6], 1.0); if (alt < 0) out[5] = std::max(out[5], -1.0);
    return OK;
}
// [원리] χ² = Σ (O−E)²/E, df = k−1. 기대도수 5 미만 칸이 많으면 근사가 나빠진다(minExpected 확인).
int chiGoodness(const double* obs, const double* expected, int k, double* out) {
    if (!obs || !out) return ERR_NULL; if (k < 2) return ERR_SIZE; if (!allFinite(obs, k)) return ERR_NAN;
    double so = sum(obs, k); if (so <= 0) return ERR_DEGENERATE;
    std::vector<double> e(k);
    if (expected) { if (!allFinite(expected, k)) return ERR_NAN; double se = sum(expected, k); if (se <= 0) return ERR_PARAM;
        for (int i = 0; i < k; ++i) { e[i] = expected[i] * so / se; if (e[i] <= 0) return ERR_PARAM; } }
    else for (int i = 0; i < k; ++i) e[i] = so / k;
    double s = 0; for (int i = 0; i < k; ++i) s += (obs[i] - e[i]) * (obs[i] - e[i]) / e[i];
    out[0] = s; out[1] = k - 1; out[2] = chi2Sf(s, k - 1); return OK;
}
// [원리] 독립이면 E_ij = (행합·열합)/N. χ² = Σ(O−E)²/E, df=(r−1)(c−1).
//        Cramér V = √(χ²/(N·min(r−1,c−1))) — 유의성이 아니라 "연관 강도".  2×2 는 Yates 보정 선택.
int chiIndependence(const double* table, int r, int c, int correct, double* out, double* expectedOut) {
    if (!table || !out) return ERR_NULL; if (r < 2 || c < 2) return ERR_SIZE; if (!allFinite(table, r * c)) return ERR_NAN;
    std::vector<double> rs(r, 0), cs(c, 0); double N = 0;
    for (int i = 0; i < r; ++i) for (int j = 0; j < c; ++j) { double v = table[i * c + j]; if (v < 0) return ERR_PARAM; rs[i] += v; cs[j] += v; N += v; }
    if (N <= 0) return ERR_DEGENERATE;
    for (int i = 0; i < r; ++i) if (rs[i] <= 0) return ERR_DEGENERATE;
    for (int j = 0; j < c; ++j) if (cs[j] <= 0) return ERR_DEGENERATE;
    double s = 0, minE = INF; bool yates = correct && r == 2 && c == 2;
    for (int i = 0; i < r; ++i) for (int j = 0; j < c; ++j) {
        double e = rs[i] * cs[j] / N; if (expectedOut) expectedOut[i * c + j] = e; minE = std::min(minE, e);
        double d = std::fabs(table[i * c + j] - e); if (yates) d = std::max(0.0, d - 0.5);
        s += d * d / e;
    }
    double df = (r - 1.0) * (c - 1.0);
    out[0] = s; out[1] = df; out[2] = chi2Sf(s, df);
    out[3] = std::sqrt(s / (N * std::min(r - 1, c - 1))); out[4] = minE; return OK;
}
// 동률 보정항 Σ(t³−t)
static double tieTerm(const double* v, int n) {
    std::vector<double> s(v, v + n); std::sort(s.begin(), s.end()); double T = 0;
    for (int i = 0; i < n;) { int j = i; while (j < n && s[j] == s[i]) ++j; double t = j - i; if (t > 1) T += t * t * t - t; i = j; }
    return T;
}
// [원리] 두 표본을 합쳐 순위를 매기고 x 의 순위합 R₁ 로 U₁ = R₁ − n₁(n₁+1)/2.
//        H0(분포 동일) 아래 U 의 평균 n₁n₂/2, 분산 n₁n₂(N+1)/12 (동률 보정). 정규근사 + 연속성보정 0.5.
//        (n 이 매우 작으면(각 <8) 정확검정과 p 가 다를 수 있다.)
int mannWhitney(const double* x, int nx, const double* y, int ny, int alt, double* out) {
    if (!x || !y || !out) return ERR_NULL; if (nx < 1 || ny < 1 || nx + ny < 3) return ERR_SIZE;
    if (!allFinite(x, nx) || !allFinite(y, ny)) return ERR_NAN; if (!altOk(alt)) return ERR_PARAM;
    int N = nx + ny; std::vector<double> all(x, x + nx); all.insert(all.end(), y, y + ny);
    std::vector<double> rk(N); rankAverage(all.data(), N, rk.data());
    double R1 = 0; for (int i = 0; i < nx; ++i) R1 += rk[i];
    double U1 = R1 - nx * (nx + 1.0) / 2, U2 = (double)nx * ny - U1;
    double mu = nx * (double)ny / 2, T = tieTerm(all.data(), N);
    double var = nx * (double)ny / 12 * ((N + 1) - T / ((double)N * (N - 1)));
    if (!(var > 0)) return ERR_DEGENERATE;
    double sd = std::sqrt(var), z, p;
    if (alt == 0) { double U = std::min(U1, U2); z = (U1 - mu); z = (std::fabs(z) - 0.5) / sd * (U1 >= mu ? 1 : -1); p = 2 * normCdf(-std::fabs(z)); out[0] = U; }
    else if (alt > 0) { z = (U1 - mu - 0.5) / sd; p = normCdf(-z); out[0] = U1; }
    else { z = (U1 - mu + 0.5) / sd; p = normCdf(z); out[0] = U1; }
    out[1] = z; out[2] = std::min(1.0, p); out[3] = U1; out[4] = U2; out[5] = 1 - 2 * U2 / ((double)nx * ny) ; // rank-biserial (x>y 방향 +)
    return OK;
}
// [원리] d = x−y, 0 제거, |d| 순위. W⁺ = 양의 차이 순위합. H0 아래 평균 n(n+1)/4, 분산 n(n+1)(2n+1)/24 − Σ(t³−t)/48.
int wilcoxonSR(const double* x, const double* y, int n, int alt, double* out) {
    if (!x || !y || !out) return ERR_NULL; if (n < 2) return ERR_SIZE;
    if (!allFinite(x, n) || !allFinite(y, n)) return ERR_NAN; if (!altOk(alt)) return ERR_PARAM;
    std::vector<double> d, ad; for (int i = 0; i < n; ++i) { double v = x[i] - y[i]; if (v != 0) { d.push_back(v); ad.push_back(std::fabs(v)); } }
    int m = (int)d.size(); if (m < 2) return ERR_DEGENERATE;
    std::vector<double> rk(m); rankAverage(ad.data(), m, rk.data());
    double Wp = 0, Wn = 0; for (int i = 0; i < m; ++i) (d[i] > 0 ? Wp : Wn) += rk[i];
    double mu = m * (m + 1.0) / 4, var = m * (m + 1.0) * (2 * m + 1.0) / 24 - tieTerm(ad.data(), m) / 48;
    if (!(var > 0)) return ERR_DEGENERATE;
    double sd = std::sqrt(var), z, p;
    if (alt == 0) { double dv = Wp - mu; z = (std::fabs(dv) - 0.5) / sd * (dv >= 0 ? 1 : -1); p = 2 * normCdf(-std::fabs(z)); }
    else if (alt > 0) { z = (Wp - mu - 0.5) / sd; p = normCdf(-z); }
    else { z = (Wp - mu + 0.5) / sd; p = normCdf(z); }
    out[0] = Wp; out[1] = z; out[2] = std::min(1.0, p); out[3] = m; out[4] = Wn; return OK;
}
// 집단 라벨 검증: 0..k-1, 각 집단 최소 minN
static int groupCounts(const int* g, int n, int minN, std::vector<int>& cnt, int& k) {
    if (!g) return ERR_NULL; k = 0;
    for (int i = 0; i < n; ++i) { if (g[i] < 0 || g[i] > 100000) return ERR_PARAM; k = std::max(k, g[i] + 1); }
    cnt.assign(k, 0); for (int i = 0; i < n; ++i) cnt[g[i]]++;
    if (k < 2) return ERR_SIZE;
    for (int i = 0; i < k; ++i) if (cnt[i] < minN) return ERR_SIZE;
    return OK;
}
// [원리] 일원 ANOVA 의 순위 버전. H = 12/(N(N+1)) Σ R_i²/n_i − 3(N+1), 동률 보정 ÷(1 − Σ(t³−t)/(N³−N)). H ~ χ²_{k−1}.
int kruskalWallis(const double* v, const int* g, int n, double* out) {
    if (!v || !out) return ERR_NULL; if (n < 3) return ERR_SIZE; if (!allFinite(v, n)) return ERR_NAN;
    std::vector<int> cnt; int k; int rc = groupCounts(g, n, 1, cnt, k); if (rc) return rc;
    std::vector<double> rk(n); rankAverage(v, n, rk.data());
    std::vector<double> R(k, 0); for (int i = 0; i < n; ++i) R[g[i]] += rk[i];
    double H = 0; for (int i = 0; i < k; ++i) H += R[i] * R[i] / cnt[i];
    H = 12.0 / (n * (n + 1.0)) * H - 3 * (n + 1.0);
    double corr = 1 - tieTerm(v, n) / ((double)n * n * n - n); if (corr <= 0) return ERR_DEGENERATE; H /= corr;
    out[0] = H; out[1] = k - 1; out[2] = chi2Sf(H, k - 1); out[3] = k; out[4] = H / (n - 1.0); return OK;
}
// [원리] 블록(행)마다 처리(열)에 순위. 처리별 순위합 R_j. χ²_F = (k−1)[ΣR_j² − b²k(k+1)²/4] / [Σr_ij² − bk(k+1)²/4] (동률 일반식).
int friedman(const double* data, int b, int k, double* out) {
    if (!data || !out) return ERR_NULL; if (b < 2 || k < 2) return ERR_SIZE; if (!allFinite(data, b * k)) return ERR_NAN;
    std::vector<double> R(k, 0), rk(k); double sumSq = 0;
    for (int i = 0; i < b; ++i) { rankAverage(data + i * k, k, rk.data()); for (int j = 0; j < k; ++j) { R[j] += rk[j]; sumSq += rk[j] * rk[j]; } }
    double sr2 = 0; for (int j = 0; j < k; ++j) sr2 += R[j] * R[j];
    double den = sumSq - b * k * (k + 1.0) * (k + 1.0) / 4; if (!(den > 0)) return ERR_DEGENERATE;
    double chi = (k - 1.0) * (sr2 - (double)b * b * k * (k + 1.0) * (k + 1.0) / 4) / den;
    out[0] = chi; out[1] = k - 1; out[2] = chi2Sf(chi, k - 1); out[3] = chi / (b * (k - 1.0)); return OK;
}
// [원리] Shapiro-Wilk: 정렬표본 x₍ᵢ₎ 와 "정규분포에서 기대되는 순서통계량 m_i" 의 상관 제곱이 W.
//        W≈1 이면 정규. Royston(1995) AS R94 근사로 3≤n≤5000 p-value.
static double polyEval(const double* c, int n, double x) { double r = c[n - 1]; for (int i = n - 2; i >= 0; --i) r = r * x + c[i]; return r; }
int shapiroWilk(const double* x, int n, double* out) {
    if (!x || !out) return ERR_NULL; if (n < 3 || n > 5000) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    static const double g[2] = { -2.273, .459 };
    static const double c1[6] = { 0., .221157, -.147981, -2.07119, 4.434685, -2.706056 };
    static const double c2[6] = { 0., .042981, -.293762, -1.752461, 5.682633, -3.582633 };
    static const double c3[4] = { .544, -.39978, .025054, -6.714e-4 };
    static const double c4[4] = { 1.3822, -.77857, .062767, -.0020322 };
    static const double c5[4] = { -1.5861, -.31082, -.083751, .0038915 };
    static const double c6[3] = { -.4803, -.082676, .0030302 };
    std::vector<double> xs(x, x + n); std::sort(xs.begin(), xs.end());
    if (xs.back() - xs.front() <= 0) return ERR_DEGENERATE;
    int nn2 = n / 2; std::vector<double> m(nn2), a(nn2);
    double an25 = n + .25, summ2 = 0;
    for (int i = 0; i < nn2; ++i) { m[i] = normQuantile((i + 1 - .375) / an25); summ2 += m[i] * m[i]; }
    summ2 *= 2; double ssumm2 = std::sqrt(summ2), rsn = 1 / std::sqrt((double)n);
    double a1 = polyEval(c1, 6, rsn) - m[0] / ssumm2; int i1; double fac;
    if (n > 5) { i1 = 2; double a2 = -m[1] / ssumm2 + polyEval(c2, 6, rsn);
        fac = std::sqrt((summ2 - 2 * m[0] * m[0] - 2 * m[1] * m[1]) / (1 - 2 * a1 * a1 - 2 * a2 * a2)); a[1] = a2; }
    else { i1 = 1; fac = std::sqrt((summ2 - 2 * m[0] * m[0]) / (1 - 2 * a1 * a1)); }
    a[0] = a1; for (int i = i1; i < nn2; ++i) a[i] = -m[i] / fac;
    if (n == 3) a[0] = std::sqrt(0.5);
    double mx = mean(xs.data(), n), ss = 0; for (int i = 0; i < n; ++i) ss += (xs[i] - mx) * (xs[i] - mx);
    double num = 0; for (int i = 0; i < nn2; ++i) num += a[i] * (xs[n - 1 - i] - xs[i]);
    double w = num * num / ss; if (w > 1) w = 1;
    out[0] = w;
    if (n == 3) { double pw = 1.90985931710274 * (std::asin(std::sqrt(w)) - 1.04719755119660); out[1] = std::max(0.0, std::min(1.0, pw)); return OK; }
    double y = std::log(1 - w), an = n, mm, s;
    if (n <= 11) { double gam = polyEval(g, 2, an); if (y >= gam) { out[1] = 1e-19; return OK; } y = -std::log(gam - y); mm = polyEval(c3, 4, an); s = std::exp(polyEval(c4, 4, an)); }
    else { double lx = std::log(an); mm = polyEval(c5, 4, lx); s = std::exp(polyEval(c6, 3, lx)); }
    out[1] = normCdf(-(y - mm) / s); return OK;
}
// [원리] Levene: Z_ij = |x_ij − 중심_i| 에 대한 일원 ANOVA. 중심=중앙값(Brown-Forsythe)이 비정규에 강건.
int levene(const double* v, const int* g, int n, int center, double* out) {
    if (!v || !out) return ERR_NULL; if (n < 3) return ERR_SIZE; if (!allFinite(v, n)) return ERR_NAN;
    std::vector<int> cnt; int k; int rc = groupCounts(g, n, 2, cnt, k); if (rc) return rc;
    std::vector<double> c(k, 0);
    if (center == 0) { for (int i = 0; i < n; ++i) c[g[i]] += v[i]; for (int i = 0; i < k; ++i) c[i] /= cnt[i]; }
    else { for (int gi = 0; gi < k; ++gi) { std::vector<double> t; for (int i = 0; i < n; ++i) if (g[i] == gi) t.push_back(v[i]); quantile(t.data(), (int)t.size(), 0.5, 0, c[gi]); } }
    std::vector<double> z(n); for (int i = 0; i < n; ++i) z[i] = std::fabs(v[i] - c[g[i]]);
    double res[12]; rc = oneWayANOVA(z.data(), g, n, res, nullptr, nullptr); if (rc) return rc;
    out[0] = res[0]; out[1] = res[1]; out[2] = res[2]; out[3] = res[3]; return OK;
}
// [원리] JB = n/6·(S² + K²/4), S=왜도, K=초과첨도(모집단식). 정규이면 χ²₂. 대표본용(n 작으면 보수적).
int jarqueBera(const double* x, int n, double* out) {
    if (!x || !out) return ERR_NULL; if (n < 4) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    double m = mean(x, n), m2 = 0, m3 = 0, m4 = 0;
    for (int i = 0; i < n; ++i) { double d = x[i] - m; m2 += d * d; m3 += d * d * d; m4 += d * d * d * d; }
    m2 /= n; m3 /= n; m4 /= n; if (m2 <= 0) return ERR_DEGENERATE;
    double S = m3 / std::pow(m2, 1.5), K = m4 / (m2 * m2) - 3, JB = n / 6.0 * (S * S + K * K / 4);
    out[0] = JB; out[1] = chi2Sf(JB, 2); out[2] = S; out[3] = K; return OK;
}

// ============================================================================
//  6. 상관 / 회귀
// ============================================================================
// [원리] r 의 검정: t = r√((n−2)/(1−r²)) ~ t_{n−2}.  CI: Fisher z = atanh(r) 가 근사정규(se=1/√(n−3)).
int pearsonTest(const double* x, const double* y, int n, double conf, double* out) {
    if (!out) return ERR_NULL; if (n < 3) return ERR_SIZE; if (!confOk(conf)) return ERR_PARAM;
    double r; int rc = pearson(x, y, n, r); if (rc) return rc;
    double df = n - 2, t = std::fabs(r) >= 1 ? INF : r * std::sqrt(df / (1 - r * r));
    out[0] = r; out[1] = t; out[2] = df; out[3] = std::fabs(r) >= 1 ? 0 : pFromT(t, df, 0);
    if (n >= 4 && std::fabs(r) < 1) { double z = std::atanh(r), se = 1 / std::sqrt(n - 3.0), c = normQuantile(1 - (1 - conf) / 2);
        out[4] = std::tanh(z - c * se); out[5] = std::tanh(z + c * se); } else { out[4] = NaN; out[5] = NaN; }
    return OK;
}
// [원리] 값을 순위로 바꾼 뒤 Pearson. 단조관계면 ±1. 이상치/비선형에 강건.
int spearman(const double* x, const double* y, int n, double* out) {
    if (!x || !y || !out) return ERR_NULL; if (n < 3) return ERR_SIZE; if (!allFinite(x, n) || !allFinite(y, n)) return ERR_NAN;
    std::vector<double> rx(n), ry(n); rankAverage(x, n, rx.data()); rankAverage(y, n, ry.data());
    double r; int rc = pearson(rx.data(), ry.data(), n, r); if (rc) return rc;
    double df = n - 2, t = std::fabs(r) >= 1 ? INF : r * std::sqrt(df / (1 - r * r));
    out[0] = r; out[1] = t; out[2] = df; out[3] = std::fabs(r) >= 1 ? 0 : pFromT(t, df, 0); return OK;
}
// [원리] 모든 쌍 (i,j) 에 대해 x,y 방향이 같으면 일치(C) 다르면 불일치(D). τ_b = (C−D)/√((n₀−n₁)(n₀−n₂)) (동률 보정).
//        p: 동률 없고 n≤33 → 정확분포(역전수 열거), 그 외 정규근사(동률 보정 분산).
int kendall(const double* x, const double* y, int n, double* out) {
    if (!x || !y || !out) return ERR_NULL; if (n < 3) return ERR_SIZE; if (!allFinite(x, n) || !allFinite(y, n)) return ERR_NAN;
    double C = 0, D = 0;
    for (int i = 0; i < n; ++i) for (int j = i + 1; j < n; ++j) { double s = (x[i] - x[j]) * (y[i] - y[j]); if (s > 0) ++C; else if (s < 0) ++D; }
    auto tieStats = [&](const double* v, double& n1, double& vt, double& t2, double& t3) {
        std::vector<double> s(v, v + n); std::sort(s.begin(), s.end()); n1 = vt = t2 = t3 = 0;
        for (int i = 0; i < n;) { int j = i; while (j < n && s[j] == s[i]) ++j; double t = j - i; if (t > 1) { n1 += t * (t - 1) / 2; vt += t * (t - 1) * (2 * t + 5); t2 += t * (t - 1); t3 += t * (t - 1) * (t - 2); } i = j; } };
    double n1, vt, xt2, xt3, n2, vu, yt2, yt3; tieStats(x, n1, vt, xt2, xt3); tieStats(y, n2, vu, yt2, yt3);
    double n0 = n * (n - 1.0) / 2, den = std::sqrt((n0 - n1) * (n0 - n2)); if (!(den > 0)) return ERR_DEGENERATE;
    double tau = (C - D) / den;
    double v0 = n * (n - 1.0) * (2 * n + 5.0), v1 = xt2 * yt2 / (2 * n * (n - 1.0)), v2 = xt3 * yt3 / (9 * n * (n - 1.0) * (n - 2.0));
    double var = (v0 - vt - vu) / 18 + v1 + v2; if (!(var > 0)) return ERR_DEGENERATE;
    double z = (C - D) / std::sqrt(var), p = pFromZ(z, 0);
    // 동률이 없고 (n≤33 또는 극단값) 이면 정확 p (Kendall 1970: 역전수 분포 DP 열거, scipy auto 규칙과 동일)
    if (n1 == 0 && n2 == 0 && (n <= 33 || std::min(D, n0 - D) <= 1)) {
        double c = std::min(D, n0 - D); int cc = (int)c;
        std::vector<double> cur(cc + 1, 0.0), old;
        cur[0] = 1; if (cc >= 1) cur[1] = 1;
        for (int j = 3; j <= n; ++j) {
            old = cur;
            for (int k = 1; k < std::min(j, cc + 1); ++k) cur[k] += cur[k - 1];
            for (int k = j; k <= cc; ++k) cur[k] += cur[k - 1] - old[k - j];
        }
        double s = 0; for (double v : cur) s += v;
        p = std::min(1.0, 2.0 * s / std::exp(lgammaFn(n + 1.0)));
    }
    out[0] = tau; out[1] = z; out[2] = p; out[3] = C; out[4] = D; return OK;
}

// ---- 선형대수: Householder QR 최소제곱 ------------------------------------
// [원리] A = QR 이면 min‖Ax−b‖ 의 해는 Rx = Qᵀb (상삼각 → 후진대입). (AᵀA)⁻¹ = R⁻¹R⁻ᵀ.
bool invertSPD(std::vector<double>& M, int p) {   // Cholesky 기반 in-place 역행렬
    std::vector<double> L(p * p, 0);
    for (int i = 0; i < p; ++i) for (int j = 0; j <= i; ++j) {
        double s = M[i * p + j]; for (int k = 0; k < j; ++k) s -= L[i * p + k] * L[j * p + k];
        if (i == j) { if (s <= 0) return false; L[i * p + i] = std::sqrt(s); } else L[i * p + j] = s / L[j * p + j];
    }
    std::vector<double> Li(p * p, 0);   // L⁻¹
    for (int i = 0; i < p; ++i) { Li[i * p + i] = 1 / L[i * p + i];
        for (int j = 0; j < i; ++j) { double s = 0; for (int k = j; k < i; ++k) s -= L[i * p + k] * Li[k * p + j]; Li[i * p + j] = s / L[i * p + i]; } }
    for (int i = 0; i < p; ++i) for (int j = 0; j < p; ++j) { double s = 0; for (int k = std::max(i, j); k < p; ++k) s += Li[k * p + i] * Li[k * p + j]; M[i * p + j] = s; }
    return true;
}
int leastSquares(const double* A, const double* b, int n, int p, double* coef, double* XtXinv, double* fitted) {
    if (!A || !b || !coef) return ERR_NULL; if (n < p || p < 1) return ERR_SIZE;
    std::vector<double> R(A, A + (size_t)n * p), qtb(b, b + n);
    double maxDiag = 0;
    for (int k = 0; k < p; ++k) {
        double norm = 0; for (int i = k; i < n; ++i) norm += R[i * p + k] * R[i * p + k]; norm = std::sqrt(norm);
        if (norm == 0) return ERR_DEGENERATE;
        double alpha = R[k * p + k] > 0 ? -norm : norm;
        std::vector<double> v(n - k); v[0] = R[k * p + k] - alpha; for (int i = k + 1; i < n; ++i) v[i - k] = R[i * p + k];
        double vn = 0; for (double t : v) vn += t * t; if (vn == 0) { continue; }
        for (int j = k; j < p; ++j) { double s = 0; for (int i = k; i < n; ++i) s += v[i - k] * R[i * p + j]; s = 2 * s / vn; for (int i = k; i < n; ++i) R[i * p + j] -= s * v[i - k]; }
        { double s = 0; for (int i = k; i < n; ++i) s += v[i - k] * qtb[i]; s = 2 * s / vn; for (int i = k; i < n; ++i) qtb[i] -= s * v[i - k]; }
        maxDiag = std::max(maxDiag, std::fabs(R[k * p + k]));
    }
    for (int k = 0; k < p; ++k) if (std::fabs(R[k * p + k]) < 1e-11 * std::max(maxDiag, 1e-300)) return ERR_DEGENERATE;
    for (int i = p - 1; i >= 0; --i) { double s = qtb[i]; for (int j = i + 1; j < p; ++j) s -= R[i * p + j] * coef[j]; coef[i] = s / R[i * p + i]; }
    if (XtXinv) {   // R⁻¹ (상삼각) → R⁻¹R⁻ᵀ
        std::vector<double> Ri(p * p, 0);
        for (int j = p - 1; j >= 0; --j) { Ri[j * p + j] = 1 / R[j * p + j];
            for (int i = j - 1; i >= 0; --i) { double s = 0; for (int k = i + 1; k <= j; ++k) s -= R[i * p + k] * Ri[k * p + j]; Ri[i * p + j] = s / R[i * p + i]; } }
        for (int i = 0; i < p; ++i) for (int j = 0; j < p; ++j) { double s = 0; for (int k = std::max(i, j); k < p; ++k) s += Ri[i * p + k] * Ri[j * p + k]; XtXinv[i * p + j] = s; }
    }
    if (fitted) for (int i = 0; i < n; ++i) { double s = 0; for (int j = 0; j < p; ++j) s += A[i * p + j] * coef[j]; fitted[i] = s; }
    return OK;
}
double durbinWatson(const double* e, int n) {
    if (!e || n < 2) return NaN; double num = 0, den = 0;
    for (int i = 0; i < n; ++i) { den += e[i] * e[i]; if (i) num += (e[i] - e[i - 1]) * (e[i] - e[i - 1]); }
    return den > 0 ? num / den : NaN;
}
// [원리] 계수 표준오차 se_j = σ̂ √[(XᵀX)⁻¹]_jj, σ̂² = SSE/(n−p−1). t_j = b_j/se_j ~ t_{n−p−1}.
//        F = (SSR/p)/(SSE/(n−p−1)) 는 "모든 기울기 = 0" 검정. AIC = n·ln(SSE/n)+2(p+1)+상수(statsmodels 정의).
int multipleRegression(const double* X, const double* y, int n, int p, double* coef, double* se, double* t, double* pv,
                       double* stats, double* fitted, double* resid) {
    if (!X || !y || !coef || !stats) return ERR_NULL; if (p < 1 || n < p + 2) return ERR_SIZE;
    if (!allFinite(X, n * p) || !allFinite(y, n)) return ERR_NAN;
    int q = p + 1; std::vector<double> A((size_t)n * q), inv(q * q), fit(n);
    for (int i = 0; i < n; ++i) { A[i * q] = 1; for (int j = 0; j < p; ++j) A[i * q + j + 1] = X[i * p + j]; }
    int rc = leastSquares(A.data(), y, n, q, coef, inv.data(), fit.data()); if (rc) return rc;
    double my = mean(y, n), SSE = 0, SST = 0, MAE = 0; std::vector<double> e(n);
    for (int i = 0; i < n; ++i) { e[i] = y[i] - fit[i]; SSE += e[i] * e[i]; SST += (y[i] - my) * (y[i] - my); MAE += std::fabs(e[i]); }
    double dfRes = n - q, sigma2 = SSE / dfRes, R2 = SST > 0 ? 1 - SSE / SST : NaN;
    for (int j = 0; j < q; ++j) { double s = std::sqrt(std::max(0.0, sigma2 * inv[j * q + j]));
        if (se) se[j] = s; double tv = s > 0 ? coef[j] / s : (coef[j] == 0 ? 0 : INF); if (t) t[j] = tv; if (pv) pv[j] = s > 0 ? pFromT(tv, dfRes, 0) : (coef[j] == 0 ? 1 : 0); }
    double SSR = SST - SSE, F = SSE > 0 ? (SSR / p) / sigma2 : INF;
    stats[0] = R2; stats[1] = 1 - (1 - R2) * (n - 1) / dfRes; stats[2] = std::sqrt(SSE / n); stats[3] = MAE / n;
    stats[4] = F; stats[5] = SSE > 0 ? fSf(F, p, dfRes) : 0; stats[6] = p; stats[7] = dfRes; stats[8] = durbinWatson(e.data(), n);
    stats[9] = SSE > 0 ? n * (std::log(2 * 3.14159265358979323846 * SSE / n) + 1) + 2 * q : -INF; stats[10] = std::sqrt(sigma2); stats[11] = SSE;
    if (fitted) std::copy(fit.begin(), fit.end(), fitted); if (resid) std::copy(e.begin(), e.end(), resid);
    return OK;
}
int simpleRegression(const double* x, const double* y, int n, double* out, double* fitted, double* resid) {
    if (!x || !y || !out) return ERR_NULL;
    double coef[2], se[2], t[2], pv[2], st[12];
    int rc = multipleRegression(x, y, n, 1, coef, se, t, pv, st, fitted, resid); if (rc) return rc;
    out[0] = coef[0]; out[1] = coef[1]; out[2] = se[0]; out[3] = se[1]; out[4] = t[0]; out[5] = t[1]; out[6] = pv[0]; out[7] = pv[1];
    out[8] = st[0]; out[9] = st[1]; out[10] = st[2]; out[11] = st[3]; out[12] = st[4]; out[13] = st[5]; out[14] = st[7]; out[15] = st[8]; out[16] = st[10]; out[17] = st[11];
    return OK;
}
int polynomialRegression(const double* x, const double* y, int n, int deg, double* coef, double* se, double* t, double* pv,
                         double* stats, double* fitted, double* resid) {
    if (!x) return ERR_NULL; if (deg < 1 || deg > 10) return ERR_PARAM; if (n < deg + 2) return ERR_SIZE;
    std::vector<double> X((size_t)n * deg);
    for (int i = 0; i < n; ++i) { double v = 1; for (int j = 0; j < deg; ++j) { v *= x[i]; X[i * deg + j] = v; } }
    return multipleRegression(X.data(), y, n, deg, coef, se, t, pv, stats, fitted, resid);
}
// [원리] 레버리지 h_ii = x_iᵀ(XᵀX)⁻¹x_i (설명변수 공간에서 얼마나 외곽인가). 표준화잔차 e_i/(σ̂√(1−h_ii)).
//        Cook D = e_i²/((p+1)σ̂²)·h_ii/(1−h_ii)² — 관측치 하나를 빼면 계수가 얼마나 움직이는가. 4/n 이상이면 검토.
int regressionDiag(const double* X, const double* y, int n, int p, double* leverage, double* stdResid, double* cookD) {
    if (!X || !y) return ERR_NULL; if (p < 1 || n < p + 2) return ERR_SIZE; if (!allFinite(X, n * p) || !allFinite(y, n)) return ERR_NAN;
    int q = p + 1; std::vector<double> A((size_t)n * q), inv(q * q), fit(n), coef(q);
    for (int i = 0; i < n; ++i) { A[i * q] = 1; for (int j = 0; j < p; ++j) A[i * q + j + 1] = X[i * p + j]; }
    int rc = leastSquares(A.data(), y, n, q, coef.data(), inv.data(), fit.data()); if (rc) return rc;
    double SSE = 0; for (int i = 0; i < n; ++i) SSE += (y[i] - fit[i]) * (y[i] - fit[i]);
    double s2 = SSE / (n - q); if (!(s2 > 0)) return ERR_DEGENERATE;
    for (int i = 0; i < n; ++i) {
        double h = 0; for (int a = 0; a < q; ++a) for (int b = 0; b < q; ++b) h += A[i * q + a] * inv[a * q + b] * A[i * q + b];
        double e = y[i] - fit[i], sr = e / std::sqrt(s2 * (1 - h));
        if (leverage) leverage[i] = h; if (stdResid) stdResid[i] = sr; if (cookD) cookD[i] = sr * sr / q * h / (1 - h);
    }
    return OK;
}
// [원리] VIF_j = 1/(1−R_j²), R_j² 은 X_j 를 나머지 설명변수로 회귀한 결정계수. 10 이상이면 다중공선성 의심.
int vif(const double* X, int n, int p, double* out) {
    if (!X || !out) return ERR_NULL; if (p < 2 || n < p + 1) return ERR_SIZE; if (!allFinite(X, n * p)) return ERR_NAN;
    std::vector<double> Xo((size_t)n * (p - 1)), yj(n), coef(p), st(12);
    for (int j = 0; j < p; ++j) {
        for (int i = 0; i < n; ++i) { yj[i] = X[i * p + j]; int c = 0; for (int k = 0; k < p; ++k) if (k != j) Xo[i * (p - 1) + c++] = X[i * p + k]; }
        int rc = multipleRegression(Xo.data(), yj.data(), n, p - 1, coef.data(), nullptr, nullptr, nullptr, st.data(), nullptr, nullptr);
        if (rc) return rc;
        out[j] = st[0] >= 1 ? INF : 1 / (1 - st[0]);
    }
    return OK;
}
int covMatrix(const double* X, int n, int p, double* out) {
    if (!X || !out) return ERR_NULL; if (p < 1 || n < 2) return ERR_SIZE; if (!allFinite(X, n * p)) return ERR_NAN;
    std::vector<double> m(p, 0); for (int i = 0; i < n; ++i) for (int j = 0; j < p; ++j) m[j] += X[i * p + j]; for (int j = 0; j < p; ++j) m[j] /= n;
    for (int a = 0; a < p; ++a) for (int b = a; b < p; ++b) { double s = 0; for (int i = 0; i < n; ++i) s += (X[i * p + a] - m[a]) * (X[i * p + b] - m[b]); out[a * p + b] = out[b * p + a] = s / (n - 1); }
    return OK;
}
int corrMatrix(const double* X, int n, int p, double* out) {
    int rc = covMatrix(X, n, p, out); if (rc) return rc;
    std::vector<double> sd(p); for (int j = 0; j < p; ++j) { sd[j] = std::sqrt(out[j * p + j]); if (!(sd[j] > 0)) return ERR_DEGENERATE; }
    for (int a = 0; a < p; ++a) for (int b = 0; b < p; ++b) out[a * p + b] = a == b ? 1 : out[a * p + b] / (sd[a] * sd[b]);
    return OK;
}

// ============================================================================
//  7. 분산분석
// ============================================================================
struct GroupStat { std::vector<int> cnt; std::vector<double> mean, var; int k; double grand; };
static int groupStats(const double* v, const int* g, int n, int minN, GroupStat& gs) {
    int rc = groupCounts(g, n, minN, gs.cnt, gs.k); if (rc) return rc;
    gs.mean.assign(gs.k, 0); gs.var.assign(gs.k, 0);
    for (int i = 0; i < n; ++i) gs.mean[g[i]] += v[i]; for (int i = 0; i < gs.k; ++i) gs.mean[i] /= gs.cnt[i];
    for (int i = 0; i < n; ++i) { double d = v[i] - gs.mean[g[i]]; gs.var[g[i]] += d * d; }
    for (int i = 0; i < gs.k; ++i) gs.var[i] = gs.cnt[i] > 1 ? gs.var[i] / (gs.cnt[i] - 1) : NaN;
    gs.grand = mean(v, n); return OK;
}
// [원리] SST = SSB + SSW.  F = MSB/MSW = (SSB/(k−1)) / (SSW/(N−k)).  집단 간 변동이 집단 내 변동보다 충분히 크면 유의.
//        η² = SSB/SST (표본 설명비율, 과대추정), ω² = (SSB − (k−1)MSW)/(SST + MSW) (편의 보정).
int oneWayANOVA(const double* v, const int* g, int n, double* out, double* gm, double* gn) {
    if (!v || !out) return ERR_NULL; if (n < 3) return ERR_SIZE; if (!allFinite(v, n)) return ERR_NAN;
    GroupStat gs; int rc = groupStats(v, g, n, 1, gs); if (rc) return rc;
    if (n - gs.k < 1) return ERR_SIZE;
    double SSB = 0, SSW = 0, SST = 0;
    for (int i = 0; i < gs.k; ++i) SSB += gs.cnt[i] * (gs.mean[i] - gs.grand) * (gs.mean[i] - gs.grand);
    for (int i = 0; i < n; ++i) { SSW += (v[i] - gs.mean[g[i]]) * (v[i] - gs.mean[g[i]]); SST += (v[i] - gs.grand) * (v[i] - gs.grand); }
    double df1 = gs.k - 1, df2 = n - gs.k, MSB = SSB / df1, MSW = SSW / df2;
    if (!(MSW > 0)) return ERR_DEGENERATE;
    double F = MSB / MSW;
    out[0] = F; out[1] = df1; out[2] = df2; out[3] = fSf(F, df1, df2); out[4] = SSB; out[5] = SSW; out[6] = SST; out[7] = MSB; out[8] = MSW;
    out[9] = SST > 0 ? SSB / SST : NaN; out[10] = (SSB - df1 * MSW) / (SST + MSW); out[11] = gs.k;
    if (gm) for (int i = 0; i < gs.k; ++i) gm[i] = gs.mean[i]; if (gn) for (int i = 0; i < gs.k; ++i) gn[i] = gs.cnt[i];
    return OK;
}
// [원리] 등분산 가정 없는 ANOVA. 가중치 w_i = n_i/s_i². 자유도(분모)를 Welch–Satterthwaite 로 줄인다.
int welchANOVA(const double* v, const int* g, int n, double* out) {
    if (!v || !out) return ERR_NULL; if (n < 3) return ERR_SIZE; if (!allFinite(v, n)) return ERR_NAN;
    GroupStat gs; int rc = groupStats(v, g, n, 2, gs); if (rc) return rc;
    double W = 0, mw = 0; std::vector<double> w(gs.k);
    for (int i = 0; i < gs.k; ++i) { if (!(gs.var[i] > 0)) return ERR_DEGENERATE; w[i] = gs.cnt[i] / gs.var[i]; W += w[i]; mw += w[i] * gs.mean[i]; }
    mw /= W; double num = 0, lam = 0;
    for (int i = 0; i < gs.k; ++i) { num += w[i] * (gs.mean[i] - mw) * (gs.mean[i] - mw); double t = 1 - w[i] / W; lam += t * t / (gs.cnt[i] - 1); }
    double k = gs.k, df1 = k - 1, F = (num / df1) / (1 + 2 * (k - 2) / (k * k - 1) * lam), df2 = (k * k - 1) / (3 * lam);
    out[0] = F; out[1] = df1; out[2] = df2; out[3] = fSf(F, df1, df2); return OK;
}
// [원리] 균형 이원배치: SST = SS_A + SS_B + SS_AB + SS_E.  SS_AB = SS_cells − SS_A − SS_B (셀평균 변동 중 주효과로 설명 안 되는 부분).
//        반복 r=1 이면 교호작용을 오차와 분리할 수 없으므로 가법모형(교호작용 없음)으로 계산.
int twoWayANOVA(const double* v, const int* a, const int* b, int n, double* out) {
    if (!v || !a || !b || !out) return ERR_NULL; if (n < 4) return ERR_SIZE; if (!allFinite(v, n)) return ERR_NAN;
    int A = 0, B = 0; for (int i = 0; i < n; ++i) { if (a[i] < 0 || b[i] < 0 || a[i] > 1000 || b[i] > 1000) return ERR_PARAM; A = std::max(A, a[i] + 1); B = std::max(B, b[i] + 1); }
    if (A < 2 || B < 2) return ERR_SIZE;
    std::vector<int> cn(A * B, 0); std::vector<double> cm(A * B, 0), am(A, 0), bm(B, 0);
    for (int i = 0; i < n; ++i) { cn[a[i] * B + b[i]]++; cm[a[i] * B + b[i]] += v[i]; am[a[i]] += v[i]; bm[b[i]] += v[i]; }
    int r = cn[0]; for (int c = 0; c < A * B; ++c) if (cn[c] != r) return ERR_PARAM;   // 불균형 → 오류 (회귀 기반 필요)
    if (r < 1) return ERR_PARAM;
    for (int c = 0; c < A * B; ++c) cm[c] /= r; for (int i = 0; i < A; ++i) am[i] /= (double)r * B; for (int j = 0; j < B; ++j) bm[j] /= (double)r * A;
    double m = mean(v, n), SSA = 0, SSB = 0, SSC = 0, SST = 0;
    for (int i = 0; i < A; ++i) SSA += (double)r * B * (am[i] - m) * (am[i] - m);
    for (int j = 0; j < B; ++j) SSB += (double)r * A * (bm[j] - m) * (bm[j] - m);
    for (int c = 0; c < A * B; ++c) SSC += r * (cm[c] - m) * (cm[c] - m);
    for (int i = 0; i < n; ++i) SST += (v[i] - m) * (v[i] - m);
    double SSAB = SSC - SSA - SSB, dfA = A - 1, dfB = B - 1, dfAB = dfA * dfB, dfE, SSE;
    if (r >= 2) { dfE = (double)A * B * (r - 1); SSE = SST - SSC; } else { dfE = dfAB; SSE = SST - SSA - SSB; SSAB = NaN; dfAB = NaN; }
    double MSE = SSE / dfE; if (!(MSE > 0)) return ERR_DEGENERATE;
    double FA = (SSA / dfA) / MSE, FB = (SSB / dfB) / MSE, FAB = r >= 2 ? (SSAB / dfAB) / MSE : NaN;
    out[0] = FA; out[1] = dfA; out[2] = fSf(FA, dfA, dfE); out[3] = FB; out[4] = dfB; out[5] = fSf(FB, dfB, dfE);
    out[6] = FAB; out[7] = dfAB; out[8] = r >= 2 ? fSf(FAB, dfAB, dfE) : NaN; out[9] = dfE;
    out[10] = SSA; out[11] = SSB; out[12] = SSAB; out[13] = SSE; out[14] = SST; out[15] = MSE;
    out[16] = SSA / SST; out[17] = SSB / SST; out[18] = r >= 2 ? SSAB / SST : NaN; out[19] = r; return OK;
}
// [원리] 같은 피험자를 여러 조건에서 측정 → 피험자 변동(SS_subj)을 오차에서 분리. F = MS_treat/MS_error, df=(k−1),(k−1)(s−1).
int repeatedANOVA(const double* d, int s, int k, double* out) {
    if (!d || !out) return ERR_NULL; if (s < 2 || k < 2) return ERR_SIZE; if (!allFinite(d, s * k)) return ERR_NAN;
    double m = mean(d, s * k), SSs = 0, SSt = 0, SST = 0;
    for (int i = 0; i < s; ++i) { double r = 0; for (int j = 0; j < k; ++j) r += d[i * k + j]; r /= k; SSs += k * (r - m) * (r - m); }
    for (int j = 0; j < k; ++j) { double c = 0; for (int i = 0; i < s; ++i) c += d[i * k + j]; c /= s; SSt += s * (c - m) * (c - m); }
    for (int i = 0; i < s * k; ++i) SST += (d[i] - m) * (d[i] - m);
    double SSe = SST - SSs - SSt, df1 = k - 1, df2 = (k - 1.0) * (s - 1.0), MSe = SSe / df2; if (!(MSe > 0)) return ERR_DEGENERATE;
    double F = (SSt / df1) / MSe;
    out[0] = F; out[1] = df1; out[2] = df2; out[3] = fSf(F, df1, df2); out[4] = SSt; out[5] = SSe; out[6] = SSs; out[7] = SSt / (SSt + SSe); return OK;
}
// [원리] Tukey HSD(Tukey–Kramer): q = |m_i − m_j| / √(MSW/2·(1/n_i+1/n_j)) 를 스튜던트화 범위 분포(k, N−k)와 비교.
//        모든 쌍을 동시에 비교해도 전체 오류율 α 를 유지한다(가족오류율 보정).
int tukeyHSD(const double* v, const int* g, int n, double conf, double* pairs, int cap, int& nPairs) {
    if (!v || !pairs) return ERR_NULL; if (!confOk(conf)) return ERR_PARAM;
    double an[12]; std::vector<double> gm(100001), gn(100001);
    int rc = oneWayANOVA(v, g, n, an, gm.data(), gn.data()); if (rc) return rc;
    int k = (int)an[11]; double MSW = an[8], df = an[2]; nPairs = k * (k - 1) / 2; if (cap < nPairs) return ERR_CAPACITY;
    double qc = tukeyQuantile(conf, k, df); int idx = 0;
    for (int i = 0; i < k; ++i) for (int j = i + 1; j < k; ++j) {
        double diff = gm[j] - gm[i], se = std::sqrt(MSW / 2 * (1 / gn[i] + 1 / gn[j])), q = std::fabs(diff) / se;
        double* o = pairs + idx * 6; o[0] = i; o[1] = j; o[2] = diff; o[3] = diff - qc * se; o[4] = diff + qc * se; o[5] = std::max(0.0, 1 - tukeyCdf(q, k, df)); ++idx;
    }
    return OK;
}
// [원리] 쌍별 t 검정 후 p 에 비교횟수 m 을 곱한다(보수적, 검정력 낮음). 비교 수가 적을 때만 권장.
int bonferroniPairs(const double* v, const int* g, int n, double* pairs, int cap, int& nPairs) {
    if (!v || !pairs) return ERR_NULL;
    double an[12]; std::vector<double> gm(100001), gn(100001);
    int rc = oneWayANOVA(v, g, n, an, gm.data(), gn.data()); if (rc) return rc;
    int k = (int)an[11]; double MSW = an[8], df = an[2]; nPairs = k * (k - 1) / 2; if (cap < nPairs) return ERR_CAPACITY;
    int idx = 0;
    for (int i = 0; i < k; ++i) for (int j = i + 1; j < k; ++j) {
        double diff = gm[j] - gm[i], se = std::sqrt(MSW * (1 / gn[i] + 1 / gn[j])), t = diff / se, p = pFromT(t, df, 0);
        double* o = pairs + idx * 6; o[0] = i; o[1] = j; o[2] = diff; o[3] = t; o[4] = p; o[5] = std::min(1.0, p * nPairs); ++idx;
    }
    return OK;
}
// [원리] Games-Howell: 등분산 가정 없이 각 쌍마다 Welch 자유도와 자체 분산으로 q 계산. 표본크기·분산이 다를 때 Tukey 대신.
int gamesHowell(const double* v, const int* g, int n, double conf, double* pairs, int cap, int& nPairs) {
    if (!v || !pairs) return ERR_NULL; if (n < 3) return ERR_SIZE; if (!allFinite(v, n)) return ERR_NAN; if (!confOk(conf)) return ERR_PARAM;
    GroupStat gs; int rc = groupStats(v, g, n, 2, gs); if (rc) return rc;
    int k = gs.k; nPairs = k * (k - 1) / 2; if (cap < nPairs) return ERR_CAPACITY; int idx = 0;
    for (int i = 0; i < k; ++i) for (int j = i + 1; j < k; ++j) {
        double a = gs.var[i] / gs.cnt[i], b = gs.var[j] / gs.cnt[j]; if (!(a + b > 0)) return ERR_DEGENERATE;
        double df = (a + b) * (a + b) / (a * a / (gs.cnt[i] - 1) + b * b / (gs.cnt[j] - 1)), se = std::sqrt((a + b) / 2), diff = gs.mean[j] - gs.mean[i], q = std::fabs(diff) / se;
        double qc = tukeyQuantile(conf, k, df);
        double* o = pairs + idx * 6; o[0] = i; o[1] = j; o[2] = diff; o[3] = diff - qc * se; o[4] = diff + qc * se; o[5] = std::max(0.0, 1 - tukeyCdf(q, k, df)); ++idx;
    }
    return OK;
}

// ============================================================================
//  8. 전처리
// ============================================================================
// [원리] IQR 규칙: [Q1 − k·IQR, Q3 + k·IQR] 밖 = 이상치. k=1.5 관례, 3.0 은 극단 이상치. 분포 가정 없음.
int outlierIQR(const double* x, int n, double k, int* flags, double* fences) {
    if (!x || !flags) return ERR_NULL; if (n < 4) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN; if (k <= 0) return ERR_PARAM;
    double q1, q3; quantile(x, n, 0.25, 0, q1); quantile(x, n, 0.75, 0, q3);
    double lo = q1 - k * (q3 - q1), hi = q3 + k * (q3 - q1); if (fences) { fences[0] = lo; fences[1] = hi; }
    int c = 0; for (int i = 0; i < n; ++i) { flags[i] = (x[i] < lo || x[i] > hi) ? 1 : 0; c += flags[i]; } return c;
}
// [원리] |z| > thr. 평균/표준편차 자체가 이상치에 끌려가므로(마스킹) 이상치가 많으면 IQR/MAD 가 낫다.
int outlierZ(const double* x, int n, double thr, int* flags) {
    if (!x || !flags) return ERR_NULL; if (thr <= 0) return ERR_PARAM;
    std::vector<double> z(n); int rc = standardize(x, n, z.data()); if (rc) return rc;
    int c = 0; for (int i = 0; i < n; ++i) { flags[i] = std::fabs(z[i]) > thr ? 1 : 0; c += flags[i]; } return c;
}
// [원리] 윈저화: 하위 lowP·상위 highP 분위수 밖 값을 그 분위수로 "잘라 붙임". 제거 대신 영향만 줄인다.
int winsorize(const double* x, int n, double lowP, double highP, double* out) {
    if (!x || !out) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    if (lowP < 0 || highP < 0 || lowP + highP >= 1) return ERR_PARAM;
    double lo, hi; quantile(x, n, lowP, 0, lo); quantile(x, n, 1 - highP, 0, hi);
    for (int i = 0; i < n; ++i) out[i] = x[i] < lo ? lo : x[i] > hi ? hi : x[i]; return OK;
}
// [원리] 결측 대체는 분포를 왜곡한다(평균 대체 → 분산 축소). 결측 원인(MCAR/MAR/MNAR)을 먼저 따진다.
int impute(const double* x, const int* miss, int n, int method, double* out) {
    if (!x || !miss || !out) return ERR_NULL; if (n < 1) return ERR_SIZE; if (method < 0 || method > 4) return ERR_PARAM;
    std::vector<double> ok; for (int i = 0; i < n; ++i) if (!miss[i]) { if (!std::isfinite(x[i])) return ERR_NAN; ok.push_back(x[i]); }
    if (ok.empty()) return ERR_DEGENERATE;
    double fill = 0; int cnt;
    if (method == 0) fill = mean(ok.data(), (int)ok.size()); else if (method == 1) quantile(ok.data(), (int)ok.size(), 0.5, 0, fill);
    else if (method == 2) mode(ok.data(), (int)ok.size(), fill, cnt);
    if (method <= 2) { for (int i = 0; i < n; ++i) out[i] = miss[i] ? fill : x[i]; return OK; }
    if (method == 3) { double last = NaN; int first = -1; for (int i = 0; i < n; ++i) if (!miss[i]) { first = i; break; }
        for (int i = 0; i < n; ++i) { if (!miss[i]) last = x[i]; out[i] = miss[i] ? (std::isnan(last) ? x[first] : last) : x[i]; } return OK; }
    // 선형보간
    int prev = -1; for (int i = 0; i < n; ++i) if (!miss[i]) { out[i] = x[i]; prev = i; }
    for (int i = 0; i < n; ++i) if (miss[i]) {
        int l = i - 1; while (l >= 0 && miss[l]) --l; int r = i + 1; while (r < n && miss[r]) ++r;
        if (l < 0 && r >= n) out[i] = NaN; else if (l < 0) out[i] = x[r]; else if (r >= n) out[i] = x[l];
        else out[i] = x[l] + (x[r] - x[l]) * (i - l) / (double)(r - l);
    }
    (void)prev; return OK;
}
// [원리] Box-Cox: y = (x^λ − 1)/λ (λ≠0), ln x (λ=0). 오른쪽 꼬리 분포를 대칭에 가깝게. x>0 필요.
int boxCox(const double* x, int n, double lambda, double* out) {
    if (!x || !out) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n) || !std::isfinite(lambda)) return ERR_NAN;
    for (int i = 0; i < n; ++i) { if (x[i] <= 0) return ERR_PARAM; out[i] = lambda == 0 ? std::log(x[i]) : (std::pow(x[i], lambda) - 1) / lambda; } return OK;
}
int missingStats(const int* miss, int n, double* out) {
    if (!miss || !out) return ERR_NULL; if (n < 1) return ERR_SIZE;
    int c = 0; for (int i = 0; i < n; ++i) if (miss[i]) ++c; out[0] = n; out[1] = c; out[2] = (double)c / n; return OK;
}

} // namespace adp

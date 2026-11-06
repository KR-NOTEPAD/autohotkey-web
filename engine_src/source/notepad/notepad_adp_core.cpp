// ============================================================================
//  NOTEPAD.dll  -  notepad_adp_core.cpp
//  [NOTEPAD] 세트 AL-1: ADP 통계 코어 — 기술통계 / 배열처리 / 특수함수 / 확률분포 / 난수
//
//  공부 포인트(각 함수 위 주석에 '원리' 를 적어 두었다):
//   * 합은 Neumaier 보정합, 분산은 Welford 온라인 알고리즘 → 큰 수/긴 배열에서도 오차 누적 억제
//   * 분포 CDF 는 전부 "불완전베타 / 불완전감마" 두 특수함수로 귀결된다.
//       t, F, 이항  → I_x(a,b) (정규화 불완전베타)
//       χ², 포아송, 감마 → P(a,x) (정규화 불완전감마)
//     이 둘만 정확히 만들면 ADP 에 나오는 연속/이산 분포 CDF 를 모두 만들 수 있다.
//   * Quantile(역CDF) 은 닫힌 공식이 없는 분포가 대부분이라 "CDF 를 괄호로 묶고 이분법+보간" 으로 푼다.
// ============================================================================
#include "notepad_adp.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>
#include <chrono>

namespace adp {

static const double PI = 3.14159265358979323846;
static const double SQRT2 = 1.41421356237309504880;
static const double EPS = std::numeric_limits<double>::epsilon();
static const double NaN = std::numeric_limits<double>::quiet_NaN();

const char* statusText(int code) {
    switch (code) {
    case OK:             return "OK";
    case ERR_NULL:       return "null pointer";
    case ERR_SIZE:       return "sample size too small or length mismatch";
    case ERR_NAN:        return "input contains NaN/Inf";
    case ERR_DEGENERATE: return "degenerate data (zero variance / singular matrix)";
    case ERR_PARAM:      return "parameter out of range";
    case ERR_CONVERGE:   return "iteration did not converge";
    case ERR_CAPACITY:   return "output capacity insufficient";
    default:             return "unknown error";
    }
}

bool allFinite(const double* x, int n) {
    if (!x) return false;
    for (int i = 0; i < n; ++i) if (!std::isfinite(x[i])) return false;
    return true;
}

// ============================================================================
//  1. 기술통계
// ============================================================================

// [원리] 단순 누적합은 큰 값 + 작은 값에서 하위 비트를 잃는다. Neumaier 보정합은
//        잃어버린 하위 비트를 별도 변수(c)에 모아 마지막에 더한다.
double sum(const double* x, int n) {
    double s = 0.0, c = 0.0;
    for (int i = 0; i < n; ++i) {
        double t = s + x[i];
        if (std::fabs(s) >= std::fabs(x[i])) c += (s - t) + x[i];
        else                                 c += (x[i] - t) + s;
        s = t;
    }
    return s + c;
}
double mean(const double* x, int n) { return n > 0 ? sum(x, n) / n : NaN; }

int weightedMean(const double* x, const double* w, int n, double& out) {
    if (!x || !w) return ERR_NULL;
    if (n < 1) return ERR_SIZE;
    if (!allFinite(x, n) || !allFinite(w, n)) return ERR_NAN;
    double sw = 0, sxw = 0;
    for (int i = 0; i < n; ++i) { if (w[i] < 0) return ERR_PARAM; sw += w[i]; sxw += w[i] * x[i]; }
    if (sw <= 0) return ERR_DEGENERATE;
    out = sxw / sw; return OK;
}

// [원리] Welford: 평균과 제곱편차합(M2)을 한 번의 순회로 갱신. (Σx² - nx̄²) 방식은 상쇄오차가 크다.
static void welford(const double* x, int n, double& m, double& M2) {
    m = 0; M2 = 0;
    for (int i = 0; i < n; ++i) {
        double d = x[i] - m;
        m += d / (i + 1);
        M2 += d * (x[i] - m);
    }
}
int variance(const double* x, int n, int ddof, double& out) {
    if (!x) return ERR_NULL;
    if (ddof != 0 && ddof != 1) return ERR_PARAM;
    if (n < 1 + ddof) return ERR_SIZE;
    if (!allFinite(x, n)) return ERR_NAN;
    double m, M2; welford(x, n, m, M2);
    out = M2 / (n - ddof); return OK;
}
int stddev(const double* x, int n, int ddof, double& out) {
    int rc = variance(x, n, ddof, out); if (rc) return rc;
    out = std::sqrt(out); return OK;
}

// [원리] 분위수는 "정렬 후 위치 h 를 어떻게 정의하느냐" 에 따라 9가지 정의(Hyndman & Fan)가 있다.
//   type7 (R 기본, Excel PERCENTILE.INC): h = (n-1)p         → 선형보간
//   type6 (Minitab/SPSS):                 h = (n+1)p - 1     → 선형보간(양끝 클램프)
//   nearest rank:                         ceil(n p) 번째 값
int quantile(const double* x, int n, double p, int method, double& out) {
    if (!x) return ERR_NULL;
    if (n < 1) return ERR_SIZE;
    if (p < 0 || p > 1) return ERR_PARAM;
    if (!allFinite(x, n)) return ERR_NAN;
    std::vector<double> v(x, x + n); std::sort(v.begin(), v.end());
    if (method == 1) { int k = (int)std::ceil(n * p) - 1; if (k < 0) k = 0; if (k > n - 1) k = n - 1; out = v[k]; return OK; }
    double h = (method == 2) ? (n + 1) * p - 1 : (n - 1) * p;
    if (h <= 0) { out = v[0]; return OK; }
    if (h >= n - 1) { out = v[n - 1]; return OK; }
    int lo = (int)std::floor(h); double f = h - lo;
    out = v[lo] + f * (v[lo + 1] - v[lo]); return OK;
}

int mode(const double* x, int n, double& out, int& count) {
    if (!x) return ERR_NULL;
    if (n < 1) return ERR_SIZE;
    if (!allFinite(x, n)) return ERR_NAN;
    std::vector<double> v(x, x + n); std::sort(v.begin(), v.end());
    out = v[0]; count = 0; int run = 1;
    for (int i = 1; i <= n; ++i) {
        if (i < n && v[i] == v[i - 1]) { ++run; continue; }
        if (run > count) { count = run; out = v[i - 1]; }
        run = 1;
    }
    return OK;
}
int mad(const double* x, int n, double& out) {
    double med; int rc = quantile(x, n, 0.5, 0, med); if (rc) return rc;
    std::vector<double> d(n); for (int i = 0; i < n; ++i) d[i] = std::fabs(x[i] - med);
    return quantile(d.data(), n, 0.5, 0, out);
}
// 중심적률 m2,m3,m4 (모집단식 1/n)
static void moments(const double* x, int n, double& m, double& m2, double& m3, double& m4) {
    m = mean(x, n); m2 = m3 = m4 = 0;
    for (int i = 0; i < n; ++i) { double d = x[i] - m, d2 = d * d; m2 += d2; m3 += d2 * d; m4 += d2 * d2; }
    m2 /= n; m3 /= n; m4 /= n;
}
// [원리] g1 = m3/m2^1.5 (모집단 왜도). 표본 보정 G1 = g1·√(n(n-1))/(n-2)  (Excel SKEW, scipy bias=False)
int skewness(const double* x, int n, double& out) {
    if (!x) return ERR_NULL; if (n < 3) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    double m, m2, m3, m4; moments(x, n, m, m2, m3, m4);
    if (m2 <= 0) return ERR_DEGENERATE;
    out = (m3 / std::pow(m2, 1.5)) * std::sqrt((double)n * (n - 1)) / (n - 2); return OK;
}
// [원리] g2 = m4/m2² - 3 (초과첨도). 표본 보정 G2 = (n-1)/((n-2)(n-3)) · ((n+1)g2 + 6)  (Excel KURT)
int kurtosis(const double* x, int n, double& out) {
    if (!x) return ERR_NULL; if (n < 4) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    double m, m2, m3, m4; moments(x, n, m, m2, m3, m4);
    if (m2 <= 0) return ERR_DEGENERATE;
    double g2 = m4 / (m2 * m2) - 3.0;
    out = (double)(n - 1) / ((n - 2.0) * (n - 3.0)) * ((n + 1.0) * g2 + 6.0); return OK;
}
int covariance(const double* x, const double* y, int n, int ddof, double& out) {
    if (!x || !y) return ERR_NULL;
    if (ddof != 0 && ddof != 1) return ERR_PARAM;
    if (n < 1 + ddof) return ERR_SIZE;
    if (!allFinite(x, n) || !allFinite(y, n)) return ERR_NAN;
    double mx = mean(x, n), my = mean(y, n), s = 0;
    for (int i = 0; i < n; ++i) s += (x[i] - mx) * (y[i] - my);
    out = s / (n - ddof); return OK;
}
// [원리] r = Σ(x-x̄)(y-ȳ) / √(Σ(x-x̄)² Σ(y-ȳ)²). 분모가 0(한쪽 상수)이면 정의 불가 → 오류.
int pearson(const double* x, const double* y, int n, double& r) {
    if (!x || !y) return ERR_NULL; if (n < 2) return ERR_SIZE;
    if (!allFinite(x, n) || !allFinite(y, n)) return ERR_NAN;
    double mx = mean(x, n), my = mean(y, n), sxy = 0, sxx = 0, syy = 0;
    for (int i = 0; i < n; ++i) { double a = x[i] - mx, b = y[i] - my; sxy += a * b; sxx += a * a; syy += b * b; }
    if (sxx <= 0 || syy <= 0) return ERR_DEGENERATE;
    r = sxy / std::sqrt(sxx * syy);
    if (r > 1) r = 1; if (r < -1) r = -1;
    return OK;
}

int describe(const double* x, int n, Describe& d) {
    if (!x) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    d = Describe();
    d.n = n; d.sum = sum(x, n); d.mean = d.sum / n;
    std::vector<double> v(x, x + n); std::sort(v.begin(), v.end());
    d.min = v.front(); d.max = v.back(); d.range = d.max - d.min;
    quantile(x, n, 0.25, 0, d.q1); quantile(x, n, 0.5, 0, d.median); quantile(x, n, 0.75, 0, d.q3);
    d.iqr = d.q3 - d.q1;
    mad(x, n, d.mad);
    if (n >= 2) { variance(x, n, 1, d.var); d.sd = std::sqrt(d.var); d.se = d.sd / std::sqrt((double)n); }
    else d.var = d.sd = d.se = NaN;
    if (n >= 3 && skewness(x, n, d.skew) != OK) d.skew = NaN; if (n < 3) d.skew = NaN;
    if (n >= 4 && kurtosis(x, n, d.kurt) != OK) d.kurt = NaN; if (n < 4) d.kurt = NaN;
    return OK;
}

// ============================================================================
//  2. 배열 처리
// ============================================================================
void sortCopy(const double* x, int n, double* out, bool desc) {
    std::copy(x, x + n, out);
    if (desc) std::sort(out, out + n, std::greater<double>()); else std::sort(out, out + n);
}
void argsort(const double* x, int n, int* idx, bool desc) {
    for (int i = 0; i < n; ++i) idx[i] = i;
    if (desc) std::stable_sort(idx, idx + n, [&](int a, int b) { return x[a] > x[b]; });
    else      std::stable_sort(idx, idx + n, [&](int a, int b) { return x[a] < x[b]; });
}
int unique(const double* x, int n, double* vals, double* counts, int cap, int& k) {
    if (!x) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    std::vector<double> v(x, x + n); std::sort(v.begin(), v.end());
    k = 0;
    for (int i = 0; i < n; ) {
        int j = i; while (j < n && v[j] == v[i]) ++j;
        if (vals && counts) { if (k >= cap) return ERR_CAPACITY; vals[k] = v[i]; counts[k] = j - i; }
        ++k; i = j;
    }
    return OK;
}
void cumsum(const double* x, int n, double* out) { double s = 0; for (int i = 0; i < n; ++i) { s += x[i]; out[i] = s; } }
int diff(const double* x, int n, int lag, double* out) {
    if (!x || !out) return ERR_NULL; if (lag < 1) return ERR_PARAM; if (n <= lag) return ERR_SIZE;
    for (int i = lag; i < n; ++i) out[i - lag] = x[i] - x[i - lag];
    return OK;
}
// [원리] z = (x - x̄)/s. 평균 0, 표준편차 1 로 맞춰 단위가 다른 변수를 비교/합산 가능하게 한다.
int standardize(const double* x, int n, double* out) {
    double sd; int rc = stddev(x, n, 1, sd); if (rc) return rc;
    if (sd <= 0) return ERR_DEGENERATE;
    double m = mean(x, n); for (int i = 0; i < n; ++i) out[i] = (x[i] - m) / sd; return OK;
}
int minmaxScale(const double* x, int n, double lo, double hi, double* out) {
    if (!x || !out) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN;
    double mn = *std::min_element(x, x + n), mx = *std::max_element(x, x + n);
    if (mx <= mn) return ERR_DEGENERATE;
    for (int i = 0; i < n; ++i) out[i] = lo + (x[i] - mn) / (mx - mn) * (hi - lo); return OK;
}
// [원리] 비모수 검정의 기본: 값 대신 순위. 동률(tie)은 해당 순위들의 평균을 준다.
void rankAverage(const double* x, int n, double* rank) {
    std::vector<int> idx(n); argsort(x, n, idx.data(), false);
    int i = 0;
    while (i < n) {
        int j = i; while (j + 1 < n && x[idx[j + 1]] == x[idx[i]]) ++j;
        double r = (i + 1 + j + 1) / 2.0;
        for (int t = i; t <= j; ++t) rank[idx[t]] = r;
        i = j + 1;
    }
}
int histogram(const double* x, int n, int bins, double lo, double hi, double* edges, double* counts) {
    if (!x || !edges || !counts) return ERR_NULL; if (n < 1) return ERR_SIZE; if (bins < 1) return ERR_PARAM;
    if (!allFinite(x, n)) return ERR_NAN;
    if (lo >= hi) { lo = *std::min_element(x, x + n); hi = *std::max_element(x, x + n); if (hi <= lo) hi = lo + 1; }
    double w = (hi - lo) / bins;
    for (int b = 0; b <= bins; ++b) edges[b] = lo + b * w;
    for (int b = 0; b < bins; ++b) counts[b] = 0;
    for (int i = 0; i < n; ++i) {
        if (x[i] < lo || x[i] > hi) continue;
        int b = (int)((x[i] - lo) / w); if (b >= bins) b = bins - 1; if (b < 0) b = 0;
        counts[b] += 1;
    }
    return OK;
}

// ============================================================================
//  3. 특수함수
// ============================================================================
double lgammaFn(double x) { return std::lgamma(x); }

// [원리] 정규화 불완전베타 I_x(a,b) = B(x;a,b)/B(a,b). Lentz 수정 연분수(NR betacf).
//        x > (a+1)/(a+b+2) 이면 대칭성 I_x(a,b) = 1 - I_{1-x}(b,a) 로 수렴이 빠른 쪽을 쓴다.
static double betacf(double a, double b, double x) {
    const int MAXIT = 500; const double FPMIN = 1e-300;
    double qab = a + b, qap = a + 1, qam = a - 1, c = 1, d = 1 - qab * x / qap;
    if (std::fabs(d) < FPMIN) d = FPMIN; d = 1 / d; double h = d;
    for (int m = 1; m <= MAXIT; ++m) {
        int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1 + aa * d; if (std::fabs(d) < FPMIN) d = FPMIN; c = 1 + aa / c; if (std::fabs(c) < FPMIN) c = FPMIN;
        d = 1 / d; h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1 + aa * d; if (std::fabs(d) < FPMIN) d = FPMIN; c = 1 + aa / c; if (std::fabs(c) < FPMIN) c = FPMIN;
        d = 1 / d; double del = d * c; h *= del;
        if (std::fabs(del - 1) < 3e-16) break;
    }
    return h;
}
double betaInc(double a, double b, double x) {
    if (x <= 0) return 0; if (x >= 1) return 1;
    double bt = std::exp(lgammaFn(a + b) - lgammaFn(a) - lgammaFn(b) + a * std::log(x) + b * std::log1p(-x));
    if (x < (a + 1) / (a + b + 2)) return bt * betacf(a, b, x) / a;
    return 1 - bt * betacf(b, a, 1 - x) / b;
}
// [원리] 정규화 하부 불완전감마 P(a,x). x < a+1 은 급수, 아니면 연분수(Q 계산 후 1-Q).
double gammaIncP(double a, double x) {
    if (x <= 0) return 0; if (a <= 0) return NaN;
    if (x < a + 1) {
        double ap = a, sum = 1 / a, del = sum;
        for (int n = 1; n < 1000; ++n) { ap += 1; del *= x / ap; sum += del; if (std::fabs(del) < std::fabs(sum) * 1e-16) break; }
        return sum * std::exp(-x + a * std::log(x) - lgammaFn(a));
    }
    return 1 - gammaIncQ(a, x);
}
double gammaIncQ(double a, double x) {
    if (x <= 0) return 1; if (a <= 0) return NaN;
    if (x < a + 1) return 1 - gammaIncP(a, x);
    const double FPMIN = 1e-300;
    double b = x + 1 - a, c = 1 / FPMIN, d = 1 / b, h = d;
    for (int i = 1; i < 1000; ++i) {
        double an = -i * (i - a); b += 2;
        d = an * d + b; if (std::fabs(d) < FPMIN) d = FPMIN;
        c = b + an / c; if (std::fabs(c) < FPMIN) c = FPMIN;
        d = 1 / d; double del = d * c; h *= del;
        if (std::fabs(del - 1) < 1e-16) break;
    }
    return std::exp(-x + a * std::log(x) - lgammaFn(a)) * h;
}

double normPdf(double z) { return std::exp(-0.5 * z * z) / std::sqrt(2 * PI); }
double normCdf(double z) { return 0.5 * std::erfc(-z / SQRT2); }
// [원리] Acklam 유리근사(상대오차 1e-9) 후 Halley 1회 보정 → 배정도 정밀.
double normQuantile(double p) {
    if (p <= 0) return -INFINITY; if (p >= 1) return INFINITY;
    static const double a[] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
    static const double b[] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01 };
    static const double c[] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
    static const double d[] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00 };
    double q, r, x;
    if (p < 0.02425) { q = std::sqrt(-2 * std::log(p)); x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
    else if (p > 1 - 0.02425) { q = std::sqrt(-2 * std::log(1 - p)); x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
    else { q = p - 0.5; r = q * q; x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1); }
    double e = normCdf(x) - p, u = e * std::sqrt(2 * PI) * std::exp(x * x / 2);
    x = x - u / (1 + x * u / 2);
    return x;
}
// [원리] t 분포 CDF: x = ν/(ν+t²) 로 두면 P(T>t) = ½ I_x(ν/2, ½).
double tCdf(double t, double df) {
    if (df <= 0) return NaN;
    if (std::isinf(t)) return t > 0 ? 1 : 0;
    double x = df / (df + t * t);
    double tail = 0.5 * betaInc(df / 2, 0.5, x);
    return t >= 0 ? 1 - tail : tail;
}
double tSf(double t, double df) {
    if (df <= 0) return NaN; if (std::isinf(t)) return t > 0 ? 0 : 1;
    double x = df / (df + t * t), tail = 0.5 * betaInc(df / 2, 0.5, x);
    return t >= 0 ? tail : 1 - tail;
}
double chi2Sf(double x, double df) { if (df <= 0) return NaN; if (x <= 0) return 1; return gammaIncQ(df / 2, x / 2); }
double fSf(double x, double d1, double d2) { if (d1 <= 0 || d2 <= 0) return NaN; if (x <= 0) return 1; return betaInc(d2 / 2, d1 / 2, d2 / (d1 * x + d2)); }
double chi2Cdf(double x, double df) { if (df <= 0) return NaN; if (x <= 0) return 0; return gammaIncP(df / 2, x / 2); }
// [원리] F 분포 CDF: F ~ (χ²₁/d1)/(χ²₂/d2) → I_{d1x/(d1x+d2)}(d1/2, d2/2)
double fCdf(double x, double d1, double d2) { if (d1 <= 0 || d2 <= 0) return NaN; if (x <= 0) return 0; return betaInc(d1 / 2, d2 / 2, d1 * x / (d1 * x + d2)); }

// 일반 역CDF: 단조 CDF 를 [lo,hi] 에서 괄호 확장 후 이분법+할선(Illinois) 으로 푼다.
template <class F>
static double invertCdf(F cdf, double p, double lo, double hi, bool unboundedLo, bool unboundedHi) {
    if (p <= 0) return unboundedLo ? -INFINITY : lo;
    if (p >= 1) return unboundedHi ? INFINITY : hi;
    // 괄호 확장
    for (int i = 0; i < 200 && cdf(lo) > p; ++i) { double w = (hi - lo); lo -= (w > 0 ? w : 1); if (!unboundedLo) break; }
    for (int i = 0; i < 200 && cdf(hi) < p; ++i) { double w = (hi - lo); hi += (w > 0 ? w : 1); if (!unboundedHi) break; }
    double flo = cdf(lo) - p, fhi = cdf(hi) - p;
    int side = 0;
    for (int it = 0; it < 300; ++it) {
        double mid;
        if (fhi != flo) mid = hi - fhi * (hi - lo) / (fhi - flo); else mid = 0.5 * (lo + hi);
        if (!(mid > lo && mid < hi)) mid = 0.5 * (lo + hi);
        double fm = cdf(mid) - p;
        if (std::fabs(fm) < 1e-15 || (hi - lo) < 1e-14 * (1 + std::fabs(mid))) return mid;
        if ((fm < 0) == (flo < 0)) { lo = mid; flo = fm; if (side == -1) fhi /= 2; side = -1; }
        else                       { hi = mid; fhi = fm; if (side == 1) flo /= 2; side = 1; }
    }
    return 0.5 * (lo + hi);
}
double tQuantile(double p, double df) {
    if (df <= 0 || p < 0 || p > 1) return NaN;
    if (df > 1e7) return normQuantile(p);
    double z = normQuantile(p); double g = std::fabs(z) + 1;
    return invertCdf([&](double t) { return tCdf(t, df); }, p, -g, g, true, true);
}
double chi2Quantile(double p, double df) {
    if (df <= 0 || p < 0 || p > 1) return NaN;
    return invertCdf([&](double x) { return chi2Cdf(x, df); }, p, 0.0, df + 10 * std::sqrt(2 * df) + 10, false, true);
}
double fQuantile(double p, double d1, double d2) {
    if (d1 <= 0 || d2 <= 0 || p < 0 || p > 1) return NaN;
    return invertCdf([&](double x) { return fCdf(x, d1, d2); }, p, 0.0, 10.0, false, true);
}

// [원리] 스튜던트화 범위 Q = (max-min)/s. Tukey HSD 의 임계값은 이 분포의 분위수.
//   P(Q<=q) = ∫₀^∞ f_ν(s) · [ k ∫ φ(z) (Φ(z) − Φ(z−qs))^{k−1} dz ] ds
//   s = √(χ²_ν/ν) 의 밀도 f_ν(s) = ν^{ν/2} s^{ν−1} e^{−νs²/2} / (Γ(ν/2) 2^{ν/2−1}).
//   닫힌 식이 없어 Gauss-Legendre 수치적분. (ν 가 크면 s≡1, 내부 적분만)
static const double GL_X[8] = { 0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438, 0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499 };
static const double GL_W[8] = { 0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767, 0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541 };
template <class F> static double gl16(F f, double a, double b) {
    double c = 0.5 * (a + b), h = 0.5 * (b - a), s = 0;
    for (int i = 0; i < 8; ++i) s += GL_W[i] * (f(c + h * GL_X[i]) + f(c - h * GL_X[i]));
    return s * h;
}
static double tukeyInner(double qs, double k) {
    if (qs <= 0) return 0;
    auto g = [&](double z) { double d = normCdf(z) - normCdf(z - qs); return d <= 0 ? 0 : normPdf(z) * std::pow(d, k - 1); };
    double lo = -8.0, hi = 8.0 + qs; int m = (int)std::ceil((hi - lo) / 0.5); double h = (hi - lo) / m, s = 0;
    for (int i = 0; i < m; ++i) s += gl16(g, lo + i * h, lo + (i + 1) * h);
    return k * s;
}
double tukeyCdf(double q, double k, double df) {
    if (q <= 0) return 0; if (k < 2 || df < 1) return NaN;
    if (df > 25000) return tukeyInner(q, k);
    double lc = (df / 2) * std::log(df) - lgammaFn(df / 2) - (df / 2 - 1) * std::log(2.0);
    auto fs = [&](double s) { return std::exp(lc + (df - 1) * std::log(s) - df * s * s / 2) * tukeyInner(q * s, k); };
    double sd = 1 / std::sqrt(2 * df); double lo = std::max(0.0, 1 - 12 * sd), hi = 1 + 12 * sd; if (df < 4) { lo = 0; hi = 8; }
    int m = std::max(24, (int)((hi - lo) / (sd)) ); if (m > 400) m = 400;
    double h = (hi - lo) / m, s = 0;
    for (int i = 0; i < m; ++i) s += gl16(fs, lo + i * h, lo + (i + 1) * h);
    return s > 1 ? 1 : s;
}
double tukeyQuantile(double p, double k, double df) {
    if (k < 2 || df < 1 || p <= 0 || p >= 1) return NaN;
    return invertCdf([&](double q) { return tukeyCdf(q, k, df); }, p, 0.5, 6.0, false, true);
}

// ============================================================================
//  4. 확률분포 (PDF/CDF/Quantile/평균/분산)
// ============================================================================
static double logChoose(double n, double k) { return lgammaFn(n + 1) - lgammaFn(k + 1) - lgammaFn(n - k + 1); }

// 이산분포 분위수: 누적합이 p 이상이 되는 최소 정수
template <class PMF> static double discreteQuantile(PMF pmf, double p, double kMax) {
    double c = 0;
    for (double k = 0; k <= kMax; ++k) { c += pmf(k); if (c >= p - 1e-12) return k; }
    return kMax;
}

int dist(int id, int func, double x, double p1, double p2, double p3, double& out) {
    if (func < 0 || func > 4) return ERR_PARAM;
    if (!std::isfinite(p1) || !std::isfinite(p2) || !std::isfinite(p3)) return ERR_NAN;
    if (func <= 2 && !std::isfinite(x) && !(func <= 1 && std::isinf(x))) return ERR_NAN;
    if (func == 2 && (x < 0 || x > 1)) return ERR_PARAM;
    out = NaN;
    switch (id) {
    case D_NORMAL: { double mu = p1, sg = p2; if (sg <= 0) return ERR_PARAM;
        if (func == 0) out = normPdf((x - mu) / sg) / sg; else if (func == 1) out = normCdf((x - mu) / sg);
        else if (func == 2) out = mu + sg * normQuantile(x); else if (func == 3) out = mu; else out = sg * sg; return OK; }
    case D_T: { double df = p1; if (df <= 0) return ERR_PARAM;
        if (func == 0) out = std::exp(lgammaFn((df + 1) / 2) - lgammaFn(df / 2)) / std::sqrt(df * PI) * std::pow(1 + x * x / df, -(df + 1) / 2);
        else if (func == 1) out = tCdf(x, df); else if (func == 2) out = tQuantile(x, df);
        else if (func == 3) out = df > 1 ? 0 : NaN; else out = df > 2 ? df / (df - 2) : NaN; return OK; }
    case D_CHISQ: { double df = p1; if (df <= 0) return ERR_PARAM;
        if (func == 0) out = x <= 0 ? 0 : std::exp((df / 2 - 1) * std::log(x) - x / 2 - (df / 2) * std::log(2.0) - lgammaFn(df / 2));
        else if (func == 1) out = chi2Cdf(x, df); else if (func == 2) out = chi2Quantile(x, df);
        else if (func == 3) out = df; else out = 2 * df; return OK; }
    case D_F: { double d1 = p1, d2 = p2; if (d1 <= 0 || d2 <= 0) return ERR_PARAM;
        if (func == 0) out = x <= 0 ? 0 : std::exp(0.5 * (d1 * std::log(d1 * x) + d2 * std::log(d2) - (d1 + d2) * std::log(d1 * x + d2)) - std::log(x) - (lgammaFn(d1 / 2) + lgammaFn(d2 / 2) - lgammaFn((d1 + d2) / 2)));
        else if (func == 1) out = fCdf(x, d1, d2); else if (func == 2) out = fQuantile(x, d1, d2);
        else if (func == 3) out = d2 > 2 ? d2 / (d2 - 2) : NaN; else out = d2 > 4 ? 2 * d2 * d2 * (d1 + d2 - 2) / (d1 * (d2 - 2) * (d2 - 2) * (d2 - 4)) : NaN; return OK; }
    case D_UNIFORM: { double a = p1, b = p2; if (b <= a) return ERR_PARAM;
        if (func == 0) out = (x >= a && x <= b) ? 1 / (b - a) : 0; else if (func == 1) out = x <= a ? 0 : x >= b ? 1 : (x - a) / (b - a);
        else if (func == 2) out = a + x * (b - a); else if (func == 3) out = (a + b) / 2; else out = (b - a) * (b - a) / 12; return OK; }
    case D_EXPONENTIAL: { double lam = p1; if (lam <= 0) return ERR_PARAM;
        if (func == 0) out = x < 0 ? 0 : lam * std::exp(-lam * x); else if (func == 1) out = x <= 0 ? 0 : 1 - std::exp(-lam * x);
        else if (func == 2) out = -std::log1p(-x) / lam; else if (func == 3) out = 1 / lam; else out = 1 / (lam * lam); return OK; }
    case D_GAMMA: { double k = p1, th = p2; if (k <= 0 || th <= 0) return ERR_PARAM;
        if (func == 0) out = x <= 0 ? 0 : std::exp((k - 1) * std::log(x) - x / th - lgammaFn(k) - k * std::log(th));
        else if (func == 1) out = x <= 0 ? 0 : gammaIncP(k, x / th);
        else if (func == 2) out = invertCdf([&](double v) { return v <= 0 ? 0.0 : gammaIncP(k, v / th); }, x, 0.0, k * th + 10 * std::sqrt(k) * th + th, false, true);
        else if (func == 3) out = k * th; else out = k * th * th; return OK; }
    case D_BETA: { double a = p1, b = p2; if (a <= 0 || b <= 0) return ERR_PARAM;
        if (func == 0) out = (x <= 0 || x >= 1) ? 0 : std::exp((a - 1) * std::log(x) + (b - 1) * std::log1p(-x) + lgammaFn(a + b) - lgammaFn(a) - lgammaFn(b));
        else if (func == 1) out = betaInc(a, b, x);
        else if (func == 2) out = invertCdf([&](double v) { return betaInc(a, b, v); }, x, 0.0, 1.0, false, false);
        else if (func == 3) out = a / (a + b); else out = a * b / ((a + b) * (a + b) * (a + b + 1)); return OK; }
    case D_LOGNORMAL: { double mu = p1, sg = p2; if (sg <= 0) return ERR_PARAM;
        if (func == 0) out = x <= 0 ? 0 : normPdf((std::log(x) - mu) / sg) / (x * sg); else if (func == 1) out = x <= 0 ? 0 : normCdf((std::log(x) - mu) / sg);
        else if (func == 2) out = std::exp(mu + sg * normQuantile(x)); else if (func == 3) out = std::exp(mu + sg * sg / 2); else out = (std::exp(sg * sg) - 1) * std::exp(2 * mu + sg * sg); return OK; }
    case D_BERNOULLI: { double p = p1; if (p < 0 || p > 1) return ERR_PARAM;
        if (func == 0) out = x == 0 ? 1 - p : x == 1 ? p : 0; else if (func == 1) out = x < 0 ? 0 : x < 1 ? 1 - p : 1;
        else if (func == 2) out = x <= 1 - p ? 0 : 1; else if (func == 3) out = p; else out = p * (1 - p); return OK; }
    case D_BINOMIAL: { double n = std::floor(p1), p = p2; if (n < 0 || p < 0 || p > 1) return ERR_PARAM;
        auto pmf = [&](double k) { if (k < 0 || k > n) return 0.0; if (p == 0) return k == 0 ? 1.0 : 0.0; if (p == 1) return k == n ? 1.0 : 0.0; return std::exp(logChoose(n, k) + k * std::log(p) + (n - k) * std::log1p(-p)); };
        if (func == 0) out = pmf(std::floor(x));
        else if (func == 1) { double k = std::floor(x); if (k < 0) out = 0; else if (k >= n) out = 1; else out = betaInc(n - k, k + 1, 1 - p); } // P(X<=k)=I_{1-p}(n-k,k+1)
        else if (func == 2) out = discreteQuantile(pmf, x, n); else if (func == 3) out = n * p; else out = n * p * (1 - p); return OK; }
    case D_POISSON: { double lam = p1; if (lam < 0) return ERR_PARAM;
        auto pmf = [&](double k) { if (k < 0) return 0.0; return std::exp(k * std::log(lam) - lam - lgammaFn(k + 1)); };
        if (func == 0) out = lam == 0 ? (std::floor(x) == 0 ? 1 : 0) : pmf(std::floor(x));
        else if (func == 1) { double k = std::floor(x); out = k < 0 ? 0 : (lam == 0 ? 1 : gammaIncQ(k + 1, lam)); }   // P(X<=k)=Q(k+1,λ)
        else if (func == 2) out = discreteQuantile(pmf, x, lam + 20 * std::sqrt(lam + 1) + 50); else if (func == 3) out = lam; else out = lam; return OK; }
    case D_GEOMETRIC: { double p = p1; if (p <= 0 || p > 1) return ERR_PARAM;   // 첫 성공까지 실패 횟수 k=0,1,2...
        auto pmf = [&](double k) { return k < 0 ? 0.0 : p * std::pow(1 - p, k); };
        if (func == 0) out = pmf(std::floor(x)); else if (func == 1) out = x < 0 ? 0 : 1 - std::pow(1 - p, std::floor(x) + 1);
        else if (func == 2) out = x >= 1 ? INFINITY : std::max(0.0, std::ceil(std::log1p(-x) / std::log1p(-p) - 1));
        else if (func == 3) out = (1 - p) / p; else out = (1 - p) / (p * p); return OK; }
    case D_HYPERGEOMETRIC: { double N = std::floor(p1), K = std::floor(p2), n = std::floor(p3);
        if (N < 0 || K < 0 || K > N || n < 0 || n > N) return ERR_PARAM;
        auto pmf = [&](double k) { if (k < std::max(0.0, n - (N - K)) || k > std::min(n, K)) return 0.0; return std::exp(logChoose(K, k) + logChoose(N - K, n - k) - logChoose(N, n)); };
        if (func == 0) out = pmf(std::floor(x));
        else if (func == 1) { double kk = std::floor(x); double c = 0; for (double k = 0; k <= kk && k <= n; ++k) c += pmf(k); out = kk < 0 ? 0 : std::min(1.0, c); }
        else if (func == 2) out = discreteQuantile(pmf, x, n); else if (func == 3) out = n * K / N;
        else out = N > 1 ? n * (K / N) * (1 - K / N) * (N - n) / (N - 1) : 0; return OK; }
    default: return ERR_PARAM;
    }
}

// ============================================================================
//  5. 난수 (xoshiro256**, 전역 상태 없음)
// ============================================================================
namespace {
struct Rng {
    uint64_t s[4];
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    explicit Rng(uint64_t seed) {
        if (seed == 0) seed = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count() ^ 0x9E3779B97F4A7C15ULL;
        uint64_t z = seed;   // splitmix64 로 상태 초기화
        for (int i = 0; i < 4; ++i) { z += 0x9E3779B97F4A7C15ULL; uint64_t r = z; r = (r ^ (r >> 30)) * 0xBF58476D1CE4E5B9ULL; r = (r ^ (r >> 27)) * 0x94D049BB133111EBULL; s[i] = r ^ (r >> 31); }
    }
    uint64_t next() {
        uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl(s[3], 45); return r;
    }
    double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }          // [0,1)
    double uniformOpen() { double u; do { u = uniform(); } while (u <= 0); return u; }  // (0,1)
    double normal() { double u1 = uniformOpen(), u2 = uniform(); return std::sqrt(-2 * std::log(u1)) * std::cos(2 * PI * u2); }
    // Marsaglia-Tsang 감마(shape k, scale 1)
    double gamma(double k) {
        if (k < 1) { double u = uniformOpen(); return gamma(k + 1) * std::pow(u, 1 / k); }
        double d = k - 1.0 / 3, c = 1 / std::sqrt(9 * d);
        for (;;) { double x, v; do { x = normal(); v = 1 + c * x; } while (v <= 0); v = v * v * v; double u = uniformOpen();
            if (u < 1 - 0.0331 * x * x * x * x) return d * v; if (std::log(u) < 0.5 * x * x + d * (1 - v + std::log(v))) return d * v; }
    }
    double poisson(double lam) {   // Knuth (λ 를 500 단위로 쪼개 언더플로 회피)
        double k = 0; while (lam > 500) { k += poisson(500); lam -= 500; }
        double L = std::exp(-lam), p = 1; double kk = 0; do { ++kk; p *= uniform(); } while (p > L); return k + kk - 1;
    }
};
}

int random(int id, double p1, double p2, double p3, uint64_t seed, int count, double* out) {
    if (!out) return ERR_NULL; if (count < 1) return ERR_SIZE;
    if (!std::isfinite(p1) || !std::isfinite(p2) || !std::isfinite(p3)) return ERR_NAN;
    Rng g(seed);
    switch (id) {
    case D_NORMAL: if (p2 <= 0) return ERR_PARAM; for (int i = 0; i < count; ++i) out[i] = p1 + p2 * g.normal(); return OK;
    case D_T: { if (p1 <= 0) return ERR_PARAM; for (int i = 0; i < count; ++i) { double z = g.normal(), c = 2 * g.gamma(p1 / 2); out[i] = z / std::sqrt(c / p1); } return OK; }
    case D_CHISQ: if (p1 <= 0) return ERR_PARAM; for (int i = 0; i < count; ++i) out[i] = 2 * g.gamma(p1 / 2); return OK;
    case D_F: { if (p1 <= 0 || p2 <= 0) return ERR_PARAM; for (int i = 0; i < count; ++i) { double a = 2 * g.gamma(p1 / 2) / p1, b = 2 * g.gamma(p2 / 2) / p2; out[i] = a / b; } return OK; }
    case D_UNIFORM: if (p2 <= p1) return ERR_PARAM; for (int i = 0; i < count; ++i) out[i] = p1 + (p2 - p1) * g.uniform(); return OK;
    case D_EXPONENTIAL: if (p1 <= 0) return ERR_PARAM; for (int i = 0; i < count; ++i) out[i] = -std::log(g.uniformOpen()) / p1; return OK;
    case D_GAMMA: if (p1 <= 0 || p2 <= 0) return ERR_PARAM; for (int i = 0; i < count; ++i) out[i] = p2 * g.gamma(p1); return OK;
    case D_BETA: { if (p1 <= 0 || p2 <= 0) return ERR_PARAM; for (int i = 0; i < count; ++i) { double a = g.gamma(p1), b = g.gamma(p2); out[i] = a / (a + b); } return OK; }
    case D_LOGNORMAL: if (p2 <= 0) return ERR_PARAM; for (int i = 0; i < count; ++i) out[i] = std::exp(p1 + p2 * g.normal()); return OK;
    case D_BERNOULLI: if (p1 < 0 || p1 > 1) return ERR_PARAM; for (int i = 0; i < count; ++i) out[i] = g.uniform() < p1 ? 1 : 0; return OK;
    case D_BINOMIAL: { int n = (int)p1; if (n < 0 || p2 < 0 || p2 > 1) return ERR_PARAM;
        for (int i = 0; i < count; ++i) { int k = 0; for (int j = 0; j < n; ++j) if (g.uniform() < p2) ++k; out[i] = k; } return OK; }
    case D_POISSON: if (p1 < 0) return ERR_PARAM; for (int i = 0; i < count; ++i) out[i] = p1 == 0 ? 0 : g.poisson(p1); return OK;
    case D_GEOMETRIC: if (p1 <= 0 || p1 > 1) return ERR_PARAM; for (int i = 0; i < count; ++i) out[i] = p1 == 1 ? 0 : std::floor(std::log(g.uniformOpen()) / std::log1p(-p1)); return OK;
    case D_HYPERGEOMETRIC: { int N = (int)p1, K = (int)p2, n = (int)p3; if (N < 0 || K < 0 || K > N || n < 0 || n > N) return ERR_PARAM;
        for (int i = 0; i < count; ++i) { int rem = N, good = K, k = 0; for (int j = 0; j < n; ++j) { if (g.uniform() * rem < good) { ++k; --good; } --rem; } out[i] = k; } return OK; }
    default: return ERR_PARAM;
    }
}

} // namespace adp

// ============================================================================
//  NOTEPAD.dll  -  notepad_adp_exports.cpp
//  [NOTEPAD] 세트 AL: ADP 통계/데이터분석 export 인터페이스 (AHK v1 64비트 DllCall)
//
//  [규약 — 기존 문자열 규약과 다르다. 배열 통계라 숫자 배열을 직접 넘긴다]
//   * 배열 입력: (const double* x, int n)  — AHK 에서 VarSetCapacity + NumPut 으로 double 배열 생성.
//   * 집단 라벨: (const int* groups)  — 0..k-1 정수 (Int 4바이트).
//   * 결과: 호출자가 준비한 double 배열(out)에 기록. 각 함수 주석의 레이아웃 참고.
//   * 반환값: 0 = 성공, 음수 = 오류코드 (Notepad_AdpErrorText 로 설명 문자열).
//       -1 null  -2 표본부족/길이불일치  -3 NaN/Inf  -4 분산0/특이행렬  -5 파라미터  -6 미수렴  -7 버퍼부족
//   * 메모리 소유권: DLL 은 아무것도 할당하지 않는다. 모든 버퍼는 호출자 소유. 해제 불필요.
//   * 전역 상태 없음 → 스레드 안전. 난수 seed 는 인자로(0 = 시간 기반).
//   * Notepad_ADP.ahk 클래스가 이 마샬링을 전부 감싸므로 AHK 사용자는 배열/객체만 다루면 된다.
// ============================================================================
#include "notepad_adp.h"
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#define ADP_CALL __stdcall
#ifndef NOTEPAD_API
#define NOTEPAD_API extern "C" __declspec(dllexport)
#endif
#else
#define ADP_CALL
#define NOTEPAD_API extern "C"
typedef wchar_t WCHAR;
#endif

using namespace adp;

// ---- 버전 / 오류 문자열 ----------------------------------------------------
NOTEPAD_API int ADP_CALL Notepad_AdpVersion() { return 10000; }   // 1.00.00
// 오류코드 -> 설명(UTF-16). 기존 2단계 버퍼 규약(문자수 반환).
NOTEPAD_API int ADP_CALL Notepad_AdpErrorText(int code, wchar_t* out, int outCch) {
    const char* s = statusText(code); int need = (int)std::strlen(s);
    if (!out || outCch == 0) return need; if (outCch < need + 1) return -need;
    for (int i = 0; i < need; ++i) out[i] = (wchar_t)s[i]; out[need] = 0; return need;
}

// ============================================================================
//  1. 기술통계
// ============================================================================
// out[16]: n,sum,mean,var,sd,se,min,q1,median,q3,max,range,iqr,mad,skew,kurt
NOTEPAD_API int ADP_CALL Notepad_AdpDescribe(const double* x, int n, double* out) {
    if (!out) return ERR_NULL; Describe d; int rc = describe(x, n, d); if (rc) return rc;
    const double* p = &d.n; for (int i = 0; i < 16; ++i) out[i] = p[i]; return OK;
}
NOTEPAD_API int ADP_CALL Notepad_AdpSum(const double* x, int n, double* out) { if (!x || !out) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN; *out = sum(x, n); return OK; }
NOTEPAD_API int ADP_CALL Notepad_AdpMean(const double* x, int n, double* out) { if (!x || !out) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN; *out = mean(x, n); return OK; }
NOTEPAD_API int ADP_CALL Notepad_AdpWeightedMean(const double* x, const double* w, int n, double* out) { if (!out) return ERR_NULL; return weightedMean(x, w, n, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpVariance(const double* x, int n, int ddof, double* out) { if (!out) return ERR_NULL; return variance(x, n, ddof, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpStddev(const double* x, int n, int ddof, double* out) { if (!out) return ERR_NULL; return stddev(x, n, ddof, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpQuantile(const double* x, int n, double p, int method, double* out) { if (!out) return ERR_NULL; return quantile(x, n, p, method, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpMode(const double* x, int n, double* out, int* count) { if (!out || !count) return ERR_NULL; return mode(x, n, *out, *count); }
NOTEPAD_API int ADP_CALL Notepad_AdpMAD(const double* x, int n, double* out) { if (!out) return ERR_NULL; return mad(x, n, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpSkewness(const double* x, int n, double* out) { if (!out) return ERR_NULL; return skewness(x, n, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpKurtosis(const double* x, int n, double* out) { if (!out) return ERR_NULL; return kurtosis(x, n, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpCovariance(const double* x, const double* y, int n, int ddof, double* out) { if (!out) return ERR_NULL; return covariance(x, y, n, ddof, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpCorrelation(const double* x, const double* y, int n, double* out) { if (!out) return ERR_NULL; return pearson(x, y, n, *out); }

// ============================================================================
//  2. 배열 처리
// ============================================================================
NOTEPAD_API int ADP_CALL Notepad_AdpSort(const double* x, int n, double* out, int desc) { if (!x || !out) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN; sortCopy(x, n, out, desc != 0); return OK; }
NOTEPAD_API int ADP_CALL Notepad_AdpArgsort(const double* x, int n, int* idx, int desc) { if (!x || !idx) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN; argsort(x, n, idx, desc != 0); return OK; }
// vals/counts 길이 cap. k 에 고유값 개수. vals==nullptr 이면 개수만.
NOTEPAD_API int ADP_CALL Notepad_AdpUnique(const double* x, int n, double* vals, double* counts, int cap, int* k) { if (!k) return ERR_NULL; return unique(x, n, vals, counts, cap, *k); }
NOTEPAD_API int ADP_CALL Notepad_AdpCumSum(const double* x, int n, double* out) { if (!x || !out) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN; cumsum(x, n, out); return OK; }
NOTEPAD_API int ADP_CALL Notepad_AdpDiff(const double* x, int n, int lag, double* out) { if (x && n > 0 && !allFinite(x, n)) return ERR_NAN; return diff(x, n, lag, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpStandardize(const double* x, int n, double* out) { if (!out) return ERR_NULL; return standardize(x, n, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpMinMaxScale(const double* x, int n, double lo, double hi, double* out) { return minmaxScale(x, n, lo, hi, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpRank(const double* x, int n, double* out) { if (!x || !out) return ERR_NULL; if (n < 1) return ERR_SIZE; if (!allFinite(x, n)) return ERR_NAN; rankAverage(x, n, out); return OK; }
NOTEPAD_API int ADP_CALL Notepad_AdpHistogram(const double* x, int n, int bins, double lo, double hi, double* edges, double* counts) { return histogram(x, n, bins, lo, hi, edges, counts); }

// ============================================================================
//  3. 확률분포 / 난수
// ============================================================================
// distId: 0정규(mu,sigma) 1t(df) 2χ²(df) 3F(d1,d2) 4균등(a,b) 5지수(λ) 6감마(shape,scale) 7베타(a,b) 8로그정규(mu,sigma)
//         10베르누이(p) 11이항(n,p) 12포아송(λ) 13기하(p) 14초기하(N,K,n)
// func: 0=PDF/PMF 1=CDF 2=Quantile 3=평균 4=분산
NOTEPAD_API int ADP_CALL Notepad_AdpDist(int distId, int func, double x, double p1, double p2, double p3, double* out) { if (!out) return ERR_NULL; return dist(distId, func, x, p1, p2, p3, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpRandom(int distId, double p1, double p2, double p3, long long seed, int count, double* out) { return random(distId, p1, p2, p3, (uint64_t)seed, count, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpTukeyCdf(double q, double k, double df, double* out) { if (!out) return ERR_NULL; if (k < 2 || df < 1 || q < 0) return ERR_PARAM; *out = tukeyCdf(q, k, df); return OK; }
NOTEPAD_API int ADP_CALL Notepad_AdpTukeyQuantile(double p, double k, double df, double* out) { if (!out) return ERR_NULL; if (k < 2 || df < 1 || p <= 0 || p >= 1) return ERR_PARAM; *out = tukeyQuantile(p, k, df); return OK; }

// ============================================================================
//  4. 추정
// ============================================================================
NOTEPAD_API int ADP_CALL Notepad_AdpMeanCI(const double* x, int n, double conf, double* out) { return meanCI(x, n, conf, out); }                       // [mean,lo,hi,se,df]
NOTEPAD_API int ADP_CALL Notepad_AdpProportionCI(int k, int n, double conf, int method, double* out) { return proportionCI(k, n, conf, method, out); }   // [phat,lo,hi,se]
NOTEPAD_API int ADP_CALL Notepad_AdpVarianceCI(const double* x, int n, double conf, double* out) { return varianceCI(x, n, conf, out); }               // [var,lo,hi,sd,sdLo,sdHi]
NOTEPAD_API int ADP_CALL Notepad_AdpDiffMeansCI(const double* x, int nx, const double* y, int ny, double conf, int equalVar, double* out) { return diffMeansCI(x, nx, y, ny, conf, equalVar, out); } // [diff,lo,hi,se,df]
NOTEPAD_API int ADP_CALL Notepad_AdpDiffPropsCI(int k1, int n1, int k2, int n2, double conf, double* out) { return diffPropsCI(k1, n1, k2, n2, conf, out); }   // [diff,lo,hi,se]
NOTEPAD_API int ADP_CALL Notepad_AdpSampleSizeMean(double sigma, double E, double conf, double* out) { if (!out) return ERR_NULL; return sampleSizeMean(sigma, E, conf, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpSampleSizeProportion(double p, double E, double conf, double* out) { if (!out) return ERR_NULL; return sampleSizeProportion(p, E, conf, *out); }
NOTEPAD_API int ADP_CALL Notepad_AdpSampleSizeTwoGroups(double delta, double sigma, double alpha, double power, double* out) { if (!out) return ERR_NULL; return sampleSizeTwoGroups(delta, sigma, alpha, power, *out); }

// ============================================================================
//  5. 가설검정 (alt: 0 양측, 1 우측, -1 좌측)
// ============================================================================
NOTEPAD_API int ADP_CALL Notepad_AdpTTestOne(const double* x, int n, double mu0, int alt, double conf, double* out) { return tTestOne(x, n, mu0, alt, conf, out); }   // [t,df,p,ciLo,ciHi,d,mean,se]
NOTEPAD_API int ADP_CALL Notepad_AdpTTestTwo(const double* x, int nx, const double* y, int ny, int equalVar, int alt, double conf, double* out) { return tTestTwo(x, nx, y, ny, equalVar, alt, conf, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpTTestPaired(const double* x, const double* y, int n, int alt, double conf, double* out) { return tTestPaired(x, y, n, alt, conf, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpVarTestChi(const double* x, int n, double sigma0Sq, int alt, double conf, double* out) { return varTestChi(x, n, sigma0Sq, alt, conf, out); }   // [chi2,df,p,lo,hi,var]
NOTEPAD_API int ADP_CALL Notepad_AdpVarTestF(const double* x, int nx, const double* y, int ny, int alt, double conf, double* out) { return varTestF(x, nx, y, ny, alt, conf, out); }   // [F,df1,df2,p,ratio,lo,hi]
NOTEPAD_API int ADP_CALL Notepad_AdpPropTestOne(int k, int n, double p0, int alt, int correct, double conf, double* out) { return propTestOne(k, n, p0, alt, correct, conf, out); }   // [z,p,phat,se,lo,hi]
NOTEPAD_API int ADP_CALL Notepad_AdpPropTestTwo(int k1, int n1, int k2, int n2, int alt, int correct, double conf, double* out) { return propTestTwo(k1, n1, k2, n2, alt, correct, conf, out); }   // [z,p,p1,p2,diff,lo,hi]
NOTEPAD_API int ADP_CALL Notepad_AdpChiGoodness(const double* obs, const double* expected, int k, double* out) { return chiGoodness(obs, expected, k, out); }   // [chi2,df,p]
NOTEPAD_API int ADP_CALL Notepad_AdpChiIndependence(const double* table, int r, int c, int correct, double* out, double* expectedOut) { return chiIndependence(table, r, c, correct, out, expectedOut); }   // [chi2,df,p,V,minE]
NOTEPAD_API int ADP_CALL Notepad_AdpMannWhitney(const double* x, int nx, const double* y, int ny, int alt, double* out) { return mannWhitney(x, nx, y, ny, alt, out); }   // [U,z,p,U1,U2,rb]
NOTEPAD_API int ADP_CALL Notepad_AdpWilcoxonSR(const double* x, const double* y, int n, int alt, double* out) { return wilcoxonSR(x, y, n, alt, out); }   // [W+,z,p,nEff,W-]
NOTEPAD_API int ADP_CALL Notepad_AdpKruskalWallis(const double* v, const int* groups, int n, double* out) { return kruskalWallis(v, groups, n, out); }   // [H,df,p,k,eps2]
NOTEPAD_API int ADP_CALL Notepad_AdpFriedman(const double* data, int b, int k, double* out) { return friedman(data, b, k, out); }   // [chi2,df,p,W]
NOTEPAD_API int ADP_CALL Notepad_AdpShapiroWilk(const double* x, int n, double* out) { return shapiroWilk(x, n, out); }   // [W,p]
NOTEPAD_API int ADP_CALL Notepad_AdpLevene(const double* v, const int* groups, int n, int center, double* out) { return levene(v, groups, n, center, out); }   // [F,df1,df2,p]
NOTEPAD_API int ADP_CALL Notepad_AdpJarqueBera(const double* x, int n, double* out) { return jarqueBera(x, n, out); }   // [JB,p,skew,kurt]

// ============================================================================
//  6. 상관 / 회귀
// ============================================================================
NOTEPAD_API int ADP_CALL Notepad_AdpPearsonTest(const double* x, const double* y, int n, double conf, double* out) { return pearsonTest(x, y, n, conf, out); }   // [r,t,df,p,lo,hi]
NOTEPAD_API int ADP_CALL Notepad_AdpSpearman(const double* x, const double* y, int n, double* out) { return spearman(x, y, n, out); }   // [rho,t,df,p]
NOTEPAD_API int ADP_CALL Notepad_AdpKendall(const double* x, const double* y, int n, double* out) { return kendall(x, y, n, out); }   // [tau,z,p,C,D]
// out[18]: b0,b1,se0,se1,t0,t1,p0,p1,R2,adjR2,RMSE,MAE,F,pF,dfRes,DW,sigma,SSE   fitted/resid 길이 n (nullptr 허용)
NOTEPAD_API int ADP_CALL Notepad_AdpSimpleRegression(const double* x, const double* y, int n, double* out, double* fitted, double* resid) { return simpleRegression(x, y, n, out, fitted, resid); }
// X: n×p 행우선. coef/se/t/pv 길이 p+1. stats[12]. fitted/resid 길이 n (nullptr 허용)
NOTEPAD_API int ADP_CALL Notepad_AdpMultipleRegression(const double* X, const double* y, int n, int p, double* coef, double* se, double* t, double* pv, double* stats, double* fitted, double* resid) { return multipleRegression(X, y, n, p, coef, se, t, pv, stats, fitted, resid); }
NOTEPAD_API int ADP_CALL Notepad_AdpPolynomialRegression(const double* x, const double* y, int n, int deg, double* coef, double* se, double* t, double* pv, double* stats, double* fitted, double* resid) { return polynomialRegression(x, y, n, deg, coef, se, t, pv, stats, fitted, resid); }
NOTEPAD_API int ADP_CALL Notepad_AdpRegressionDiag(const double* X, const double* y, int n, int p, double* leverage, double* stdResid, double* cookD) { return regressionDiag(X, y, n, p, leverage, stdResid, cookD); }
NOTEPAD_API int ADP_CALL Notepad_AdpVIF(const double* X, int n, int p, double* out) { return vif(X, n, p, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpDurbinWatson(const double* resid, int n, double* out) { if (!resid || !out) return ERR_NULL; if (n < 2) return ERR_SIZE; if (!allFinite(resid, n)) return ERR_NAN; *out = durbinWatson(resid, n); return OK; }
NOTEPAD_API int ADP_CALL Notepad_AdpCovMatrix(const double* X, int n, int p, double* out) { return covMatrix(X, n, p, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpCorrMatrix(const double* X, int n, int p, double* out) { return corrMatrix(X, n, p, out); }

// ============================================================================
//  7. 분산분석
// ============================================================================
// out[12]: F,df1,df2,p,SSB,SSW,SST,MSB,MSW,eta2,omega2,k   groupMeans/groupN 길이 k (nullptr 허용)
NOTEPAD_API int ADP_CALL Notepad_AdpOneWayANOVA(const double* v, const int* groups, int n, double* out, double* groupMeans, double* groupN) { return oneWayANOVA(v, groups, n, out, groupMeans, groupN); }
NOTEPAD_API int ADP_CALL Notepad_AdpWelchANOVA(const double* v, const int* groups, int n, double* out) { return welchANOVA(v, groups, n, out); }   // [F,df1,df2,p]
NOTEPAD_API int ADP_CALL Notepad_AdpTwoWayANOVA(const double* v, const int* a, const int* b, int n, double* out) { return twoWayANOVA(v, a, b, n, out); }   // out[20]
NOTEPAD_API int ADP_CALL Notepad_AdpRepeatedANOVA(const double* data, int s, int k, double* out) { return repeatedANOVA(data, s, k, out); }   // out[8]
// pairsOut: cap 쌍 × 6 [i,j,diff,lo,hi,p].  nPairs 에 쌍 수.
NOTEPAD_API int ADP_CALL Notepad_AdpTukeyHSD(const double* v, const int* groups, int n, double conf, double* pairsOut, int cap, int* nPairs) { if (!nPairs) return ERR_NULL; return tukeyHSD(v, groups, n, conf, pairsOut, cap, *nPairs); }
NOTEPAD_API int ADP_CALL Notepad_AdpBonferroniPairs(const double* v, const int* groups, int n, double* pairsOut, int cap, int* nPairs) { if (!nPairs) return ERR_NULL; return bonferroniPairs(v, groups, n, pairsOut, cap, *nPairs); }   // [i,j,diff,t,pRaw,pAdj]
NOTEPAD_API int ADP_CALL Notepad_AdpGamesHowell(const double* v, const int* groups, int n, double conf, double* pairsOut, int cap, int* nPairs) { if (!nPairs) return ERR_NULL; return gamesHowell(v, groups, n, conf, pairsOut, cap, *nPairs); }

// ============================================================================
//  8. 전처리
// ============================================================================
NOTEPAD_API int ADP_CALL Notepad_AdpOutlierIQR(const double* x, int n, double k, int* flags, double* fences) { return outlierIQR(x, n, k, flags, fences); }   // 반환: 이상치 수(>=0) / 음수 오류
NOTEPAD_API int ADP_CALL Notepad_AdpOutlierZ(const double* x, int n, double thr, int* flags) { return outlierZ(x, n, thr, flags); }
NOTEPAD_API int ADP_CALL Notepad_AdpWinsorize(const double* x, int n, double lowP, double highP, double* out) { return winsorize(x, n, lowP, highP, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpImpute(const double* x, const int* miss, int n, int method, double* out) { return impute(x, miss, n, method, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpBoxCox(const double* x, int n, double lambda, double* out) { return boxCox(x, n, lambda, out); }
NOTEPAD_API int ADP_CALL Notepad_AdpMissingStats(const int* miss, int n, double* out) { return missingStats(miss, n, out); }

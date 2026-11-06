// ============================================================================
//  NOTEPAD.dll  -  notepad_adp.h
//  [NOTEPAD] 세트 AL: ADP 통계/데이터분석 코어 (내부 API, 순수 C++17, 의존성 0)
//
//  설계 원칙
//   * 외부 라이브러리 없음. <cmath>/<vector>/<algorithm> 만 사용.
//   * 모든 배열 입력은 (const double* x, int n) — DLL 경계에서 STL 을 넘기지 않는다.
//   * 결과는 호출자가 준비한 double 배열/구조체에 기록하고, int 상태코드를 돌려준다.
//   * 통계적으로 의미 없는 상황(n 부족, 분산 0, NaN)은 오류코드로 알린다. 억지값 금지.
//   * 전역 상태 없음(thread-safe). 난수는 호출자가 seed 를 준다(seed 0 = 시간 기반).
//   * 수치안정: Welford 온라인 분산, 보정된 이단계 합, Lentz 연분수, 로그감마 사용.
// ============================================================================
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace adp {

// ---- 상태코드 -------------------------------------------------------------
enum Status {
    OK            =  0,
    ERR_NULL      = -1,   // 필수 포인터가 null
    ERR_SIZE      = -2,   // 표본 수 부족 / 배열 길이 불일치
    ERR_NAN       = -3,   // 입력에 NaN / Inf 포함
    ERR_DEGENERATE= -4,   // 분산 0, 특이행렬 등 계산 불가
    ERR_PARAM     = -5,   // 파라미터 범위 오류 (p∉(0,1), df<=0 ...)
    ERR_CONVERGE  = -6,   // 반복 계산 미수렴
    ERR_CAPACITY  = -7    // 출력 버퍼 용량 부족
};
const char* statusText(int code);

// ---- 입력 검증 헬퍼 -------------------------------------------------------
bool allFinite(const double* x, int n);

// ============================================================================
//  1. 기술통계
// ============================================================================
struct Describe {
    double n, sum, mean, var, sd, se, min, q1, median, q3, max, range, iqr, mad, skew, kurt;
    // var/sd/se 는 표본(n-1) 기준. skew = G1(표본 보정), kurt = 초과첨도(표본 보정, 정규=0)
};
int    describe(const double* x, int n, Describe& d);

double sum(const double* x, int n);                    // Kahan-Neumaier 보정합
double mean(const double* x, int n);
int    weightedMean(const double* x, const double* w, int n, double& out);
int    variance(const double* x, int n, int ddof, double& out);   // ddof=1 표본, 0 모집단
int    stddev(const double* x, int n, int ddof, double& out);
int    quantile(const double* x, int n, double p, int method, double& out);
//   method 0 = 선형보간(R type7, Excel PERCENTILE.INC)  1 = 최근접순위  2 = 가중평균(R type6, Minitab)
int    mode(const double* x, int n, double& out, int& count);   // 최빈값(동률이면 작은 값)
int    mad(const double* x, int n, double& out);                // 중앙값 절대편차(중앙값 기준, 스케일 보정 없음)
int    skewness(const double* x, int n, double& out);           // 표본 왜도 G1
int    kurtosis(const double* x, int n, double& out);           // 표본 초과첨도 G2
int    covariance(const double* x, const double* y, int n, int ddof, double& out);
int    pearson(const double* x, const double* y, int n, double& r);

// ============================================================================
//  2. 배열 처리
// ============================================================================
void   sortCopy(const double* x, int n, double* out, bool desc);
void   argsort(const double* x, int n, int* idx, bool desc);       // 안정 정렬 인덱스(0-기반)
int    unique(const double* x, int n, double* vals, double* counts, int cap, int& k); // 정렬된 고유값+빈도
void   cumsum(const double* x, int n, double* out);
int    diff(const double* x, int n, int lag, double* out);          // out 길이 n-lag
int    standardize(const double* x, int n, double* out);            // z = (x-mean)/sd(n-1)
int    minmaxScale(const double* x, int n, double lo, double hi, double* out);
void   rankAverage(const double* x, int n, double* rank);           // 동률=평균순위(1-기반)
int    histogram(const double* x, int n, int bins, double lo, double hi, double* edges, double* counts);
//   lo>=hi 이면 min/max 자동. edges 길이 bins+1, counts 길이 bins.

// ============================================================================
//  3. 특수함수 / 확률분포
// ============================================================================
double lgammaFn(double x);
double betaInc(double a, double b, double x);           // 정규화 불완전베타 I_x(a,b)
double gammaIncP(double a, double x);                   // 정규화 하부 불완전감마 P(a,x)
double gammaIncQ(double a, double x);                   // Q = 1-P
double normPdf(double z);
double normCdf(double z);
double normQuantile(double p);
double tCdf(double t, double df);
double tSf(double t, double df);                          // P(T>t) 상단꼬리(작은 p 정밀)
double tQuantile(double p, double df);
double chi2Cdf(double x, double df);
double chi2Sf(double x, double df);
double chi2Quantile(double p, double df);
double fCdf(double x, double d1, double d2);
double fSf(double x, double d1, double d2);
double fQuantile(double p, double d1, double d2);
double tukeyCdf(double q, double k, double df);         // 스튜던트화 범위 분포 P(Q<=q)
double tukeyQuantile(double p, double k, double df);

enum Dist {
    D_NORMAL=0, D_T=1, D_CHISQ=2, D_F=3, D_UNIFORM=4, D_EXPONENTIAL=5, D_GAMMA=6, D_BETA=7, D_LOGNORMAL=8,
    D_BERNOULLI=10, D_BINOMIAL=11, D_POISSON=12, D_GEOMETRIC=13, D_HYPERGEOMETRIC=14
};
// func: 0=PDF/PMF  1=CDF  2=Quantile(역CDF)  3=평균  4=분산
// 파라미터: NORMAL(mu,sigma) T(df) CHISQ(df) F(d1,d2) UNIFORM(a,b) EXPONENTIAL(lambda) GAMMA(shape,scale)
//           BETA(a,b) LOGNORMAL(mu,sigma) BERNOULLI(p) BINOMIAL(n,p) POISSON(lambda) GEOMETRIC(p: 실패횟수 0,1,..)
//           HYPERGEOMETRIC(N,K,n)
int    dist(int distId, int func, double x, double p1, double p2, double p3, double& out);

// 난수: seed 0 이면 시간 기반. 내부 xoshiro256** (전역 상태 없음).
int    random(int distId, double p1, double p2, double p3, uint64_t seed, int count, double* out);

// ============================================================================
//  4. 추정 (신뢰구간 / 표본크기)
// ============================================================================
// out: [mean, lo, hi, se, df]
int meanCI(const double* x, int n, double conf, double* out);
// method 0=Wald 1=Wilson 2=Agresti-Coull.  out: [phat, lo, hi, se]
int proportionCI(int k, int n, double conf, int method, double* out);
// out: [var, lo, hi, sd, sdLo, sdHi]
int varianceCI(const double* x, int n, double conf, double* out);
// equalVar 1=합동분산 0=Welch.  out: [diff, lo, hi, se, df]
int diffMeansCI(const double* x, int nx, const double* y, int ny, double conf, int equalVar, double* out);
// out: [diff, lo, hi, se]
int diffPropsCI(int k1, int n1, int k2, int n2, double conf, double* out);
int sampleSizeMean(double sigma, double marginE, double conf, double& n);
int sampleSizeProportion(double p, double marginE, double conf, double& n);
// 두 집단 평균차 검정력 기반 (각 집단 크기). 양측.
int sampleSizeTwoGroups(double delta, double sigma, double alpha, double power, double& nPerGroup);

// ============================================================================
//  5. 가설검정   alt: 0=양측(!=)  1=우측(>)  -1=좌측(<)
// ============================================================================
// out: [stat, df, p, ciLo, ciHi, effect, est, se]
int tTestOne(const double* x, int n, double mu0, int alt, double conf, double* out);
int tTestTwo(const double* x, int nx, const double* y, int ny, int equalVar, int alt, double conf, double* out);
int tTestPaired(const double* x, const double* y, int n, int alt, double conf, double* out);
// out: [chi2, df, p, ciLo, ciHi, sampleVar]
int varTestChi(const double* x, int n, double sigma0Sq, int alt, double conf, double* out);
// out: [F, df1, df2, p, ratio, ciLo, ciHi]
int varTestF(const double* x, int nx, const double* y, int ny, int alt, double conf, double* out);
// out: [z, p, phat, se, ciLo, ciHi]   correct: 연속성 보정 1/0
int propTestOne(int k, int n, double p0, int alt, int correct, double conf, double* out);
// out: [z, p, p1, p2, diff, ciLo, ciHi]
int propTestTwo(int k1, int n1, int k2, int n2, int alt, int correct, double conf, double* out);
// out: [chi2, df, p]   expected==nullptr 이면 균등 기대.
int chiGoodness(const double* obs, const double* expected, int k, double* out);
// table: r×c 행우선.  out: [chi2, df, p, cramerV, minExpected]  expectedOut(선택, r*c)
int chiIndependence(const double* table, int r, int c, int correct, double* out, double* expectedOut);
// out: [U, z, p, U1, U2, rankBiserial]
int mannWhitney(const double* x, int nx, const double* y, int ny, int alt, double* out);
// out: [W(양의순위합), z, p, nEff, Wneg]
int wilcoxonSR(const double* x, const double* y, int n, int alt, double* out);
// groups: 각 원소의 집단 번호(0..k-1).  out: [H, df, p, k, epsilonSq]
int kruskalWallis(const double* v, const int* groups, int n, double* out);
// data: b행(블록) × k열(처리) 행우선.  out: [chi2, df, p, kendallW]
int friedman(const double* data, int b, int k, double* out);
// Shapiro-Wilk (Royston 1995, 3<=n<=5000).  out: [W, p]
int shapiroWilk(const double* x, int n, double* out);
// Levene(center 0=평균 / 1=중앙값=Brown-Forsythe).  out: [F, df1, df2, p]
int levene(const double* v, const int* groups, int n, int center, double* out);
// Jarque-Bera.  out: [JB, p, skew, kurt]
int jarqueBera(const double* x, int n, double* out);

// ============================================================================
//  6. 상관 / 회귀
// ============================================================================
// out: [r, t, df, p, ciLo, ciHi]  (Fisher z 신뢰구간)
int pearsonTest(const double* x, const double* y, int n, double conf, double* out);
// out: [rho, t, df, p]
int spearman(const double* x, const double* y, int n, double* out);
// out: [tau_b, z, p, concordant, discordant]
int kendall(const double* x, const double* y, int n, double* out);

// 단순회귀 out(18): [b0,b1,se0,se1,t0,t1,p0,p1,R2,adjR2,RMSE,MAE,F,pF,dfRes,DW,sigma,SSE]
int simpleRegression(const double* x, const double* y, int n, double* out,
                     double* fitted, double* resid);
// 다중회귀. X: n×p 행우선(절편 없이), 절편 자동 추가.
//   coef/se/t/p: 길이 p+1 (0=절편).
//   stats(12): [R2,adjR2,RMSE(√(SSE/n)),MAE,F,pF,dfModel,dfRes,DW,AIC,sigma(√(SSE/dfRes)),SSE]
//   fitted/resid: 길이 n (nullptr 허용).
int multipleRegression(const double* X, const double* y, int n, int p,
                       double* coef, double* se, double* t, double* pv, double* stats,
                       double* fitted, double* resid);
// 다항회귀(차수 deg): x -> [x, x^2, ..., x^deg] 후 multipleRegression.
int polynomialRegression(const double* x, const double* y, int n, int deg,
                         double* coef, double* se, double* t, double* pv, double* stats,
                         double* fitted, double* resid);
// 회귀 진단: leverage(hat 대각), 표준화잔차, Cook 거리 (각 길이 n)
int regressionDiag(const double* X, const double* y, int n, int p,
                   double* leverage, double* stdResid, double* cookD);
// VIF: 각 설명변수(길이 p). p>=2 필요.
int vif(const double* X, int n, int p, double* out);
double durbinWatson(const double* resid, int n);
// 공분산/상관 행렬 (p×p 행우선). X: n×p
int covMatrix(const double* X, int n, int p, double* out);
int corrMatrix(const double* X, int n, int p, double* out);

// ============================================================================
//  7. 분산분석
// ============================================================================
// groups: 0..k-1.  out(12): [F, df1, df2, p, SSB, SSW, SST, MSB, MSW, eta2, omega2, k]
//   groupMeans(선택, 길이 k), groupN(선택, 길이 k)
int oneWayANOVA(const double* v, const int* groups, int n, double* out,
                double* groupMeans, double* groupN);
// Welch ANOVA out: [F, df1, df2, p]
int welchANOVA(const double* v, const int* groups, int n, double* out);
// 이원 ANOVA (균형 설계, 셀 크기 동일 필요). v 와 a(0..A-1), b(0..B-1)
//   out(20): [F_A,df_A,p_A, F_B,df_B,p_B, F_AB,df_AB,p_AB, dfE, SS_A,SS_B,SS_AB,SS_E,SS_T, MSE, eta2_A, eta2_B, eta2_AB, cellN]
int twoWayANOVA(const double* v, const int* a, const int* b, int n, double* out);
// 반복측정 ANOVA (피험자 s행 × 처리 k열 행우선, 결측 없음)
//   out(8): [F, df1, df2, p, SS_treat, SS_error, SS_subj, partialEta2]
int repeatedANOVA(const double* data, int s, int k, double* out);
// Tukey HSD 사후검정. pairsOut: 각 쌍 6개 [i, j, diff, lo, hi, p] (행우선), 최대 cap 쌍. 반환 nPairs via 참조.
int tukeyHSD(const double* v, const int* groups, int n, double conf, double* pairsOut, int cap, int& nPairs);
// Bonferroni 보정 쌍별 t (합동분산). 동일 레이아웃 [i, j, diff, t, pRaw, pAdj]
int bonferroniPairs(const double* v, const int* groups, int n, double* pairsOut, int cap, int& nPairs);
// Games-Howell (등분산 가정 없음). [i, j, diff, lo, hi, p]
int gamesHowell(const double* v, const int* groups, int n, double conf, double* pairsOut, int cap, int& nPairs);

// ============================================================================
//  8. 전처리
// ============================================================================
// 이상치: flags[i]=1 이상치. 반환 개수(음수=오류). IQR: k 배수(1.5). Z: 임계(3.0)
int outlierIQR(const double* x, int n, double k, int* flags, double* fences);   // fences[2] lo,hi
int outlierZ(const double* x, int n, double thr, int* flags);
int winsorize(const double* x, int n, double lowP, double highP, double* out);
// 결측 대체: miss[i]!=0 이면 결측. method 0=평균 1=중앙값 2=최빈값 3=직전값(ffill) 4=선형보간
int impute(const double* x, const int* miss, int n, int method, double* out);
// Box-Cox 변환(lambda 고정). x>0 필요.
int boxCox(const double* x, int n, double lambda, double* out);
// 결측 마스크 통계: [총수, 결측수, 결측비율]
int missingStats(const int* miss, int n, double* out);

// ============================================================================
//  9. 내부 유틸(선형대수)
// ============================================================================
// 최소제곱 (Householder QR). A: n×p 행우선, b: n. coef p. XtXinv: p×p(선택). 반환 상태.
int leastSquares(const double* A, const double* b, int n, int p, double* coef, double* XtXinv, double* fitted);
bool invertSPD(std::vector<double>& M, int p);     // 대칭양정치 역행렬(Cholesky) in-place

} // namespace adp

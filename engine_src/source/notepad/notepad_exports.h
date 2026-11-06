// ============================================================================
//  NOTEPAD.dll  -  notepad_exports.h
//  [NOTEPAD] AHK v1 (64비트) DllCall 인터페이스. 모든 export 는 Notepad_ 접두.
// ============================================================================
//
// [문자열 규약]
//   * 모든 문자열 입력/출력은 UTF-16LE (wchar_t*, AHK 기본 "Str"/"WStr").
//   * 바이너리(해시/암호문/솔트/논스)는 항상 Base64 문자열로 주고받는다.
//   * 평문(plaintext)은 사람이 읽는 텍스트라 가정하고 내부에서 UTF-8로 변환해
//     암호화한다. 복호화 시 UTF-8 -> UTF-16으로 되돌린다.
//
// [반환 규약 - 출력 버퍼 패턴]
//   각 함수는 결과를 out 버퍼(wchar_t*, 호출자가 할당)에 '\0' 종료 문자열로
//   기록하고, 반환값으로 '결과 문자열의 문자 수(널 제외)'를 돌려준다.
//     * out == nullptr 또는 outCch == 0 : 아무것도 쓰지 않고, 필요한 문자 수
//       (널 제외)를 반환한다 -> 2단계 호출(길이 조회 후 버퍼 확보)에 사용.
//     * 버퍼가 부족하면 (필요문자수) 를 음수로 반환한다. (예: -37)
//     * 오류(암호화 실패, 잘못된 Base64, tag 불일치 등)는 0 을 반환한다.
//   실무에서는 넉넉한 버퍼(예: 평문의 2배 + 64)를 한 번에 넘기는 걸 권장.
#pragma once
#include <windows.h>

#ifdef NOTEPAD_EXPORTS
#define NOTEPAD_API extern "C" __declspec(dllexport)
#else
#define NOTEPAD_API extern "C" __declspec(dllimport)
#endif

// SHA-256. text(UTF-16) -> hex 소문자 64자 out. 반환: 문자 수(64).
NOTEPAD_API int __stdcall Notepad_SHA256(const wchar_t* text, wchar_t* out, int outCch);

// 안전한 난수 n바이트를 Base64로. 반환: 문자 수.
NOTEPAD_API int __stdcall Notepad_RandomBase64(int nBytes, wchar_t* out, int outCch);

// AES-256-GCM 암호화(비밀번호 기반, 자기완결 토큰).
//   plaintext, password: UTF-16.
//   out: "v1:<saltB64>:<nonceB64>:<tagB64>:<cipherB64>" 형태 토큰 문자열.
//   내부에서 salt(16B)/nonce(12B) 자동 생성, PBKDF2로 32B 키 유도.
// 반환: 토큰 문자 수 / 실패 0.
NOTEPAD_API int __stdcall Notepad_EncryptPassword(const wchar_t* plaintext,
                                           const wchar_t* password,
                                           int iterations,
                                           wchar_t* out, int outCch);

// 위 토큰을 password로 복호화. 성공 시 평문(UTF-16)을 out에. tag 불일치/틀린
// 비밀번호면 0 반환. 반환: 평문 문자 수 / 실패 0.
NOTEPAD_API int __stdcall Notepad_DecryptPassword(const wchar_t* token,
                                           const wchar_t* password,
                                           wchar_t* out, int outCch);

// 비밀번호 저장/검증용 해시 토큰 생성: "pbkdf2$<iter>$<saltB64>$<hashB64>".
NOTEPAD_API int __stdcall Notepad_HashPassword(const wchar_t* password,
                                        int iterations,
                                        wchar_t* out, int outCch);

// Notepad_HashPassword 토큰과 password 대조. 일치 1 / 불일치 0.
NOTEPAD_API int __stdcall Notepad_VerifyPassword(const wchar_t* password,
                                          const wchar_t* token);

// [NOTEPAD] 텍스트(UTF-16) 압축 -> Base64 문자열. level 0~9(-1이면 기본6).
//   내부에서 UTF-8로 변환 후 DEFLATE. 반환: Base64 문자 수 / 실패 0.
NOTEPAD_API int __stdcall Notepad_Compress(const wchar_t* text, int level,
                                           wchar_t* out, int outCch);

// [NOTEPAD] Notepad_Compress 결과(Base64)를 원래 텍스트(UTF-16)로 복원.
//   반환: 문자 수 / 실패(손상 등) 0.
NOTEPAD_API int __stdcall Notepad_Decompress(const wchar_t* b64,
                                             wchar_t* out, int outCch);

// ---- [NOTEPAD] 파일 해시 / 파일 암호화 -------------------------------------

// 파일의 SHA-256(hex 64자). path: UTF-16 경로. 파일 없으면 0.
NOTEPAD_API int __stdcall Notepad_SHA256File(const wchar_t* path,
                                             wchar_t* out, int outCch);

// 파일을 비밀번호로 암호화해 outPath 에 저장. 성공 1 / 실패 0.
//   (내부: 파일 바이트 -> AES-256-GCM, salt/nonce/tag 포함 자체 포맷)
NOTEPAD_API int __stdcall Notepad_EncryptFile(const wchar_t* inPath,
                                              const wchar_t* outPath,
                                              const wchar_t* password,
                                              int iterations);

// 암호화된 파일을 복호화해 outPath 에 저장. 성공 1 / 실패(틀린pw/변조) 0.
NOTEPAD_API int __stdcall Notepad_DecryptFile(const wchar_t* inPath,
                                              const wchar_t* outPath,
                                              const wchar_t* password);

// ---- [NOTEPAD] URL 인코딩 --------------------------------------------------

// URL 퍼센트 인코딩 (RFC3986 비예약문자 제외). 반환: 문자 수 / 실패 0.
NOTEPAD_API int __stdcall Notepad_UrlEncode(const wchar_t* text,
                                            wchar_t* out, int outCch);

// URL 퍼센트 디코딩. 반환: 문자 수 / 실패 0.
NOTEPAD_API int __stdcall Notepad_UrlDecode(const wchar_t* text,
                                            wchar_t* out, int outCch);

// ---- [NOTEPAD] JSON --------------------------------------------------------

// JSON 유효성 검사. 유효 1 / 무효 0.
NOTEPAD_API int __stdcall Notepad_JsonValid(const wchar_t* json);

// JSON에서 경로로 값 추출. path 예: "user.name", "items.0.id" (점/인덱스).
//   문자열/숫자/불리언은 문자열로 반환. 객체/배열은 그 JSON 부분을 반환.
//   경로 없음/오류 시 0.
NOTEPAD_API int __stdcall Notepad_JsonGet(const wchar_t* json,
                                          const wchar_t* path,
                                          wchar_t* out, int outCch);

// JSON을 보기 좋게 정렬(pretty print). 실패 0.
NOTEPAD_API int __stdcall Notepad_JsonPretty(const wchar_t* json,
                                             wchar_t* out, int outCch);

// ============================================================================
//  [세트 A] 해시/인코딩 확장
// ============================================================================
NOTEPAD_API int __stdcall Notepad_SHA512(const wchar_t* text, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_SHA384(const wchar_t* text, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_SHA1(const wchar_t* text, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_MD5(const wchar_t* text, wchar_t* out, int outCch);
// HMAC (text, key) -> hex. SHA-256 / SHA-512.
NOTEPAD_API int __stdcall Notepad_HMAC(const wchar_t* text, const wchar_t* key,
                                       wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_HMAC512(const wchar_t* text, const wchar_t* key,
                                          wchar_t* out, int outCch);
// Base64 / Base64URL (UTF-8 텍스트 기준).
NOTEPAD_API int __stdcall Notepad_Base64Encode(const wchar_t* text, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_Base64Decode(const wchar_t* b64, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_Base64UrlEncode(const wchar_t* text, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_Base64UrlDecode(const wchar_t* text, wchar_t* out, int outCch);
// Hex (UTF-8 텍스트 <-> hex 소문자).
NOTEPAD_API int __stdcall Notepad_HexEncode(const wchar_t* text, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_HexDecode(const wchar_t* hex, wchar_t* out, int outCch);
// CRC32 -> hex 8자. UUID v4 문자열.
NOTEPAD_API int __stdcall Notepad_CRC32(const wchar_t* text, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_UUID(wchar_t* out, int outCch);

// ============================================================================
//  [세트 B] 압축/아카이브
// ============================================================================
// 대용량 파일 압축/해제 (스트리밍). 성공 1 / 실패 0.
NOTEPAD_API int __stdcall Notepad_CompressFile(const wchar_t* inPath,
                                               const wchar_t* outPath, int level);
NOTEPAD_API int __stdcall Notepad_DecompressFile(const wchar_t* inPath,
                                                 const wchar_t* outPath);
// ZIP: 파일 추가(없으면 생성)/목록/전체 추출/단일 추출.
NOTEPAD_API int __stdcall Notepad_ZipAddFile(const wchar_t* zipPath, const wchar_t* srcPath,
                                             const wchar_t* nameInZip, int level);
NOTEPAD_API int __stdcall Notepad_ZipList(const wchar_t* zipPath, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_ZipExtractAll(const wchar_t* zipPath, const wchar_t* destDir);
NOTEPAD_API int __stdcall Notepad_ZipExtractFile(const wchar_t* zipPath,
                                                 const wchar_t* nameInZip,
                                                 const wchar_t* outPath);

// ============================================================================
//  [세트 C] HTTP (WinHTTP)
// ============================================================================
// GET/POST: 응답 본문(UTF-8->UTF-16). 실패 0. 상태코드는 Notepad_HttpLastStatus.
NOTEPAD_API int __stdcall Notepad_HttpGet(const wchar_t* url, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_HttpPost(const wchar_t* url, const wchar_t* body,
                                           const wchar_t* contentType,
                                           wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_HttpLastStatus();          // 직전 요청 HTTP 상태코드
NOTEPAD_API int __stdcall Notepad_HttpDownload(const wchar_t* url, const wchar_t* outPath);

// ============================================================================
//  [세트 D] 데이터/텍스트
// ============================================================================
// CSV (RFC4180). row/col 0-기반. 값/없으면 0.
NOTEPAD_API int __stdcall Notepad_CsvGet(const wchar_t* csv, int row, int col,
                                         wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_CsvRowCount(const wchar_t* csv);
NOTEPAD_API int __stdcall Notepad_CsvColCount(const wchar_t* csv, int row);
// HTML 엔티티 인/디코딩.
NOTEPAD_API int __stdcall Notepad_HtmlEncode(const wchar_t* text, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_HtmlDecode(const wchar_t* text, wchar_t* out, int outCch);
// URL 쿼리스트링에서 key 값 추출(디코딩됨).
NOTEPAD_API int __stdcall Notepad_QueryGet(const wchar_t* query, const wchar_t* key,
                                           wchar_t* out, int outCch);
// 시간: 현재 Unix초 / Unix초->ISO8601(UTC) / ISO->Unix초. 모두 문자열.
NOTEPAD_API int __stdcall Notepad_NowUnix(wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_UnixToIso(const wchar_t* unixStr, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_IsoToUnix(const wchar_t* iso, wchar_t* out, int outCch);

// ============================================================================
//  [세트 E] 보호
// ============================================================================
// 파일 SHA-256 이 expectedHex 와 일치하면 1(상수시간 비교), 아니면 0. 무결성 검사용.
NOTEPAD_API int __stdcall Notepad_VerifyFileHash(const wchar_t* path,
                                                 const wchar_t* expectedHex);

// ============================================================================
//  [세트 F] 유사도 분석
// ============================================================================
NOTEPAD_API int __stdcall Notepad_Levenshtein(const wchar_t* a, const wchar_t* b);
NOTEPAD_API int __stdcall Notepad_SimilarityRatio(const wchar_t* a, const wchar_t* b); // 0~100
NOTEPAD_API int __stdcall Notepad_JaroWinkler(const wchar_t* a, const wchar_t* b);     // 0~100
NOTEPAD_API int __stdcall Notepad_FuzzyBestMatch(const wchar_t* needle,
                                                 const wchar_t* listNl,
                                                 wchar_t* out, int outCch);

// ============================================================================
//  [세트 G] 화면 캡처 / 이미지서치 / 픽셀서치
// ============================================================================
NOTEPAD_API int __stdcall Notepad_GetPixelColor(int x, int y);                 // 0xRRGGBB / -1
NOTEPAD_API int __stdcall Notepad_CaptureRegion(int x, int y, int w, int h,
                                                const wchar_t* outPath);        // 1/0
NOTEPAD_API int __stdcall Notepad_ImageSearch(const wchar_t* imgPath,
                                              int x1, int y1, int x2, int y2,
                                              int tolerance, wchar_t* out, int outCch); // "x,y"
NOTEPAD_API int __stdcall Notepad_PixelSearch(int x1, int y1, int x2, int y2,
                                              int colorRgb, int tolerance,
                                              wchar_t* out, int outCch);        // "x,y"

// ============================================================================
//  [세트 H] 내장 OCR (Windows.Media.Ocr)
// ============================================================================
NOTEPAD_API int __stdcall Notepad_OcrImage(const wchar_t* imgPath, const wchar_t* lang,
                                           wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_OcrScreen(int x, int y, int w, int h,
                                            const wchar_t* lang, wchar_t* out, int outCch);
NOTEPAD_API int __stdcall Notepad_OcrLanguages(wchar_t* out, int outCch);

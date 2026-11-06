// ============================================================================
//  NOTEPAD.dll  -  notepad_crypto.h
//  [NOTEPAD] 내부 암호화/압축/아카이브/네트워크 핵심 로직 (CNG + miniz + WinHTTP).
//  이 헤더의 함수들은 순수 std::string(바이트 버퍼) 단위로 동작한다. export 아님.
//  UTF 변환 / Base64 / AHK 인터페이스는 notepad_exports.cpp가 담당한다.
// ============================================================================
#pragma once
#include <string>
#include <cstdint>
#include <vector>

// [NOTEPAD] 모든 내부 핵심 로직은 notepad 네임스페이스에 둔다.
namespace notepad {

// 반환값: 성공 true / 실패 false. 실패 시 out은 비워짐.

// ---- 해시 (일반화) ---------------------------------------------------------
// [NOTEPAD] 임의 CNG 해시 알고리즘 다이제스트. algId 예: BCRYPT_SHA512_ALGORITHM.
//   out: 원시 다이제스트 바이트(hex 아님).  hmac=true 면 HMAC(key 필요).
bool hashDigest(const wchar_t* algId, const std::string& data, std::string& out);
bool hmacDigest(const wchar_t* algId, const std::string& key,
                const std::string& data, std::string& out);

// SHA-256 (기존 호환). 내부적으로 hashDigest 재사용 -> 중복 코드 제거.
bool sha256(const std::string& data, std::string& out);

// PBKDF2-HMAC-SHA256: password/salt 바이트 -> dkLen 바이트 키
bool pbkdf2(const std::string& password, const std::string& salt,
            unsigned int iterations, unsigned int dkLen, std::string& out);

// 안전한 난수 바이트 (salt/nonce/UUID 생성용)
bool randomBytes(unsigned int n, std::string& out);

// ---- [NOTEPAD] 세트 M: ECDSA P-256 전자서명 (CNG) --------------------------
// 키 쌍 생성. privBlob/pubBlob 은 CNG BCRYPT_ECCPRIVATE/PUBLIC BLOB 원시 바이트.
//   (exports 계층에서 Base64 로 감싸 AHK 로 전달)
bool eccGenerateKeyPair(std::string& privBlob, std::string& pubBlob);
// privBlob 로 data 서명 -> sigOut(원시 서명 바이트). 성공 true.
bool eccSign(const std::string& privBlob, const std::string& data, std::string& sigOut);
// pubBlob 로 서명 검증. 유효하면 true.
bool eccVerify(const std::string& pubBlob, const std::string& data, const std::string& sig);

// ---- [NOTEPAD] 세트 P: DPAPI (Windows 사용자 계정 기반 자동 암호화) ---------
// 비밀번호 없이 현재 Windows 사용자/머신에 묶어 암/복호화. optionalEntropy 로
// 추가 비밀(선택, 빈문자 가능)을 섞을 수 있다. machineScope=true 면 머신 전체.
//   반환: 성공 true. out 은 원시 바이트(exports 계층에서 Base64 로 감쌈).
bool dpapiProtect(const std::string& data, const std::string& optionalEntropy,
                  bool machineScope, std::string& out);
bool dpapiUnprotect(const std::string& blob, const std::string& optionalEntropy,
                    std::string& out);

// ---- AES-256-GCM -----------------------------------------------------------
bool aesGcmEncrypt(const std::string& key, const std::string& nonce,
                   const std::string& plaintext, std::string& out,
                   std::string& tag);
bool aesGcmDecrypt(const std::string& key, const std::string& nonce,
                   const std::string& ciphertext, const std::string& tag,
                   std::string& out);

// ---- 압축 (텍스트/인메모리, 기존) ------------------------------------------
// 형식: [원본크기 4B LE] + DEFLATE(zlib).
bool zipDeflate(const std::string& data, std::string& out, int level = 6);
bool zipInflate(const std::string& data, std::string& out);

// [NOTEPAD] CRC32 체크섬 (miniz). 이름은 miniz zlib 매크로(crc32)와 충돌 방지 위해 crc32sum.
uint32_t crc32sum(const std::string& data);

// ---- 파일 I/O / 파일 해시 --------------------------------------------------
bool sha256File(const std::string& pathUtf8, std::string& out);
// [NOTEPAD] 임의 알고리즘 파일 해시(스트리밍, 대용량 안전).
bool hashFile(const wchar_t* algId, const std::string& pathUtf8, std::string& out);
bool readFileBytes(const std::string& pathUtf8, std::string& out);
bool writeFileBytes(const std::string& pathUtf8, const std::string& data);

// ---- [NOTEPAD] 대용량 파일 압축/해제 (스트리밍) ----------------------------
// 형식: "NPZ1"(4B) + 원본크기(8B LE) + raw DEFLATE 스트림.
//   64KB 청크로 읽어 스트리밍 압축 -> 대용량 파일도 메모리 안전.
bool compressFileStream(const std::string& inPathUtf8, const std::string& outPathUtf8,
                        int level);
bool decompressFileStream(const std::string& inPathUtf8, const std::string& outPathUtf8);

// ---- [NOTEPAD] ZIP 아카이브 (miniz archive API) ----------------------------
// zip 파일에 디스크 파일 하나를 추가(없으면 새로 생성, 있으면 append).
bool zipAddFile(const std::string& zipPathUtf8, const std::string& srcPathUtf8,
                const std::string& nameInZip, int level);
// zip 안 항목 이름들을 개행(\n) 구분 문자열로.
bool zipList(const std::string& zipPathUtf8, std::string& namesOut);
// zip 전체를 destDir 아래로 추출.
bool zipExtractAll(const std::string& zipPathUtf8, const std::string& destDirUtf8);
// zip 안 특정 항목 하나를 outPath 로 추출.
bool zipExtractFile(const std::string& zipPathUtf8, const std::string& nameInZip,
                    const std::string& outPathUtf8);

// ---- [NOTEPAD] HTTP (WinHTTP) ----------------------------------------------
// method: "GET"/"POST". body/contentType 는 POST 에서만 사용(GET 은 빈 문자열).
//   respOut: 응답 본문 바이트. statusOut: HTTP 상태코드. 실패 시 false.
bool httpRequest(const std::string& method, const std::string& url,
                 const std::string& body, const std::string& contentType,
                 std::string& respOut, int& statusOut);
// url 을 받아 파일로 저장. 성공 true.
bool httpDownload(const std::string& url, const std::string& outPathUtf8, int& statusOut);

// [NOTEPAD] 세트 J: multipart/form-data 파일 업로드 (POST).
//   filePathUtf8: 업로드할 로컬 파일. fieldName: 폼 필드명(예 "file").
//   extraFields: "key1=val1\nkey2=val2" 형식의 추가 텍스트 필드(옵션, 빈문자 가능).
//   respOut: 서버 응답 본문. statusOut: HTTP 상태코드. 성공 true.
bool httpUploadFile(const std::string& url, const std::string& fieldName,
                    const std::string& filePathUtf8, const std::string& fileNameInForm,
                    const std::string& extraFields,
                    std::string& respOut, int& statusOut);

// ---- [NOTEPAD] 화면 캡처 / 이미지서치 (GDI + GDI+) — notepad_screen.cpp ----
// 화면 좌표 (x,y) 픽셀 색을 0xRRGGBB 로. 성공 true.
bool getPixelColor(int x, int y, uint32_t& rgbOut);
// 화면 영역(좌상 x,y / 크기 w,h)을 PNG 파일로 저장.
bool captureRegionToPng(int x, int y, int w, int h, const std::string& outPngUtf8);
// 화면 영역 [x1,y1]~[x2,y2)(우하 배타) 안에서 needle 이미지(PNG/BMP 등) 찾기.
//   tolerance: 채널당 허용 오차(0=정확). 성공 시 fx,fy = 매치 좌상 화면좌표.
bool imageSearch(const std::string& needlePathUtf8, int x1, int y1, int x2, int y2,
                 int tolerance, int& fx, int& fy);
// 화면 영역에서 rgb(0xRRGGBB) 색(±tolerance) 첫 픽셀 찾기.
bool pixelSearch(int x1, int y1, int x2, int y2, uint32_t rgb, int tolerance,
                 int& fx, int& fy);

// [NOTEPAD] 세트 K: 이미지서치(모두 찾기). 영역 안 needle 의 모든 매치 좌상 좌표.
//   겹치는 매치는 needle 크기만큼 건너뛰어 중복 억제(대략). maxMatches<=0 이면 제한 없음.
//   결과: fxs/fys 에 화면좌표들이 push 된다. 반환: 찾은 개수(0 가능). 실패 시 -1.
int imageSearchAll(const std::string& needlePathUtf8, int x1, int y1, int x2, int y2,
                   int tolerance, int maxMatches,
                   std::vector<int>& fxs, std::vector<int>& fys);

// ---- [NOTEPAD] OCR (Windows.Media.Ocr / WinRT) — notepad_ocr.cpp ----
// 이미지 파일 OCR. langTag 예: "ko","en"(빈 문자열이면 사용자 프로필 언어).
//   textOut: 인식 텍스트(UTF-8). 성공 true.
bool ocrImageFile(const std::string& imgPathUtf8, const std::string& langTag,
                  std::string& textOut);
// 화면 영역 OCR (내부에서 PNG 캡처 후 인식).
bool ocrScreenRegion(int x, int y, int w, int h, const std::string& langTag,
                     std::string& textOut);
// 사용 가능한 OCR 언어 태그 목록(공백 구분, 예 "en-US ko"). 실패 시 false.
bool ocrAvailableLanguages(std::string& langsOut);

} // namespace notepad

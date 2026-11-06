# 변경 이력 (Changelog)

버전 체계: **엔진 버전**(`A_AhkWebVersion`, 원본 AutoHotkey = 1.1.37.02)과
**포크 빌드**(`A_AhkWebBuild`, 날짜 기반 `YYYY.M.N`)를 따로 관리합니다.
새 릴리스마다 `engine_src/source/ahkweb_version.h`의 `AHKWEB_BUILD`를 올리고
여기에 항목을 추가합니다. 날짜·형식은 [Keep a Changelog] 스타일을 느슨하게 따릅니다.

[Keep a Changelog]: https://keepachangelog.com/ko/

## [2026.11.1] - 2026-11-06
엔진: AutoHotkey 1.1.37.02 기반

### Added (추가)
- **메소드 체이닝** — 문자열/배열/객체에 메소드를 이어 쓰기(`"  a,b ".Trim().Split(",").Join("|")`,
  `[3,1,2].Sort("N").Map(fn).Sum()`, `obj.Get("a.b")`). 사용자 정의(키·클래스 메소드·`"".base`)가 항상 우선.
- **JSON** — `JsonParse` / `JsonStringify` 내장(기존 `JsonGet`는 유지).
- **한글 OCR** — 딥러닝 `OcrImageAI` / `OcrScreenAI`(PaddleOCR ONNX) + Windows 내장 `OcrImage` / `OcrScreen`.
- **암호화 툴킷** — SHA/HMAC/CRC, AES-256-GCM 문자열·파일, 비밀번호 해시(PBKDF2), DPAPI,
  ECDSA 서명, 보안 난수(약 30종).
- **툴킷(약 150종)** — HTTP·병렬요청·WebSocket 클라이언트, ZIP·인코딩·휴지통, 이미지서치 전체·
  템플릿 매칭·QR·클립보드 이미지, 문자열 유사도·CSV·시간·수학 등. `Note_X()` → `X()` 규칙으로 노출.
- **EXE 스크립트 암호화** — `AhkWeb2Exe` 컴파일 시 스크립트를 DEFLATE 압축 + 키스트림 XOR(컨테이너 `AHKW1`).
  리소스 뷰어/strings로 평문 노출 안 됨. 엔진이 메모리에서 복원.
- **`A_AhkWebBuild`** — 포크 빌드 버전 변수(이 항목의 버전).
- **AhkWeb2Exe: Ahk2Exe식 아이콘·버전 정보** — `/icon`과 `;@Ahk2Exe-SetMainIcon`이 실제
  `RT_GROUP_ICON`/`RT_ICON`을 교체(탐색기·작업표시줄·트레이 아이콘 반영). 이전엔 RCDATA에만 넣어 효과가 없었음.
  `SetVersion`·`SetDescription`·`SetCopyright`·`SetCompanyName`·`SetProductName`·`Set <속성>, <값>` 등으로
  `VS_VERSIONINFO` 재작성, `AddResource`(아이콘 그룹·비트맵·HTML·RCDATA), `ExeName`, `;@AhkWeb-Base` 지원.
- **AhkWeb2Exe GUI** — 인자 없이 실행하면 Ahk2Exe 같은 창(스크립트/출력/아이콘 미리보기/베이스/버전 정보,
  드래그&드롭, 마지막 아이콘 기억). `.ahkw` 출력 이름 처리, 출력 폴더 자동 생성, GUI 서브시스템(콘솔 깜빡임 없음).
  테스트: `tests\compiler\verify_icon_test.ps1`.

### Changed (변경)
- **식별자 중립화(안티치트 오탐 완화)** — 컴파일 배포 exe의 창 클래스(`AhkwShell`/`AhkwShellGui`/`AhkwWebView`),
  후킹 뮤텍스, 다운로드 User-Agent(`Mozilla/5.0`), 창 제목에서 "AutoHotkey" 문자열 제거.
  컴파일 exe의 버전 정보(제품명 등)는 공백. 개발용 인터프리터는 SciTE4 디버거 호환을 위해
  제품명·DBGp appid에 "AutoHotkey" 유지(F5/F7/컴파일 정상).
- 일부 함수 이름을 v2 스타일로: `StrUpper/StrLower/StrTitle/StrRepeat/StrReverse/StrPadLeft/StrPadRight/StrCount`,
  서명은 `EcdsaKeyPair/EcdsaSign/EcdsaVerify`, `StrEntropy`.
- SciTE4 연동을 `*.ahkw` 전용 문법으로 재구성, 우클릭 "새로 만들기"·탐색기 캐시 갱신 보완.

### Docs
- 도움말에 메소드 체이닝/암호화/OCR/툴킷/컴파일 문서 추가, 라이선스·배포·책임 고지 확장.
- 배포용 문서 추가: `LICENSE`(GPLv2), `SECURITY.md`, `RELEASING.md`, `MODELS.md`, `.gitignore`.
- 번들 모델 라이선스 직접 확인: kor_unsmile(Apache-2.0), KoSimCSE(CC BY-SA 4.0), PP-OCRv5 한글(Apache-2.0).

### Notes
- 이번이 공개 배포 기준 첫 포크 빌드입니다. 이전 내부 작업(WebGui, CDP, 딥러닝 분류/학습)은
  이 빌드에 모두 포함되어 있습니다.

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
- **설치 프로그램** — 시작 메뉴 "AhkWeb2Exe 컴파일러" 바로가기(+ 바탕화면 선택), `.ahkw` 우클릭
  "컴파일 (옵션 창)" 추가, 선택 시 `.ahk` 우클릭에도 "AutoHotkey Web으로 컴파일 (옵션 창)"(연결은 건드리지 않음).
- **파일 포함(단일 EXE)** — `;@AhkWeb-Include <파일|폴더> [, 대상]`으로 확장자 제한 없이 exe에 묶고,
  엔진이 auto-execute 전에 추출(`;@AhkWeb-ExtractTo temp|exe|appdata|localappdata|경로`,
  `ExtractMode newer|always|missing`). 새 내장 변수 **`A_AhkWebFiles`**(미컴파일 시 `A_ScriptDir`).
  임시 폴더는 종료 시 삭제, 비정상 종료로 남은 `AhkWeb_<pid>` 폴더는 다음 실행 때 정리(기존 assets 폴더 누수도 해결).
- **설치 마법사 생성** — 출력 방식 "EXE + 설치 마법사" / `;@AhkWeb-Setup` / `/setup`으로 네이티브 Setup.exe 생성
  (`AhkWebSetup.bin`, 외부 도구 불필요): 환영·사용권 계약·설치 폴더·바로가기·진행·완료 페이지, 제거 프로그램,
  앱 및 기능 등록, 업그레이드 설치, 현재 사용자/모든 사용자(UAC), 무인 설치/제거(`/S`, `/D=`), 한국어/영어 UI.
- **컴파일러 GUI 확장** — 탭(기본·버전 정보·파일 포함·설치 마법사·고급), 최근 스크립트, 스크립트별 설정 저장,
  컴파일 후 실행/폴더 열기, 관리자 권한 실행, 트레이 상태 아이콘(206/207/208), 코드 서명(signtool, 암호 DPAPI 저장),
  백그라운드 컴파일.
- **지시어 추가** — `SuspendIcon`·`PauseIcon`·`PauseSuspendIcon`, `UpdateManifest`/`RequireAdmin`, `PostExec`,
  `Sign`·`SignSubject`·`SignTimestamp`, `Setup*`, `IgnoreBegin/End`, `Keep`.
  테스트: `tests\compiler\verify_bundle_test.ps1` (24개 항목).

### Fixed (수정)
- **컴파일된 exe에서 `#Include`·Lib 함수·`FileInstall`이 동작하지 않던 문제** — Self-contained 베이스는 런타임에
  #Include를 처리하지 않는데 컴파일러가 메인 스크립트만 넣고 있었음. 이제 Ahk2Exe처럼 컴파일 시 모두 합치고
  (`AutoHotkeyWeb.exe /iLib`로 Lib 자동 포함 + 문법 검사), FileInstall 원본을 리소스로 포함.

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

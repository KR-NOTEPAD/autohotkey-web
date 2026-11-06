# AutoHotkey Web

> **빌드:** `A_AhkWebBuild` = 2026.11.1 (포크 버전, 날짜 기반) · 엔진 1.1.37.02 · [변경 이력(CHANGELOG.md)](CHANGELOG.md)
>
> **라이선스:** GPLv2 (AutoHotkey v1.1 포크) · **참고:** [LICENSE](LICENSE) · [보안/책임 있는 사용(SECURITY.md)](SECURITY.md) · [배포(RELEASING.md)](RELEASING.md) · [모델 출처(MODELS.md)](MODELS.md)
> 
> **범위 안내:** 이 저장소는 합법적인 자동화·개발 용도의 기능만 포함합니다. 포렌식/데이터 복구 성격의 민감 기능은 포함하지 않습니다.
>


AutoHotkey v1.1.37.02([원 저장소](https://github.com/AutoHotkey/AutoHotkey))를 포크한 독립 실행 엔진. **문법은 v1과 완전히 동일**하며,
웹 기술 하이브리드 UI(`WebGui`),
딥러닝 텍스트 분류·학습(`TextCls*`/`TextNN*`, ONNX), CDP 브라우저 제어(`WsServer*`+`Lib\Cdp.ahk`)를
**전부 엔진에 네이티브 내장**했습니다.

## 무엇이 다른가 (vs stock AutoHotkey v1.1)

| 항목 | stock | AutoHotkey Web |
|---|---|---|
| 메인 창 클래스 | `AutoHotkey` | `AhkwShell` (컴파일 exe, 안티치트 오탐 완화) |
| GUI 창 클래스 | `AutoHotkeyGUI` | `AhkwShellGui` |
| 컴파일 리소스 마커 | `>AUTOHOTKEY SCRIPT<` | `>AHKWEB SCRIPT<` (난독화·압축 컨테이너 `AHKW1`) |
| 레지스트리 | `SOFTWARE\AutoHotkey` | `SOFTWARE\AutoHotkeyWeb` |
| 추가 명령 | — | `WebGui`, `WebGuiGet` |
| 추가 함수 | — | `TextCls*`/`TextNN*`(딥러닝), `WsServer*`/`JsonGet`/`Base64Decode`(CDP), 한글 OCR(`OcrImageAI`/`OcrImage`), 암호화(`SHA256`·`EncryptPassword`·`DpapiProtect`·`EcdsaSign`…), HTTP·ZIP·이미지·QR·문자열 툴킷 약 150종, `JsonParse`/`JsonStringify` |
| 메소드 체이닝 | — | 문자열 `"  a ".Trim().Upper()`, 배열 `[3,1,2].Sort("N").Join()`, 객체 `obj.Get("a.b")` (사용자 정의가 항상 우선) |
| 추가 변수 | — | `A_AhkWebVersion`, `A_IsAhkWeb`, `A_AhkWebAssets` |
| DBGp appid | `AutoHotkey` | `AutoHotkey` (SciTE4 F7 디버거 호환 유지) |

창 클래스는 `Script::Edit()` 불변식(앞 10자 "AutoHotkey") 때문에 접두사를 유지합니다.

## 구성

```
AutoHotkeyWeb.exe        인터프리터 (빌드 산출물)
onnxruntime.dll          딥러닝용 ONNX 런타임 (엔진이 지연 로드)
build.bat                엔진 빌드 (MSBuild)
fetch_webview2.ps1       WebView2 SDK 다운로드
engine_src/              AHK v1.1 포크 소스
  source/script_webgui.{h,cpp}   WebGui/WebGuiGet + WebView2 브릿지
  source/script_webassets.cpp    단일 EXE 자산 추출 (miniz)
  source/script_ml.cpp           딥러닝 BIF (TextCls*/TextNN*)
  source/notepad_textcls/textnn/onnx.cpp   ONNX 분류·학습 백엔드
  source/script_cdp.cpp          CDP BIF (WsServer*/JsonGet/Base64Decode)
  source/notepad_wsserver/json.cpp + cJSON.c   WebSocket 서버 + JSON
third_party/onnxruntime/ ONNX Runtime SDK (헤더 + import lib)
Lib/Cdp.ahk              CDP 고수준 라이브러리 (번들)
examples/                딥러닝 분류·CDP·딥러닝 학습·한글 OCR·암호화·메소드 체이닝·HTTP/ZIP/QR 예제 (네이티브)
models/                  ocr(PaddleOCR) + kor_unsmile 분류 + kor_embed 백본 (설치 선택)
  engine_src/source/notepad/   내장 툴킷 소스 (암호화·OCR·HTTP·ZIP·이미지·QR ...)
  engine_src/source/script_ext.cpp     툴킷 BIF 표 (Note_X -> X)
  engine_src/source/script_chain.cpp   문자열·배열 메소드 체이닝 + JSON 직렬화
compiler/
  ahkweb2exe.c           스크립트 -> 단일 EXE 컴파일러
  AhkWeb2Exe.exe         (빌드 산출물)
  AutoHotkeyWeb.bin      self-contained 베이스
  build_compiler.bat
scite/ahkweb.api         SciTE4 자동완성/콜팁
docs/                    공식 docs 스타일 가이드 (+ CHM 프로젝트)
installer/
  AutoHotkeyWeb.iss      Inno Setup 스크립트 (한/영)
  Template.ahk           우클릭 "새로 만들기" 템플릿 (UTF-8 BOM)
  build_installer.bat    setup.exe 빌드
  output/                AutoHotkeyWeb_Setup_*.exe
tests/                   스모크/브릿지 테스트
```

## 설치 (권장)

`installer\output\AutoHotkeyWeb_Setup_1.1.37.02.exe` 실행 → 전부 자동 설치:

- 엔진·컴파일러·문서를 `C:\Program Files\AutoHotkey Web`에 설치
- `.ahkw` 파일 형식 등록(더블클릭 실행 / Edit→SciTE4 / Compile→EXE)
- **우클릭 → 새로 만들기 → "AutoHotkey Web Script"** (UTF-8 BOM, 한글 템플릿)
- SciTE4에서 `.ahkw` 구문 강조·F5 실행·F7 디버그·컴파일·자동완성 자동 구성
- 한국어/영어 마법사, WebView2 런타임 점검, 제거 프로그램 포함

설치 프로그램 재빌드: `installer\build_installer.bat` (Inno Setup 6 필요).

> Windows "새로 만들기" 메뉴는 평면 목록이라 "AutoHotkey Web Script"가 stock
> "AutoHotkey Script" 옆에 나타납니다. 생성 확장자는 `.ahkw`.

SciTE4 설정은 `C:\Program Files\AutoHotkey\SciTE\AutoHotkeyWeb.properties`(`*.ahkw` 전용)에
설치되며 `SciTEGlobal.properties`에서 import 됩니다(기존 `.ahk` 설정은 그대로).

## 빌드

```bat
REM 최초 1회: WebView2 SDK
powershell -ExecutionPolicy Bypass -File fetch_webview2.ps1

REM 엔진 (x64 Native Tools Command Prompt for VS 2022)
build.bat

REM Self-contained 베이스 + 컴파일러
"%VS%\...\msbuild" engine_src\AutoHotkeyx.sln /t:AutoHotkeyx /p:Configuration=Self-contained /p:Platform=x64 /p:IntDir=C:\ahkweb_build\obj_sc\ /p:LinkIncremental=false /p:TreatLinkerWarningAsErrors=false
cd compiler && build_compiler.bat
```

> Google Drive 경로에서 빌드하므로 `IntDir`을 로컬(`C:\ahkweb_build`)로 지정합니다.
> Drive 동기화가 증분 링크의 `.ipdb`를 잠가 LNK1257이 나는 것을 피하기 위함입니다.

## 사용

```bat
AutoHotkeyWeb.exe myapp.ahk                    REM 실행
compiler\AhkWeb2Exe.exe /in myapp.ahk /out myapp.exe   REM 단일 EXE
```

SciTE4에서 `.ahkw` 파일을 열고 F5(실행) / F7(디버그) / Ctrl+F7(컴파일).

## 문서

`docs/index.htm`를 브라우저로 열거나, HTML Help Workshop으로 CHM 생성:

```bat
"%ProgramFiles(x86)%\HTML Help Workshop\hhc.exe" docs\AutoHotkeyWeb.hhp
```

생성된 `docs\AutoHotkeyWeb.chm`가 SciTE4 F1 도움말과 연결됩니다.

## 배포·라이선스·책임 있는 사용

- **라이선스:** AutoHotkey v1.1의 GPLv2 포크. 바이너리(AhkWeb2Exe로 컴파일한 exe 포함)를 배포하면 대응 소스를 GPLv2로 공개해야 합니다. 전문은 [`LICENSE`](LICENSE).
- **서드파티:** ONNX Runtime(MIT), WebView2 SDK(별도 약관, `third_party/webview2/LICENSE.txt`), miniz·cJSON·qrcodegen(MIT). SDK는 리포에 포함하지 않고 `fetch_webview2.ps1` 등으로 받습니다.
- **모델:** 번들 ONNX 모델은 각 원본 라이선스를 따릅니다(출처·재배포 조건은 [`MODELS.md`](MODELS.md)). 용량 때문에 리포에 넣지 않고 Releases로 배포합니다.
- **상표:** "AutoHotkey"는 원 상표권자의 상표이며, 본 포크는 원 프로젝트와 무관합니다. 컴파일된 배포 exe의 창 클래스·버전 문자열은 중립화되어 있습니다(개발용 인터프리터는 디버거 호환을 위해 "AutoHotkey" 유지).
- **책임 있는 사용:** 입력 자동화, 화면 인식 등 민감 기능은 본인 데이터·합법적 용도로만. 자세한 내용은 [`SECURITY.md`](SECURITY.md).

## 라이선스

AutoHotkey v1.1의 포크 = **GPLv2**. 배포 시 `WebGui`/딥러닝(`TextCls*`/`TextNN*`)/
CDP(`WsServer*`) 코드를 포함한 전체 소스를 공개해야 합니다. `onnxruntime.dll`(MIT),
WebView2, 번들 모델(kor_unsmile/kosimcse)은 각 저작권자의 라이선스를 따릅니다.
"AutoHotkey" 상표는 제품명 사용 전 확인 권장. (법률 자문이 아님.)

## WebGui 아키텍처 기능

- **브릿지**: `Expose`/`On`(JS→AHK), `Emit`/`Eval`(AHK→JS) — 전부 MsgSleep 큐
  경유라 `Critical`·`#MaxThreads`·Thread 우선순위 적용.
- **Mount**: `WebGui, Name:Mount, host, folder` — 가상 호스트→폴더. file:// 제약
  없이 ES 모듈·fetch가 되는 진짜 오리진. 실제 SPA 배포 필수.
- **WebGuiGet**: `WebGuiGet, Out, Name:Eval, js`(동기 값 반환), `...:Hwnd`(창 핸들).
- **창 옵션**: `x y Center -Resize Frameless AlwaysOnTop NoActivate OpacityNNN`.
- **DevTools**: `WebGui, Name:DevTools`.
- **단일 EXE 번들**: `;@AhkWeb-Assets dist` → 컴파일 시 웹 자산을 EXE에 zip 내장,
  실행 시 추출해 `A_AhkWebAssets` 제공. 엔진+스크립트+자산이 EXE 하나.

## 검증 상태 (실제 실행으로 확인)

- 엔진 Release|x64 + Self-contained 빌드 성공, `AutoHotkeyWeb.exe` 실행 확인
- `A_AhkWebVersion`/`A_IsAhkWeb`/`A_AhkWebAssets` 등록 확인
- `WebGui` End-to-end: 창 생성 → HTML 로드 → JS↔AHK 양방향 메시지 확인
- `Mount` + ES 모듈 import 동작 확인(file://에서 불가한 것이 되므로 가상 호스트 입증)
- `WebGuiGet Eval`(document.title, 임의 식) / `Hwnd`(비영 핸들) 확인
- `;@AhkWeb-Assets`: 격리 폴더(www 없음)에서 컴파일된 EXE 실행 → 내장 자산 추출·로드 확인
- `AhkWeb2Exe`로 컴파일한 단일 EXE 실행 확인
- SciTE4 `.ahkw` 설정 + api 설치 확인
- **체이닝/툴킷 자체 테스트** `tests\features\ext_chain_test.ahk`: 87/87 통과 (문자열·배열 체이닝, 사용자 정의 우선순위, JSON, SHA/HMAC/CRC, AES-GCM 문자열·파일, PBKDF2, DPAPI, ECDSA, ZIP, QR, 캡처·회전, AI/Windows OCR, HTTP)
- 예제 05·06·07 실행 출력 확인, `AhkWeb2Exe` 단일 EXE에서 체이닝·SHA256·JSON·AES 동작 확인
- **딥러닝 네이티브**: `onnxruntime.dll` 지연 로드, `TextClsAvailable`/`TextNNAvailable`=1,
  `TextClsVerdict`(kor_unsmile int8): 정상→`선`(0.0055), 악성→`악`(0.9874);
  `TextNNTrain`→`TextNNClassify`(kor_embed int8 백본) 라운드트립 확인
- **CDP 네이티브**: `WsServer*` 서버 기동/포트/`JsonGet`(불리언 `"true"`/`"false"`) 확인,
  번들 `Lib\Cdp.ahk`가 외부 `#Include` 없이 로드·연결 확인
- 설치 프로그램(ISCC) 컴파일 성공 → `AutoHotkeyWeb_Setup_1.1.37.02.exe` (약 153MB, 모델 포함)
```
```

# 배포(Release) 가이드

이 리포에는 **소스·문서·예제만** 커밋합니다. 빌드 산출물과 대용량 바이너리는
`.gitignore`로 제외하고, 실행 파일·설치 파일·모델은 **GitHub Releases**로 올립니다.
(GitHub는 100MB 초과 파일을 거부하고, 모델/설치파일은 리포를 수백 MB로 불립니다.)

## Releases에 올리는 것
| 파일 | 크기(대략) | 비고 |
|---|---|---|
| `AutoHotkeyWeb_Setup_<ver>.exe` | ~166MB | 설치 프로그램(엔진+컴파일러+문서+예제, 모델은 선택 구성요소) |
| `AutoHotkeyWeb-engine-<ver>.zip` | ~20MB | `AutoHotkeyWeb.exe` + `onnxruntime.dll` + `compiler\` + `Lib\` (모델 없이 쓰는 최소 묶음) |
| `models-<ver>.zip` | ~230MB | `models\` 전체 (한글 OCR + kor_unsmile + kor_embed). 라이선스는 `MODELS.md` |

> 모델을 별도 zip으로 나누면, 모델이 필요 없는 사용자는 엔진만 받을 수 있고
> 모델 재배포 라이선스 고지도 한곳(`MODELS.md`)으로 모읍니다.

## 소스에서 전체 빌드하기
```bat
REM 0) WebView2 SDK (최초 1회) — third_party\webview2 생성
powershell -ExecutionPolicy Bypass -File fetch_webview2.ps1

REM 0-2) (선택) ONNX Runtime SDK — 딥러닝(TextCls/TextNN)·AI OCR 을 포함하려면 필요.
REM     third_party\onnxruntime\{include, lib\onnxruntime.lib} 에 배치하고
REM     onnxruntime.dll 을 엔진 옆에 둡니다. 현재 배포본은 onnxruntime 1.29(API 29, MIT) 사용.
REM     SDK 가 없으면 build.bat 이 자동으로 EnableOnnx=false 로 빌드합니다(딥러닝/AI OCR 만 빠지고
REM     나머지 전 기능은 그대로). 수동 지정도 가능: msbuild ... /p:EnableOnnx=false

REM 1) 엔진 (x64 Native Tools Command Prompt for VS 2022)
build.bat
REM -> AutoHotkeyWeb.exe  (ONNX SDK 유무를 자동 감지)

REM 2) Self-contained 베이스 + 컴파일러 (ONNX SDK 없으면 /p:EnableOnnx=false 추가)
msbuild engine_src\AutoHotkeyx.sln /t:AutoHotkeyx /p:Configuration=Self-contained /p:Platform=x64 /p:IntDir=C:\ahkweb_build\obj_sc\ /p:LinkIncremental=false /p:TreatLinkerWarningAsErrors=false
cd compiler && build_compiler.bat

REM 3) 설치 파일 (Inno Setup 6)
cd installer && build_installer.bat
```

> Google Drive/OneDrive 동기화 폴더에서 빌드하면 `.ipdb` 잠금(LNK1257)이 납니다.
> `IntDir`을 로컬 경로(`C:\ahkweb_build`)로 지정하세요.

## 태그/릴리스 체크리스트
1. `README.md`·`HANDOFF.md`의 버전 문자열 확인
2. 엔진→`.bin`→컴파일러→설치파일 순으로 빌드, 스모크 테스트(`tests\features\ext_chain_test.ahk`) 통과
3. `git tag v<ver>` → push → GitHub에서 Release 생성
4. 위 표의 zip/exe 3종 첨부, 본문에 변경점과 `MODELS.md` 링크

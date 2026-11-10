<p align="center">
  <img src="assets/banner.png" alt="AutoHotkey Web" width="100%">
</p>

<h1 align="center">AutoHotkey Web</h1>

<p align="center">
  AutoHotkey v1.1을 바탕으로 웹 UI, 한글 OCR, 딥러닝, 브라우저 제어를 엔진에 직접 넣은 포크입니다.<br>
  <sub>A fork of AutoHotkey v1.1 with a WebView2 UI, Korean OCR, on-device text models and Chrome control built into the engine.</sub>
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv2-blue.svg" alt="License: GPLv2"></a>
  <img src="https://img.shields.io/badge/platform-Windows%20x64-0078D6" alt="Windows x64">
  <img src="https://img.shields.io/badge/base-AutoHotkey%20v1.1.37.02-2b3a4a" alt="AutoHotkey v1.1.37.02">
  <img src="https://img.shields.io/badge/build-2026.11.2-2ea043" alt="build">
</p>

AutoHotkey Web은 [AutoHotkey v1.1.37.02](https://github.com/AutoHotkey/AutoHotkey)를 포크해서 만든 실행 엔진입니다. 문법은 v1 그대로라서 지금 쓰고 있는 `.ahk` 스크립트를 고치지 않고 돌릴 수 있습니다. 여기에 웹 기술로 만드는 창, 한국어 OCR과 텍스트 분류, 크롬 제어, 암호화 함수, 메소드 체이닝을 추가했는데, 모두 엔진 안에 들어 있어서 따로 DLL을 받거나 라이브러리를 `#Include`할 필요가 없습니다.

지금 빌드는 2026.11.2이고 엔진 버전은 1.1.37.02입니다. 바뀐 내용은 [CHANGELOG](CHANGELOG.md)에 적어 두었습니다. 업무 자동화와 개발에 쓰는 기능만 넣었고, 사용할 때 주의할 점은 [SECURITY.md](SECURITY.md)에 있습니다.

## 시작하기

[Releases](https://github.com/KR-NOTEPAD/autohotkey-web/releases)에서 설치 프로그램을 받아 실행하면 엔진, 컴파일러, 도움말이 설치되고 `.ahkw` 파일 연결과 SciTE4 설정까지 끝납니다. 탐색기에서 마우스 오른쪽 버튼을 누르고 새로 만들기를 고르면 "AutoHotkey Web Script"가 생기고, SciTE4에서는 F5로 실행, F7로 디버그, Ctrl+F7로 컴파일할 수 있습니다.

```bat
AutoHotkeyWeb.exe myapp.ahkw
compiler\AhkWeb2Exe.exe /in myapp.ahkw /out myapp.exe /icon myapp.ico
```

`AhkWeb2Exe.exe`를 인자 없이 실행하면 Ahk2Exe처럼 컴파일러 창이 뜹니다. 창에서 아이콘과 버전 정보를 넣고, 함께 배포할 파일을 고르고, 원하면 설치 프로그램(Setup.exe)까지 같이 만들 수 있습니다. 스크립트에 지시어를 적어 두는 방법도 Ahk2Exe와 같습니다. 스크립트에 적은 값이 창에서 넣은 값보다 우선합니다.

```autohotkey
;@Ahk2Exe-SetMainIcon app.ico
;@Ahk2Exe-SetVersion 1.2.3.4
;@Ahk2Exe-SetDescription 내 프로그램
;@Ahk2Exe-ExeName %A_ScriptDir%\dist\MyApp
;@AhkWeb-Include data          ; data 폴더를 exe 안에 넣고, 실행할 때 A_AhkWebFiles에 풀어 줍니다
;@AhkWeb-Setup                 ; exe와 함께 MyApp_Setup.exe도 만듭니다
```

`#Include`로 불러온 파일, Lib 폴더 함수, `FileInstall`로 넣을 파일은 컴파일할 때 자동으로 합쳐지고, 그 과정에서 문법 검사도 함께 합니다. 지시어 전체 목록은 도움말의 컴파일 항목(`docs\lib\Compile.htm`)에 있습니다.

## 할 수 있는 것

WebGui는 WebView2로 HTML 화면을 띄우고 AutoHotkey와 서로 함수를 부를 수 있게 해 줍니다. React나 Vue로 만든 화면도 그대로 쓸 수 있습니다.

```autohotkey
WebGui, Main:New, w600 h400 Center, 내 앱
WebGui, Main:Expose, SayHello
WebGui, Main:Html, % "<button onclick='ahk.SayHello(``Chris``)'>click</button>"
WebGui, Main:Show
return
SayHello(name) {
    WebGui, Main:Eval, % "alert('안녕 " name "')"
}
```

한글 OCR은 PaddleOCR 모델로 이미지나 화면 영역의 글자를 읽습니다. 모델이 없으면 Windows에 들어 있는 OCR로 대신 읽을 수 있습니다.

```autohotkey
text := OcrImageAI("receipt.png")
text := OcrScreenAI(0, 0, 800, 600)
```

텍스트 분류는 미리 학습된 모델로 문장을 분류하고, 가지고 있는 데이터로 그 자리에서 새 분류기를 학습시킬 수도 있습니다.

```autohotkey
v := TextClsVerdict(modelDir, "이 리뷰 악성인가요?")
TextNNTrain(backbone, "data.tsv", "model.ntc")
r := TextNNClassify("model.ntc", "분류할 문장")
```

브라우저 제어는 엔진에 들어 있는 WebSocket 서버와 함께 배포하는 `Lib\Cdp.ahk`로 이미 열려 있는 크롬을 다룹니다. 드라이버를 깔거나 디버깅 포트를 열지 않아도 됩니다.

```autohotkey
#Include %A_ScriptDir%\Lib\Cdp.ahk
cdp := new Cdp()
tab := cdp.open("https://news.ycombinator.com")
MsgBox % tab.evaluate("document.title")
```

그 밖에 문자열, 배열, 객체에 메소드를 이어 쓰는 체이닝과 JSON 처리, 해시와 AES-GCM, ECDSA, DPAPI 같은 암호화 함수, HTTP, ZIP, QR, 이미지 서치 같은 함수 150개쯤이 기본으로 들어 있습니다.

```autohotkey
out := "  사과,바나나,체리 ".Trim().Split(",").Map("StrUpper").Join(" | ")
obj := JsonParse(HttpGet("https://api.example.com/x"))
```

<p align="center">
  <img src="assets/architecture.png" alt="AutoHotkey Web 구성" width="100%">
</p>

## 원래 AutoHotkey와 다른 점

원래 AutoHotkey v1.1과 같은 PC에 같이 깔아도 서로 영향을 주지 않습니다. 창 클래스, 레지스트리 위치(`SOFTWARE\AutoHotkeyWeb`), 컴파일한 exe에 넣는 표시가 모두 다르기 때문입니다. 컴파일한 exe의 창 클래스는 `AhkwShell`이고 버전 정보는 기본으로 비어 있습니다. 엔진 버전과 빌드를 확인하는 `A_AhkWebVersion`, `A_AhkWebBuild`, `A_IsAhkWeb` 변수와, 컴파일한 exe에 넣은 파일 위치를 알려 주는 `A_AhkWebAssets`, `A_AhkWebFiles` 변수가 추가되어 있습니다.

## 도움말과 예제

도움말은 `docs/index.htm`을 브라우저로 열면 됩니다. HTML Help Workshop이 있으면 `hhc.exe docs\AutoHotkeyWeb.hhp`로 CHM 파일을 만들어 SciTE4의 F1에 연결할 수 있습니다. [`examples/`](examples/) 폴더에는 딥러닝 분류와 학습, 크롬 제어, 한글 OCR, 암호화, 메소드 체이닝, HTTP와 ZIP과 QR 예제가 있고, 모두 받는 즉시 실행해 볼 수 있습니다.

## 직접 빌드하기

Visual Studio 2022의 x64 Native Tools 명령 프롬프트에서 아래 순서로 빌드합니다. 처음 한 번은 WebView2 SDK를 받아야 합니다.

```bat
powershell -ExecutionPolicy Bypass -File fetch_webview2.ps1
build.bat
cd compiler && build_compiler.bat
```

`build.bat`은 ONNX Runtime SDK가 없으면 알아서 딥러닝 기능을 빼고 빌드합니다. 나머지 기능은 그대로 됩니다. Google Drive처럼 동기화되는 폴더에서 빌드하면 링커가 파일 잠금 때문에 실패할 수 있으니 중간 파일 폴더(`IntDir`)를 `C:\ahkweb_build` 같은 로컬 경로로 지정해 주세요. 설치 프로그램은 Inno Setup 6이 있으면 `installer\build_installer.bat`으로 다시 만들 수 있고, 자세한 순서는 [RELEASING.md](RELEASING.md)에 있습니다.

## 라이선스

AutoHotkey v1.1과 같은 GPLv2입니다. 이 소스로 만든 프로그램을 배포할 때는, AhkWeb2Exe로 컴파일한 exe라도 대응하는 소스 전체를 GPLv2로 공개해야 합니다. 전문은 [LICENSE](LICENSE)에 있습니다.

함께 들어 있는 ONNX Runtime은 MIT, WebView2 Loader는 BSD-3-Clause, miniz와 cJSON과 qrcodegen은 MIT 라이선스를 따릅니다. Releases로 따로 받는 모델은 kor_unsmile과 PP-OCRv5가 Apache-2.0, KoSimCSE가 CC BY-SA 4.0이고 자세한 조건은 [MODELS.md](MODELS.md)에 정리했습니다. "AutoHotkey"는 원래 상표권자의 상표이며, 이 포크는 AutoHotkey 프로젝트와 관계가 없습니다.

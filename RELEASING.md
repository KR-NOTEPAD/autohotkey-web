# 릴리스 만드는 법

저장소에는 소스와 문서, 예제만 올립니다. GitHub는 100MB가 넘는 파일을 받지 않고, 모델이나 설치 프로그램까지 넣으면 저장소가 수백 MB로 커지기 때문입니다. 빌드한 실행 파일과 설치 프로그램, 모델은 `.gitignore`로 빼 두고 GitHub Releases에 올립니다.

릴리스마다 올리는 파일은 세 가지입니다. 설치 프로그램 `AutoHotkeyWeb_Setup_<엔진버전>_<빌드>.exe`(약 170MB)는 엔진과 컴파일러, 도움말, 예제를 담고 있고 모델은 설치할 때 고를 수 있습니다. `AutoHotkeyWeb-engine-<버전>.zip`(약 9MB)은 설치하지 않고 바로 쓰는 최소 묶음으로, `AutoHotkeyWeb.exe`와 `onnxruntime.dll`, `compiler\`, `Lib\`가 들어 있습니다. `models-<버전>.zip`(약 160MB)은 `models\` 폴더 전체이고 라이선스는 [MODELS.md](MODELS.md)에 있습니다. 모델을 따로 묶어 두면 필요 없는 사람은 엔진만 받으면 되고, 모델 라이선스 고지도 한곳에서 관리할 수 있습니다.

## 소스에서 전부 빌드하기

```bat
REM WebView2 SDK (처음 한 번)
powershell -ExecutionPolicy Bypass -File fetch_webview2.ps1

REM 엔진 (x64 Native Tools Command Prompt for VS 2022)
build.bat

REM 컴파일러가 쓰는 베이스(.bin)와 컴파일러, 설치 마법사 스텁
msbuild engine_src\AutoHotkeyx.sln /t:AutoHotkeyx /p:Configuration=Self-contained /p:Platform=x64 /p:IntDir=C:\ahkweb_build\obj_sc\ /p:LinkIncremental=false /p:TreatLinkerWarningAsErrors=false
cd compiler && build_compiler.bat

REM 설치 프로그램 (Inno Setup 6)
cd installer && build_installer.bat
```

딥러닝 기능과 AI OCR을 넣으려면 ONNX Runtime SDK를 `third_party\onnxruntime\`에 `include`와 `lib\onnxruntime.lib`로 두고 `onnxruntime.dll`을 엔진 옆에 둡니다. 지금 배포본은 ONNX Runtime 1.29(MIT)를 씁니다. SDK가 없으면 `build.bat`이 딥러닝 기능만 빼고 빌드하고, Self-contained 빌드에는 `/p:EnableOnnx=false`를 붙이면 됩니다. Self-contained 구성은 `.vcxproj`가 아니라 `.sln`으로 빌드해야 합니다. Google Drive나 OneDrive처럼 동기화되는 폴더에서 빌드하면 링커가 LNK1257로 실패하니 `IntDir`을 로컬 경로로 지정하세요.

## 릴리스 순서

먼저 `engine_src/source/ahkweb_version.h`의 `AHKWEB_BUILD`와 `installer/AutoHotkeyWeb.iss`의 `AppBuild`를 같은 값으로 올리고, CHANGELOG에 새 항목을 씁니다. README의 빌드 배지도 같이 고칩니다. 그다음 엔진, `.bin`, 컴파일러, 설치 프로그램 순서로 빌드하고 `tests\features\ext_chain_test.ahk`와 `tests\compiler\` 테스트를 돌려 모두 통과하는지 확인합니다. 문제가 없으면 `v<엔진버전>-<빌드>` 형식으로 태그를 붙여 올리고, GitHub에서 릴리스를 만들어 위 세 파일과 SHA-256 값, 바뀐 내용을 적습니다.

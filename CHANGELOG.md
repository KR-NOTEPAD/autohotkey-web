# 변경 이력

버전은 두 가지를 따로 매깁니다. 엔진 버전(`A_AhkWebVersion`)은 바탕이 된 AutoHotkey의 버전으로 지금은 1.1.37.02이고, 빌드(`A_AhkWebBuild`)는 이 포크를 릴리스할 때마다 올리는 날짜 기반 번호(`YYYY.M.N`)입니다. 새로 릴리스할 때는 `engine_src/source/ahkweb_version.h`의 `AHKWEB_BUILD`를 올리고 여기에 내용을 적습니다.

## 2026.11.2 (2026-11-06)

이번 빌드는 컴파일러 AhkWeb2Exe를 크게 손본 것입니다. 엔진은 여전히 AutoHotkey 1.1.37.02를 바탕으로 합니다.

먼저 고친 버그가 있습니다. 지금까지는 컴파일한 exe에서 `#Include`, Lib 폴더 함수, `FileInstall`이 동작하지 않았습니다. 컴파일용 베이스는 실행 중에 `#Include`를 읽지 않는데, 컴파일러가 메인 스크립트 하나만 넣고 있었기 때문입니다. 이제는 Ahk2Exe처럼 컴파일할 때 포함 파일과 Lib 함수를 모두 합치고, `AutoHotkeyWeb.exe /iLib`으로 필요한 Lib 파일을 찾으면서 문법 검사도 합니다. `FileInstall`로 넣을 파일도 exe 안에 들어갑니다. 그 밖에 `/icon`으로 준 아이콘이 실제로는 적용되지 않던 문제와, 웹 자산을 풀어 둔 임시 폴더(`%TEMP%\AhkWeb_<pid>`)가 지워지지 않고 쌓이던 문제도 고쳤습니다.

`AhkWeb2Exe.exe`를 인자 없이 실행하면 Ahk2Exe처럼 컴파일러 창이 뜹니다. 기본, 버전 정보, 파일 포함, 설치 마법사, 고급 탭이 있고, 최근에 연 스크립트와 스크립트마다 넣은 설정을 기억합니다. 파일 포함 탭에는 스크립트 안에 적은 `;@AhkWeb-Include` 항목도 함께 보여 줍니다. 아이콘은 exe와 트레이 아이콘을 실제로 바꾸고, 핫키 일시중지와 일시정지 상태의 트레이 아이콘도 따로 정할 수 있습니다. 버전 정보는 `SetVersion`, `SetDescription`, `SetCopyright`, `SetCompanyName`, `SetProductName` 같은 Ahk2Exe 지시어로 넣고, 아무것도 지정하지 않으면 예전처럼 비워 둡니다.

`;@AhkWeb-Include`로 폴더나 파일을 확장자에 상관없이 exe 하나에 넣을 수 있습니다. 넣은 파일은 스크립트가 시작되기 전에 풀리고, 위치는 새 변수 `A_AhkWebFiles`로 알 수 있습니다. 컴파일하지 않고 실행할 때 이 변수는 `A_ScriptDir`과 같습니다. 풀어 둘 곳은 `;@AhkWeb-ExtractTo`로 임시 폴더, exe 폴더, AppData, LocalAppData 가운데 고르고, `ExtractMode`로 항상 덮어쓸지, 바뀐 파일만 쓸지, 없을 때만 쓸지 정합니다.

외부 도구 없이 설치 프로그램도 만들 수 있습니다. 컴파일러 창에서 "EXE + 설치 마법사"를 고르거나 `;@AhkWeb-Setup`을 적으면 `<앱 이름>_Setup.exe`가 같이 만들어집니다. 사용권 계약, 설치 폴더, 바로가기를 고르는 화면이 있고, 설치하면 제거 프로그램이 생기고 Windows의 앱 및 기능 목록에 등록됩니다. 같은 앱을 다시 설치하면 기존 폴더에 덮어씁니다. 설치 범위는 현재 사용자, 모든 사용자, 또는 설치할 때 사용자가 고르게 할 수 있고, 모든 사용자를 고르면 그때 관리자 권한을 요청합니다. `/S`, `/D=폴더`, `/ALLUSERS`, `/CURRENTUSER`, `/NODESKTOP`, `/NOSTARTMENU`로 화면 없이 설치할 수 있고, 화면은 한국어와 영어를 지원합니다.

인증서가 있으면 컴파일하면서 바로 코드 서명을 할 수 있습니다. signtool을 알아서 찾고, 인증서 저장소의 주체 이름이나 .pfx 파일 중 하나로 exe와 Setup.exe를 함께 서명합니다. 컴파일러 창에 넣은 인증서 암호는 이 PC에서만 풀 수 있게 암호화해서 저장합니다. 이 밖에 관리자 권한으로 실행되게 하는 `RequireAdmin`, 컴파일 뒤에 명령을 실행하는 `PostExec`, 출력 경로를 정하는 `ExeName`, 리소스를 추가하는 `AddResource`, 컴파일본에서 코드를 빼거나 넣는 `IgnoreBegin`과 `Keep` 지시어를 지원합니다.

설치 프로그램에는 시작 메뉴의 "AhkWeb2Exe 컴파일러" 바로가기와, `.ahkw` 파일 오른쪽 버튼 메뉴의 "컴파일 (옵션 창)"이 추가되었습니다. 원하면 `.ahk` 파일 메뉴에도 넣을 수 있고, 이때 `.ahk` 파일 연결 자체는 바꾸지 않습니다.

테스트는 `tests\compiler\`에 있습니다. `verify_icon_test.ps1`은 아이콘과 버전 정보를, `verify_bundle_test.ps1`은 포함 파일과 설치 프로그램을, `verify_wizard_sign_test.ps1`은 서명과 설치 마법사를 실제로 눌러 가며 확인합니다.

## 2026.11.1 (2026-11-06)

첫 공개 빌드입니다. 엔진은 AutoHotkey 1.1.37.02를 바탕으로 합니다.

문자열, 배열, 객체에 메소드를 이어서 쓸 수 있게 했습니다. `"  a,b ".Trim().Split(",").Join("|")`, `[3,1,2].Sort("N").Map(fn).Sum()`, `obj.Get("a.b")` 같은 식입니다. 키나 클래스 메소드, `"".base`처럼 사용자가 정의한 것이 있으면 그쪽이 먼저 쓰입니다. JSON을 다루는 `JsonParse`와 `JsonStringify`도 추가했고, 예전부터 있던 `JsonGet`은 그대로 둡니다.

한글 OCR은 PaddleOCR 모델을 쓰는 `OcrImageAI`, `OcrScreenAI`와, Windows에 들어 있는 OCR을 쓰는 `OcrImage`, `OcrScreen`이 있습니다. 암호화 함수로는 SHA, HMAC, CRC, 문자열과 파일용 AES-256-GCM, PBKDF2 비밀번호 해시, DPAPI, ECDSA 서명, 보안 난수 등 30개쯤을 넣었습니다. 이 밖에 HTTP와 병렬 요청, WebSocket 클라이언트, ZIP과 인코딩, 휴지통, 이미지 서치와 템플릿 매칭, QR, 클립보드 이미지, 문자열 유사도, CSV, 시간과 수학 함수까지 150개쯤이 들어 있습니다. 원래 `Note_X()`였던 함수는 `X()`라는 이름으로 씁니다.

AhkWeb2Exe로 컴파일하면 스크립트를 압축한 뒤 섞어서 넣기 때문에 리소스 편집기나 strings로 열어도 원문이 보이지 않습니다. 실행할 때 엔진이 메모리에서 되돌립니다. 포크 빌드 번호를 알려 주는 `A_AhkWebBuild` 변수도 이때 추가했습니다.

컴파일한 exe에서는 창 클래스(`AhkwShell`, `AhkwShellGui`, `AhkwWebView`), 키보드·마우스 훅 뮤텍스, 다운로드할 때의 User-Agent(`Mozilla/5.0`), 창 제목에서 "AutoHotkey"라는 이름을 뺐고, 버전 정보는 비워 두었습니다. 평범한 프로그램이 매크로로 잘못 걸리는 일을 줄이려는 것입니다. 개발할 때 쓰는 인터프리터는 SciTE4 디버거가 제품명과 DBGp appid를 보고 동작하기 때문에 "AutoHotkey"를 그대로 남겼고, F5, F7, 컴파일 모두 전과 같이 됩니다.

몇몇 함수 이름은 v2에 맞췄습니다. 문자열 함수는 `StrUpper`, `StrLower`, `StrTitle`, `StrRepeat`, `StrReverse`, `StrPadLeft`, `StrPadRight`, `StrCount`, `StrEntropy`이고, 서명 함수는 `EcdsaKeyPair`, `EcdsaSign`, `EcdsaVerify`입니다. SciTE4 설정은 `*.ahkw` 파일에만 적용되도록 새로 짰고, 탐색기의 새로 만들기 메뉴가 바로 반영되도록 캐시를 지우는 부분도 손봤습니다. 도움말에는 메소드 체이닝, 암호화, OCR, 툴킷, 컴파일 문서와 라이선스 안내를 추가했습니다.

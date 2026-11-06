@echo off
REM ============================================================================
REM  AutoHotkey Web - one-step engine build (MSBuild, Release x64 Unicode).
REM  Produces:  AutoHotkeyWeb.exe  (the interpreter EXE)
REM             engine_src\bin\AutoHotkeyU64.exe  (same file, MSBuild's name)
REM  Keep this file ASCII-only.
REM
REM  Prereqs: Visual Studio 2022 (C++), and third_party\webview2 populated
REM  (run:  powershell -ExecutionPolicy Bypass -File fetch_webview2.ps1).
REM
REM  The intermediate dir is forced to a LOCAL path (C:\ahkweb_build) because
REM  building directly on a Google Drive / OneDrive folder lets the sync client
REM  lock the incremental .ipdb mid-link (LNK1257). Final EXE still lands here.
REM ============================================================================
setlocal

set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERROR] VS2022 vcvars64 not found at:
    echo         %VCVARS%
    echo         Install "Desktop development with C++" or fix the VS path.
    exit /b 1
)

if not exist "third_party\webview2\build\native\include\WebView2.h" (
    echo [ERROR] WebView2 SDK missing. Run:
    echo         powershell -ExecutionPolicy Bypass -File fetch_webview2.ps1
    exit /b 1
)

REM ONNX Runtime SDK is OPTIONAL. If third_party\onnxruntime is present, deep-learning
REM (TextCls/TextNN) and AI OCR are built in; otherwise set EnableOnnx=false so the engine
REM still builds (those features become stubs that return empty; everything else works).
set "ONNX_FLAG="
if not exist "third_party\onnxruntime\include\onnxruntime_c_api.h" (
    echo [INFO] ONNX Runtime SDK not found -^> building WITHOUT deep learning / AI OCR.
    echo        To include them, place the ONNX Runtime SDK under third_party\onnxruntime
    echo        ^(include\ + lib\onnxruntime.lib^) and re-run. See RELEASING.md / MODELS.md.
    set "ONNX_FLAG=/p:EnableOnnx=false"
)

call "%VCVARS%" >nul 2>&1

echo [BUILD] AutoHotkey Web engine (Release^|x64) ...
msbuild "engine_src\AutoHotkeyx.vcxproj" ^
    /p:Configuration=Release /p:Platform=x64 ^
    %ONNX_FLAG% ^
    /p:IntDir=C:\ahkweb_build\obj\ ^
    /p:LinkIncremental=false ^
    /p:TreatLinkerWarningAsErrors=false ^
    /m /v:minimal /nologo
if errorlevel 1 (
    echo [FAILED] engine build error.
    exit /b 1
)

copy /y "engine_src\bin\AutoHotkeyU64.exe" "AutoHotkeyWeb.exe" >nul
echo.
echo [DONE] AutoHotkeyWeb.exe is ready.
endlocal
exit /b 0

@echo off
REM  Build AhkWeb2Exe.exe (the AutoHotkey Web script compiler) and refresh the
REM  bundled self-contained base. Run from x64 Native Tools Command Prompt.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cl.exe not found. Run from "x64 Native Tools Command Prompt for VS".
    exit /b 1
)

REM Keep the compiled-script container format identical to the engine's by copying the one
REM shared header from the engine source every build (compiler and engine must never drift).
if exist "..\engine_src\source\script_crypt.h" copy /y "..\engine_src\source\script_crypt.h" "script_crypt.h" >nul

echo [BUILD] AhkWeb2Exe.exe ...
cl /nologo /O2 /W3 ahkweb2exe.c miniz.c /Fe:AhkWeb2Exe.exe /link user32.lib
if errorlevel 1 ( echo [FAILED] exit /b 1 )
del ahkweb2exe.obj miniz.obj 2>nul

REM Refresh the base .bin from the engine's Self-contained build output.
set "BIN=..\engine_src\bin\Unicode 64-bit.bin"
if exist "%BIN%" (
    copy /y "%BIN%" "AutoHotkeyWeb.bin" >nul
    echo [OK] AutoHotkeyWeb.bin updated from engine build.
) else (
    echo [WARN] %BIN% not found. Build the engine Self-contained^|x64 config first:
    echo        msbuild engine_src\AutoHotkeyx.sln /t:AutoHotkeyx /p:Configuration=Self-contained /p:Platform=x64
)

echo [DONE]
endlocal
exit /b 0

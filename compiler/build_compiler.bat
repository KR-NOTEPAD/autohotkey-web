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
set "RES="
rc /nologo /fo ahkweb2exe.res ahkweb2exe.rc >nul 2>nul && set "RES=ahkweb2exe.res"
REM GUI subsystem (Ahk2Exe-style window when run without /in); CLI output attaches to the
REM parent console or uses redirected stdout (SciTE output pane).
cl /nologo /O2 /W3 /utf-8 ahkweb2exe.c miniz.c %RES% /Fe:AhkWeb2Exe.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /MANIFEST:EMBED
if errorlevel 1 ( echo [FAILED] & exit /b 1 )
del ahkweb2exe.obj miniz.obj ahkweb2exe.res 2>nul

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

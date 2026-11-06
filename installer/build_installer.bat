@echo off
REM  Build the AutoHotkey Web installer (setup.exe) with Inno Setup 6.
REM  Prereqs (built first): ..\AutoHotkeyWeb.exe, ..\compiler\AhkWeb2Exe.exe,
REM  ..\compiler\AutoHotkeyWeb.bin, ..\docs\, ..\scite\ahkweb.api.
REM  Output: output\AutoHotkeyWeb_Setup_<ver>.exe
setlocal
cd /d "%~dp0"

set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo [ERROR] Inno Setup 6 not found at "%ISCC%".
    echo         Install from https://jrsoftware.org/isdl.php
    exit /b 1
)

"%ISCC%" "AutoHotkeyWeb.iss"
if errorlevel 1 ( echo [FAILED] exit /b 1 )
echo.
echo [DONE] see output\
endlocal
exit /b 0

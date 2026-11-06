; AhkWeb2Exe end-to-end test: #Include, Lib auto-include, FileInstall, bundled files,
; IgnoreBegin/End, Keep. Writes result.txt next to the exe. Run: verify_bundle_test.ps1
#NoEnv
#NoTrayIcon
;@AhkWeb-Include data
;@AhkWeb-Include readme.md, docs\
;@Ahk2Exe-SetVersion 2.0.1
;@Ahk2Exe-SetProductName Bundle Test
;@Ahk2Exe-SetCompanyName KR-NOTEPAD
;@AhkWeb-SetupScope user
;@AhkWeb-SetupShortcuts none
;@AhkWeb-SetupRun 0
;@AhkWeb-SetupLicense LICENSE.txt
#Include %A_ScriptDir%\sub\helper.ahk

out := ""
out .= "helper=" HelperValue() "`n"
out .= "lib=" BundleLib_Value() "`n"

FileInstall, fi\fi.txt, %A_Temp%\ahkw_fi_test.txt, 1
FileRead, fiTxt, %A_Temp%\ahkw_fi_test.txt
FileDelete, %A_Temp%\ahkw_fi_test.txt
out .= "fileinstall=" fiTxt "`n"

FileRead, a, %A_AhkWebFiles%\data\a.txt
FileRead, b, %A_AhkWebFiles%\data\deep\b.json
out .= "files=" (FileExist(A_AhkWebFiles "\data\a.txt") ? a : "MISSING") "|" b "`n"
out .= "docs=" (FileExist(A_AhkWebFiles "\docs\readme.md") ? "yes" : "no") "`n"

mode := "source"
;@Ahk2Exe-IgnoreBegin
mode := "ignored-block-ran"
;@Ahk2Exe-IgnoreEnd
/*@Ahk2Exe-Keep
mode := "compiled"
*/
out .= "mode=" mode "`n"
out .= "compiled=" A_IsCompiled "`n"

FileDelete, %A_ScriptDir%\result.txt
FileAppend, %out%, %A_ScriptDir%\result.txt, UTF-8
ExitApp

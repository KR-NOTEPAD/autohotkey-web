; AhkWeb2Exe directive test: custom icon, version info, extra icon group, ExeName.
;   compiler\AhkWeb2Exe.exe /in tests\compiler\icon_test.ahk
;@Ahk2Exe-SetMainIcon ..\..\engine_src\source\resources\icon_suspend.ico
;@Ahk2Exe-SetVersion 1.2.3.4
;@Ahk2Exe-SetDescription 아이콘 테스트 앱
;@Ahk2Exe-SetCopyright (c) 2026 KR-NOTEPAD
;@Ahk2Exe-SetCompanyName KR-NOTEPAD
;@Ahk2Exe-Set ProductName, AhkWeb Icon Test
;@Ahk2Exe-AddResource ..\..\engine_src\source\resources\icon_pause.ico, 207
;@Ahk2Exe-ExeName %A_ScriptDir%\out\icon_test
FileDelete, %A_ScriptDir%\out\ran.txt
FileAppend, ok, %A_ScriptDir%\out\ran.txt
ExitApp

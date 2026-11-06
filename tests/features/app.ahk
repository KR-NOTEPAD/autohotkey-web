#NoEnv
#SingleInstance Force
logf := A_ScriptDir . "\_feat_log.txt"
FileDelete, %logf%

WebGui, Main:New, w640 h480 Center, Feature Test
; Mount the www folder as a real origin so ES modules / fetch work.
WebGui, Main:Mount, app.ahkweb, %A_ScriptDir%\www
WebGui, Main:Load, https://app.ahkweb/index.html
WebGui, Main:Show
; give the page time to load + run its module
SetTimer, DoChecks, -2500
SetTimer, Done, -7000
return

DoChecks:
  WebGuiGet, hwnd, Main:Hwnd
  Log("Hwnd=" . hwnd . " (nonzero=" . (hwnd+0 > 0) . ")")

  WebGuiGet, title, Main:Eval, document.title
  Log("title=" . title)

  WebGuiGet, esm, Main:Eval, window.__esm
  Log("esm=" . esm)

  WebGuiGet, loc, Main:Eval, window.__loc
  Log("origin=" . loc)

  WebGuiGet, calc, Main:Eval, (6*7)
  Log("calc=" . calc)
return

Done:
  Log("done")
  ExitApp
return

Log(s) {
  global logf
  FileAppend, % s . "`n", %logf%, UTF-8
}
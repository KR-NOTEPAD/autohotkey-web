;@AhkWeb-Assets www
#NoEnv
#SingleInstance Force
logf := A_Temp . "\_bundle_log.txt"
FileDelete, %logf%

; Compiled: A_AhkWebAssets = extracted temp dir. Uncompiled: fall back to source.
assets := A_AhkWebAssets
if (assets = "")
    assets := A_ScriptDir . "\www"
Log("A_AhkWebAssets=" . A_AhkWebAssets)
Log("using assets=" . assets)

WebGui, Main:New, w600 h400 Center, Bundled App
WebGui, Main:Mount, app.ahkweb, %assets%
WebGui, Main:Load, https://app.ahkweb/index.html
WebGui, Main:Show
SetTimer, Checks, -2500
SetTimer, Done, -6000
return

Checks:
  WebGuiGet, t, Main:Eval, document.title
  Log("title=" . t)
  WebGuiGet, tag, Main:Eval, window.__tag
  Log("esm_tag=" . tag)
return

Done:
  Log("done")
  ExitApp
return

Log(s) {
  global logf
  FileAppend, % s . "`n", %logf%, UTF-8
}
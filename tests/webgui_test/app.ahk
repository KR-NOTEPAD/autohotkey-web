#NoEnv
#SingleInstance Force
SetBatchLines, -1
logf := A_ScriptDir . "\_webgui_log.txt"
FileDelete, %logf%
Log("script start, IsAhkWeb=" . A_IsAhkWeb)

WebGui, Main:New, w720 h520, AutoHotkey Web Test
WebGui, Main:Expose, Add
WebGui, Main:Expose, SaveNote
WebGui, Main:On, __Ready, OnReady
WebGui, Main:Load, %A_ScriptDir%\index.html
WebGui, Main:Show
Log("after Show")

; Push an event to JS after 1.5s to test AHK->JS direction.
SetTimer, PushTick, -1500
; Auto-exit after 6s so the headless test terminates.
SetTimer, Done, -6000
return

PushTick:
  WebGui, Main:Emit, tick, hello-from-ahk
  Log("emitted tick")
return

Done:
  Log("done, exiting")
  ExitApp
return

; --- exposed handlers: param is a JSON string ---
Add(params) {
  a := JGet(params, "a"), b := JGet(params, "b")
  Log("Add called a=" . a . " b=" . b)
  return a + b
}
SaveNote(params) {
  t := JGet(params, "text")
  Log("SaveNote called text=" . t)
  FileAppend, % t . "`n", %A_ScriptDir%\_saved_note.txt, UTF-8
  return "saved"
}
OnReady(params) {
  Log("page ready: " . params)
}

; tiny value extractor for flat JSON {"k":v} (number or string)
JGet(json, key) {
  if RegExMatch(json, """" . key . """\s*:\s*""([^""]*)""", m)
    return m1
  if RegExMatch(json, """" . key . """\s*:\s*(-?\d+(?:\.\d+)?)", m)
    return m1
  return ""
}

Log(s) {
  global logf
  FileAppend, % s . "`n", %logf%, UTF-8
}
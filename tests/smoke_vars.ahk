#NoTrayIcon
out := A_ScriptDir . "\_smoke_result.txt"
txt := "A_AhkVersion=" . A_AhkVersion . "`n"
txt .= "A_AhkWebVersion=" . A_AhkWebVersion . "`n"
txt .= "A_IsAhkWeb=" . A_IsAhkWeb . "`n"
FileDelete, %out%
FileAppend, %txt%, %out%, UTF-8
ExitApp
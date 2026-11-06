; ============================================================================
;  AutoHotkey Web 예제 4 - 한글 OCR (네이티브 내장)
;  두 엔진을 같은 화면에서 비교합니다.
;    AI OCR      : OcrImageAI / OcrScreenAI  (PaddleOCR ONNX, 한글·영문·숫자, 권장)
;    Windows OCR : OcrImage / OcrScreen      (모델 불필요, OS 언어팩 사용)
;
;  의존성: AI OCR 은 onnxruntime.dll + models\ocr (설치 시 '딥러닝 모델' 구성요소).
;  실행:   AutoHotkeyWeb.exe app.ahk
; ============================================================================
#NoEnv
#SingleInstance Force
SetWorkingDir %A_ScriptDir%

global CurImage := A_ScriptDir "\sample.png"
global CurRegion := ""

Gui, +Resize +MinSize560x420
Gui, Font, s10, 맑은 고딕
Gui, Add, Button, x10 y10 w110 gOpenImage, 이미지 열기
Gui, Add, Button, x+6 w130 gPickRegion, 화면 영역 선택
Gui, Add, Button, x+6 w120 gFromClipboard, 클립보드 이미지
Gui, Add, Button, x+6 w90 gRunOcr Default, 인식
Gui, Add, Radio, x10 y+12 vEngAI Checked, AI OCR (권장)
Gui, Add, Radio, x+14 vEngWin, Windows OCR
Gui, Add, Text, x+24, AI 옵션:
Gui, Add, CheckBox, x+8 vOptEnhance, 화질향상
Gui, Add, CheckBox, x+8 vOptCls, 각도보정
Gui, Add, CheckBox, x+8 vOptRot, 세로자동
Gui, Add, CheckBox, x+8 vOptVin, 차대번호
Gui, Add, CheckBox, x+8 vOptData, 좌표포함
Gui, Add, Edit, x10 y+12 w760 h360 vResult ReadOnly
Gui, Add, StatusBar
Gui, Show, w780 h470, AutoHotkey Web - 한글 OCR
SB_SetText(" 대상: " CurImage)
if (OcrLanguages() = "")
    GuiControl, Disable, EngWin
return

GuiSize:
    if (A_EventInfo = 1)
        return
    GuiControl, Move, Result, % "w" (A_GuiWidth - 20) " h" (A_GuiHeight - 135)
return

OpenImage:
    FileSelectFile, f, 3, %A_ScriptDir%, 이미지 선택, 이미지 (*.png; *.jpg; *.jpeg; *.bmp; *.gif; *.tif)
    if (f = "")
        return
    CurImage := f, CurRegion := ""
    SB_SetText(" 대상: " f)
    Gosub, RunOcr
return

PickRegion:
    Gui, Hide
    r := SelectRegion()          ; 드래그로 영역 선택 -> "x,y,w,h" (취소하면 "")
    Gui, Show
    if (r = "")
        return
    CurRegion := r
    SB_SetText(" 대상: 화면 영역 " r)
    Gosub, RunOcr
return

FromClipboard:
    tmp := A_Temp "\ahkweb_ocr_clip.png"
    if (!ClipboardImageToPng(tmp)) {
        SB_SetText(" 클립보드에 이미지가 없습니다 (Win+Shift+S 로 캡처해 보세요)")
        return
    }
    CurImage := tmp, CurRegion := ""
    SB_SetText(" 대상: 클립보드 이미지")
    Gosub, RunOcr
return

RunOcr:
    Gui, Submit, NoHide
    opts := Trim((OptEnhance ? "enhance " : "") (OptCls ? "cls " : "") (OptRot ? "rot=auto " : "")
        . (OptVin ? "vin " : "") (OptData ? "data" : ""))
    GuiControl,, Result, 인식 중...
    t0 := A_TickCount
    if (CurRegion != "") {
        p := StrSplit(CurRegion, ",")
        txt := EngAI ? OcrScreenAI(p[1], p[2], p[3], p[4], "", opts) : OcrScreen(p[1], p[2], p[3], p[4])
    } else
        txt := EngAI ? OcrImageAI(CurImage, "", opts) : OcrImage(CurImage)
    ms := A_TickCount - t0
    err := EngAI ? OcrAIError() : ""
    GuiControl,, Result, % txt != "" ? txt : "(인식된 글자가 없습니다)" (err != "" ? "`r`n오류: " err : "")
    ; 메소드 체이닝: 줄 수와 글자 수를 바로 계산
    SB_SetText(" " (EngAI ? "AI" : "Windows") " OCR  " ms "ms  |  " txt.Lines().Compact().Length() "줄, "
        . StrReplace(txt, "`n").Length() "자" (opts != "" ? "  |  옵션: " opts : ""))
return

GuiClose:
ExitApp

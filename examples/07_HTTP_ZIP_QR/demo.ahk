; ============================================================================
;  AutoHotkey Web 예제 7 - HTTP · JSON · ZIP · QR · 화면/이미지 (전부 엔진 내장)
;    1) HttpGet + JsonParse 체이닝으로 웹 API 값 꺼내기
;    2) HttpJob 으로 여러 요청 병렬 처리
;    3) ZIP 압축/목록/추출 + 파일 인코딩 감지
;    4) QR 코드 생성 → 화면에 띄운 뒤 ImageSearchAll 로 다시 찾기
;    5) 클립보드 이미지 저장
;  실행: AutoHotkeyWeb.exe demo.ahk   (인터넷 연결 필요: 1, 2번)
; ============================================================================
#NoEnv
#SingleInstance Force
FileEncoding, UTF-8
work := A_Temp "\ahkweb_demo7"
FileCreateDir, %work%
r := ""

; ---- 1) HTTP + JSON ------------------------------------------------------------------
body := HttpGet("https://api.github.com/repos/AutoHotkey/AutoHotkey")
if (HttpLastStatus() = 200) {
    repo := body.JsonParse()
    r .= "[HTTP+JSON] " repo.Get("full_name") "  ★" repo.Get("stargazers_count")
        . "  언어:" repo.Get("language") "  소유자:" repo.Get("owner.login") "`r`n"
} else
    r .= "[HTTP+JSON] 요청 실패 (상태 " HttpLastStatus() ")`r`n"

; ---- 2) 병렬 요청 (HttpJob 풀) --------------------------------------------------------
urls := ["https://example.com/", "https://www.iana.org/help/example-domains", "https://httpbin.org/uuid"]
HttpJobSetConcurrency(4)
jobs := urls.Map(Func("StartJob"))                    ; 배열 체이닝으로 작업 시작
StartJob(url) {
    return HttpJobStart("GET", url, "", "", 15000, 2)
}
t0 := A_TickCount
Loop {
    pending := 0
    for i, id in jobs
        if (StrSplit(HttpJobStatus(id), ",")[1] < 2)  ; 0대기 1실행 2완료 3실패
            pending++
    if (!pending || A_TickCount - t0 > 20000)
        break
    HttpJobWaitAny(200)
}
for i, id in jobs {
    st := StrSplit(HttpJobStatus(id), ",")
    r .= "[병렬 " i "] HTTP " st[2] "  " StrLen(HttpJobResult(id)) "자  " st[5] "ms  " urls[i] "`r`n"
    HttpJobFree(id)
}

; ---- 3) ZIP + 인코딩 -----------------------------------------------------------------
FileDelete, %work%\*.*
FileAppend, 첫 번째 파일`n한글 내용, %work%\a.txt
FileAppend, second file, %work%\b.txt, CP0
zip := work "\bundle.zip"
ZipAddFile(zip, work "\a.txt", "docs/a.txt")
ZipAddFile(zip, work "\b.txt", "docs/b.txt", 9)
r .= "[ZIP] 목록: " StrReplace(ZipList(zip), "`n", ", ") "`r`n"
ZipExtractAll(zip, work "\out")
r .= "[ZIP] 추출 후 a.txt 인코딩: " DetectEncoding(work "\out\docs\a.txt")
    . " / b.txt: " DetectEncoding(work "\out\docs\b.txt") "  (" FormatBytes(FileSizeOf(zip)) ")`r`n"
FileSizeOf(f) {
    FileGetSize, s, %f%
    return s
}

; ---- 4) QR 생성 → 화면에 표시 → ImageSearchAll 로 찾기 -----------------------------------
qr := work "\qr.png"
QrEncode("https://www.autohotkey.com", qr, 6, 2)
Gui, Q:+AlwaysOnTop -Caption +ToolWindow
Gui, Q:Add, Picture, x0 y0, %qr%
Gui, Q:Show, x100 y100 NoActivate
Sleep, 400
found := ImageSearchAll(qr, 0, 0, A_ScreenWidth, A_ScreenHeight, 10, 5)  ; "x1,y1;x2,y2;..."
Gui, Q:Destroy
r .= "[QR] " ImageWidth(qr) "x" ImageHeight(qr) " 생성, 화면에서 찾은 위치: " (found != "" ? found : "없음") "`r`n"

; ---- 5) 클립보드 이미지 ----------------------------------------------------------------
clip := work "\clip.png"
r .= "[클립보드] " (ClipboardImageToPng(clip) ? "이미지 저장됨 → " clip : "클립보드에 이미지 없음 (Win+Shift+S 후 다시 실행)") "`r`n"

Gui, Font, s10, Consolas
Gui, Add, Edit, w860 h360 ReadOnly, %r%
Gui, Add, Button, gOpenWork, 작업 폴더 열기
Gui, Show,, AutoHotkey Web - HTTP · ZIP · QR
return

OpenWork:
    Run, %work%
return

GuiClose:
ExitApp

; ============================================================================
;  AutoHotkey Web 시작 템플릿 - 한 파일로 바로 실행됩니다.
;  실행: 이 파일을 더블클릭하거나 편집기에서 F5. 문법은 AutoHotkey v1과 동일.
;  HTML/CSS/JS UI(프런트엔드) + 오토핫키(백엔드) 하이브리드 앱입니다.
; ============================================================================
#NoEnv
#SingleInstance Force

WebGui, Main:New, w720 h520 Center, 내 첫 AutoHotkey Web 앱
WebGui, Main:Expose, Greet            ; JS에서 ahk.call("Greet", ...) 로 호출
WebGui, Main:Html, % PageHtml()       ; 외부 파일 없이 인라인 HTML 로드
WebGui, Main:Show
return

; JS가 보낸 params 는 JSON 문자열. 반환 문자열이 JS Promise 로 돌아갑니다.
Greet(params) {
    name := ""
    if RegExMatch(params, """name""\s*:\s*""([^""]*)""", m)
        name := m1
    if (name = "")
        name := "친구"
    return "안녕하세요, " . name . "님! AutoHotkey 가 응답했습니다."
}

MainWebGuiClose:                        ; 창 닫기 버튼
ExitApp

PageHtml() {
    s =
( LTrim Join`r`n
<!doctype html>
<html lang="ko"><head><meta charset="utf-8">
<style>
  body{font-family:"Segoe UI","Malgun Gothic",sans-serif;margin:32px;color:#222}
  h1{color:#3F5770} p{color:#555}
  input,button{font-size:16px;padding:8px;margin-right:6px}
  button{background:#3F5770;color:#fff;border:0;border-radius:6px;cursor:pointer}
  #out{margin-top:18px;font-size:18px;color:#A04040;min-height:24px}
</style></head>
<body>
  <h1>AutoHotkey Web</h1>
  <p>HTML/CSS/JS 로 UI 를 그리고, 오토핫키가 로직을 처리합니다.</p>
  <input id="name" placeholder="이름을 입력하세요">
  <button id="go">인사하기</button>
  <div id="out"></div>
  <script>
    document.getElementById('go').onclick = async () => {
      const name = document.getElementById('name').value;
      const msg = await ahk.call("Greet", { name });
      document.getElementById('out').textContent = msg;
    };
  </script>
</body></html>
)
    return s
}
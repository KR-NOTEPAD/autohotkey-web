; ============================================================================
;  AutoHotkey Web 예제 2 - CDP 웹 제어 (WebGui UI + 번들 Lib\Cdp.ahk)
;  웹 UI 의 주소창에 URL 을 넣으면, 엔진 내장 CDP(Chrome DevTools Protocol)
;  로 실제 크롬 탭을 이동시키고 제목/링크수/머리말을 추출해 보여줍니다.
;
;  준비(1회): 크롬에서 chrome://extensions → 개발자 모드 →
;             "압축해제된 확장 프로그램 로드" → cdp_extension 폴더 선택.
;  의존성:    없음 - WsServer/JsonGet 은 AutoHotkey Web 엔진 네이티브, Cdp.ahk 는 번들 Lib.
; ============================================================================
#NoEnv
#SingleInstance Force
; AutoHotkey Web 네이티브 CDP (WsServer/JsonGet 는 엔진 내장)
#Include %A_ScriptDir%\..\..\Lib\Cdp.ahk

global g_cdp := ""

WebGui, Main:New, w820 h640 Center, CDP 웹 제어 데모
WebGui, Main:Expose, Go
WebGui, Main:Expose, RunJs
WebGui, Main:Html, % PageHtml()
WebGui, Main:Show
return

; {url} 로 이동 후 페이지 정보를 추출해 JSON 으로 반환.
Go(params) {
    url := JGet(params, "url")
    if (url = "")
        return Err("URL 을 입력하세요")
    try {
        tab := Conn().goto(url)          ; 실제 크롬 탭 이동 + 로드 대기
        title := tab.evaluate("document.title")
        cur   := tab.url
        nlink := tab.evaluate("document.links.length")
        heads := tab.evaluate("JSON.stringify([...document.querySelectorAll('h1,h2')].map(e=>e.textContent.trim()).filter(Boolean).slice(0,12))")
        return "{""ok"":true,""title"":" JStr(title) ",""url"":" JStr(cur) ",""links"":" (nlink+0) ",""heads"":" (heads="" ? "[]" : heads) "}"
    } catch e {
        return Err(ConnHint(e))
    }
}

; 임의 JS 를 활성 탭에서 실행하고 결과를 반환(간단 콘솔).
RunJs(params) {
    js := JGet(params, "js")
    if (js = "")
        return Err("JS 를 입력하세요")
    try {
        res := Conn().activeTab().evaluate(js)
        return "{""ok"":true,""result"":" JStr(res "") "}"
    } catch e {
        return Err(ConnHint(e))
    }
}

MainWebGuiClose:
ExitApp

; --- CDP 연결(지연 생성, 1회) ---
Conn() {
    global g_cdp
    if (!IsObject(g_cdp))
        g_cdp := new Cdp()               ; 포트 19222, 확장 자동 연결(최대 15초)
    return g_cdp
}
ConnHint(e) {
    msg := IsObject(e) ? e.Message : e
    if InStr(msg, "연결") or InStr(msg, "19222") or InStr(msg, "timeout") or InStr(msg, "WsServer")
        return "크롬 확장(cdp_extension)이 로드되었는지 확인하세요. " msg
    return msg
}

JGet(json, key) {
    if RegExMatch(json, """" key """\s*:\s*""([^""]*)""", m)
        return m1
    return ""
}
JStr(v) {   ; 문자열 -> JSON 리터럴(따옴표+이스케이프)
    v := StrReplace(v, "\", "\\")
    v := StrReplace(v, """", "\""")
    v := StrReplace(v, "`n", "\n")
    v := StrReplace(v, "`r", "")
    v := StrReplace(v, "`t", "\t")
    return """" v """"
}
Err(msg) {
    return "{""ok"":false,""error"":" JStr(msg) "}"
}

PageHtml() {
    s =
( LTrim Join`r`n
<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><style>
  body{font-family:"Segoe UI","Malgun Gothic",sans-serif;margin:24px;color:#222}
  h1{color:#3F5770;margin:0 0 4px} p.sub{color:#777;margin:0 0 16px}
  .row{display:flex;gap:8px;margin-bottom:10px}
  input{flex:1;font-size:15px;padding:9px;border:1px solid #ccc;border-radius:8px}
  button{font-size:15px;padding:9px 16px;background:#3F5770;color:#fff;border:0;border-radius:8px;cursor:pointer}
  .panel{background:#f6f7f9;border:1px solid #e3e6ea;border-radius:10px;padding:14px;margin-top:12px}
  .k{color:#3F5770;font-weight:700} .err{color:#A04040}
  ul{margin:6px 0 0 18px} li{margin:2px 0}
  textarea{width:100__P__;height:70px;font-size:14px;font-family:Consolas,monospace;padding:8px;box-sizing:border-box;border:1px solid #ccc;border-radius:8px}
  #js_out{white-space:pre-wrap;font-family:Consolas,monospace;color:#333;margin-top:6px}
</style></head>
<body>
  <h1>CDP 웹 제어</h1>
  <p class="sub">실제 크롬 탭을 AutoHotkey Web UI 로 제어 (엔진 내장 CDP + 번들 Cdp.ahk)</p>
  <div class="row">
    <input id="url" placeholder="https://example.com" value="https://example.com">
    <button id="go">이동 + 분석</button>
  </div>
  <div id="out" class="panel" style="display:none"></div>

  <h3 style="margin:18px 0 6px;color:#3F5770">임의 JS 실행 (활성 탭)</h3>
  <textarea id="js">document.title + " / links:" + document.links.length</textarea>
  <div class="row"><button id="runjs">실행</button></div>
  <div id="js_out"></div>
<script>
  const $ = id => document.getElementById(id);
  $('go').onclick = async () => {
    const out = $('out'); out.style.display='block'; out.innerHTML='이동 중...';
    const r = JSON.parse(await ahk.call("Go", { url: $('url').value }));
    if (!r.ok){ out.innerHTML = '<span class="err">오류: '+r.error+'</span>'; return; }
    let h = (r.heads||[]).map(t => '<li>'+t.replace(/</g,'&lt;')+'</li>').join('');
    out.innerHTML =
      '<div><span class="k">제목</span> '+r.title+'</div>'+
      '<div><span class="k">URL</span> '+r.url+'</div>'+
      '<div><span class="k">링크 수</span> '+r.links+'</div>'+
      '<div><span class="k">머리말(h1/h2)</span><ul>'+(h||'<li>(없음)</li>')+'</ul></div>';
  };
  $('runjs').onclick = async () => {
    const r = JSON.parse(await ahk.call("RunJs", { js: $('js').value }));
    $('js_out').textContent = r.ok ? ('= ' + r.result) : ('오류: ' + r.error);
  };
</script>
</body></html>
)
    s := StrReplace(s, "__P__", "%")
    return s
}
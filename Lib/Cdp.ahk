; ============================================================================
;  Cdp.ahk  -  AutoHotkey Web 용 CDP(크롬 DevTools Protocol) 브라우저 제어 라이브러리
;  별도 DLL 불필요: WsServer / JsonGet / Base64Decode 는 AutoHotkey Web 엔진
;  내장 함수를 호출합니다. (순수 AHK JSON 유틸은 아래에 함께 번들)
;
;  준비(1회): 크롬 chrome://extensions → 개발자 모드 →
;             "압축해제된 확장 프로그램 로드" → (플러그인)\cdp_extension 폴더 선택.
;  사용:      cdp := new Cdp()  /  tab := cdp.goto("https://..")  /  tab.evaluate("..")
; ============================================================================

; ---- 네이티브 어댑터: 플러그인 Note_* 이름을 엔진 내장 함수로 매핑 ----
Note_Init(dll := "") {
    return 1    ; 네이티브 엔진 - DLL 로드 불필요
}
Note_WsServerAvailable() {
    return WsServerAvailable()
}
Note_WsServerStart(port := 19222) {
    return WsServerStart(port)
}
Note_WsServerStop() {
    return WsServerStop()
}
Note_WsServerSend(text) {
    return WsServerSend(text)
}
Note_WsServerRecv(timeoutMs := 0) {
    return WsServerRecv(timeoutMs)
}
Note_WsServerClients() {
    return WsServerClients()
}
Note_WsServerLastError() {
    return WsServerLastError()
}
Note_WsServerPort() {
    return WsServerPort()
}
Note_WsServerPending() {
    return WsServerPending()
}
Note_JsonGet(json, path := "") {
    return JsonGet(json, path)
}
Note_Base64Decode(b64) {
    return Base64Decode(b64)
}

; ---- 순수 AHK JSON 유틸 (Note_ArrToJson / Note_ArrFromJson) ----
Note_ArrCount(a) {
    n := 0
    if (IsObject(a))
        for k in a
            n += 1
    return n
}
Note_ArrIsLinear(a) {
    if (!IsObject(a))
        return false
    n := 0
    for k in a {
        n += 1
        if (IsObject(k) || k != n)
            return false
    }
    return true
}
Note_ArrWrap(a) {
    return a    ; AutoHotkey Web 번들: 순서유지맵(wrap) 미사용 - CDP 는 평범한 객체만 다룸
}
Note_ArrToJson(a, pretty := true, indent := "  ") {
    return _Note_ArrJsonEnc(a, pretty ? indent : "", "")
}
_Note_ArrJsonEnc(v, indent, cur) {
    if (!IsObject(v))
        return _Note_ArrJsonScalar(v)
    nl := (indent != "") ? "`n" : ""
    inner := cur . indent
    if (Note_ArrIsLinear(v)) {
        if (!Note_ArrCount(v))
            return "[]"
        out := "["
        for i, x in v
            out .= (i > 1 ? "," : "") . nl . inner . _Note_ArrJsonEnc(x, indent, inner)
        return out . nl . cur . "]"
    }
    out := "{", n := 0
    for k, x in v
        out .= (n++ ? "," : "") . nl . inner . _Note_ArrJsonStr(k) . ((indent != "") ? ": " : ":") . _Note_ArrJsonEnc(x, indent, inner)
    return out . nl . cur . "}"
}
_Note_ArrJsonScalar(v) {
    if (v = "")
        return """"""
    if (RegExMatch(v, "^-?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?$"))
        return _Note_ArrNumStr(v)
    return _Note_ArrJsonStr(v)
}
; AHK v1 은 실수를 "1.500000" 처럼 문자열화하므로 소수부 끝의 0 을 정리 ("2.000000" → "2.0")
_Note_ArrNumStr(v) {
    if (RegExMatch(v, "^-?\d+\.\d*?0+$")) {
        v := RegExReplace(v, "0+$")
        if (SubStr(v, 0) = ".")
            v .= "0"
    }
    return v
}
_Note_ArrJsonStr(s) {
    s := StrReplace(s, "\", "\\")
    s := StrReplace(s, """", "\""")
    s := StrReplace(s, "`n", "\n")
    s := StrReplace(s, "`r", "\r")
    s := StrReplace(s, "`t", "\t")
    s := StrReplace(s, Chr(8), "\b")
    s := StrReplace(s, Chr(12), "\f")
    pos := 1
    while (q := RegExMatch(s, "[\x00-\x1F]", c, pos)) {
        rep := Format("\u{:04X}", Ord(c))
        s := SubStr(s, 1, q - 1) . rep . SubStr(s, q + 1)
        pos := q + StrLen(rep)
    }
    return """" . s . """"
}
; ▶ JSON 문자열 → 배열/객체 (순수 AHK 파서). true/false → 1/0, null → "". wrap=true 면 메소드 부착.
;   ordered=true 면 객체를 순서 유지 맵(NoteOMap)으로 만들어 키 순서를 보존(다시 ToJson 하면 원래 순서).
;   잘못된 JSON 이면 예외(throw). 최상위가 스칼라여도 그대로 반환.
Note_ArrFromJson(json, wrap := false, ordered := false) {
    if (SubStr(json, 1, 1) = Chr(0xFEFF))
        json := SubStr(json, 2)
    p := 1
    _Note_ArrJsonWs(json, p)
    v := _Note_ArrJsonVal(json, p, ordered)
    return (wrap && IsObject(v)) ? Note_ArrWrap(v) : v
}
_Note_ArrJsonWs(ByRef s, ByRef p) {
    while (RegExMatch(s, "\G[ \t\r\n]+", m, p))
        p += StrLen(m)
}
_Note_ArrJsonVal(ByRef s, ByRef p, ordered := false) {
    c := SubStr(s, p, 1)
    if (c = "{") {
        obj := ordered ? new NoteOMap() : {}
        p += 1
        _Note_ArrJsonWs(s, p)
        if (SubStr(s, p, 1) = "}") {
            p += 1
            return obj
        }
        Loop {
            _Note_ArrJsonWs(s, p)
            if (SubStr(s, p, 1) != """")
                throw Exception("JSON: 키는 문자열이어야 합니다 (위치 " p ")")
            k := _Note_ArrJsonString(s, p)
            _Note_ArrJsonWs(s, p)
            if (SubStr(s, p, 1) != ":")
                throw Exception("JSON: ':' 가 필요합니다 (위치 " p ")")
            p += 1
            _Note_ArrJsonWs(s, p)
            obj[k] := _Note_ArrJsonVal(s, p, ordered)
            _Note_ArrJsonWs(s, p)
            c := SubStr(s, p, 1)
            p += 1
            if (c = "}")
                return obj
            if (c != ",")
                throw Exception("JSON: ',' 또는 '}' 가 필요합니다 (위치 " p - 1 ")")
        }
    }
    if (c = "[") {
        arr := []
        p += 1
        _Note_ArrJsonWs(s, p)
        if (SubStr(s, p, 1) = "]") {
            p += 1
            return arr
        }
        Loop {
            _Note_ArrJsonWs(s, p)
            arr.Push(_Note_ArrJsonVal(s, p, ordered))
            _Note_ArrJsonWs(s, p)
            c := SubStr(s, p, 1)
            p += 1
            if (c = "]")
                return arr
            if (c != ",")
                throw Exception("JSON: ',' 또는 ']' 가 필요합니다 (위치 " p - 1 ")")
        }
    }
    if (c = """")
        return _Note_ArrJsonString(s, p)
    if (SubStr(s, p, 4) == "true") {
        p += 4
        return 1
    }
    if (SubStr(s, p, 5) == "false") {
        p += 5
        return 0
    }
    if (SubStr(s, p, 4) == "null") {
        p += 4
        return ""
    }
    if (RegExMatch(s, "\G-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?", m, p)) {
        p += StrLen(m)
        if (InStr(m, "e")) {   ; 지수 표기(3e2, -1.5E-3): v1 숫자 변환이 불안정하므로 직접 계산
            parts := StrSplit(m, ["e", "E"])
            return (parts[1] + 0.0) * (10.0 ** (parts[2] + 0))
        }
        return m + 0
    }
    throw Exception("JSON: 예상치 못한 문자 '" c "' (위치 " p ")")
}
_Note_ArrJsonString(ByRef s, ByRef p) {
    if (!RegExMatch(s, "\G""((?:[^""\\]|\\.)*)""", m, p))
        throw Exception("JSON: 닫히지 않은 문자열 (위치 " p ")")
    p += StrLen(m)
    return InStr(m1, "\") ? _Note_ArrJsonUnescape(m1) : m1
}
_Note_ArrJsonUnescape(t) {
    out := "", pos := 1
    while (q := RegExMatch(t, "\\(u[0-9a-fA-F]{4}|.)", e, pos)) {
        out .= SubStr(t, pos, q - pos)
        c := e1
        len := StrLen(e)
        if (SubStr(c, 1, 1) == "u") {
            cp := ("0x" . SubStr(c, 2)) + 0
            if (cp >= 0xD800 && cp <= 0xDBFF && SubStr(t, q + 6, 2) == "\u") {
                lo := ("0x" . SubStr(t, q + 8, 4)) + 0
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                    cp := 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00), len += 6
            }
            out .= Chr(cp)
        } else if (c == "n")
            out .= "`n"
        else if (c == "r")
            out .= "`r"
        else if (c == "t")
            out .= "`t"
        else if (c == "b")
            out .= Chr(8)
        else if (c == "f")
            out .= Chr(12)
        else
            out .= c
        pos := q + len
    }
    return out . SubStr(t, pos)
}

; ---------------------------------------------------------------- 표 데이터 정규화 ----
; 무엇이든 → {headers:[...], rows:[[cell,...],...], numeric:[열별 숫자여부]}
;   맵(스칼라) → 키|값,  선형(스칼라) → #|값,  객체 배열 → 열=키 합집합,  2차원 배열 → 1..m

; ============================================================================
;  Cdp.ahk — 크롬 확장(cdp_extension) 브리지 기반 브라우저 제어 (AHK v1 64비트)
;  드라이버/디버깅 포트 없이 "이미 열려 있는" 크롬/엣지에 붙는다.
;
;  구조:  AHK(Cdp.ahk) ⇄ ws://127.0.0.1:19222 (엔진 WsServer) ⇄ 크롬 확장 ⇄ 탭
;  준비:  1) 엔진으로 실행(WsServer 내장)  2) chrome://extensions → 개발자 모드 →
;         "압축해제된 확장 프로그램 로드" → 플러그인\cdp_extension 폴더 선택 (1회)
;
;  API 는 puppeteer / jQuery 이름을 따른다 (JS 하던 사람이 바로 쓰도록):
;     cdp := new Cdp()                       ; 서버 시작 + 확장 접속 대기
;     tab := cdp.tab("naver.com")            ; url/제목 부분일치, 숫자=tabId, ""=활성탭, "/정규식/"
;     tab := cdp.open("https://...")         ; 새 탭 (로드 완료까지 대기)
;     tab.goto(url)  tab.reload()  tab.goBack()  tab.activate()  tab.close()
;     tab.title  tab.url                     ; 프로퍼티
;     tab.evaluate("document.title")         ; JS 실행(값 반환). 여러 줄/return 포함 코드는 자동으로 async 함수로 감쌈
;     tab.q("#q").val("검색어").focus()      ; jQuery 식 체이닝  (.text() .html() .attr() .css() .prop() .click() .type())
;                                            ;   q = querySelector, qa = querySelectorAll  (AHK v1 은 메서드명에 $ 불가)
;     tab.qa("a")                            ; 요소 배열,  tab.evalAll("a", "els => els.map(e => e.href)")  (= puppeteer $$eval)
;     tab.waitForSelector("#result", 5000)   ; 나타날 때까지
;     tab.type("#q", "hello")  tab.click("#btn")  tab.press("Enter")
;     tab.screenshot("shot.png")  tab.pdf("page.pdf")  tab.cookies()
;     tab.send("Network.enable")             ; 원시 CDP
;     tab.on("cdp:Runtime.consoleAPICalled", Func("MyHandler"))
;
;  evaluate 는 기본적으로 chrome.debugger(Runtime.evaluate) 를 사용 → 탭 상단에 "디버깅 중" 배너.
;  cdp.silent := true 로 두면 chrome.scripting 을 우선 사용해 배너 없이 실행(페이지 CSP 가 막으면 자동 폴백).
; ============================================================================
; (AutoHotkey Web: Notepad.ahk 의존 제거 - Note_* 는 이 파일 상단에 번들됨)

class Cdp {
    static DefaultPort := 19222
    static _instance := ""

    ; port: 확장 옵션의 포트와 동일해야 함. connectTimeoutMs: 확장 접속 대기(0=대기 안 함)
    __New(port := "", connectTimeoutMs := 15000, dllPath := "") {
        global _NotepadModule
        if (!_NotepadModule)
            Note_Init(dllPath)
        this.port := (port = "") ? Cdp.DefaultPort : port
        this.timeout := 30000          ; 요청 기본 타임아웃(ms)
        this.silent := false           ; true: 배너 없는 scripting eval 우선
        this._seq := 0
        this._pending := {}
        this._handlers := {}
        this._eventLog := []
        this._eventLogMax := 500
        this.ext := ""
        if (!Note_WsServerStart(this.port))
            throw Exception("WebSocket 서버 시작 실패(포트 " this.port "): " Note_WsServerLastError(), -1)
        Cdp._instance := &this
        if (connectTimeoutMs > 0)
            this.waitForConnection(connectTimeoutMs)
    }
    __Delete() {
        Note_WsServerStop()
    }
    ; 확장이 붙어 있는지
    connected[] {
        get {
            return Note_WsServerClients() > 0
        }
    }
    ; 확장 접속 대기. 실패 시 예외
    waitForConnection(timeoutMs := 15000) {
        t := A_TickCount
        while (!this.connected) {
            if (A_TickCount - t > timeoutMs)
                throw Exception("크롬 확장이 접속하지 않습니다(포트 " this.port "). 확장 로드/포트를 확인하세요.", -1)
            this.pump(50)
        }
        this.pump(150)          ; hello 등 소비
        return true
    }
    ; 서버 종료(스크립트 종료 시 자동)
    close() {
        Note_WsServerStop()
    }
    ; 확장/크롬 버전 정보 객체
    version() {
        v := this.request("version", "")
        if (IsObject(v) && v.ext != "")
            this.ext := v                 ; hello 이벤트를 놓쳤어도 ext 채움
        return v
    }
    ; 왕복 확인(ms)
    ping() {
        t := A_TickCount
        this.request("echo", """data"":1")
        return A_TickCount - t
    }

    ; ------------------------------------------------------------ 탭 ----
    ; 모든 탭 → CdpTab 배열
    tabs() {
        out := []
        for i, info in this.request("tabs", "")
            out.Push(new CdpTab(this, info))
        return out
    }
    ; 활성 탭
    activeTab() {
        return new CdpTab(this, this.request("activeTab", ""))
    }
    ; 탭 찾기(없으면 예외). match: ""=활성탭, 정수=tabId, "/re/"=정규식, 그 외 url·제목 부분일치(대소문자 무시)
    tab(match := "") {
        t := this.find(match)
        if (!IsObject(t))
            throw Exception("탭을 찾을 수 없음: " match, -1)
        return t
    }
    ; 탭 찾기(없으면 "")
    find(match := "") {
        if (IsObject(match))
            return match
        if (match = "")
            return this.activeTab()
        if match is integer
        {
            try return new CdpTab(this, this.request("tab", """tabId"":" match))
            catch
                return ""
        }
        isRe := (StrLen(match) > 2 && SubStr(match, 1, 1) = "/" && SubStr(match, 0) = "/")
        re := isRe ? "i)" SubStr(match, 2, -1) : ""
        for i, info in this.request("tabs", "") {
            if (isRe ? (RegExMatch(info.url, re) || RegExMatch(info.title, re))
                     : (InStr(info.url, match) || InStr(info.title, match)))
                return new CdpTab(this, info)
        }
        return ""
    }
    ; 같은 조건의 모든 탭
    findAll(match) {
        out := []
        isRe := (StrLen(match) > 2 && SubStr(match, 1, 1) = "/" && SubStr(match, 0) = "/")
        re := isRe ? "i)" SubStr(match, 2, -1) : ""
        for i, info in this.request("tabs", "")
            if (isRe ? (RegExMatch(info.url, re) || RegExMatch(info.title, re))
                     : (InStr(info.url, match) || InStr(info.title, match)))
                out.Push(new CdpTab(this, info))
        return out
    }
    ; 새 탭. waitLoad=true 면 로드 완료까지 대기
    open(url := "about:blank", active := true, waitLoad := true) {
        t := new CdpTab(this, this.request("open", """url"":" Cdp.q(url) ",""active"":" (active ? "true" : "false")))
        if (waitLoad && url != "about:blank")
            t.waitForLoad()
        return t
    }

    ; ------------------------------------------------------------ 전송 ----
    ; 요청 후 응답 원문(JSON) 반환. fields: 추가 멤버 JSON 조각 ("\"tabId\":3,\"url\":\"..\"")
    requestRaw(cmd, fields := "", timeoutMs := "") {
        id := ++this._seq
        json := "{""id"":" id ",""cmd"":" Cdp.q(cmd) (fields != "" ? "," fields : "") "}"
        if (!Note_WsServerSend(json))
            throw Exception("크롬 확장이 연결되어 있지 않습니다(포트 " this.port ")", -1)
        timeoutMs := (timeoutMs = "") ? this.timeout : timeoutMs
        t := A_TickCount
        Loop {
            ; 중첩 요청(이벤트 핸들러 안에서 request) 중 다른 요청의 응답이 먼저 오면 보관해 두었다가 여기서 회수
            if (this._pending.HasKey(id)) {
                msg := this._pending.Delete(id)
            } else {
                remain := timeoutMs - (A_TickCount - t)
                if (remain <= 0)
                    throw Exception("응답 시간 초과(" timeoutMs "ms): " cmd, -1)
                msg := Note_WsServerRecv(remain > 1000 ? 1000 : remain)
                if (msg = "")
                    continue
                rid := Note_JsonGet(msg, "id")
                if (rid = "") {
                    this._onEvent(msg)
                    continue
                }
                if (rid != id) {
                    this._pending[rid] := msg
                    continue
                }
            }
            if (Note_JsonGet(msg, "ok") = "true")
                return msg
            throw Exception(cmd ": " Note_JsonGet(msg, "error"), -1)
        }
    }
    ; 요청 후 result 를 AHK 값으로 반환(객체/배열은 파싱, 스칼라는 문자열, true/false→1/0, null→"")
    request(cmd, fields := "", timeoutMs := "") {
        return Cdp._resultValue(this.requestRaw(cmd, fields, timeoutMs))
    }
    ; 응답 원문에서 result 값 추출
    _resultValue(msg) {
        if (!RegExMatch(msg, "^\{""id"":\d+,""ok"":true,""result"":(.)", m))
            return ""
        c := m1
        if (c = "n")
            return ""
        if (c = "t")
            return 1
        if (c = "f")
            return 0
        v := Note_JsonGet(msg, "result")
        if (c = "{" || c = "[")
            return Note_ArrFromJson(v)
        return v
    }
    ; JSON 문자열 리터럴로 인용 (JS 문자열 리터럴로도 사용 가능)
    q(s) {
        return _Note_ArrJsonStr(s)
    }
    ; JSON 값 조각: 숫자면 그대로, 그 외 인용
    v(x) {
        if x is number
            return x
        return _Note_ArrJsonStr(x)
    }
    ; AHK 객체 → JSON (숫자 모양 문자열은 숫자가 됨에 주의). JS 불리언이 필요하면 Cdp.jsTrue / Cdp.jsFalse
    json(obj) {
        if (!IsObject(obj))
            return Cdp.v(obj)
        s := Note_ArrToJson(obj, false)
        s := StrReplace(s, "{""__cdpbool"":1}", "true")
        s := StrReplace(s, "{""__cdpbool"":0}", "false")
        return s
    }
    static jsTrue := {__cdpbool: 1}
    static jsFalse := {__cdpbool: 0}
    ; 실행 코드 정규화: return/여러 줄/세미콜론 포함 → (async()=>{...})() 로 감싸 값 반환
    wrapCode(js) {
        if (RegExMatch(js, "^\s*return\b") || InStr(js, "`n") || RegExMatch(js, ";\s*\S"))
            return "(async () => {" js "`n})()"
        return js
    }

    ; ------------------------------------------------------------ 이벤트 ----
    ; 대기 중인 이벤트 처리(핸들러 호출). waitMs 동안 새 메시지도 기다림
    pump(waitMs := 0) {
        n := 0
        Loop {
            msg := Note_WsServerRecv(waitMs)
            if (msg = "")
                break
            waitMs := 0
            rid := Note_JsonGet(msg, "id")
            if (rid != "") {
                this._pending[rid] := msg                ; 진행 중인 요청의 응답 → 보관
                continue
            }
            this._onEvent(msg)
            n++
        }
        return n
    }
    ; 이벤트 자동 처리 타이머(ms). 0 이면 해제. 핸들러가 request 를 호출해도 안전(재진입 방지)
    autoPump(intervalMs := 100) {
        fn := this._pumpFn ? this._pumpFn : (this._pumpFn := ObjBindMethod(this, "_timerPump"))
        if (intervalMs > 0)
            SetTimer, % fn, % intervalMs
        else
            SetTimer, % fn, Off
    }
    _timerPump() {
        if (this._pumping)
            return
        this._pumping := true
        try this.pump(0)
        finally this._pumping := false
    }
    ; 핸들러 등록. name: "navigated" "loaded" "detach" "tabRemoved" "hello" "cdp"(모든 CDP) "cdp:Runtime.consoleAPICalled" ...
    ;   fn(ev) — ev: {event, tabId, method, params, ...}. tabId 지정 시 그 탭 이벤트만
    on(name, fn, tabId := "") {
        if (!this._handlers.HasKey(name))
            this._handlers[name] := []
        this._handlers[name].Push({fn: fn, tabId: tabId})
        return this
    }
    off(name := "") {
        if (name = "")
            this._handlers := {}
        else
            this._handlers.Delete(name)
        return this
    }
    _onEvent(msg) {
        ev := Note_JsonGet(msg, "event")
        if (ev = "ping" || ev = "__open" || ev = "__close")
            return
        obj := Note_ArrFromJson(msg)
        if (ev = "hello") {
            this.ext := obj
            return
        }
        this._eventLog.Push(obj)
        if (this._eventLog.Length() > this._eventLogMax)
            this._eventLog.RemoveAt(1)
        keys := [ev]
        if (ev = "cdp")
            keys.Push("cdp:" obj.method)
        for i, k in keys {
            if (!this._handlers.HasKey(k))
                continue
            for j, h in this._handlers[k] {
                if (h.tabId != "" && h.tabId != obj.tabId)
                    continue
                h.fn.Call(obj)
            }
        }
    }
    ; 이벤트가 올 때까지 대기 → 이벤트 객체, 시간 초과 "" . name 규칙은 on() 과 동일
    waitForEvent(name, timeoutMs := 10000, tabId := "") {
        start := this._eventLog.Length()
        t := A_TickCount
        Loop {
            this.pump(50)
            n := this._eventLog.Length()
            Loop % (n - start) {
                e := this._eventLog[start + A_Index]
                k := (e.event = "cdp") ? "cdp:" e.method : e.event
                if ((k = name || e.event = name) && (tabId = "" || tabId = e.tabId))
                    return e
            }
            start := n
            if (A_TickCount - t > timeoutMs)
                return ""
        }
    }
    ; Base64 → 파일 (스크린샷/PDF 저장)
    _base64ToFile(b64, path) {
        size := 0
        if (!DllCall("Crypt32.dll\CryptStringToBinary", "Str", b64, "UInt", 0, "UInt", 1, "Ptr", 0, "UInt*", size, "Ptr", 0, "Ptr", 0))
            return false
        VarSetCapacity(buf, size, 0)
        if (!DllCall("Crypt32.dll\CryptStringToBinary", "Str", b64, "UInt", 0, "UInt", 1, "Ptr", &buf, "UInt*", size, "Ptr", 0, "Ptr", 0))
            return false
        f := FileOpen(path, "w")
        if (!IsObject(f))
            return false
        f.RawWrite(buf, size)
        f.Close()
        return true
    }
}

; ============================================================================
;  CdpTab — 탭 하나 (puppeteer 의 Page 에 해당)
; ============================================================================
class CdpTab {
    __New(cdp, info) {
        this.cdp := cdp
        this.id := info.id
        this._info := info
    }
    _t() {
        return """tabId"":" this.id
    }
    ; 최신 탭 정보 {id,url,title,active,windowId,index,status}
    info() {
        this._info := this.cdp.request("tab", this._t())
        return this._info
    }
    url[] {
        get {
            return this.info().url
        }
    }
    title[] {
        get {
            return this.info().title
        }
    }
    status[] {
        get {
            return this.info().status
        }
    }
    ; 탭이 아직 열려 있는지
    isOpen[] {
        get {
            try return (this.info().id = this.id)
            catch
                return false
        }
    }

    ; ------------------------------------------------------------ 이동 ----
    goto(url, waitLoad := true, timeoutMs := 30000) {
        this.cdp.request("navigate", this._t() ",""url"":" Cdp.q(url))
        if (waitLoad) {
            Sleep, 150
            this.waitForLoad(timeoutMs)
        }
        return this
    }
    navigate(url, waitLoad := true, timeoutMs := 30000) {
        return this.goto(url, waitLoad, timeoutMs)
    }
    reload(bypassCache := false, waitLoad := true, timeoutMs := 30000) {
        this.cdp.request("reload", this._t() ",""bypassCache"":" (bypassCache ? "true" : "false"))
        if (waitLoad) {
            Sleep, 150
            this.waitForLoad(timeoutMs)
        }
        return this
    }
    goBack(waitLoad := true) {
        this.cdp.request("back", this._t())
        if (waitLoad) {
            Sleep, 150
            this.waitForLoad()
        }
        return this
    }
    goForward(waitLoad := true) {
        this.cdp.request("forward", this._t())
        if (waitLoad) {
            Sleep, 150
            this.waitForLoad()
        }
        return this
    }
    ; 탭 활성화 + 창 앞으로
    activate() {
        this._info := this.cdp.request("activate", this._t())
        return this
    }
    bringToFront() {
        return this.activate()
    }
    close() {
        return this.cdp.request("close", this._t())
    }
    ; 로드 완료(탭 status=complete && document.readyState=complete) 대기
    waitForLoad(timeoutMs := 30000) {
        t := A_TickCount
        Loop {
            ok := false
            try {
                if (this.info().status = "complete")
                    ok := (this.evaluate("document.readyState") = "complete")
            }
            if (ok)
                return this
            if (A_TickCount - t > timeoutMs)
                throw Exception("페이지 로드 시간 초과(" timeoutMs "ms): " this._info.url, -1)
            Sleep, 100
        }
    }
    ; 다음 페이지 이동(loaded 이벤트) 대기 — 클릭으로 이동하는 경우: tab.click("a"), tab.waitForNavigation()
    waitForNavigation(timeoutMs := 30000) {
        e := this.cdp.waitForEvent("loaded", timeoutMs, this.id)
        if (!IsObject(e))
            throw Exception("페이지 이동 대기 시간 초과(" timeoutMs "ms)", -1)
        return this.waitForLoad(timeoutMs)
    }
    ; 창 상태 "normal"|"minimized"|"maximized"|"fullscreen"
    windowState(state) {
        return this.cdp.request("windowState", this._t() ",""state"":" Cdp.q(state))
    }
    ; 줌 배율 조회/설정 (1 = 100%)
    zoom(factor*) {
        return this.cdp.request("zoom", this._t() (factor.Length() ? ",""factor"":" factor[1] : ""))
    }
    ; 프레임 목록 [{frameId,parentFrameId,url}]
    frames() {
        return this.cdp.request("frames", this._t())
    }

    ; ------------------------------------------------------------ JS ----
    ; JS 실행 후 값 반환. 표현식 하나면 그대로, return/여러 줄이면 async 함수 본문으로 취급.
    ;   cdp.silent=true 면 배너 없는 scripting eval 우선(페이지 CSP 가 eval 을 막으면 debugger 로 폴백)
    evaluate(js, timeoutMs := "") {
        return this.evalExpr(Cdp.wrapCode(js), timeoutMs)
    }
    eval(js, timeoutMs := "") {
        return this.evaluate(js, timeoutMs)
    }
    ; 이미 "값을 내는 JS 식"인 코드를 그대로 실행(wrapCode 안 함). 라이브러리 내부(IIFE)용.
    evalExpr(code, timeoutMs := "") {
        if (this.cdp.silent) {
            try return this.evalScript(code, "MAIN", "", timeoutMs)
            catch e {
                if (!RegExMatch(e.Message, "i)unsafe-eval|Content Security Policy|EvalError|Refused to evaluate"))
                    throw e
            }
        }
        msg := this.sendRaw("Runtime.evaluate", "{""expression"":" Cdp.q(code)
            . ",""returnByValue"":true,""awaitPromise"":true,""userGesture"":true}", timeoutMs)
        if (Note_JsonGet(msg, "result.exceptionDetails") != "") {
            desc := Note_JsonGet(msg, "result.exceptionDetails.exception.description")
            if (desc = "")
                desc := Note_JsonGet(msg, "result.exceptionDetails.text")
            throw Exception("JS 오류: " desc, -1)
        }
        return CdpTab._remoteValue(msg, "result.result")
    }
    ; 값을 AHK 객체로 파싱하지 않고 반환(대용량 데이터용): 객체/배열은 JSON 원문, 스칼라는 문자열
    evaluateJson(js, timeoutMs := "") {
        code := Cdp.wrapCode(js)
        msg := this.sendRaw("Runtime.evaluate", "{""expression"":" Cdp.q(code)
            . ",""returnByValue"":true,""awaitPromise"":true,""userGesture"":true}", timeoutMs)
        if (Note_JsonGet(msg, "result.exceptionDetails") != "")
            throw Exception("JS 오류: " Note_JsonGet(msg, "result.exceptionDetails.exception.description"), -1)
        return Note_JsonGet(msg, "result.result.value")
    }
    ; chrome.scripting 으로 실행(디버거 배너 없음). world: "MAIN"|"ISOLATED", frameId 지정 가능
    evalScript(js, world := "MAIN", frameId := "", timeoutMs := "") {
        f := this._t() ",""code"":" Cdp.q(js) ",""world"":" Cdp.q(world)
        if (frameId != "")
            f .= ",""frameId"":" frameId
        return this.cdp.request("eval", f, timeoutMs)
    }
    ; 원시 CDP 명령 → result 객체. params: JSON 문자열 또는 AHK 객체
    send(method, params := "", timeoutMs := "") {
        return Cdp._resultValue(this.sendRaw(method, params, timeoutMs))
    }
    sendRaw(method, params := "", timeoutMs := "") {
        p := IsObject(params) ? Cdp.json(params) : (params = "" ? "{}" : params)
        return this.cdp.requestRaw("cdp", this._t() ",""method"":" Cdp.q(method) ",""params"":" p, timeoutMs)
    }
    ; Runtime.RemoteObject → AHK 값
    _remoteValue(msg, base) {
        type := Note_JsonGet(msg, base ".type")
        if (type = "undefined" || type = "")
            return ""
        if (type = "string")
            return Note_JsonGet(msg, base ".value")
        if (type = "number") {
            v := Note_JsonGet(msg, base ".value")
            return (v = "") ? Note_JsonGet(msg, base ".unserializableValue") : v
        }
        if (type = "boolean")
            return (Note_JsonGet(msg, base ".value") = "true") ? 1 : 0
        if (type = "object") {
            if (Note_JsonGet(msg, base ".subtype") = "null")
                return ""
            v := Note_JsonGet(msg, base ".value")
            return (v = "") ? "" : Note_ArrFromJson(v)
        }
        return Note_JsonGet(msg, base ".description")
    }
    ; 디버거 연결 관리 (명령 시 자동 attach 되므로 보통 불필요). detach 하면 배너가 사라짐
    attach() {
        return this.cdp.request("attach", this._t())
    }
    detach() {
        return this.cdp.request("detach", this._t())
    }
    attached[] {
        get {
            return this.cdp.request("attached", this._t())
        }
    }

    ; ------------------------------------------------------------ DOM ----
    ; 첫 번째 일치 요소 핸들 (존재 확인은 .exists). idx: 0-기반 n번째.  (AHK v1 은 메서드명에 $ 를 못 써서 puppeteer 의 $ 대신 DOM 이름 그대로)
    querySelector(sel, idx := 0) {
        return new CdpEl(this, "document.querySelectorAll(" Cdp.q(sel) ")[" idx "]", sel)
    }
    q(sel, idx := 0) {
        return this.querySelector(sel, idx)
    }
    ; 모든 일치 요소 → CdpEl 배열
    querySelectorAll(sel) {
        n := this.evaluate("document.querySelectorAll(" Cdp.q(sel) ").length")
        out := []
        Loop % n
            out.Push(new CdpEl(this, "document.querySelectorAll(" Cdp.q(sel) ")[" (A_Index - 1) "]", sel))
        return out
    }
    qa(sel) {
        return this.querySelectorAll(sel)
    }
    ; XPath 로 첫 요소
    xpath(xp) {
        return new CdpEl(this, "document.evaluate(" Cdp.q(xp) ", document, null, 9, null).singleNodeValue", xp)
    }
    ; puppeteer $eval: fn 은 JS 함수 소스 ("el => el.href")
    evalOn(sel, fn) {
        return this.evaluate("(" fn ")(document.querySelector(" Cdp.q(sel) "))")
    }
    ; puppeteer $$eval: fn 은 배열을 받는 JS 함수 소스 ("els => els.map(e => e.textContent)")
    evalAll(sel, fn) {
        return this.evaluate("(" fn ")(Array.from(document.querySelectorAll(" Cdp.q(sel) ")))")
    }
    exists(sel) {
        return this.evaluate("!!document.querySelector(" Cdp.q(sel) ")")
    }
    count(sel) {
        return this.evaluate("document.querySelectorAll(" Cdp.q(sel) ").length")
    }
    ; 단축: 요소 핸들을 거치지 않는 한 줄 버전
    text(sel, v*) {
        return this.q(sel).text(v*)
    }
    html(sel, v*) {
        return this.q(sel).html(v*)
    }
    value(sel, v*) {
        return this.q(sel).val(v*)
    }
    attr(sel, name, v*) {
        return this.q(sel).attr(name, v*)
    }
    click(sel) {
        this.q(sel).click()
        return this
    }
    ; 입력 (React/Vue 호환: 네이티브 setter + input/change 이벤트). clear=false 면 뒤에 덧붙임
    type(sel, text, clear := true) {
        this.q(sel).type(text, clear)
        return this
    }
    focus(sel) {
        this.q(sel).focus()
        return this
    }
    select(sel, value) {
        this.q(sel).select(value)
        return this
    }
    check(sel, on := true) {
        this.q(sel).check(on)
        return this
    }
    scrollIntoView(sel) {
        this.q(sel).scrollIntoView()
        return this
    }
    ; 요소가 나타날 때까지 대기 → CdpEl. visible=true 면 화면에 보이는 것까지 확인. 실패 시 예외
    waitForSelector(sel, timeoutMs := 10000, visible := false, pollMs := 100) {
        js := "(function(){var el=document.querySelector(" Cdp.q(sel) ");if(!el)return false;"
            . (visible ? "var r=el.getBoundingClientRect();var s=getComputedStyle(el);return r.width>0&&r.height>0&&s.visibility!='hidden'&&s.display!='none';" : "return true;")
            . "})()"
        t := A_TickCount
        Loop {
            ok := false
            try ok := this.evalExpr(js)
            if (ok)
                return this.q(sel)
            if (A_TickCount - t > timeoutMs)
                throw Exception("요소 대기 시간 초과(" timeoutMs "ms): " sel, -1)
            Sleep, pollMs
        }
    }
    ; 요소가 사라질 때까지
    waitForSelectorGone(sel, timeoutMs := 10000, pollMs := 100) {
        t := A_TickCount
        Loop {
            gone := false
            try gone := !this.evalExpr("!!document.querySelector(" Cdp.q(sel) ")")
            if (gone)
                return this
            if (A_TickCount - t > timeoutMs)
                throw Exception("요소 소멸 대기 시간 초과: " sel, -1)
            Sleep, pollMs
        }
    }
    ; JS 식이 참이 될 때까지 → 그 값
    waitForFunction(js, timeoutMs := 10000, pollMs := 100) {
        t := A_TickCount
        Loop {
            v := ""
            try v := this.evaluate(js)
            if (v)
                return v
            if (A_TickCount - t > timeoutMs)
                throw Exception("조건 대기 시간 초과: " js, -1)
            Sleep, pollMs
        }
    }
    ; 본문에 텍스트가 나타날 때까지
    waitForText(text, timeoutMs := 10000, pollMs := 100) {
        this.waitForFunction("document.body && document.body.innerText.includes(" Cdp.q(text) ")", timeoutMs, pollMs)
        return this
    }
    ; 전체 HTML
    content() {
        return this.evaluate("document.documentElement.outerHTML")
    }
    ; 본문 텍스트
    innerText() {
        return this.evaluate("document.body ? document.body.innerText : ''")
    }
    ; ------------------------------------------------------------ 캡처/쿠키 ----
    ; 스크린샷 저장. path 생략 시 Base64 반환. fullPage=true 전체 페이지. format png|jpeg|webp
    screenshot(path := "", fullPage := false, format := "", quality := 90) {
        if (format = "")
            format := RegExMatch(path, "i)\.jpe?g$") ? "jpeg" : (RegExMatch(path, "i)\.webp$") ? "webp" : "png")
        p := """format"":" Cdp.q(format) ",""captureBeyondViewport"":" (fullPage ? "true" : "false")
        if (format != "png")
            p .= ",""quality"":" quality
        if (fullPage) {
            m := this.send("Page.getLayoutMetrics")
            cs := m.HasKey("cssContentSize") ? m.cssContentSize : m.contentSize
            p .= ",""clip"":{""x"":0,""y"":0,""width"":" cs.width ",""height"":" cs.height ",""scale"":1}"
        }
        b64 := Note_JsonGet(this.sendRaw("Page.captureScreenshot", "{" p "}"), "result.data")
        if (path = "")
            return b64
        if (!Cdp._base64ToFile(b64, path))
            throw Exception("스크린샷 저장 실패: " path, -1)
        return path
    }
    ; PDF 저장 (배경 포함, A4 세로). extraJson: Page.printToPDF 추가 옵션 조각
    pdf(path, landscape := false, extraJson := "") {
        p := "{""printBackground"":true,""landscape"":" (landscape ? "true" : "false") (extraJson != "" ? "," extraJson : "") "}"
        b64 := Note_JsonGet(this.sendRaw("Page.printToPDF", p), "result.data")
        if (!Cdp._base64ToFile(b64, path))
            throw Exception("PDF 저장 실패: " path, -1)
        return path
    }
    ; 쿠키 배열 [{name,value,domain,path,expires,httpOnly,secure,...}]. url 지정 시 그 URL 기준
    cookies(url := "") {
        r := this.send("Network.getCookies", url != "" ? "{""urls"":[" Cdp.q(url) "]}" : "{}")
        return r.cookies
    }
    ; 쿠키 하나 → 값 ("" 없음)
    cookie(name, url := "") {
        for i, c in this.cookies(url)
            if (c.name = name)
                return c.value
        return ""
    }
    ; 쿠키 설정. url 생략 시 현재 탭 URL. extraJson: "\"httpOnly\":true,\"expires\":1700000000"
    setCookie(name, value, url := "", extraJson := "") {
        if (url = "")
            url := this.url
        p := "{""name"":" Cdp.q(name) ",""value"":" Cdp.q(value) ",""url"":" Cdp.q(url) (extraJson != "" ? "," extraJson : "") "}"
        return this.send("Network.setCookie", p).success
    }
    deleteCookie(name, url := "") {
        if (url = "")
            url := this.url
        this.send("Network.deleteCookies", "{""name"":" Cdp.q(name) ",""url"":" Cdp.q(url) "}")
        return this
    }
    ; localStorage 읽기/쓰기
    localStorage(key, v*) {
        if (v.Length())
            return this.evaluate("localStorage.setItem(" Cdp.q(key) "," Cdp.q(v[1]) "), true")
        return this.evaluate("localStorage.getItem(" Cdp.q(key) ")")
    }

    ; ------------------------------------------------------------ 입력(실제 이벤트, Input.*) ----
    ; 좌표는 뷰포트 CSS 픽셀. 요소 중심 좌표는 el.center()
    mouseMove(x, y) {
        this.send("Input.dispatchMouseEvent", "{""type"":""mouseMoved"",""x"":" x ",""y"":" y "}")
        return this
    }
    ; button: left|right|middle. clickCount 2 = 더블클릭
    mouseClick(x, y, button := "left", clickCount := 1) {
        this.mouseMove(x, y)
        Loop % clickCount {
            this.send("Input.dispatchMouseEvent", "{""type"":""mousePressed"",""x"":" x ",""y"":" y ",""button"":" Cdp.q(button) ",""clickCount"":" A_Index "}")
            this.send("Input.dispatchMouseEvent", "{""type"":""mouseReleased"",""x"":" x ",""y"":" y ",""button"":" Cdp.q(button) ",""clickCount"":" A_Index "}")
        }
        return this
    }
    mouseWheel(x, y, deltaX := 0, deltaY := 300) {
        this.send("Input.dispatchMouseEvent", "{""type"":""mouseWheel"",""x"":" x ",""y"":" y ",""deltaX"":" deltaX ",""deltaY"":" deltaY "}")
        return this
    }
    ; 드래그 (from → to)
    mouseDrag(x1, y1, x2, y2, steps := 10) {
        this.mouseMove(x1, y1)
        this.send("Input.dispatchMouseEvent", "{""type"":""mousePressed"",""x"":" x1 ",""y"":" y1 ",""button"":""left"",""clickCount"":1}")
        Loop % steps
            this.mouseMove(Round(x1 + (x2 - x1) * A_Index / steps), Round(y1 + (y2 - y1) * A_Index / steps))
        this.send("Input.dispatchMouseEvent", "{""type"":""mouseReleased"",""x"":" x2 ",""y"":" y2 ",""button"":""left"",""clickCount"":1}")
        return this
    }
    ; 포커스된 요소에 텍스트 삽입(IME 입력처럼, 한글 OK)
    insertText(text) {
        this.send("Input.insertText", "{""text"":" Cdp.q(text) "}")
        return this
    }
    ; 한 글자씩 입력 (delayMs 간격)
    typeKeys(text, delayMs := 30) {
        Loop, Parse, text
        {
            this.insertText(A_LoopField)
            if (delayMs)
                Sleep, delayMs
        }
        return this
    }
    ; 키 누름. key: Enter Tab Escape Backspace Delete ArrowUp/Down/Left/Right Home End PageUp PageDown Space F1~F12 또는 한 글자
    ;   modifiers: 1=Alt 2=Ctrl 4=Meta 8=Shift (합산)
    press(key, modifiers := 0) {
        static vk := {Enter: 13, Tab: 9, Escape: 27, Backspace: 8, Delete: 46, Space: 32, Home: 36, End: 35
            , PageUp: 33, PageDown: 34, ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Insert: 45}
        text := ""
        if (vk.HasKey(key))
            code := vk[key]
        else if (RegExMatch(key, "^F(\d{1,2})$", m))
            code := 111 + m1
        else if (StrLen(key) = 1) {
            code := Ord(Format("{:U}", key))
            text := key
        } else {
            throw Exception("알 수 없는 키: " key, -1)
        }
        if (key = "Enter")
            text := "`r"
        if (key = "Space")
            text := " "
        keyName := key
        down := "{""type"":" (text != "" ? """keyDown""" : """rawKeyDown""") ",""key"":" Cdp.q(keyName) ",""windowsVirtualKeyCode"":" code ",""nativeVirtualKeyCode"":" code ",""modifiers"":" modifiers
        if (text != "")
            down .= ",""text"":" Cdp.q(text) ",""unmodifiedText"":" Cdp.q(text)
        this.send("Input.dispatchKeyEvent", down "}")
        this.send("Input.dispatchKeyEvent", "{""type"":""keyUp"",""key"":" Cdp.q(keyName) ",""windowsVirtualKeyCode"":" code ",""nativeVirtualKeyCode"":" code ",""modifiers"":" modifiers "}")
        return this
    }
    ; 뷰포트 좌표 → 화면 좌표 (AHK MouseMove/Click 연동용. 창 테두리·툴바 높이를 outer/inner 차이로 근사)
    viewportToScreen(x, y) {
        g := this.evaluate("({sx:window.screenX,sy:window.screenY,ow:window.outerWidth,iw:window.innerWidth,oh:window.outerHeight,ih:window.innerHeight,dpr:window.devicePixelRatio})")
        bx := (g.ow - g.iw) / 2
        by := g.oh - g.ih
        return {x: Round((g.sx + bx + x) * g.dpr), y: Round((g.sy + by + y) * g.dpr)}
    }
    ; ------------------------------------------------------------ 이벤트 ----
    ; 이 탭의 이벤트만. name 규칙은 Cdp.on 과 동일 ("cdp:Page.loadEventFired" 등). 도메인은 먼저 enable 필요
    on(name, fn) {
        this.cdp.on(name, fn, this.id)
        return this
    }
    waitForEvent(name, timeoutMs := 10000) {
        return this.cdp.waitForEvent(name, timeoutMs, this.id)
    }
    ; console.log 등 콘솔 출력 수신: fn(ev) — ev.params.type, ev.params.args[1].value
    onConsole(fn) {
        this.send("Runtime.enable")
        return this.on("cdp:Runtime.consoleAPICalled", fn)
    }
    ; 대화상자(alert/confirm/prompt) 자동 처리: accept=true 수락, promptText 입력값
    autoDialog(accept := true, promptText := "") {
        this.send("Page.enable")
        this._dialogFn := ObjBindMethod(this, "_onDialog", accept, promptText)
        return this.on("cdp:Page.javascriptDialogOpening", this._dialogFn)
    }
    _onDialog(accept, promptText, ev) {
        try this.send("Page.handleJavaScriptDialog", "{""accept"":" (accept ? "true" : "false") ",""promptText"":" Cdp.q(promptText) "}")
    }
    ; 네트워크 응답 감시 시작: fn(ev) — ev.params.response.url / status. urlFilter 부분일치
    onResponse(fn, urlFilter := "") {
        this.send("Network.enable")
        this._respFn := ObjBindMethod(this, "_onResp", fn, urlFilter)
        return this.on("cdp:Network.responseReceived", this._respFn)
    }
    _onResp(fn, urlFilter, ev) {
        if (urlFilter = "" || InStr(ev.params.response.url, urlFilter))
            fn.Call(ev)
    }
    ; 응답 본문 가져오기 (Network.responseReceived 의 requestId)
    responseBody(requestId) {
        r := this.send("Network.getResponseBody", "{""requestId"":" Cdp.q(requestId) "}")
        return r.base64Encoded ? Note_Base64Decode(r.body) : r.body
    }
    ; 파일 업로드: <input type=file> 에 로컬 파일 지정
    uploadFile(sel, paths*) {
        node := this.send("DOM.getDocument", "{""depth"":0}")
        q := this.send("DOM.querySelector", "{""nodeId"":" node.root.nodeId ",""selector"":" Cdp.q(sel) "}")
        if (!q.nodeId)
            throw Exception("파일 입력 요소 없음: " sel, -1)
        list := ""
        for i, p in paths
            list .= (i > 1 ? "," : "") Cdp.q(p)
        this.send("DOM.setFileInputFiles", "{""nodeId"":" q.nodeId ",""files"":[" list "]}")
        return this
    }
    ; 뷰포트/기기 에뮬레이션 (0,0 이면 해제)
    setViewport(width, height, mobile := false, dpr := 1) {
        if (width = 0)
            this.send("Emulation.clearDeviceMetricsOverride")
        else
            this.send("Emulation.setDeviceMetricsOverride", "{""width"":" width ",""height"":" height ",""deviceScaleFactor"":" dpr ",""mobile"":" (mobile ? "true" : "false") "}")
        return this
    }
    setUserAgent(ua) {
        this.send("Network.setUserAgentOverride", "{""userAgent"":" Cdp.q(ua) "}")
        return this
    }
}

; ============================================================================
;  CdpEl — 요소 핸들 (jQuery 식. 값을 주면 설정하고 this 반환, 비우면 읽기)
;    내부적으로 요소를 가리키는 JS 식(expr)을 들고 매 호출마다 다시 찾으므로 페이지가 바뀌어도 안전.
; ============================================================================
class CdpEl {
    __New(tab, expr, desc := "") {
        this.tab := tab
        this.expr := expr
        this.desc := desc
    }
    ; 요소 찾기 + 본문 실행. 요소 없으면 예외
    _run(body) {
        r := this.tab.evalExpr("(function(){var el=" this.expr ";if(!el)return {__nf:1};" body "})()")
        if (IsObject(r) && r.HasKey("__nf"))
            throw Exception("요소 없음: " this.desc, -1)
        return r
    }
    ; 요소 없으면 예외 대신 "" 반환
    _try(body) {
        try return this._run(body)
        catch
            return ""
    }
    exists[] {
        get {
            return this.tab.evalExpr("!!(" this.expr ")")
        }
    }
    ; 하위 요소
    querySelector(sel, idx := 0) {
        return new CdpEl(this.tab, "(function(b){return b?b.querySelectorAll(" Cdp.q(sel) ")[" idx "]:null})(" this.expr ")", this.desc " > " sel)
    }
    querySelectorAll(sel) {
        n := this._try("return el.querySelectorAll(" Cdp.q(sel) ").length;")
        out := []
        Loop % n
            out.Push(this.q(sel, A_Index - 1))
        return out
    }
    q(sel, idx := 0) {
        return this.querySelector(sel, idx)
    }
    qa(sel) {
        return this.querySelectorAll(sel)
    }
    count(sel) {
        return this._try("return el.querySelectorAll(" Cdp.q(sel) ").length;")
    }
    parent() {
        return new CdpEl(this.tab, "(function(b){return b?b.parentElement:null})(" this.expr ")", this.desc " parent")
    }
    closest(sel) {
        return new CdpEl(this.tab, "(function(b){return b?b.closest(" Cdp.q(sel) "):null})(" this.expr ")", this.desc " closest " sel)
    }
    next() {
        return new CdpEl(this.tab, "(function(b){return b?b.nextElementSibling:null})(" this.expr ")", this.desc " next")
    }
    prev() {
        return new CdpEl(this.tab, "(function(b){return b?b.previousElementSibling:null})(" this.expr ")", this.desc " prev")
    }
    ; JS 함수 소스로 임의 처리: el.eval("e => e.dataset.id")
    eval(fn) {
        return this._run("return (" fn ")(el);")
    }

    ; ------------------------------------------------------------ 읽기/쓰기 ----
    text(v*) {
        if (v.Length()) {
            this._run("el.textContent=" Cdp.q(v[1]) ";return true;")
            return this
        }
        return this._run("return el.innerText!==undefined?el.innerText:el.textContent;")
    }
    innerText() {
        return this.text()
    }
    html(v*) {
        if (v.Length()) {
            this._run("el.innerHTML=" Cdp.q(v[1]) ";return true;")
            return this
        }
        return this._run("return el.innerHTML;")
    }
    outerHtml() {
        return this._run("return el.outerHTML;")
    }
    ; 값 읽기/쓰기 (React/Vue 호환 setter + input/change 이벤트)
    val(v*) {
        if (!v.Length())
            return this._run("return el.value;")
        this._run(CdpEl._setValueJs(Cdp.q(v[1]), true) "return true;")
        return this
    }
    value(v*) {
        return this.val(v*)
    }
    attr(name, v*) {
        if (v.Length()) {
            this._run("el.setAttribute(" Cdp.q(name) "," Cdp.q(v[1]) ");return true;")
            return this
        }
        return this._run("var a=el.getAttribute(" Cdp.q(name) ");return a===null?'':a;")
    }
    removeAttr(name) {
        this._run("el.removeAttribute(" Cdp.q(name) ");return true;")
        return this
    }
    ; JS 프로퍼티 (el.checked, el.disabled, el.href ...)
    prop(name, v*) {
        if (v.Length()) {
            this._run("el[" Cdp.q(name) "]=" Cdp.json(v[1]) ";return true;")
            return this
        }
        return this._run("var p=el[" Cdp.q(name) "];return p===undefined||p===null?'':p;")
    }
    ; 계산된 스타일 읽기 / 인라인 스타일 쓰기
    css(name, v*) {
        if (v.Length()) {
            this._run("el.style[" Cdp.q(name) "]=" Cdp.q(v[1]) ";return true;")
            return this
        }
        return this._run("return getComputedStyle(el)[" Cdp.q(name) "];")
    }
    hasClass(c) {
        return this._run("return el.classList.contains(" Cdp.q(c) ");")
    }
    addClass(c) {
        this._run("el.classList.add(" Cdp.q(c) ");return true;")
        return this
    }
    removeClass(c) {
        this._run("el.classList.remove(" Cdp.q(c) ");return true;")
        return this
    }
    tag() {
        return this._run("return el.tagName.toLowerCase();")
    }
    id() {
        return this._run("return el.id;")
    }
    ; 뷰포트 기준 사각형 {x,y,width,height,top,left,right,bottom}
    rect() {
        return this._run("var r=el.getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height,top:r.top,left:r.left,right:r.right,bottom:r.bottom};")
    }
    center() {
        r := this.rect()
        return {x: Round(r.x + r.width / 2), y: Round(r.y + r.height / 2)}
    }
    ; 화면 좌표 중심 (AHK Click 용, 근사)
    screenPos() {
        c := this.center()
        return this.tab.viewportToScreen(c.x, c.y)
    }
    isVisible() {
        return this._try("var r=el.getBoundingClientRect(),s=getComputedStyle(el);return r.width>0&&r.height>0&&s.visibility!='hidden'&&s.display!='none';")
    }

    ; ------------------------------------------------------------ 동작 ----
    click() {
        this._run("el.click();return true;")
        return this
    }
    ; 실제 마우스 이벤트로 클릭 (Input.dispatchMouseEvent). 화면에 보이도록 먼저 스크롤
    mouseClick(button := "left", clickCount := 1) {
        this.scrollIntoView()
        c := this.center()
        this.tab.mouseClick(c.x, c.y, button, clickCount)
        return this
    }
    hover() {
        this.scrollIntoView()
        c := this.center()
        this.tab.mouseMove(c.x, c.y)
        return this
    }
    focus() {
        this._run("el.focus();return true;")
        return this
    }
    blur() {
        this._run("el.blur();return true;")
        return this
    }
    ; 값 입력 (clear=false 면 덧붙임). 포커스 후 setter + input/change 이벤트
    type(text, clear := true) {
        this._run("el.focus();" CdpEl._setValueJs(Cdp.q(text), clear) "return true;")
        return this
    }
    ; 실제 키 입력으로 한 글자씩 (자동완성/키 이벤트가 필요한 입력창)
    typeKeys(text, delayMs := 30) {
        this.focus()
        this.tab.typeKeys(text, delayMs)
        return this
    }
    press(key, modifiers := 0) {
        this.focus()
        this.tab.press(key, modifiers)
        return this
    }
    clear() {
        return this.val("")
    }
    ; <select> 값 선택 (value 기준). 없으면 옵션 텍스트로 재시도
    select(value) {
        this._run("var v=" Cdp.q(value) ";var ok=false;for(var o of el.options){if(o.value==v){o.selected=true;ok=true;}}"
            . "if(!ok){for(var o of el.options){if(o.text.trim()==v){o.selected=true;ok=true;}}}"
            . "el.dispatchEvent(new Event('input',{bubbles:true}));el.dispatchEvent(new Event('change',{bubbles:true}));return ok;")
        return this
    }
    check(on := true) {
        this._run("if(el.checked!=" (on ? "true" : "false") "){el.click();}return true;")
        return this
    }
    scrollIntoView() {
        this._run("el.scrollIntoView({block:'center',inline:'center'});return true;")
        return this
    }
    ; 임의 DOM 이벤트 발생
    dispatch(eventName) {
        this._run("el.dispatchEvent(new Event(" Cdp.q(eventName) ",{bubbles:true,cancelable:true}));return true;")
        return this
    }
    ; 폼 제출 (요소가 form 이거나 form 안의 요소)
    submit() {
        this._run("var f=el.tagName=='FORM'?el:el.form;if(!f)return false;if(f.requestSubmit)f.requestSubmit();else f.submit();return true;")
        return this
    }
    remove() {
        this._run("el.remove();return true;")
        return this
    }
    ; 디버깅용 테두리 표시
    highlight(color := "red") {
        this._run("el.style.outline='3px solid " color "';return true;")
        return this
    }
    ; 파일 입력에 로컬 파일 지정 (<input type=file>) — 첫 번째 일치 요소 기준 selector 필요
    upload(paths*) {
        this.tab.uploadFile(this.desc, paths*)
        return this
    }
    ; 값 설정 JS 조각(네이티브 setter 로 React/Vue 감지 가능)
    _setValueJs(quoted, clear) {
        return "var v=" quoted ";if(!" (clear ? "true" : "false") "){v=(el.value||'')+v;}"
            . "if(el.isContentEditable&&!('value' in el)){el.textContent=v;}else{"
            . "var proto=el.tagName=='TEXTAREA'?HTMLTextAreaElement.prototype:(el.tagName=='SELECT'?HTMLSelectElement.prototype:HTMLInputElement.prototype);"
            . "var d=Object.getOwnPropertyDescriptor(proto,'value');if(d&&d.set){d.set.call(el,v);}else{el.value=v;}}"
            . "el.dispatchEvent(new Event('input',{bubbles:true}));el.dispatchEvent(new Event('change',{bubbles:true}));"
    }
}

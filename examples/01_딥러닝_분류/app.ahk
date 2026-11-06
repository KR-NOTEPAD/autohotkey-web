; ============================================================================
;  AutoHotkey Web 예제 1 - 딥러닝 텍스트 분류 (네이티브 내장)
;  웹 UI 에 글을 입력하면 엔진에 내장된 한국어 선악 분류 모델(kor_unsmile)이
;  악성/정상 여부와 확신도를 돌려주고, 결과를 화면에 막대로 표시합니다.
;
;  의존성: 엔진 내장 TextCls* 함수 + onnxruntime.dll + models\kor_unsmile.
;          (외부 플러그인 전혀 필요 없음)
;  실행:   AutoHotkeyWeb.exe app.ahk
; ============================================================================
#NoEnv
#SingleInstance Force

global ModelDir := A_ScriptDir "\..\..\models\kor_unsmile"

if (!TextClsAvailable()) {
    MsgBox, 16, ONNX 없음, onnxruntime.dll 이 엔진 옆에 있어야 딥러닝이 동작합니다.
    ExitApp
}
if (!FileExist(ModelDir "\model_int8.onnx") && !FileExist(ModelDir "\model.onnx")) {
    MsgBox, 16, 모델 없음, % "분류 모델이 없습니다:`n" ModelDir "`n설치 시 '딥러닝 모델' 구성요소를 선택하세요."
    ExitApp
}

WebGui, Main:New, w780 h620 Center, 딥러닝 분류 데모
WebGui, Main:Expose, Classify
WebGui, Main:Html, % PageHtml()
WebGui, Main:Show
return

Classify(params) {
    global ModelDir
    text := JGet(params, "text")
    if (text = "")
        return "{""ok"":false}"
    ; 네이티브 TextClsVerdict 는 "판정,라벨,악성확률,정상확률" 형태의 문자열을 돌려준다.
    r := TextClsVerdict(ModelDir, text, 50)
    parts   := StrSplit(r, ",")
    verdict := parts[1]
    label   := parts[2]
    prob    := parts[3]
    clean   := parts[4]
    evil := (verdict = "악") ? "true" : "false"
    return "{""ok"":true,""evil"":" evil ",""label"":""" label """,""prob"":" prob ",""clean"":" clean "}"
}

MainWebGuiClose:
ExitApp

JGet(json, key) {
    if RegExMatch(json, """" key """\s*:\s*""([^""]*)""", m)
        return m1
    return ""
}

PageHtml() {
    ; 연속 섹션 안에서는 %가 변수참조로 해석되므로, 리터럴 %는 __P__ 로 쓴 뒤 치환.
    s =
( LTrim Join`r`n
<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><style>
  body{font-family:"Segoe UI","Malgun Gothic",sans-serif;margin:28px;color:#222}
  h1{color:#3F5770;margin:0 0 4px} p.sub{color:#777;margin:0 0 18px}
  textarea{width:100__P__;height:120px;font-size:16px;padding:10px;box-sizing:border-box;border:1px solid #ccc;border-radius:8px}
  button{font-size:16px;padding:10px 20px;margin-top:10px;background:#3F5770;color:#fff;border:0;border-radius:8px;cursor:pointer}
  #card{margin-top:22px;padding:18px;border-radius:10px;display:none}
  #card.evil{background:#fdecec;border:1px solid #f0b4b4}
  #card.ok{background:#eef7ee;border:1px solid #b7ddb7}
  .verdict{font-size:22px;font-weight:700}
  .bar{height:14px;border-radius:7px;background:#e3e3e3;margin-top:10px;overflow:hidden}
  .bar>i{display:block;height:100__P__;background:#A04040}
  .meta{color:#555;margin-top:8px;font-size:14px}
  .samples{margin-top:16px;color:#888;font-size:13px}
  .samples b{cursor:pointer;color:#3a63a0;text-decoration:underline;margin-right:10px}
</style></head>
<body>
  <h1>딥러닝 텍스트 분류</h1>
  <p class="sub">한국어 악성/정상 분류 (엔진 내장 kor_unsmile) · UI 는 AutoHotkey Web</p>
  <textarea id="t" placeholder="문장을 입력하세요"></textarea>
  <div class="samples">예시:
    <b onclick="setT('안녕하세요 오늘도 좋은 하루 되세요')">정상</b>
    <b onclick="setT('너 진짜 짜증나 꺼져')">악성</b>
    <b onclick="setT('이 바보 멍청아 다 네 탓이야')">악성</b>
  </div>
  <button id="go">분류하기</button>
  <div id="card">
    <div class="verdict" id="v"></div>
    <div class="bar"><i id="bar"></i></div>
    <div class="meta" id="m"></div>
  </div>
<script>
  function setT(s){ document.getElementById('t').value = s; }
  document.getElementById('go').onclick = async () => {
    const text = document.getElementById('t').value;
    const r = JSON.parse(await ahk.call("Classify", { text }));
    const card = document.getElementById('card');
    if (!r.ok){ card.style.display='none'; return; }
    card.style.display='block';
    card.className = r.evil ? 'evil' : 'ok';
    document.getElementById('v').textContent = r.evil ? '[악성으로 분류]' : '[정상으로 분류]';
    const pct = Math.round((r.evil ? r.prob : r.clean) * 100);
    document.getElementById('bar').style.width = pct + '__P__';
    document.getElementById('bar').style.background = r.evil ? '#A04040' : '#3f9442';
    document.getElementById('m').textContent =
      '라벨 ' + r.label + ' · 악성확률 ' + r.prob + ' · 정상확률 ' + r.clean;
  };
</script>
</body></html>
)
    s := StrReplace(s, "__P__", "%")
    return s
}
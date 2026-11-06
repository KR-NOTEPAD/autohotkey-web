; ============================================================================
;  AutoHotkey Web 예제 3 - 딥러닝 학습 (네이티브 내장 TextNN)
;  웹 UI 에서 "학습" 을 누르면 작은 감정(긍정/부정) 데이터셋으로 분류 모델(.ntc)
;  을 즉석에서 학습하고, 이어서 입력 문장을 분류합니다.
;  원리: 한국어 임베딩 백본(kor_embed) + C++ MLP 헤드를 엔진 내장 함수로 학습.
;
;  의존성: 엔진 내장 TextNN* 함수 + onnxruntime.dll + models\kor_embed.
;          (외부 플러그인 전혀 필요 없음)
; ============================================================================
#NoEnv
#SingleInstance Force

global Backbone  := A_ScriptDir "\..\..\models\kor_embed\kosimcse_embed_int8.onnx"
global ModelPath := A_Temp "\ahkweb_sentiment.ntc"
global Trained   := false

if (!TextNNAvailable()) {
    MsgBox, 16, 학습 불가, onnxruntime.dll 이 엔진 옆에 있어야 학습이 동작합니다.
    ExitApp
}
if (!FileExist(Backbone)) {
    MsgBox, 16, 백본 없음, % "임베딩 백본이 없습니다:`n" Backbone "`n설치 시 '딥러닝 모델' 구성요소를 선택하세요."
    ExitApp
}

WebGui, Main:New, w780 h600 Center, 딥러닝 학습 데모
WebGui, Main:Expose, Train
WebGui, Main:Expose, Classify
WebGui, Main:Html, % PageHtml()
WebGui, Main:Show
return

; 인라인 감정 데이터셋으로 학습. 반환: 학습 문장 수 + 소요 시간.
Train(params) {
    tsv := A_Temp "\ahkweb_sentiment.tsv"
    FileDelete, %tsv%
    FileAppend, % Dataset(), %tsv%, UTF-8
    t0 := A_TickCount
    n := TextNNTrain(Backbone, tsv, ModelPath, 300, 64, 64, 0)
    ms := A_TickCount - t0
    if (n = 0)
        return "{""ok"":false,""error"":" JStr(TextNNLastError()) "}"
    Trained := true
    return "{""ok"":true,""count"":" n ",""sec"":" Round(ms/1000, 1) "}"
}

; 학습한 모델로 입력 문장 분류.
Classify(params) {
    if (!Trained)
        return "{""ok"":false,""error"":""먼저 [학습 시작] 을 누르세요""}"
    text := JGet(params, "text")
    if (text = "")
        return "{""ok"":false,""error"":""문장을 입력하세요""}"
    r := TextNNClassify(ModelPath, text, 3)      ; "label,prob;label,prob;..." (model, text, topK)
    first := StrSplit(r, ";")[1]
    parts := StrSplit(first, ",")
    return "{""ok"":true,""label"":" JStr(parts[1]) ",""prob"":" (parts[2]+0) ",""raw"":" JStr(r) "}"
}

MainWebGuiClose:
ExitApp

Dataset() {
    d =
( LTrim Join`r`n
긍정	정말 좋아요 최고예요
긍정	너무 만족스럽고 행복해요
긍정	친절하고 빠른 서비스 감사합니다
긍정	기대 이상이에요 추천합니다
긍정	분위기도 좋고 음식도 맛있어요
긍정	가격 대비 훌륭해요 또 올게요
긍정	배송도 빠르고 품질이 좋네요
긍정	정말 감동받았어요 완벽해요
부정	별로예요 실망했어요
부정	너무 불친절하고 느려요
부정	다시는 안 올 것 같아요
부정	품질이 형편없어요 돈이 아까워요
부정	기대했는데 최악이었어요
부정	배송이 너무 느리고 포장도 엉망
부정	맛없고 비싸기만 해요
부정	불량품이 왔어요 환불해주세요
)
    return d
}

JGet(json, key) {
    if RegExMatch(json, """" key """\s*:\s*""([^""]*)""", m)
        return m1
    return ""
}
JStr(v) {
    v := StrReplace(v, "\", "\\")
    v := StrReplace(v, """", "\""")
    v := StrReplace(v, "`n", "\n")
    v := StrReplace(v, "`r", "")
    return """" v """"
}

PageHtml() {
    s =
( LTrim Join`r`n
<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><style>
  body{font-family:"Segoe UI","Malgun Gothic",sans-serif;margin:28px;color:#222}
  h1{color:#3F5770;margin:0 0 4px} p.sub{color:#777;margin:0 0 16px}
  button{font-size:16px;padding:10px 18px;background:#3F5770;color:#fff;border:0;border-radius:8px;cursor:pointer}
  button:disabled{background:#aaa;cursor:default}
  .box{background:#f6f7f9;border:1px solid #e3e6ea;border-radius:10px;padding:14px;margin-top:14px}
  input{width:100__P__;font-size:16px;padding:10px;box-sizing:border-box;border:1px solid #ccc;border-radius:8px;margin-top:6px}
  .res{margin-top:12px;font-size:20px;font-weight:700}
  .pos{color:#2e7d32} .neg{color:#A04040}
  .bar{height:12px;border-radius:6px;background:#e3e3e3;margin-top:8px;overflow:hidden}
  .bar>i{display:block;height:100__P__}
  .k{color:#3F5770}
</style></head>
<body>
  <h1>딥러닝 학습 (감정 분류)</h1>
  <p class="sub">인라인 데이터셋으로 즉석 학습 → 분류 · 백본 kor_embed + MLP (엔진 내장)</p>
  <button id="train">학습 시작</button>
  <span id="tinfo" style="margin-left:10px;color:#555"></span>
  <div class="box">
    <div class="k">문장 입력 후 분류</div>
    <input id="t" placeholder="예: 생각보다 정말 괜찮네요" disabled>
    <button id="go" style="margin-top:10px" disabled>분류</button>
    <div id="res" class="res"></div>
    <div class="bar"><i id="bar"></i></div>
  </div>
<script>
  const $ = id => document.getElementById(id);
  $('train').onclick = async () => {
    $('train').disabled = true; $('tinfo').textContent = '학습 중... (백본 로딩 포함 수십 초)';
    const r = JSON.parse(await ahk.call("Train", {}));
    if (!r.ok){ $('tinfo').textContent = '오류: ' + r.error; $('train').disabled = false; return; }
    $('tinfo').textContent = '완료: ' + r.count + '문장, ' + r.sec + '초';
    $('t').disabled = false; $('go').disabled = false;
  };
  $('go').onclick = async () => {
    const r = JSON.parse(await ahk.call("Classify", { text: $('t').value }));
    const res = $('res'), bar = $('bar');
    if (!r.ok){ res.textContent = r.error; res.className='res'; bar.style.width='0'; return; }
    const pos = (r.label === '긍정');
    res.textContent = r.label + '  (' + Math.round(r.prob*100) + '__P__)';
    res.className = 'res ' + (pos ? 'pos' : 'neg');
    bar.style.width = Math.round(r.prob*100) + '__P__';
    bar.style.background = pos ? '#2e7d32' : '#A04040';
  };
</script>
</body></html>
)
    s := StrReplace(s, "__P__", "%")
    return s
}
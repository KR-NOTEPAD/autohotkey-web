// ============================================================================
//  AHK CDP Bridge - background.js (MV3 service worker)
//  [AHKWEB] AutoHotkey(WsServer) <-> Chrome 브리지.
//
//  연결: ws://127.0.0.1:<port>  (기본 19222, 옵션 페이지에서 변경)
//  요청: {"id":N,"cmd":"...", ...}         → 응답 {"id":N,"ok":true,"result":...} / {"id":N,"ok":false,"error":"..."}
//  이벤트(확장→AHK): {"event":"hello"|"cdp"|"detach"|"tabRemoved"|"navigated"|"loaded"|"ping", ...}
//
//  cmd 목록
//    version                       확장/크롬 버전
//    tabs                          모든 탭 [{id,url,title,active,windowId,index,status}]
//    tab       {tabId}             탭 1개 정보
//    activeTab                     현재 포커스 창의 활성 탭
//    open      {url,active}        새 탭
//    close     {tabId}
//    activate  {tabId}             탭 활성화 + 창 포커스
//    navigate  {tabId,url}
//    reload    {tabId,bypassCache}
//    back/forward {tabId}
//    attach/detach/attached {tabId}   chrome.debugger 연결 관리
//    cdp       {tabId,method,params}  CDP 명령 (자동 attach). 예: Runtime.evaluate
//    eval      {tabId,code,world,frameId,allFrames}  chrome.scripting 로 실행(배너 없음, 페이지 CSP 영향 받음)
//    frames    {tabId}             프레임 목록 [{frameId,url,parentFrameId}]
//    zoom      {tabId,factor}
//    windowState {tabId,state}     "normal"|"minimized"|"maximized"|"fullscreen"
// ============================================================================

const DEFAULT_PORT = 19222;
const EXT_VERSION = chrome.runtime.getManifest().version;

let ws = null;
let wsPort = DEFAULT_PORT;
let reconnectTimer = null;
let pingTimer = null;
const attachedTabs = new Set();

// ---------------------------------------------------------------- 유틸 ----
function log(...a) { try { console.log('[AHK-CDP]', ...a); } catch (_) {} }

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    try { ws.send(JSON.stringify(obj)); return true; } catch (e) { log('send fail', e); }
  }
  return false;
}

function tabInfo(t) {
  if (!t) return null;
  return { id: t.id, url: t.url || t.pendingUrl || '', title: t.title || '', active: !!t.active,
           windowId: t.windowId, index: t.index, status: t.status || '', incognito: !!t.incognito };
}

async function resolveTabId(tabId) {
  if (tabId !== undefined && tabId !== null && tabId !== '' && tabId !== 0) return Number(tabId);
  const [t] = await chrome.tabs.query({ active: true, lastFocusedWindow: true });
  if (!t) { const [t2] = await chrome.tabs.query({ active: true }); if (t2) return t2.id; throw new Error('활성 탭이 없습니다'); }
  return t.id;
}

function errMsg(e) { return (e && (e.message || e.toString())) || String(e); }

// ---------------------------------------------------------------- debugger ----
async function ensureAttached(tabId) {
  if (attachedTabs.has(tabId)) return true;
  try {
    await chrome.debugger.attach({ tabId }, '1.3');
  } catch (e) {
    const m = errMsg(e);
    if (!/already attached/i.test(m)) throw e;   // 이미 (이 확장이) 붙어 있으면 통과
  }
  attachedTabs.add(tabId);
  return true;
}
async function detachTab(tabId) {
  if (!attachedTabs.has(tabId)) return false;
  attachedTabs.delete(tabId);
  try { await chrome.debugger.detach({ tabId }); } catch (_) {}
  return true;
}
async function cdpSend(tabId, method, params) {
  await ensureAttached(tabId);
  return await chrome.debugger.sendCommand({ tabId }, method, params || {});
}

chrome.debugger.onEvent.addListener((source, method, params) => {
  if (source.tabId !== undefined) send({ event: 'cdp', tabId: source.tabId, method, params: params || {} });
});
chrome.debugger.onDetach.addListener((source, reason) => {
  if (source.tabId !== undefined) { attachedTabs.delete(source.tabId); send({ event: 'detach', tabId: source.tabId, reason }); }
});
chrome.tabs.onRemoved.addListener((tabId) => { attachedTabs.delete(tabId); send({ event: 'tabRemoved', tabId }); });
chrome.webNavigation.onCommitted.addListener((d) => {
  if (d.frameId === 0) send({ event: 'navigated', tabId: d.tabId, url: d.url, transitionType: d.transitionType });
});
chrome.webNavigation.onCompleted.addListener((d) => {
  if (d.frameId === 0) send({ event: 'loaded', tabId: d.tabId, url: d.url });
});

// ---------------------------------------------------------------- scripting eval ----
// 페이지 MAIN world 에서 코드 문자열 실행 (디버거 배너 없음). 페이지 CSP 가 eval 을 막으면 error 반환 → AHK 가 cdp 로 폴백.
async function pageEval(code) {
  try {
    let v = (0, eval)(code);
    if (v && typeof v.then === 'function') v = await v;
    if (v === undefined) return { ok: true, undef: true };
    try { JSON.stringify(v); } catch (_) { v = String(v); }
    return { ok: true, value: v };
  } catch (e) {
    return { ok: false, error: String((e && e.message) || e), name: e && e.name };
  }
}
async function scriptingEval(tabId, code, world, frameId, allFrames) {
  const target = { tabId };
  if (allFrames) target.allFrames = true;
  else if (frameId !== undefined && frameId !== null && frameId !== '') target.frameIds = [Number(frameId)];
  const res = await chrome.scripting.executeScript({ target, world: world || 'MAIN', func: pageEval, args: [String(code)] });
  if (allFrames) return res.map(r => ({ frameId: r.frameId, ...r.result }));
  const r = res && res[0] && res[0].result;
  if (!r) throw new Error('executeScript 결과 없음');
  if (!r.ok) throw new Error(r.error || 'eval 오류');
  return r.undef ? null : r.value;
}

// ---------------------------------------------------------------- 명령 처리 ----
async function handle(req) {
  const cmd = req.cmd;
  switch (cmd) {
    case 'version':
      return { ext: 'ahk-cdp-bridge', version: EXT_VERSION, userAgent: navigator.userAgent, attached: [...attachedTabs] };
    case 'tabs': {
      const q = {};
      if (req.windowId !== undefined) q.windowId = req.windowId;
      const tabs = await chrome.tabs.query(q);
      return tabs.map(tabInfo);
    }
    case 'tab':
      return tabInfo(await chrome.tabs.get(await resolveTabId(req.tabId)));
    case 'activeTab':
      return tabInfo(await chrome.tabs.get(await resolveTabId()));
    case 'open': {
      const t = await chrome.tabs.create({ url: req.url || 'about:blank', active: req.active !== false });
      return tabInfo(t);
    }
    case 'close': {
      const id = await resolveTabId(req.tabId);
      await detachTab(id);
      await chrome.tabs.remove(id);
      return true;
    }
    case 'activate': {
      const id = await resolveTabId(req.tabId);
      const t = await chrome.tabs.update(id, { active: true });
      try { await chrome.windows.update(t.windowId, { focused: true }); } catch (_) {}
      return tabInfo(t);
    }
    case 'navigate': {
      const id = await resolveTabId(req.tabId);
      return tabInfo(await chrome.tabs.update(id, { url: req.url }));
    }
    case 'reload': {
      const id = await resolveTabId(req.tabId);
      await chrome.tabs.reload(id, { bypassCache: !!req.bypassCache });
      return true;
    }
    case 'back':    await chrome.tabs.goBack(await resolveTabId(req.tabId)); return true;
    case 'forward': await chrome.tabs.goForward(await resolveTabId(req.tabId)); return true;
    case 'attach':  return await ensureAttached(await resolveTabId(req.tabId));
    case 'detach':  return await detachTab(await resolveTabId(req.tabId));
    case 'attached': return attachedTabs.has(await resolveTabId(req.tabId));
    case 'cdp': {
      const id = await resolveTabId(req.tabId);
      if (!req.method) throw new Error('method 누락');
      return await cdpSend(id, req.method, req.params);
    }
    case 'eval': {
      const id = await resolveTabId(req.tabId);
      return await scriptingEval(id, req.code || '', req.world, req.frameId, !!req.allFrames);
    }
    case 'frames': {
      const id = await resolveTabId(req.tabId);
      const fr = await chrome.webNavigation.getAllFrames({ tabId: id });
      return (fr || []).map(f => ({ frameId: f.frameId, parentFrameId: f.parentFrameId, url: f.url }));
    }
    case 'zoom': {
      const id = await resolveTabId(req.tabId);
      if (req.factor !== undefined) await chrome.tabs.setZoom(id, Number(req.factor));
      return await chrome.tabs.getZoom(id);
    }
    case 'windowState': {
      const id = await resolveTabId(req.tabId);
      const t = await chrome.tabs.get(id);
      const w = await chrome.windows.update(t.windowId, { state: req.state || 'normal' });
      return w.state;
    }
    case 'echo':
      return req.data === undefined ? null : req.data;
    default:
      throw new Error('알 수 없는 cmd: ' + cmd);
  }
}

async function onMessage(text) {
  let req;
  try { req = JSON.parse(text); } catch (e) { send({ ok: false, error: 'JSON 파싱 실패: ' + errMsg(e) }); return; }
  if (req.event === 'pong') return;
  const id = req.id;
  try {
    const result = await handle(req);
    send({ id, ok: true, result: result === undefined ? null : result });
  } catch (e) {
    send({ id, ok: false, error: errMsg(e) });
  }
}

// ---------------------------------------------------------------- 연결 관리 ----
function scheduleReconnect(ms) {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => { reconnectTimer = null; connect(); }, ms);
}

async function connect() {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
  try {
    const st = await chrome.storage.local.get({ port: DEFAULT_PORT });
    wsPort = Number(st.port) || DEFAULT_PORT;
  } catch (_) { wsPort = DEFAULT_PORT; }
  let sock;
  try { sock = new WebSocket('ws://127.0.0.1:' + wsPort + '/'); }
  catch (e) { scheduleReconnect(2000); return; }
  ws = sock;
  sock.onopen = () => {
    log('connected :' + wsPort);
    send({ event: 'hello', ext: 'ahk-cdp-bridge', version: EXT_VERSION, port: wsPort });
    if (pingTimer) clearInterval(pingTimer);
    pingTimer = setInterval(() => send({ event: 'ping', t: Date.now() }), 20000);   // SW keep-alive
  };
  sock.onmessage = (ev) => { onMessage(typeof ev.data === 'string' ? ev.data : ''); };
  sock.onerror = () => { /* onclose 에서 재시도 */ };
  sock.onclose = () => {
    if (ws === sock) ws = null;
    if (pingTimer) { clearInterval(pingTimer); pingTimer = null; }
    scheduleReconnect(1500);
  };
}

// 서비스워커가 잠들어도 알람으로 깨어나 재접속 (최소 주기 30초)
chrome.alarms.create('ahk-cdp-reconnect', { periodInMinutes: 0.5 });
chrome.alarms.onAlarm.addListener((a) => { if (a.name === 'ahk-cdp-reconnect') connect(); });
chrome.runtime.onStartup.addListener(connect);
chrome.runtime.onInstalled.addListener(connect);
chrome.runtime.onMessage.addListener((msg, _s, reply) => {
  if (msg && msg.type === 'reconnect') {
    try { if (ws) ws.close(); } catch (_) {}
    ws = null; connect(); reply({ ok: true }); return true;
  }
  if (msg && msg.type === 'status') {
    reply({ connected: !!(ws && ws.readyState === WebSocket.OPEN), port: wsPort, attached: [...attachedTabs] });
    return true;
  }
});
connect();

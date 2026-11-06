// [AHKWEB] AHK CDP Bridge - 옵션/팝업: 포트 설정 + 연결 상태 표시
const DEFAULT_PORT = 19222;
const $ = (id) => document.getElementById(id);

async function refresh() {
  try {
    const st = await chrome.runtime.sendMessage({ type: 'status' });
    $('dot').className = 'dot ' + (st && st.connected ? 'on' : 'off');
    $('status').textContent = st && st.connected ? ('AHK 연결됨 (포트 ' + st.port + ')') : ('연결 안 됨 (포트 ' + (st ? st.port : '?') + ')');
    $('attached').textContent = st && st.attached && st.attached.length ? ('디버거 attach 탭: ' + st.attached.join(', ')) : '';
  } catch (e) {
    $('dot').className = 'dot off';
    $('status').textContent = '백그라운드 응답 없음';
  }
}

(async () => {
  const st = await chrome.storage.local.get({ port: DEFAULT_PORT });
  $('port').value = st.port;
  refresh();
  setInterval(refresh, 1500);
})();

$('save').addEventListener('click', async () => {
  const p = Number($('port').value) || DEFAULT_PORT;
  await chrome.storage.local.set({ port: p });
  await chrome.runtime.sendMessage({ type: 'reconnect' });
  setTimeout(refresh, 500);
});
$('reconnect').addEventListener('click', async () => {
  await chrome.runtime.sendMessage({ type: 'reconnect' });
  setTimeout(refresh, 500);
});

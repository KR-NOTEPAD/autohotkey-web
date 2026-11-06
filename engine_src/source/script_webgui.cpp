// ============================================================================
//  AutoHotkey Web  -  script_webgui.cpp
//  WebGui command: host an HTML/CSS/JS UI (Microsoft Edge WebView2) inside an
//  AutoHotkey v1 window and bridge it to the script over an async JSON channel.
//
//  Threading model (important):
//    WebView2 callbacks run on the UI thread but OUTSIDE the AHK interpreter.
//    They must never call into the script. Instead each callback appends to
//    g_webEventQueue (guarded by a CRITICAL_SECTION) and PostMessage()s
//    AHK_WEB_EVENT to g_hWnd. MainWindowProc -> WebGuiProcessEvent() then runs
//    the matching AHK function as a normal quasi-thread, so Critical /
//    #MaxThreadsPerHotkey / Thread priority all apply, exactly like a GUI event.
//
//  Bridge contract (v1, all strings):
//    JS  ->  AHK : window.ahk.call(method, paramsObj)  resolves a Promise.
//                  paramsObj is JSON-stringified and passed as the single string
//                  argument to the exposed AHK function. Its (string) return
//                  value becomes the resolved value.
//    AHK ->  JS  : WebGui, Name:Emit, EventName, DataString  fires
//                  window.ahk._emit(EventName, DataString) -> ahk.on handlers.
//                  WebGui, Name:Eval, JsCode  runs code (fire-and-forget).
// ============================================================================

#include "stdafx.h"
#include "defines.h"
#include "globaldata.h"
#include "script.h"
#include "application.h"
#include "script_webgui.h"
#include "hook.h" // AHK_WEB_EVENT

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>

// --- WebView2 -------------------------------------------------------------
// The SDK headers come from the NuGet package Microsoft.Web.WebView2, copied
// (or /I-included) by the build script under third_party\webview2\.
#include "WebView2.h"

// The loader entry point. We link WebView2LoaderStatic.lib so the finished EXE
// needs no WebView2Loader.dll alongside it.
#pragma comment(lib, "version.lib")

// ----------------------------------------------------------------------------
//  Small UTF-8 <-> UTF-16 helpers (WebView2 speaks UTF-16; its message payloads
//  are JSON which we hand to the script as the engine's native TCHAR).
// ----------------------------------------------------------------------------
static std::wstring Utf8ToW(const char *s)
{
	if (!s || !*s) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
	return w;
}
static std::string WToUtf8(const wchar_t *w)
{
	if (!w || !*w) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
	return s;
}

// Minimal JSON string escaper for building the bridge's own JSON (not a parser;
// the script is responsible for the JSON shape of its own data payloads).
static std::wstring JsonEscapeW(const std::wstring &in)
{
	std::wstring out;
	out.reserve(in.size() + 8);
	for (wchar_t c : in)
	{
		switch (c)
		{
		case L'\\': out += L"\\\\"; break;
		case L'"':  out += L"\\\""; break;
		case L'\n': out += L"\\n"; break;
		case L'\r': out += L"\\r"; break;
		case L'\t': out += L"\\t"; break;
		case L'\b': out += L"\\b"; break;
		case L'\f': out += L"\\f"; break;
		default:
			if (c < 0x20) { wchar_t b[8]; swprintf(b, 8, L"\\u%04x", c); out += b; }
			else out += c;
		}
	}
	return out;
}

// ----------------------------------------------------------------------------
//  Data structures
// ----------------------------------------------------------------------------

struct WebGuiType
{
	std::wstring name;              // script-facing name (e.g. "Main")
	HWND hostWnd;                   // top-level host window (WINDOW_CLASS_GUI)
	ICoreWebView2Environment *env;
	ICoreWebView2Controller  *controller;
	ICoreWebView2            *core;
	EventRegistrationToken    msgToken;   // WebMessageReceived
	std::wstring pendingUrl;       // Load target set before the core is ready
	std::wstring pendingHtml;      // Html string set before the core is ready
	bool ready;                    // core created & bootstrap script injected
	bool noActivate;               // show without stealing focus
	std::unordered_map<std::wstring, std::wstring> exposed; // method -> AHK func name
	// Mounts requested before the core was ready; applied in ControllerHandler.
	struct PendingMount { std::wstring host, folder; int kind; };
	std::vector<PendingMount> pendingMounts;

	WebGuiType(const std::wstring &n)
		: name(n), hostWnd(NULL), env(NULL), controller(NULL), core(NULL)
		, ready(false), noActivate(false) { msgToken.value = 0; }
};

// A single web -> script event waiting to be dispatched on the main thread.
struct WebEvent
{
	WebGuiType *gui;
	std::wstring kind;   // "call" (RPC) or "event" (ahk.post, fire-and-forget)
	std::wstring id;     // RPC id to resolve, empty for events
	std::wstring method; // exposed method / event name
	std::wstring params; // JSON string payload
};

static std::vector<WebGuiType*> g_webGuis;
static std::vector<WebEvent>    g_webEventQueue;
static CRITICAL_SECTION         g_webCS;
static bool                     g_webCSInit = false;

static void WebEnsureCS()
{
	if (!g_webCSInit) { InitializeCriticalSection(&g_webCS); g_webCSInit = true; }
}

static WebGuiType *WebFindByName(LPCTSTR aName)
{
	std::wstring n = aName;
	for (auto *g : g_webGuis)
		if (g->name == n) return g;
	return NULL;
}

static WebGuiType *WebFindByHost(HWND h)
{
	for (auto *g : g_webGuis)
		if (g->hostWnd == h) return g;
	return NULL;
}

// ----------------------------------------------------------------------------
//  Invoke an exposed AHK function with one string arg; capture its string
//  return. Runs as a proper new quasi-thread (same recipe as OnMessage), so the
//  handler obeys Critical / #MaxThreads / Thread priority. Caller guarantees we
//  are on the main thread and interruptible (checked in WebGuiProcessEvent).
// ----------------------------------------------------------------------------
static bool CallExposedFunc(Func *aFunc, const std::wstring &aArg, std::wstring &aResult)
{
	aResult.clear();
	if (!aFunc)
		return false;

	// Thread-count guard, mirroring MsgMonitor().
	if (g_nThreads >= g_MaxThreadsTotal)
		if (g_nThreads >= MAX_THREADS_EMERGENCY)
			return false;
	if (g->Priority > 0) // Current thread too high-priority to interrupt.
		return false;

	TCHAR ErrorLevel_saved[ERRORLEVEL_SAVED_SIZE];
	tcslcpy(ErrorLevel_saved, g_ErrorLevel->Contents(), _countof(ErrorLevel_saved));
	InitNewThread(0, false, true, aFunc->mJumpToLine ? aFunc->mJumpToLine->mActionType : ACT_INVALID);
	DEBUGGER_STACK_PUSH(_T("WebGui"))

	// One string parameter (the params JSON). ExprTokenType holds a non-const
	// LPTSTR, and Func::Call does not modify it, so the cast is safe.
	ExprTokenType paramTok;
	paramTok.symbol = SYM_STRING;
	paramTok.marker = const_cast<LPTSTR>(aArg.c_str());
	ExprTokenType *params[] = { &paramTok };

	ExprTokenType resultTok;
	resultTok.symbol = SYM_STRING;
	resultTok.marker = _T("");

	UDFCallInfo callInfo;
	ResultType callResult = OK;
	bool ok = aFunc->Call(callInfo, callResult, resultTok, params, 1, false);
	if (ok)
	{
		TCHAR numBuf[MAX_NUMBER_SIZE];
		LPTSTR s = TokenToString(resultTok, numBuf);
		if (s)
			aResult = s;
	}

	DEBUGGER_STACK_POP()
	ResumeUnderlyingThread(ErrorLevel_saved);
	return ok;
}

// ----------------------------------------------------------------------------
//  JavaScript bootstrap injected into every document: defines window.ahk with
//  call() (promise over postMessage), on()/off() (events) and the internal
//  _resolve/_emit plumbing the native side drives.
// ----------------------------------------------------------------------------
static const wchar_t *kBootstrapJS = LR"JS(
(function(){
  if (window.ahk && window.ahk.__ahkweb) return;
  var seq = 0, pending = {}, listeners = {};
  function post(o){ window.chrome.webview.postMessage(JSON.stringify(o)); }
  var ahk = {
    __ahkweb: true,
    call: function(method, params){
      var id = "r" + (++seq);
      return new Promise(function(resolve, reject){
        pending[id] = { resolve: resolve, reject: reject };
        post({ t:"call", id:id, method:method, params: JSON.stringify(params===undefined?null:params) });
      });
    },
    post: function(method, params){
      post({ t:"event", method:method, params: JSON.stringify(params===undefined?null:params) });
    },
    on: function(ev, cb){ (listeners[ev]||(listeners[ev]=[])).push(cb); },
    off: function(ev, cb){ var a=listeners[ev]; if(a){ var i=a.indexOf(cb); if(i>=0)a.splice(i,1);} },
    _resolve: function(id, ok, value){
      var p = pending[id]; if(!p) return; delete pending[id];
      if (ok) p.resolve(value); else p.reject(new Error(value));
    },
    _emit: function(ev, data){
      var a = listeners[ev]; if(!a) return;
      for (var i=0;i<a.length;i++){ try{ a[i](data); }catch(e){ console.error(e); } }
    }
  };
  window.ahk = ahk;
})();
)JS";

// ----------------------------------------------------------------------------
//  WebMessageReceived handler: parse {t,id,method,params}, enqueue, wake main.
//  Runs on the UI thread but outside the interpreter: do NOT call the script.
// ----------------------------------------------------------------------------
class WebMsgHandler : public ICoreWebView2WebMessageReceivedEventHandler
{
	LONG mRef; WebGuiType *mGui;
public:
	WebMsgHandler(WebGuiType *g) : mRef(1), mGui(g) {}
	// IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2WebMessageReceivedEventHandler))
		{ *ppv = this; AddRef(); return S_OK; }
		*ppv = NULL; return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&mRef); }
	ULONG STDMETHODCALLTYPE Release() override
	{ LONG r = InterlockedDecrement(&mRef); if (!r) delete this; return r; }

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
		ICoreWebView2WebMessageReceivedEventArgs *args) override
	{
		LPWSTR raw = NULL;
		if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw)
			return S_OK;
		std::wstring msg = raw;
		CoTaskMemFree(raw);

		// Tiny hand-parse of the known flat shape. We only need t/id/method/params;
		// params itself is an opaque JSON string we forward verbatim to the script.
		auto field = [&](const wchar_t *key) -> std::wstring {
			std::wstring pat = std::wstring(L"\"") + key + L"\":";
			size_t p = msg.find(pat);
			if (p == std::wstring::npos) return L"";
			p += pat.size();
			while (p < msg.size() && (msg[p]==L' ')) p++;
			if (p >= msg.size()) return L"";
			if (msg[p] == L'"') { // quoted string value (unescape minimal)
				std::wstring out; p++;
				while (p < msg.size() && msg[p] != L'"') {
					if (msg[p]==L'\\' && p+1<msg.size()) {
						wchar_t n = msg[++p];
						switch(n){case L'n':out+=L'\n';break;case L'r':out+=L'\r';break;
							case L't':out+=L'\t';break;case L'"':out+=L'"';break;
							case L'\\':out+=L'\\';break;case L'/':out+=L'/';break;default:out+=n;}
						p++;
					} else out += msg[p++];
				}
				return out;
			}
			return L""; // params is always sent as a quoted JSON string by the bootstrap
		};

		WebEvent ev;
		ev.gui    = mGui;
		ev.kind   = field(L"t");
		ev.id     = field(L"id");
		ev.method = field(L"method");
		ev.params = field(L"params");

		WebEnsureCS();
		EnterCriticalSection(&g_webCS);
		g_webEventQueue.push_back(ev);
		LeaveCriticalSection(&g_webCS);

		if (g_hWnd)
			PostMessage(g_hWnd, AHK_WEB_EVENT, 0, 0);
		return S_OK;
	}
};

// Fit the WebView to the host's client area.
static void WebResizeToHost(WebGuiType *g)
{
	if (!g->controller || !g->hostWnd) return;
	RECT rc; GetClientRect(g->hostWnd, &rc);
	g->controller->put_Bounds(rc);
}

// Apply one virtual-host -> folder mapping to a ready core. Returns true if the
// core supports it (ICoreWebView2_3) and the call was made.
static bool WebApplyMount(WebGuiType *g, const std::wstring &host, const std::wstring &folder, int kind)
{
	if (!g->core) return false;
	ICoreWebView2_3 *wv3 = NULL;
	if (FAILED(g->core->QueryInterface(IID_PPV_ARGS(&wv3))) || !wv3) return false;
	wv3->SetVirtualHostNameToFolderMapping(host.c_str(), folder.c_str(),
		(COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND)kind);
	wv3->Release();
	return true;
}

// ----------------------------------------------------------------------------
//  Controller-created: grab the core, inject bootstrap, hook messages, show.
// ----------------------------------------------------------------------------
class ControllerHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
	LONG mRef; WebGuiType *mGui;
public:
	ControllerHandler(WebGuiType *g) : mRef(1), mGui(g) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler))
		{ *ppv = this; AddRef(); return S_OK; }
		*ppv = NULL; return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&mRef); }
	ULONG STDMETHODCALLTYPE Release() override
	{ LONG r = InterlockedDecrement(&mRef); if (!r) delete this; return r; }

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *controller) override
	{
		if (FAILED(result) || !controller || !WebFindByHost(mGui->hostWnd))
			return S_OK; // Host destroyed before the controller arrived.
		mGui->controller = controller;
		controller->AddRef();
		controller->get_CoreWebView2(&mGui->core);
		if (!mGui->core) return S_OK;

		// Lock things down for a desktop app shell.
		ICoreWebView2Settings *settings = NULL;
		if (SUCCEEDED(mGui->core->get_Settings(&settings)) && settings)
		{
			settings->put_AreDevToolsEnabled(FALSE);
			settings->put_AreDefaultContextMenusEnabled(FALSE);
			settings->put_IsStatusBarEnabled(FALSE);
			settings->put_AreHostObjectsAllowed(FALSE);
			settings->put_IsWebMessageEnabled(TRUE);
			settings->Release();
		}

		// Inject the bridge bootstrap into every document before its scripts run.
		mGui->core->AddScriptToExecuteOnDocumentCreated(kBootstrapJS, NULL);

		// Receive window.chrome.webview.postMessage(...) from the page.
		WebMsgHandler *h = new WebMsgHandler(mGui);
		mGui->core->add_WebMessageReceived(h, &mGui->msgToken);
		h->Release();

		WebResizeToHost(mGui);
		mGui->controller->put_IsVisible(TRUE);
		mGui->ready = true;

		// Apply any mounts requested before the core existed, BEFORE navigating,
		// so a mounted virtual-host URL resolves on the first load.
		for (auto &m : mGui->pendingMounts)
			WebApplyMount(mGui, m.host, m.folder, m.kind);
		mGui->pendingMounts.clear();

		if (!mGui->pendingUrl.empty())
			mGui->core->Navigate(mGui->pendingUrl.c_str());
		else if (!mGui->pendingHtml.empty())
			mGui->core->NavigateToString(mGui->pendingHtml.c_str());
		return S_OK;
	}
};

// ----------------------------------------------------------------------------
//  Environment-created: create the controller for the host window.
// ----------------------------------------------------------------------------
class EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
{
	LONG mRef; WebGuiType *mGui;
public:
	EnvHandler(WebGuiType *g) : mRef(1), mGui(g) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler))
		{ *ppv = this; AddRef(); return S_OK; }
		*ppv = NULL; return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&mRef); }
	ULONG STDMETHODCALLTYPE Release() override
	{ LONG r = InterlockedDecrement(&mRef); if (!r) delete this; return r; }

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) override
	{
		if (FAILED(result) || !env || !WebFindByHost(mGui->hostWnd))
			return S_OK;
		mGui->env = env; env->AddRef();
		ControllerHandler *ch = new ControllerHandler(mGui);
		env->CreateCoreWebView2Controller(mGui->hostWnd, ch);
		ch->Release();
		return S_OK;
	}
};

// ----------------------------------------------------------------------------
//  Main-thread event pump: pop one queued web event and run its handler.
//  Returns true if an event was processed (caller re-posts while more remain).
// ----------------------------------------------------------------------------
bool WebGuiProcessEvent()
{
	WebEnsureCS();
	WebEvent ev;
	bool have = false;
	EnterCriticalSection(&g_webCS);
	if (!g_webEventQueue.empty())
	{
		ev = g_webEventQueue.front();
		g_webEventQueue.erase(g_webEventQueue.begin());
		have = true;
	}
	LeaveCriticalSection(&g_webCS);
	if (!have)
		return false;

	// The WebGui may have been destroyed between posting and now.
	bool stillAlive = false;
	for (auto *g : g_webGuis) if (g == ev.gui) { stillAlive = true; break; }
	if (!stillAlive)
		return true; // Consumed; nothing to run, but check for more.

	// Resolve the exposed method -> AHK function.
	std::wstring funcName;
	auto it = ev.gui->exposed.find(ev.method);
	if (it != ev.gui->exposed.end())
		funcName = it->second;

	std::wstring resultStr;
	bool ok = false;
	if (!funcName.empty())
	{
		Func *f = g_script.FindFunc(funcName.c_str(), funcName.size());
		if (f)
			ok = CallExposedFunc(f, ev.params, resultStr);
	}

	// For RPC calls, resolve/reject the JS promise.
	if (ev.kind == L"call" && !ev.id.empty() && ev.gui->core)
	{
		std::wstring js = L"window.ahk._resolve(\"";
		js += JsonEscapeW(ev.id);
		js += L"\",";
		js += ok ? L"true" : L"false";
		js += L",\"";
		js += JsonEscapeW(ok ? resultStr : (funcName.empty()
			? std::wstring(L"WebGui: method not exposed: ") + ev.method
			: std::wstring(L"WebGui: handler failed")));
		js += L"\");";
		ev.gui->core->ExecuteScript(js.c_str(), NULL);
	}
	return true;
}

// ----------------------------------------------------------------------------
//  Host window: a top-level window whose entire client area is the WebView.
//  Neutral class name (no "AutoHotkey" substring); distinct so ahk_class can
//  target web windows specifically.
// ----------------------------------------------------------------------------
#define WEBGUI_WINDOW_CLASS _T("AhkwWebView") // AutoHotkey Web: neutral class name (no "AutoHotkey" substring).
static bool g_webClassRegistered = false;

// Queue a synthetic "event" so the script can react to window lifecycle in the
// same handler style as web events (method names prefixed with "__").
static void WebQueueLifecycle(WebGuiType *g, const wchar_t *method)
{
	WebEvent ev; ev.gui = g; ev.kind = L"event"; ev.method = method; ev.params = L"null";
	WebEnsureCS();
	EnterCriticalSection(&g_webCS);
	g_webEventQueue.push_back(ev);
	LeaveCriticalSection(&g_webCS);
	if (g_hWnd) PostMessage(g_hWnd, AHK_WEB_EVENT, 0, 0);
}

static LRESULT CALLBACK WebHostProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_SIZE:
	{
		WebGuiType *g = WebFindByHost(hWnd);
		if (g) WebResizeToHost(g);
		return 0;
	}
	case WM_CLOSE:
	{
		// Default behaviour: hide and notify the script (like Gui +LastFound close).
		// The script's __Close handler (if exposed) decides whether to ExitApp.
		WebGuiType *g = WebFindByHost(hWnd);
		ShowWindow(hWnd, SW_HIDE);
		if (g) WebQueueLifecycle(g, L"__Close");
		return 0;
	}
	case WM_DESTROY:
		return 0;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

static bool WebRegisterClass()
{
	if (g_webClassRegistered) return true;
	WNDCLASSEX wc = {0};
	wc.cbSize = sizeof(wc);
	wc.lpszClassName = WEBGUI_WINDOW_CLASS;
	wc.hInstance = g_hInstance;
	wc.lpfnWndProc = WebHostProc;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.hIcon = g_IconLarge;
	wc.hIconSm = g_IconSmall;
	if (!RegisterClassEx(&wc))
		return false;
	g_webClassRegistered = true;
	return true;
}

// Begin async WebView2 creation for a host window. The user-data folder is kept
// per-application under %LOCALAPPDATA% so multiple AHK Web apps don't collide.
static void WebStartEnvironment(WebGuiType *g)
{
	wchar_t udf[MAX_PATH]; udf[0] = 0;
	wchar_t local[MAX_PATH];
	if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
	{
		// Derive a per-script subfolder name from the main script file name.
		const wchar_t *base = L"AutoHotkeyWeb";
		swprintf(udf, MAX_PATH, L"%s\\AutoHotkeyWeb\\WebView2", local);
		(void)base;
	}
	EnvHandler *eh = new EnvHandler(g);
	HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
		NULL, udf[0] ? udf : NULL, NULL, eh);
	eh->Release();
	if (FAILED(hr))
	{
		// Most common cause: the Evergreen WebView2 Runtime is not installed.
		g_script.ScriptError(_T("WebGui: the Microsoft Edge WebView2 Runtime is required but could not be started."));
	}
}

void WebGuiShutdownAll()
{
	for (auto *g : g_webGuis)
	{
		if (g->core && g->msgToken.value)
			g->core->remove_WebMessageReceived(g->msgToken);
		if (g->controller) { g->controller->Close(); g->controller->Release(); }
		if (g->core) g->core->Release();
		if (g->env) g->env->Release();
		if (g->hostWnd) DestroyWindow(g->hostWnd);
		delete g;
	}
	g_webGuis.clear();
}

// ----------------------------------------------------------------------------
//  Command parsing helpers
// ----------------------------------------------------------------------------

// Split "Name:SubCommand" -> name + sub. If there is no colon, the name
// defaults to "1" (the default web window), matching Gui's default-name idea.
static void WebSplitNameCommand(LPTSTR aArg1, std::wstring &aName, std::wstring &aSub)
{
	LPTSTR colon = NULL;
	for (LPTSTR p = aArg1; *p; ++p)
		if (*p == ':') { colon = p; break; }
	if (colon)
	{
		aName.assign(aArg1, colon - aArg1);
		aSub.assign(colon + 1);
	}
	else
	{
		aName = L"1";
		aSub.assign(aArg1);
	}
	// Trim surrounding spaces from both.
	auto trim = [](std::wstring &s){
		size_t a = s.find_first_not_of(L" \t");
		size_t b = s.find_last_not_of(L" \t");
		if (a == std::wstring::npos) s.clear();
		else s = s.substr(a, b - a + 1);
	};
	trim(aName); trim(aSub);
}

static int WebCmpI(const std::wstring &a, const wchar_t *b)
{
	return _wcsicmp(a.c_str(), b);
}

// Create the host window (hidden) and begin WebView2 bring-up.
static WebGuiType *WebCreate(const std::wstring &aName, LPTSTR aOptions, LPTSTR aTitle)
{
	if (!WebRegisterClass())
		return NULL;
	WebGuiType *g = new WebGuiType(aName);

	// Window options (superset of stock Gui that makes sense for a web shell):
	//   wNNN hNNN      size
	//   xNNN yNNN      position (default: centered)
	//   Center         center on the work area (default when no x/y given)
	//   -Resize        fixed size (no thick frame / maximize box)
	//   Frameless      borderless popup (custom title bar in HTML)
	//   AlwaysOnTop    topmost
	//   NoActivate     do not steal focus when shown
	//   OpacityNNN     0-255 window opacity (layered window)
	DWORD style = WS_OVERLAPPEDWINDOW, exstyle = 0;
	int w = 900, h = 650, x = INT_MIN, y = INT_MIN, opacity = 255;
	bool center = false, noActivate = false;
	if (aOptions && *aOptions)
	{
		std::wstring opt = aOptions;
		for (size_t i = 0; i < opt.size(); )
		{
			while (i < opt.size() && opt[i] == L' ') i++;
			if (i >= opt.size()) break;
			size_t j = opt.find(L' ', i);
			std::wstring tok = opt.substr(i, (j == std::wstring::npos ? opt.size() : j) - i);
			if (!tok.empty())
			{
				const wchar_t *t = tok.c_str();
				if ((t[0]==L'w'||t[0]==L'W') && iswdigit(t[1])) w = _wtoi(t+1);
				else if ((t[0]==L'h'||t[0]==L'H') && iswdigit(t[1])) h = _wtoi(t+1);
				else if ((t[0]==L'x'||t[0]==L'X') && (iswdigit(t[1])||t[1]==L'-')) x = _wtoi(t+1);
				else if ((t[0]==L'y'||t[0]==L'Y') && (iswdigit(t[1])||t[1]==L'-')) y = _wtoi(t+1);
				else if (!_wcsicmp(t, L"Center")) center = true;
				else if (!_wcsicmp(t, L"-Resize")) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
				else if (!_wcsicmp(t, L"Frameless")) style = WS_POPUP | WS_SYSMENU;
				else if (!_wcsicmp(t, L"AlwaysOnTop")) exstyle |= WS_EX_TOPMOST;
				else if (!_wcsicmp(t, L"NoActivate")) noActivate = true;
				else if (!_wcsnicmp(t, L"Opacity", 7) && iswdigit(t[7])) opacity = _wtoi(t+7);
			}
			i = (j == std::wstring::npos ? opt.size() : j + 1);
		}
	}

	// Default to centering on the primary work area when no explicit x/y.
	if (x == INT_MIN && y == INT_MIN) center = (center || true);
	if (center)
	{
		RECT wa; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
		x = wa.left + ((wa.right - wa.left) - w) / 2;
		y = wa.top + ((wa.bottom - wa.top) - h) / 2;
	}
	if (opacity < 0) opacity = 0; if (opacity > 255) opacity = 255;
	if (opacity < 255) exstyle |= WS_EX_LAYERED;

	const wchar_t *title = (aTitle && *aTitle) ? aTitle : L"AutoHotkey Web";
	g->hostWnd = CreateWindowEx(exstyle, WEBGUI_WINDOW_CLASS, title, style,
		(x==INT_MIN?CW_USEDEFAULT:x), (y==INT_MIN?CW_USEDEFAULT:y),
		w, h, NULL, NULL, g_hInstance, NULL);
	if (!g->hostWnd) { delete g; return NULL; }
	if (exstyle & WS_EX_LAYERED)
		SetLayeredWindowAttributes(g->hostWnd, 0, (BYTE)opacity, LWA_ALPHA);
	g->noActivate = noActivate;

	g_webGuis.push_back(g);
	WebStartEnvironment(g);
	return g;
}

// ----------------------------------------------------------------------------
//  Line::WebGui  -  ACT_WEBGUI runtime entry point.
//  Syntax:  WebGui, Name:SubCommand [, P2, P3, P4]
//
//  Subcommands:
//    New        [, Options, Title]      Create a (hidden) web window.
//    Load        , UrlOrFile            Navigate (http(s):// or local path).
//    Show       [, Options]             Show the window (w/h/x/y, "Maximize").
//    Hide                               Hide the window.
//    Expose      , Method [, FuncName]  Map a JS method to an AHK function.
//    On          , __Event, FuncName    Map a lifecycle event (e.g. __Close).
//    Emit        , EventName [, Data]   Fire window.ahk on(EventName) in JS.
//    Eval        , JsCode               Run JS (fire-and-forget).
//    Destroy                            Close & free the window.
// ----------------------------------------------------------------------------
ResultType Line::WebGui(LPTSTR aCommand, LPTSTR aParam2, LPTSTR aParam3, LPTSTR aParam4)
{
	std::wstring name, sub;
	WebSplitNameCommand(aCommand, name, sub);

	WebGuiType *g = WebFindByName(name.c_str());

	if (!WebCmpI(sub, L"New"))
	{
		if (g) // Re-New: destroy the old one first.
		{
			for (size_t i = 0; i < g_webGuis.size(); ++i)
				if (g_webGuis[i] == g) { g_webGuis.erase(g_webGuis.begin()+i); break; }
			if (g->controller){ g->controller->Close(); g->controller->Release(); }
			if (g->core) g->core->Release();
			if (g->env) g->env->Release();
			if (g->hostWnd) DestroyWindow(g->hostWnd);
			delete g;
		}
		WebGuiType *ng = WebCreate(name, aParam2, aParam3);
		ng->name = name;
		if (!ng)
			return LineError(_T("WebGui: failed to create the web window."), FAIL, aCommand);
		return OK;
	}

	// All remaining subcommands require the window to exist.
	if (!g)
	{
		// Auto-create on first use so "WebGui, Load, ..." works without New.
		g = WebCreate(name, _T(""), _T(""));
		if (!g)
			return LineError(_T("WebGui: no such web window and auto-create failed."), FAIL, aCommand);
	}

	if (!WebCmpI(sub, L"Load"))
	{
		std::wstring target = Utf8ToW(""); // placeholder to keep type
		target = aParam2 ? std::wstring(aParam2) : std::wstring();
		if (target.empty())
			return LineError(_T("WebGui, Load: a URL or file path is required."), FAIL, aCommand);
		// A bare local path becomes a file:// URL; anything with a scheme passes through.
		bool hasScheme = target.find(L"://") != std::wstring::npos;
		std::wstring url;
		if (hasScheme)
			url = target;
		else
		{
			wchar_t full[MAX_WIDE_PATH];
			if (GetFullPathNameW(target.c_str(), MAX_WIDE_PATH, full, NULL))
			{
				url = L"file:///";
				url += full;
				for (auto &c : url) if (c == L'\\') c = L'/';
			}
			else url = target;
		}
		if (g->ready && g->core)
			g->core->Navigate(url.c_str());
		else
		{
			g->pendingUrl = url; // Navigate once the core is ready.
			g->pendingHtml.clear();
		}
		return OK;
	}

	if (!WebCmpI(sub, L"Show"))
	{
		if (g->hostWnd)
		{
			int cmd = g->noActivate ? SW_SHOWNA : SW_SHOW;
			if (aParam2 && wcsstr(aParam2, L"Maximize")) cmd = SW_SHOWMAXIMIZED;
			// Optional w/h in Show options resizes the host.
			if (aParam2 && *aParam2)
			{
				int w=0,h=0; const wchar_t *p=aParam2;
				while (*p){ if((*p==L'w'||*p==L'W')&&iswdigit(p[1]))w=_wtoi(p+1);
					else if((*p==L'h'||*p==L'H')&&iswdigit(p[1]))h=_wtoi(p+1); p++; }
				if (w>0 && h>0)
					SetWindowPos(g->hostWnd,NULL,0,0,w,h,SWP_NOMOVE|SWP_NOZORDER);
			}
			ShowWindow(g->hostWnd, cmd);
			if (!g->noActivate)
				SetForegroundWindow(g->hostWnd);
		}
		return OK;
	}

	if (!WebCmpI(sub, L"Hide"))
	{
		if (g->hostWnd) ShowWindow(g->hostWnd, SW_HIDE);
		return OK;
	}

	if (!WebCmpI(sub, L"Expose"))
	{
		if (!aParam2 || !*aParam2)
			return LineError(_T("WebGui, Expose: a method name is required."), FAIL, aCommand);
		std::wstring method = aParam2;
		std::wstring func = (aParam3 && *aParam3) ? std::wstring(aParam3) : method;
		g->exposed[method] = func;
		return OK;
	}

	if (!WebCmpI(sub, L"On"))
	{
		// Lifecycle events are delivered through the same exposed map (method
		// names start with "__", e.g. __Close, __Ready).
		if (!aParam2 || !*aParam2 || !aParam3 || !*aParam3)
			return LineError(_T("WebGui, On: an event name and function are required."), FAIL, aCommand);
		g->exposed[std::wstring(aParam2)] = aParam3;
		return OK;
	}

	if (!WebCmpI(sub, L"Emit"))
	{
		if (!aParam2 || !*aParam2)
			return LineError(_T("WebGui, Emit: an event name is required."), FAIL, aCommand);
		if (g->core)
		{
			std::wstring data = (aParam3 ? std::wstring(aParam3) : std::wstring());
			std::wstring js = L"window.ahk._emit(\"";
			js += JsonEscapeW(aParam2);
			js += L"\",\"";
			js += JsonEscapeW(data);
			js += L"\");";
			g->core->ExecuteScript(js.c_str(), NULL);
		}
		return OK;
	}

	if (!WebCmpI(sub, L"Eval"))
	{
		if (aParam2 && *aParam2 && g->core)
			g->core->ExecuteScript(aParam2, NULL);
		return OK;
	}

	if (!WebCmpI(sub, L"Mount"))
	{
		// WebGui, Name:Mount, HostName, Folder [, AccessKind]
		// Maps https://<HostName>/ to a local folder so the page loads as a real
		// web origin (ES modules, fetch/XHR, service workers) instead of file://.
		// AccessKind: "" / "CORS" (default, cross-origin allowed) or "Deny".
		if (!aParam2 || !*aParam2 || !aParam3 || !*aParam3)
			return LineError(_T("WebGui, Mount: a host name and folder are required."), FAIL, aCommand);
		int kind = (int)COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW;
		if (aParam4 && *aParam4)
		{
			if (!_wcsicmp(aParam4, L"Deny")) kind = (int)COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY;
			else if (!_wcsicmp(aParam4, L"CORS")) kind = (int)COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS;
		}
		// The WebView2 core is created asynchronously, so a Mount issued right
		// after New (the common case) runs before the core exists. Queue it and
		// apply it when the controller is ready (before the first navigation);
		// apply immediately if the core is already up.
		if (g->ready && g->core)
			WebApplyMount(g, aParam2, aParam3, kind);
		else
		{
			WebGuiType::PendingMount pm; pm.host = aParam2; pm.folder = aParam3; pm.kind = kind;
			g->pendingMounts.push_back(pm);
		}
		return OK;
	}

	if (!WebCmpI(sub, L"Html"))
	{
		// WebGui, Name:Html, HtmlString
		// Load an HTML string directly (WebView2 NavigateToString), so a simple
		// single-file app needs no external .html. The bridge (window.ahk) still
		// works. Origin is about:blank, so ES modules / fetch need Mount instead.
		if (!aParam2) return OK;
		if (g->ready && g->core)
			g->core->NavigateToString(aParam2);
		else
		{
			// Defer: stash as a data: style pending navigation via NavigateToString
			// once ready. We reuse pendingUrl with a marker handled below.
			g->pendingHtml = aParam2;
			g->pendingUrl.clear();
		}
		return OK;
	}

	if (!WebCmpI(sub, L"DevTools"))
	{
		if (g->core) g->core->OpenDevToolsWindow();
		return OK;
	}

	if (!WebCmpI(sub, L"Destroy"))
	{
		for (size_t i = 0; i < g_webGuis.size(); ++i)
			if (g_webGuis[i] == g) { g_webGuis.erase(g_webGuis.begin()+i); break; }
		if (g->core && g->msgToken.value) g->core->remove_WebMessageReceived(g->msgToken);
		if (g->controller){ g->controller->Close(); g->controller->Release(); }
		if (g->core) g->core->Release();
		if (g->env) g->env->Release();
		if (g->hostWnd) DestroyWindow(g->hostWnd);
		delete g;
		return OK;
	}

	return LineError(_T("WebGui: unknown subcommand."), FAIL, sub.c_str());
}

// ----------------------------------------------------------------------------
//  ExecuteScript completion handler used by WebGuiGet,Eval to pull a value back
//  from the page synchronously. The result WebView2 hands us is a JSON string.
// ----------------------------------------------------------------------------
class EvalGetHandler : public ICoreWebView2ExecuteScriptCompletedHandler
{
	LONG mRef;
public:
	bool done;
	std::wstring result;
	EvalGetHandler() : mRef(1), done(false) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2ExecuteScriptCompletedHandler))
		{ *ppv = this; AddRef(); return S_OK; }
		*ppv = NULL; return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&mRef); }
	ULONG STDMETHODCALLTYPE Release() override
	{ LONG r = InterlockedDecrement(&mRef); if (!r) delete this; return r; }
	HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, LPCWSTR resultObjectAsJson) override
	{
		if (SUCCEEDED(errorCode) && resultObjectAsJson)
			result = resultObjectAsJson;
		done = true;
		return S_OK;
	}
};

// Strip one layer of JSON string quoting (WebView2 returns e.g. "\"My Title\"").
// Leaves non-string JSON (numbers, objects) untouched so the script gets raw JSON.
static std::wstring UnquoteJsonString(const std::wstring &in)
{
	if (in.size() >= 2 && in.front() == L'"' && in.back() == L'"')
	{
		std::wstring out; out.reserve(in.size());
		for (size_t i = 1; i + 1 < in.size(); ++i)
		{
			if (in[i] == L'\\' && i + 2 < in.size())
			{
				wchar_t n = in[++i];
				switch (n){case L'n':out+=L'\n';break;case L'r':out+=L'\r';break;
					case L't':out+=L'\t';break;case L'"':out+=L'"';break;
					case L'\\':out+=L'\\';break;case L'/':out+=L'/';break;default:out+=n;}
			}
			else out += in[i];
		}
		return out;
	}
	return in;
}

// ----------------------------------------------------------------------------
//  Line::WebGuiGet  -  ACT_WEBGUIGET runtime entry point.
//  Syntax:  WebGuiGet, OutputVar, Name:SubCommand [, Param3]
//
//  Subcommands:
//    Eval , JsCode   Run JS and store its (JSON) result in OutputVar. The call
//                    is synchronous: it pumps messages until the result arrives
//                    or a 5s safety timeout elapses. A JSON string result is
//                    unquoted to a plain string; other JSON is returned as-is.
//    Hwnd            Store the host window handle (decimal) in OutputVar, so the
//                    stock Win* commands (WinMove, WinSet, ...) can target it.
// ----------------------------------------------------------------------------
ResultType Line::WebGuiGet(Var &aOutputVar, LPTSTR aCommand, LPTSTR aParam3)
{
	std::wstring name, sub;
	WebSplitNameCommand(aCommand, name, sub);
	WebGuiType *g = WebFindByName(name.c_str());
	if (!g)
		return aOutputVar.Assign(); // No such window -> blank, like GuiControlGet on a missing control.

	if (!WebCmpI(sub, L"Hwnd"))
	{
		return aOutputVar.Assign((__int64)(size_t)g->hostWnd);
	}

	if (!WebCmpI(sub, L"Eval"))
	{
		if (!aParam3 || !*aParam3 || !g->core)
			return aOutputVar.Assign();
		EvalGetHandler *h = new EvalGetHandler();
		h->AddRef(); // keep our own reference across the wait
		if (FAILED(g->core->ExecuteScript(aParam3, h)))
		{
			h->Release(); h->Release();
			return aOutputVar.Assign();
		}
		// Pump messages until the completion handler fires or we time out.
		DWORD start = GetTickCount();
		while (!h->done && (GetTickCount() - start) < 5000)
		{
			MSG msg;
			if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			else
			{
				MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
			}
		}
		std::wstring val = h->done ? UnquoteJsonString(h->result) : std::wstring();
		h->Release(); // release our reference; WebView2 released its own
		return aOutputVar.Assign(const_cast<LPTSTR>(val.c_str()), (VarSizeType)val.size());
	}

	return LineError(_T("WebGuiGet: unknown subcommand (use Eval or Hwnd)."), FAIL, sub.c_str());
}

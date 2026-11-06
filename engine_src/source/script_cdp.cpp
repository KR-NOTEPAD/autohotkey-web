// ============================================================================
//  AutoHotkey Web  -  script_cdp.cpp
//  Native CDP support: a WebSocket server (ported from the NOTEPAD.dll set AN
//  module, notepad_wsserver.cpp) that the bundled Cdp.ahk library and a Chrome
//  extension connect to, so Chrome DevTools Protocol browser control works with
//  NO external NOTEPAD.dll. Exposes the server as built-in functions.
//
//    WsServerStart([port=19222]) -> 1/0      start listening
//    WsServerStop()              -> 1        stop
//    WsServerPort()              -> int      current port (0 if stopped)
//    WsServerClients()           -> int      connected clients
//    WsServerPending()           -> int      queued inbound messages
//    WsServerSend(text)          -> int      broadcast a text frame
//    WsServerRecv([timeoutMs=0]) -> string   pull one inbound message ("" none)
//    WsServerLastError()         -> string   last error text
//    WsServerAvailable()         -> 1        (always, since it is built in)
// ============================================================================
#include "stdafx.h"
#include "defines.h"
#include "script.h"
#include "script_func_impl.h"

// The ported server lives in notepad_wsserver.cpp (compiled without the PCH).
// Declare its C exports here; they use __stdcall and the (out, outCch) pattern.
extern "C" {
	int __stdcall Notepad_WsServerStart(int port);
	int __stdcall Notepad_WsServerStop();
	int __stdcall Notepad_WsServerPort();
	int __stdcall Notepad_WsServerClients();
	int __stdcall Notepad_WsServerPending();
	int __stdcall Notepad_WsServerSend(const wchar_t* text);
	int __stdcall Notepad_WsServerRecv(int timeoutMs, wchar_t* out, int outCch);
	int __stdcall Notepad_WsServerLastError(wchar_t* out, int outCch);
	// notepad_json.cpp (cJSON-based JSON path getter + base64 decode)
	int __stdcall Notepad_JsonGet(const wchar_t* json, const wchar_t* path, wchar_t* out, int outCch);
	int __stdcall Notepad_Base64Decode(const wchar_t* b64, wchar_t* out, int outCch);
}

// ---- helpers --------------------------------------------------------------

// Run a (out, outCch) style getter with no extra args into aResultToken.
static void CdpSetStr_LastError(ExprTokenType &aResultToken)
{
	int need = Notepad_WsServerLastError(NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_WsServerLastError(aResultToken.marker, need + 1);
	aResultToken.symbol = SYM_STRING;
}

static void CdpSetStr_Recv(ExprTokenType &aResultToken, int timeoutMs)
{
	// First call (out==NULL) pops one message into an internal holding buffer
	// and returns its length WITHOUT discarding it; the second call fills it.
	int need = Notepad_WsServerRecv(timeoutMs, NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_WsServerRecv(0, aResultToken.marker, need + 1); // 0: take the held message
	aResultToken.symbol = SYM_STRING;
}

// ---- built-in functions ---------------------------------------------------

BIF_DECL(BIF_WsServerStart)
{
	int port = (aParamCount >= 1) ? (int)ParamIndexToInt64(0) : 19222;
	if (port <= 0) port = 19222;
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_WsServerStart(port);
}

BIF_DECL(BIF_WsServerStop)
{
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_WsServerStop();
}

BIF_DECL(BIF_WsServerPort)
{
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_WsServerPort();
}

BIF_DECL(BIF_WsServerClients)
{
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_WsServerClients();
}

BIF_DECL(BIF_WsServerPending)
{
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_WsServerPending();
}

BIF_DECL(BIF_WsServerSend)
{
	LPTSTR text = ParamIndexToString(0);
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_WsServerSend(text ? text : _T(""));
}

BIF_DECL(BIF_WsServerRecv)
{
	int timeoutMs = (aParamCount >= 1) ? (int)ParamIndexToInt64(0) : 0;
	CdpSetStr_Recv(aResultToken, timeoutMs);
}

BIF_DECL(BIF_WsServerLastError)
{
	CdpSetStr_LastError(aResultToken);
}

BIF_DECL(BIF_WsServerAvailable)
{
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = 1; // built in
}

// --- JSON path getter + base64 (support the bundled Cdp.ahk; also general use) ---

BIF_DECL(BIF_JsonGet)
{
	LPTSTR json = ParamIndexToString(0);
	LPTSTR path = (aParamCount >= 2) ? ParamIndexToString(1) : _T("");
	int need = Notepad_JsonGet(json ? json : _T(""), path ? path : _T(""), NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_JsonGet(json ? json : _T(""), path ? path : _T(""), aResultToken.marker, need + 1);
	aResultToken.symbol = SYM_STRING;
}

BIF_DECL(BIF_Base64Decode)
{
	LPTSTR b64 = ParamIndexToString(0);
	int need = Notepad_Base64Decode(b64 ? b64 : _T(""), NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_Base64Decode(b64 ? b64 : _T(""), aResultToken.marker, need + 1);
	aResultToken.symbol = SYM_STRING;
}

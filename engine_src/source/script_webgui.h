// ============================================================================
//  AutoHotkey Web  -  script_webgui.h
//  Native WebGui command: host an HTML/CSS/JS UI (WebView2) inside an AHK v1
//  window and bridge it to the script with an asynchronous JSON message channel.
//
//  Design notes
//  ------------
//  * The grammar of AutoHotkey v1 is unchanged. WebGui is an ordinary command
//    (ACT_WEBGUI) parsed like Gui: "WebGui, SubCommand [, P2, P3, P4]".
//  * WebView2 raises its events on the UI thread, but *outside* the AHK
//    interpreter. Handlers therefore never touch the script directly; they only
//    PostMessage(AHK_WEB_EVENT) to the main window and queue the payload. The
//    real script dispatch happens from MsgSleep, where every GUI/menu/hotkey
//    event runs, so Critical / #MaxThreads / Thread priority all apply.
//  * The bridge marshals strings in both directions. A script handler exposed
//    with "WebGui, Name:Expose, Func" is an AHK function taking one string
//    parameter (the request's params as JSON text) and returning a string.
// ============================================================================
#ifndef script_webgui_h
#define script_webgui_h

#include "defines.h"
#include <windows.h>

// Forward decls to avoid pulling WebView2 headers into the whole project.
struct WebGuiType;

// Called once at startup (after the main window exists) and at shutdown.
void WebGuiShutdownAll();

// Returns the folder where this EXE's embedded web assets were extracted, or an
// empty string if there are none (uncompiled run, or compiled without assets).
// Implemented in script_webassets.cpp. Backs the A_AhkWebAssets built-in var.
const wchar_t *WebAssetsGetDir();

// Drains one queued web event (called by the main message loop when it receives
// AHK_WEB_EVENT). Returns true if an event was dispatched.
bool WebGuiProcessEvent();

// The ACT_WEBGUI command implementation (defined in script_webgui.cpp).
// Declared as a member of Line in script.h via LINE_WEBGUI_DECL below.

#endif // script_webgui_h

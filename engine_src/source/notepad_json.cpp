// ============================================================================
//  AutoHotkey Web  -  notepad_json.cpp   (compiled WITHOUT the PCH)
//  Native JSON path getter + Base64 decode, ported from the NOTEPAD.dll
//  notepad_exports.cpp so the bundled Cdp.ahk library (and scripts) need no
//  external NOTEPAD.dll. cJSON-based, so boolean/null tokens are returned as
//  "true"/"false"/"null" exactly like the original (Cdp.ahk relies on this).
//
//    Notepad_JsonGet(json, "a.b.c", out, outCch)  -> value text (2-call)
//    Notepad_Base64Decode(b64, out, outCch)       -> decoded UTF-8 text
// ============================================================================
#include <windows.h>
#include <wincrypt.h>
#include <string>
#include <cstdio>
#include "cJSON.h"

#pragma comment(lib, "crypt32.lib")

namespace {

std::string JWToUtf8(const wchar_t* w) {
	if (!w || !*w) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
	return s;
}
std::wstring JUtf8ToW(const std::string& s) {
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n > 0 ? n : 0, L'\0');
	if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}
int JWriteOut(const std::wstring& s, wchar_t* out, int outCch) {
	int need = (int)s.size();
	if (out == nullptr || outCch == 0) return need;
	if (outCch < need + 1) return -need;
	memcpy(out, s.data(), s.size() * sizeof(wchar_t));
	out[s.size()] = L'\0';
	return need;
}

// Navigate a dotted path (object keys or array indices). Ported verbatim.
cJSON* jsonNavigate(cJSON* root, const std::string& path) {
	cJSON* cur = root;
	size_t start = 0;
	while (start <= path.size() && cur) {
		size_t dot = path.find('.', start);
		std::string key = (dot == std::string::npos)
			? path.substr(start) : path.substr(start, dot - start);
		if (!key.empty()) {
			if (cJSON_IsArray(cur)) {
				int idx = atoi(key.c_str());
				cur = cJSON_GetArrayItem(cur, idx);
			} else if (cJSON_IsObject(cur)) {
				cur = cJSON_GetObjectItemCaseSensitive(cur, key.c_str());
			} else {
				return nullptr;
			}
		}
		if (dot == std::string::npos) break;
		start = dot + 1;
	}
	return cur;
}

} // anonymous

extern "C" {

__declspec(dllexport) int __stdcall Notepad_JsonGet(const wchar_t* json,
		const wchar_t* path, wchar_t* out, int outCch) {
	std::string j = JWToUtf8(json);
	std::string p = JWToUtf8(path);
	cJSON* root = cJSON_Parse(j.c_str());
	if (!root) return 0;
	cJSON* node = p.empty() ? root : jsonNavigate(root, p);
	if (!node) { cJSON_Delete(root); return 0; }

	std::string result;
	bool ok = true;
	if (cJSON_IsString(node)) {
		result = node->valuestring ? node->valuestring : "";
	} else if (cJSON_IsNumber(node)) {
		double v = node->valuedouble;
		char buf[64];
		if (v == (double)(long long)v)
			snprintf(buf, sizeof(buf), "%lld", (long long)v);
		else
			snprintf(buf, sizeof(buf), "%g", v);
		result = buf;
	} else if (cJSON_IsBool(node)) {
		result = cJSON_IsTrue(node) ? "true" : "false";
	} else if (cJSON_IsNull(node)) {
		result = "null";
	} else {
		char* s = cJSON_PrintUnformatted(node);
		if (s) { result = s; cJSON_free(s); }
		else ok = false;
	}
	cJSON_Delete(root);
	if (!ok) return 0;
	return JWriteOut(JUtf8ToW(result), out, outCch);
}

// Decode standard base64 to UTF-8 text (CDP text response bodies). Binary data
// should be written to a file by the caller (Cdp.ahk uses its own crypt32 path
// for screenshots/PDF), so a text decode is enough here.
__declspec(dllexport) int __stdcall Notepad_Base64Decode(const wchar_t* b64,
		wchar_t* out, int outCch) {
	if (!b64) return JWriteOut(L"", out, outCch);
	DWORD cb = 0;
	if (!CryptStringToBinaryW(b64, 0, CRYPT_STRING_BASE64, NULL, &cb, NULL, NULL) || cb == 0)
		return JWriteOut(L"", out, outCch);
	std::string bin(cb, '\0');
	if (!CryptStringToBinaryW(b64, 0, CRYPT_STRING_BASE64, (BYTE*)&bin[0], &cb, NULL, NULL))
		return JWriteOut(L"", out, outCch);
	bin.resize(cb);
	return JWriteOut(JUtf8ToW(bin), out, outCch);
}

} // extern "C"

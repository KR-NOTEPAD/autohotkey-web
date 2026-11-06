// ============================================================================
//  AutoHotkey Web  -  script_ext.cpp
//  Built-in functions for the toolkit ported from NOTEPAD.dll (source\notepad\*).
//
//  Every ported C export uses the same convention:
//      int|__int64 __stdcall Notepad_X(<string|int args...> [, wchar_t* out, int outCch])
//  so instead of ~150 hand-written BIFs, one table maps an AHK function name to the
//  export plus a tiny signature spec, and BIF_ExtGeneric marshals the call.
//  Naming rule: plugin wrapper Note_X()  ->  built-in X()   (exceptions noted in the table).
//
//  Spec syntax  "p1|p2|...>r"
//      param  s = string, i = int, I = int64, b = bool(int)      optional:  type=default
//      ret    o = string via (out,outCch)   i = int   I = int64
//  Params after the first one carrying a default are optional.
// ============================================================================
#include "stdafx.h" // pre-compiled headers
#include "defines.h"
#include "globaldata.h"
#include "script.h"
#include "script_object.h"
#include "script_func_impl.h"
#include "cJSON.h"

// Libraries the ported toolkit needs (kept here so every build configuration gets them).
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "normaliz.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

// ---- exports (address only; the real prototypes live in source\notepad\*) ----
// extern "C" names are undecorated on x64, so a generic declaration links to the real function.
#define NX(n) extern "C" void __stdcall Notepad_##n(void);
// crypto / encoding
NX(SHA256) NX(SHA1) NX(SHA384) NX(SHA512) NX(MD5) NX(HMAC) NX(HMAC512) NX(CRC32) NX(Adler32) NX(Fnv1a)
NX(SHA256File) NX(CRC32File) NX(VerifyFileHash) NX(Base64Encode) NX(Base64UrlEncode) NX(Base64UrlDecode)
NX(HexEncode) NX(HexDecode) NX(RandomBase64) NX(RandInt) NX(UUID) NX(EncryptPassword) NX(DecryptPassword)
NX(EncryptFile) NX(DecryptFile) NX(HashPassword) NX(VerifyPassword) NX(DpapiProtect) NX(DpapiUnprotect)
NX(GenKeyPair) NX(Sign) NX(Verify) NX(Entropy) NX(LuhnCheck)
// compression / zip / encoding / files
NX(Compress) NX(Decompress) NX(CompressFile) NX(DecompressFile) NX(ZipAddFile) NX(ZipList) NX(ZipExtractAll)
NX(ZipExtractFile) NX(DetectEncoding) NX(ConvertToUtf8) NX(FileRecycle) NX(DirRecycle) NX(DirList)
NX(SpecialFolder) NX(TempFile) NX(FormatBytes) NX(WatchFolder) NX(IniKeys)
// http / url / json / websocket client
NX(HttpGet) NX(HttpPost) NX(HttpLastStatus) NX(HttpDownload) NX(DownloadStart) NX(DownloadStatus) NX(DownloadFree)
NX(HttpJobStart) NX(HttpJobStatus) NX(HttpJobResult) NX(HttpJobHeaders) NX(HttpJobError) NX(HttpJobFree)
NX(HttpJobWaitAny) NX(HttpJobSetConcurrency) NX(HttpJobSetMinGap) NX(HttpJobCancelAll) NX(HttpJobPoolInfo)
NX(UrlEncode) NX(UrlDecode) NX(QueryGet) NX(HtmlEncode) NX(HtmlDecode) NX(JsonValid) NX(JsonPretty)
NX(WsConnect) NX(WsSend) NX(WsReceive) NX(WsClose)
// screen / image / qr / clipboard image
NX(ImageSearchAll) NX(CaptureRegion) NX(QrEncode) NX(ClipboardImageToPng) NX(PngToClipboardImage)
NX(ImageWidth) NX(ImageHeight) NX(ImageResize) NX(ImageConvert) NX(ImageEnhance) NX(ImageQuality)
NX(SelectRegion) NX(MonitorsJson) NX(DetectColor) NX(MatchTemplate) NX(ColorDistance) NX(RgbToHsv) NX(HsvToRgb)
// ocr
NX(OcrImage) NX(OcrScreen) NX(OcrLanguages) NX(OcrImageAI) NX(OcrScreenAI) NX(OcrAIError)
// strings / similarity / csv / time / math
NX(Upper) NX(Lower) NX(TitleCase) NX(Repeat) NX(PadLeft) NX(PadRight) NX(Reverse) NX(CountSub)
NX(NaturalCompare) NX(ToBase) NX(FromBase) NX(Slugify) NX(ToRoman) NX(FromRoman) NX(HangulChosung) NX(ToHalfWidth)
NX(Levenshtein) NX(SimilarityRatio) NX(JaroWinkler) NX(FuzzyBestMatch) NX(FuzzyBestMatchEx) NX(DamerauLevenshtein)
NX(DamerauRatio) NX(Hamming) NX(JaccardChar) NX(DiceCoefficient) NX(CosineChar) NX(LcsLength) NX(NgramSimilarity)
NX(CsvGet) NX(CsvRowCount) NX(CsvColCount) NX(NowUnix) NX(UnixToIso) NX(IsoToUnix) NX(Gcd) NX(Lcm)
#undef NX

// ---- shims: argument order / double parameters the generic caller can't pass ----
extern "C" int __stdcall Notepad_HttpUploadFile(const wchar_t* url, const wchar_t* fieldName, const wchar_t* filePath,
	const wchar_t* fileName, const wchar_t* extraFields, wchar_t* out, int outCch);
extern "C" int __stdcall Notepad_ImageRotate(const wchar_t* inPath, const wchar_t* outPath, double angleDeg,
	int keepSize, int bgRgb, int quality);
extern "C" int __stdcall Notepad_CaptureRegionRotated(int x, int y, int w, int h, double angleDeg,
	const wchar_t* outPath, int quality);

// Same parameter order as the plugin wrapper Note_HttpUploadFile(url, filePath, fieldName, fileName, extra).
static int __stdcall Shim_HttpUploadFile(const wchar_t* url, const wchar_t* filePath, const wchar_t* fieldName,
	const wchar_t* fileName, const wchar_t* extra, wchar_t* out, int outCch)
{
	return Notepad_HttpUploadFile(url, fieldName, filePath, fileName, extra, out, outCch);
}
static int __stdcall Shim_ImageRotate(const wchar_t* inPath, const wchar_t* outPath, const wchar_t* angle,
	int keepSize, int bgRgb, int quality)
{
	return Notepad_ImageRotate(inPath, outPath, _wtof(angle), keepSize, bgRgb, quality);
}
static int __stdcall Shim_CaptureRegionRotated(int x, int y, int w, int h, const wchar_t* angle,
	const wchar_t* outPath, int quality)
{
	return Notepad_CaptureRegionRotated(x, y, w, h, _wtof(angle), outPath, quality);
}

BIF_DECL(BIF_ExtGeneric);
BIF_DECL(BIF_JsonParse);
BIF_DECL(BIF_JsonStringify);

struct ExtBifEntry
{
	LPCTSTR name;
	void *fn;            // Notepad_* export (generic) or NULL
	LPCTSTR spec;        // signature (generic) or NULL
	BuiltInFunctionType bif; // dedicated BIF (fn == NULL)
	int min_params, max_params; // only for dedicated BIFs
};

#define G(name, fn, spec) { _T(name), (void *)fn, _T(spec), NULL, 0, 0 },
#define D(name, bif, mn, mx) { _T(name), NULL, NULL, bif, mn, mx },

// Sorted case-insensitively by name at first use (binary search afterwards).
static ExtBifEntry sExtBif[] =
{
	// ---------------------------------------------------------- crypto / encoding
	G("SHA256", Notepad_SHA256, "s>o")
	G("SHA1", Notepad_SHA1, "s>o")
	G("SHA384", Notepad_SHA384, "s>o")
	G("SHA512", Notepad_SHA512, "s>o")
	G("MD5", Notepad_MD5, "s>o")
	G("HMAC", Notepad_HMAC, "s|s>o")
	G("HMAC512", Notepad_HMAC512, "s|s>o")
	G("CRC32", Notepad_CRC32, "s>o")
	G("Adler32", Notepad_Adler32, "s>o")
	G("Fnv1a", Notepad_Fnv1a, "s>o")
	G("SHA256File", Notepad_SHA256File, "s>o")
	G("CRC32File", Notepad_CRC32File, "s>o")
	G("VerifyFileHash", Notepad_VerifyFileHash, "s|s>i")
	G("Base64Encode", Notepad_Base64Encode, "s>o")       // Base64Decode is the earlier native (script_cdp.cpp)
	G("Base64UrlEncode", Notepad_Base64UrlEncode, "s>o")
	G("Base64UrlDecode", Notepad_Base64UrlDecode, "s>o")
	G("HexEncode", Notepad_HexEncode, "s>o")
	G("HexDecode", Notepad_HexDecode, "s>o")
	G("RandomBase64", Notepad_RandomBase64, "i=32>o")
	G("RandInt", Notepad_RandInt, "i|i>i")
	G("UUID", Notepad_UUID, ">o")
	G("EncryptPassword", Notepad_EncryptPassword, "s|s|i=100000>o")
	G("DecryptPassword", Notepad_DecryptPassword, "s|s>o")
	G("EncryptFile", Notepad_EncryptFile, "s|s|s|i=100000>i")
	G("DecryptFile", Notepad_DecryptFile, "s|s|s>i")
	G("HashPassword", Notepad_HashPassword, "s|i=100000>o")
	G("VerifyPassword", Notepad_VerifyPassword, "s|s>i")
	G("DpapiProtect", Notepad_DpapiProtect, "s|s=|b=0>o")
	G("DpapiUnprotect", Notepad_DpapiUnprotect, "s|s=>o")
	G("EcdsaKeyPair", Notepad_GenKeyPair, ">o")          // Note_GenKeyPair (renamed: generic name)
	G("EcdsaSign", Notepad_Sign, "s|s>o")                // Note_Sign   (Sign() would read as math sign)
	G("EcdsaVerify", Notepad_Verify, "s|s|s>i")          // Note_Verify
	G("StrEntropy", Notepad_Entropy, "s>i")              // Note_Entropy
	G("LuhnCheck", Notepad_LuhnCheck, "s>i")
	// ---------------------------------------------------------- compression / zip / encoding / files
	G("Compress", Notepad_Compress, "s|i=-1>o")
	G("Decompress", Notepad_Decompress, "s>o")
	G("CompressFile", Notepad_CompressFile, "s|s|i=-1>i")
	G("DecompressFile", Notepad_DecompressFile, "s|s>i")
	G("ZipAddFile", Notepad_ZipAddFile, "s|s|s|i=-1>i")
	G("ZipList", Notepad_ZipList, "s>o")
	G("ZipExtractAll", Notepad_ZipExtractAll, "s|s>i")
	G("ZipExtractFile", Notepad_ZipExtractFile, "s|s|s>i")
	G("DetectEncoding", Notepad_DetectEncoding, "s>o")
	G("ConvertToUtf8", Notepad_ConvertToUtf8, "s|s>i")
	G("FileRecycle", Notepad_FileRecycle, "s>i")
	G("DirRecycle", Notepad_DirRecycle, "s>i")
	G("DirList", Notepad_DirList, "s|s=*>o")
	G("SpecialFolder", Notepad_SpecialFolder, "s>o")
	G("TempFile", Notepad_TempFile, ">o")
	G("FormatBytes", Notepad_FormatBytes, "s>o")
	G("WatchFolder", Notepad_WatchFolder, "s|b=1|i=10000>o")
	G("IniKeys", Notepad_IniKeys, "s|s>o")
	// ---------------------------------------------------------- http / url / json / websocket client
	G("HttpGet", Notepad_HttpGet, "s>o")
	G("HttpPost", Notepad_HttpPost, "s|s=|s=application/x-www-form-urlencoded>o")
	G("HttpLastStatus", Notepad_HttpLastStatus, ">i")
	G("HttpDownload", Notepad_HttpDownload, "s|s>i")
	G("HttpUploadFile", Shim_HttpUploadFile, "s|s|s=file|s=|s=>o")
	G("DownloadStart", Notepad_DownloadStart, "s|s>i")
	G("DownloadStatus", Notepad_DownloadStatus, "i>o")
	G("DownloadFree", Notepad_DownloadFree, "i>i")
	G("HttpJobStart", Notepad_HttpJobStart, "s|s|s=|s=|i=30000|i=3>i")
	G("HttpJobStatus", Notepad_HttpJobStatus, "i>o")
	G("HttpJobResult", Notepad_HttpJobResult, "i>o")
	G("HttpJobHeaders", Notepad_HttpJobHeaders, "i>o")
	G("HttpJobError", Notepad_HttpJobError, "i>o")
	G("HttpJobFree", Notepad_HttpJobFree, "i>i")
	G("HttpJobWaitAny", Notepad_HttpJobWaitAny, "i=100>i")
	G("HttpJobSetConcurrency", Notepad_HttpJobSetConcurrency, "i=6>i")
	G("HttpJobSetMinGap", Notepad_HttpJobSetMinGap, "i=0>i")
	G("HttpJobCancelAll", Notepad_HttpJobCancelAll, ">i")
	G("HttpJobPoolInfo", Notepad_HttpJobPoolInfo, ">o")
	G("UrlEncode", Notepad_UrlEncode, "s>o")
	G("UrlDecode", Notepad_UrlDecode, "s>o")
	G("QueryGet", Notepad_QueryGet, "s|s>o")
	G("HtmlEncode", Notepad_HtmlEncode, "s>o")
	G("HtmlDecode", Notepad_HtmlDecode, "s>o")
	G("JsonValid", Notepad_JsonValid, "s>i")
	G("JsonPretty", Notepad_JsonPretty, "s>o")
	D("JsonParse", BIF_JsonParse, 1, 1)
	D("JsonStringify", BIF_JsonStringify, 1, 2)
	G("WsConnect", Notepad_WsConnect, "s>i")
	G("WsSend", Notepad_WsSend, "i|s>i")
	G("WsReceive", Notepad_WsReceive, "i>o")
	G("WsClose", Notepad_WsClose, "i>i")
	// ---------------------------------------------------------- screen / image / qr / clipboard image
	G("ImageSearchAll", Notepad_ImageSearchAll, "s|i|i|i|i|i=10|i=0>o")
	G("CaptureRegion", Notepad_CaptureRegion, "i|i|i|i|s>i")
	G("CaptureRegionRotated", Shim_CaptureRegionRotated, "i|i|i|i|s|s|i=95>i")
	G("QrEncode", Notepad_QrEncode, "s|s|i=8|i=4|i=1>i")
	G("ClipboardImageToPng", Notepad_ClipboardImageToPng, "s>i")
	G("PngToClipboardImage", Notepad_PngToClipboardImage, "s>i")
	G("ImageWidth", Notepad_ImageWidth, "s>i")
	G("ImageHeight", Notepad_ImageHeight, "s>i")
	G("ImageResize", Notepad_ImageResize, "s|s|i|i|i=85>i")
	G("ImageConvert", Notepad_ImageConvert, "s|s|i=75>i")
	G("ImageRotate", Shim_ImageRotate, "s|s|s|b=1|i=0|i=95>i")
	G("ImageEnhance", Notepad_ImageEnhance, "s|s|i=200|s=auto|i=95>i")
	G("ImageQuality", Notepad_ImageQuality, "s>o")
	G("SelectRegion", Notepad_SelectRegion, ">o")
	G("MonitorsJson", Notepad_MonitorsJson, ">o")
	G("DetectColor", Notepad_DetectColor, "i|i|i|i|i|i=10|i=20>o")
	G("MatchTemplate", Notepad_MatchTemplate, "s|i|i|i|i|i=80|i=120|i=10|i=2>o")
	G("ColorDistance", Notepad_ColorDistance, "i|i>i")
	G("RgbToHsv", Notepad_RgbToHsv, "i>o")
	G("HsvToRgb", Notepad_HsvToRgb, "i|i|i>i")
	// ---------------------------------------------------------- OCR (Windows built-in + AI Korean)
	G("OcrImage", Notepad_OcrImage, "s|s=>o")
	G("OcrScreen", Notepad_OcrScreen, "i|i|i|i|s=>o")
	G("OcrLanguages", Notepad_OcrLanguages, ">o")
	G("OcrImageAI", Notepad_OcrImageAI, "s|s=|s=>o")
	G("OcrScreenAI", Notepad_OcrScreenAI, "i|i|i|i|s=|s=>o")
	G("OcrAIError", Notepad_OcrAIError, ">o")
	// ---------------------------------------------------------- strings (v2-style Str* names)
	G("StrUpper", Notepad_Upper, "s>o")
	G("StrLower", Notepad_Lower, "s>o")
	G("StrTitle", Notepad_TitleCase, "s>o")
	G("StrRepeat", Notepad_Repeat, "s|i>o")
	G("StrPadLeft", Notepad_PadLeft, "s|i|s= >o")
	G("StrPadRight", Notepad_PadRight, "s|i|s= >o")
	G("StrReverse", Notepad_Reverse, "s>o")
	G("StrCount", Notepad_CountSub, "s|s>i")
	G("NaturalCompare", Notepad_NaturalCompare, "s|s>i")
	G("ToBase", Notepad_ToBase, "s|i>o")
	G("FromBase", Notepad_FromBase, "s|i>o")
	G("Slugify", Notepad_Slugify, "s>o")
	G("ToRoman", Notepad_ToRoman, "i>o")
	G("FromRoman", Notepad_FromRoman, "s>i")
	G("HangulChosung", Notepad_HangulChosung, "s>o")
	G("ToHalfWidth", Notepad_ToHalfWidth, "s>o")
	G("Levenshtein", Notepad_Levenshtein, "s|s>i")
	G("SimilarityRatio", Notepad_SimilarityRatio, "s|s>i")
	G("JaroWinkler", Notepad_JaroWinkler, "s|s>i")
	G("DamerauLevenshtein", Notepad_DamerauLevenshtein, "s|s>i")
	G("DamerauRatio", Notepad_DamerauRatio, "s|s>i")
	G("Hamming", Notepad_Hamming, "s|s>i")
	G("JaccardChar", Notepad_JaccardChar, "s|s>i")
	G("DiceCoefficient", Notepad_DiceCoefficient, "s|s>i")
	G("CosineChar", Notepad_CosineChar, "s|s>i")
	G("LcsLength", Notepad_LcsLength, "s|s>i")
	G("NgramSimilarity", Notepad_NgramSimilarity, "s|s|i=2>i")
	G("FuzzyBestMatch", Notepad_FuzzyBestMatch, "s|s>o")
	G("FuzzyBestMatchEx", Notepad_FuzzyBestMatchEx, "s|s|s=lev>o")
	G("CsvGet", Notepad_CsvGet, "s|i|i>o")
	G("CsvRowCount", Notepad_CsvRowCount, "s>i")
	G("CsvColCount", Notepad_CsvColCount, "s|i=1>i")
	G("NowUnix", Notepad_NowUnix, ">o")
	G("UnixToIso", Notepad_UnixToIso, "s>o")
	G("IsoToUnix", Notepad_IsoToUnix, "s>o")
	G("Gcd", Notepad_Gcd, "i|i>i")
	G("Lcm", Notepad_Lcm, "i|i>I")
};
#undef G
#undef D

static const int sExtBifCount = _countof(sExtBif);
static bool sExtBifSorted = false;

static int ExtBifCompare(const void *a, const void *b)
{
	return _tcsicmp(((const ExtBifEntry *)a)->name, ((const ExtBifEntry *)b)->name);
}

static ExtBifEntry *ExtBifLookup(LPCTSTR aName)
{
	if (!sExtBifSorted)
	{
		qsort(sExtBif, sExtBifCount, sizeof(ExtBifEntry), ExtBifCompare);
		sExtBifSorted = true;
	}
	int left = 0, right = sExtBifCount - 1;
	while (left <= right)
	{
		int mid = (left + right) / 2;
		int r = _tcsicmp(aName, sExtBif[mid].name);
		if (r > 0) left = mid + 1;
		else if (r < 0) right = mid - 1;
		else return &sExtBif[mid];
	}
	return NULL;
}

// Parse a spec into parameter descriptors.
#define EXT_MAX_PARAMS 12
struct ExtParam { TCHAR type; LPCTSTR def; size_t def_len; bool has_def; };
struct ExtSpec { ExtParam p[EXT_MAX_PARAMS]; int count, min; TCHAR ret; };

static void ExtParseSpec(LPCTSTR aSpec, ExtSpec &aOut)
{
	aOut.count = 0; aOut.min = 0; aOut.ret = 'i';
	LPCTSTR cp = aSpec;
	bool seen_default = false;
	while (*cp && *cp != '>' && aOut.count < EXT_MAX_PARAMS)
	{
		ExtParam &p = aOut.p[aOut.count];
		p.type = *cp++;
		p.has_def = false; p.def = _T(""); p.def_len = 0;
		if (*cp == '=')
		{
			++cp;
			p.has_def = true;
			p.def = cp;
			while (*cp && *cp != '|' && *cp != '>') ++cp;
			p.def_len = cp - p.def;
			seen_default = true;
		}
		if (!seen_default) aOut.min = aOut.count + 1;
		++aOut.count;
		if (*cp == '|') ++cp;
	}
	if (*cp == '>') aOut.ret = cp[1];
}

bool AhkWebExt_FindBif(LPCTSTR aName, BuiltInFunctionType &aBif, int &aMinParams, int &aMaxParams)
{
	ExtBifEntry *e = ExtBifLookup(aName);
	if (!e) return false;
	if (!e->fn)
	{
		aBif = e->bif;
		aMinParams = e->min_params;
		aMaxParams = e->max_params;
		return true;
	}
	ExtSpec spec;
	ExtParseSpec(e->spec, spec);
	aBif = BIF_ExtGeneric;
	aMinParams = spec.min;
	aMaxParams = spec.count;
	return true;
}

typedef INT_PTR (__stdcall *ExtFn)(INT_PTR, INT_PTR, INT_PTR, INT_PTR, INT_PTR, INT_PTR, INT_PTR,
	INT_PTR, INT_PTR, INT_PTR, INT_PTR, INT_PTR, INT_PTR, INT_PTR);

// Call an Notepad_* export by name-table entry with AHK params.  Used by the BIF and by
// script_chain.cpp (string methods route through the same table).
void AhkWebExt_Call(LPCTSTR aName, ExprTokenType &aResultToken, ExprTokenType *aParam[], int aParamCount)
{
	aResultToken.symbol = SYM_STRING;
	aResultToken.marker = _T("");
	ExtBifEntry *e = ExtBifLookup(aName);
	if (!e || !e->fn)
		return;
	ExtSpec spec;
	ExtParseSpec(e->spec, spec);

	INT_PTR a[14] = {0};
	TCHAR numbuf[EXT_MAX_PARAMS][MAX_NUMBER_SIZE];
	TCHAR *defbuf[EXT_MAX_PARAMS] = {0};
	int k;
	for (k = 0; k < spec.count; ++k)
	{
		ExtParam &p = spec.p[k];
		bool given = k < aParamCount && aParam[k]->symbol != SYM_MISSING;
		switch (p.type)
		{
		case 's':
			if (given)
				a[k] = (INT_PTR)TokenToString(*aParam[k], numbuf[k]);
			else
			{
				// Default text is not NUL-terminated inside the spec; copy it.
				defbuf[k] = (TCHAR *)_alloca((p.def_len + 1) * sizeof(TCHAR));
				memcpy(defbuf[k], p.def, p.def_len * sizeof(TCHAR));
				defbuf[k][p.def_len] = '\0';
				a[k] = (INT_PTR)defbuf[k];
			}
			break;
		case 'b':
			a[k] = given ? (TokenToBOOL(*aParam[k]) ? 1 : 0) : (INT_PTR)_tcstoi64(p.def, NULL, 0);
			break;
		case 'I':
			a[k] = given ? (INT_PTR)TokenToInt64(*aParam[k]) : (INT_PTR)_tcstoi64(p.def, NULL, 0);
			break;
		default: // 'i'
			a[k] = given ? (INT_PTR)(int)TokenToInt64(*aParam[k]) : (INT_PTR)(int)_tcstoi64(p.def, NULL, 0);
			break;
		}
	}

	ExtFn fn = (ExtFn)e->fn;
	if (spec.ret != 'o')
	{
		INT_PTR r = fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13]);
		aResultToken.symbol = SYM_INTEGER;
		aResultToken.value_int64 = spec.ret == 'I' ? (__int64)r : (__int64)(int)r;
		return;
	}

	// String result through (out, outCch).  One generous call first; the plugin functions
	// return -needed (or needed) when the buffer is too small, and cache expensive results
	// (HTTP, OCR) for exactly this second call.
	int cap = 32768;
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		wchar_t *buf = (wchar_t *)malloc((cap + 1) * sizeof(wchar_t));
		if (!buf)
			return;
		buf[0] = 0;
		a[spec.count] = (INT_PTR)buf;
		a[spec.count + 1] = (INT_PTR)(cap + 1);
		int r = (int)fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13]);
		if (r > 0 && r <= cap)
		{
			TokenSetResult(aResultToken, buf, r);
			aResultToken.symbol = SYM_STRING;
			free(buf);
			return;
		}
		free(buf);
		if (r == 0)
			return; // failure or empty result
		int need = r < 0 ? -r : r;
		if (need <= cap)
			return; // inconsistent; give up rather than loop
		cap = need + 16;
	}
}

BIF_DECL(BIF_ExtGeneric)
{
	// aResultToken.marker holds the name the function was called by (case as first written).
	TCHAR name[MAX_VAR_NAME_LENGTH + 1];
	tcslcpy(name, aResultToken.marker, _countof(name));
	AhkWebExt_Call(name, aResultToken, aParam, aParamCount);
}


// ============================================================================
//  JSON  <->  AHK objects
// ============================================================================

static char *ExtWideToUtf8(LPCTSTR aText, size_t aLen = -1)
{
	if (aLen == (size_t)-1) aLen = _tcslen(aText);
	int n = WideCharToMultiByte(CP_UTF8, 0, aText, (int)aLen, NULL, 0, NULL, NULL);
	char *s = (char *)malloc(n + 1);
	if (!s) return NULL;
	WideCharToMultiByte(CP_UTF8, 0, aText, (int)aLen, s, n, NULL, NULL);
	s[n] = 0;
	return s;
}

static LPTSTR ExtUtf8ToWide(const char *aText)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, aText, -1, NULL, 0);
	LPTSTR w = (LPTSTR)malloc((n + 1) * sizeof(TCHAR));
	if (!w) return NULL;
	MultiByteToWideChar(CP_UTF8, 0, aText, -1, w, n);
	w[n] = 0;
	return w;
}

// Convert a cJSON node into aToken.  Objects are returned with one reference owned by the caller.
// Strings are malloc'ed into *aFree (caller frees after assigning).
static void JsonNodeToToken(cJSON *aNode, ExprTokenType &aToken, LPTSTR &aFree)
{
	aFree = NULL;
	if (cJSON_IsObject(aNode) || cJSON_IsArray(aNode))
	{
		bool is_array = cJSON_IsArray(aNode) != 0;
		Object *obj = Object::Create();
		int index = 0;
		for (cJSON *child = aNode->child; obj && child; child = child->next)
		{
			ExprTokenType val;
			LPTSTR val_free;
			JsonNodeToToken(child, val, val_free);
			if (is_array)
			{
				ExprTokenType key;
				key.symbol = SYM_INTEGER;
				key.value_int64 = ++index;
				obj->SetItem(key, val);
			}
			else
			{
				LPTSTR wkey = ExtUtf8ToWide(child->string ? child->string : "");
				if (wkey)
				{
					obj->SetItem(wkey, val);
					free(wkey);
				}
			}
			if (val.symbol == SYM_OBJECT)
				val.object->Release(); // SetItem took its own reference.
			if (val_free)
				free(val_free);
		}
		aToken.symbol = SYM_OBJECT;
		aToken.object = obj;
		if (!obj) { aToken.symbol = SYM_STRING; aToken.marker = _T(""); }
		return;
	}
	if (cJSON_IsTrue(aNode)) { aToken.symbol = SYM_INTEGER; aToken.value_int64 = 1; return; }
	if (cJSON_IsFalse(aNode)) { aToken.symbol = SYM_INTEGER; aToken.value_int64 = 0; return; }
	if (cJSON_IsNumber(aNode))
	{
		double d = aNode->valuedouble;
		if (d == (double)(__int64)d && d > -9.0e15 && d < 9.0e15)
		{
			aToken.symbol = SYM_INTEGER;
			aToken.value_int64 = (__int64)d;
		}
		else
		{
			// Keep non-integers as numeric strings ("1.5"), exactly as a v1 literal would be stored;
			// a SYM_FLOAT would display as "1.500000" under the default float format.
			aFree = (LPTSTR)malloc(40 * sizeof(TCHAR));
			if (aFree)
			{
				_stprintf_s(aFree, 40, _T("%.15g"), d);
				aToken.symbol = SYM_STRING;
				aToken.marker = aFree;
			}
			else
			{
				aToken.symbol = SYM_FLOAT;
				aToken.value_double = d;
			}
		}
		return;
	}
	if (cJSON_IsString(aNode) && aNode->valuestring)
	{
		aFree = ExtUtf8ToWide(aNode->valuestring);
		aToken.symbol = SYM_STRING;
		aToken.marker = aFree ? aFree : _T("");
		return;
	}
	aToken.symbol = SYM_STRING; // null
	aToken.marker = _T("");
}

// Exposed for the string method .JsonParse() in script_chain.cpp.
void AhkWebExt_JsonParse(LPCTSTR aText, ExprTokenType &aResultToken)
{
	aResultToken.symbol = SYM_STRING;
	aResultToken.marker = _T("");
	char *u8 = ExtWideToUtf8(aText);
	if (!u8) return;
	cJSON *root = cJSON_Parse(u8);
	free(u8);
	if (!root) return;
	ExprTokenType val;
	LPTSTR val_free;
	JsonNodeToToken(root, val, val_free);
	cJSON_Delete(root);
	if (val.symbol == SYM_OBJECT)
	{
		aResultToken.symbol = SYM_OBJECT;
		aResultToken.object = val.object; // transfer the reference
	}
	else if (val.symbol == SYM_STRING)
	{
		TokenSetResult(aResultToken, val.marker);
		aResultToken.symbol = SYM_STRING;
	}
	else
	{
		aResultToken.symbol = val.symbol;
		aResultToken.value_int64 = val.value_int64;
	}
	if (val_free) free(val_free);
}

BIF_DECL(BIF_JsonParse)
{
	TCHAR buf[MAX_NUMBER_SIZE];
	AhkWebExt_JsonParse(ParamIndexToString(0, buf), aResultToken);
}

// Serializer lives in script_chain.cpp (needs Object internals).
LPTSTR AhkWebChain_ToJson(ExprTokenType &aValue, LPCTSTR aIndent);

BIF_DECL(BIF_JsonStringify)
{
	TCHAR indent_buf[MAX_NUMBER_SIZE] = _T("");
	LPCTSTR indent = _T("");
	if (!ParamIndexIsOmitted(1))
	{
		if (TokenIsPureNumeric(*aParam[1]))
		{
			int n = (int)ParamIndexToInt64(1);
			if (n > 16) n = 16;
			for (int i = 0; i < n; ++i) indent_buf[i] = ' ';
			indent_buf[n > 0 ? n : 0] = '\0';
			indent = indent_buf;
		}
		else
			indent = ParamIndexToString(1, indent_buf);
	}
	LPTSTR s = AhkWebChain_ToJson(*aParam[0], indent);
	aResultToken.symbol = SYM_STRING;
	aResultToken.marker = _T("");
	if (s)
	{
		TokenSetResult(aResultToken, s);
		aResultToken.symbol = SYM_STRING;
		free(s);
	}
}

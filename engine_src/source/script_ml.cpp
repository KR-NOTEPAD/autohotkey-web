// ============================================================================
//  AutoHotkey Web  -  script_ml.cpp
//  Native deep-learning built-in functions, wrapping the ONNX-based text
//  classification (notepad_textcls.cpp) and trainable text NN (notepad_textnn.cpp)
//  ported from NOTEPAD.dll, so ML works with NO external NOTEPAD.dll. Requires
//  onnxruntime.dll next to the engine (delay-loaded).
//
//  Classification (pretrained ONNX folder: model.onnx + labels.txt + vocab.txt):
//    TextClsAvailable()                         -> 1/0 (ONNX runtime present)
//    TextClsLoad(modelDir [, useInt8=1])        -> 1/0
//    TextClsVerdict(modelDir, text [, thr=50, maxLen=128]) -> "verdict,label,prob,clean"
//    TextCls(modelDir, text [, maxLen=128])     -> per-label probabilities
//    TextClsLabels(modelDir)                    -> label list
//    TextClsLastError()                         -> error text
//    TextClsUnload()                            -> 1
//
//  Trainable NN (embedding backbone .onnx + C++ MLP head -> .ntc):
//    TextNNAvailable()                          -> 1/0
//    TextNNTrain(backbone, dataset, model [, epochs=300, hidden=64, maxlen=64, pool=0]) -> #sentences
//    TextNNClassify(model, text [, topK=3])     -> "label,prob;label,prob;..."
//    TextNNLoad(model)                          -> 1/0
//    TextNNInfo(model)                          -> "backbone|dim|pool|labels"
//    TextNNLastError()                          -> error text
//    TextNNUnload()                             -> 1
// ============================================================================
#include "stdafx.h"
#include "defines.h"
#include "script.h"
#include "script_func_impl.h"

extern "C" {
	// notepad_textcls.cpp
	int __stdcall Notepad_TextClsLoad(const wchar_t* modelDir, int useInt8);
	int __stdcall Notepad_TextClsUnload();
	int __stdcall Notepad_TextClsVerdict(const wchar_t* modelDir, const wchar_t* text, int thrPct, int maxLen, wchar_t* out, int outCch);
	int __stdcall Notepad_TextClsLabels(const wchar_t* modelDir, wchar_t* out, int outCch);
	int __stdcall Notepad_TextCls(const wchar_t* modelDir, const wchar_t* text, int maxLen, wchar_t* out, int outCch);
	int __stdcall Notepad_TextClsLastError(wchar_t* out, int outCch);
	// notepad_textnn.cpp
	int __stdcall Notepad_TextNNAvailable();
	int __stdcall Notepad_TextNNTrain(const wchar_t* backbone, const wchar_t* datasetPath, const wchar_t* modelPath, int epochs, int hidden, int maxlen, int pool);
	int __stdcall Notepad_TextNNClassify(const wchar_t* modelPath, const wchar_t* text, int topK, wchar_t* out, int outCch);
	int __stdcall Notepad_TextNNLoad(const wchar_t* modelPath);
	int __stdcall Notepad_TextNNUnload();
	int __stdcall Notepad_TextNNInfo(const wchar_t* modelPath, wchar_t* out, int outCch);
	int __stdcall Notepad_TextNNLastError(wchar_t* out, int outCch);
}

// ---- helpers --------------------------------------------------------------

#define ML_ARG_STR(i) ((aParamCount > (i)) ? ParamIndexToString(i) : _T(""))
#define ML_ARG_INT(i, dflt) ((aParamCount > (i)) ? (int)ParamIndexToInt64(i) : (dflt))

// ---- TextCls --------------------------------------------------------------

BIF_DECL(BIF_TextClsAvailable)
{
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_TextNNAvailable(); // same ONNX gate
}

BIF_DECL(BIF_TextClsLoad)
{
	LPTSTR dir = ParamIndexToString(0);
	int useInt8 = ML_ARG_INT(1, 1);
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_TextClsLoad(dir ? dir : _T(""), useInt8);
}

BIF_DECL(BIF_TextClsUnload)
{
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_TextClsUnload();
}

BIF_DECL(BIF_TextClsVerdict)
{
	LPTSTR dir = ParamIndexToString(0);
	LPTSTR text = ParamIndexToString(1);
	int thr = ML_ARG_INT(2, 50);
	int maxLen = ML_ARG_INT(3, 128);
	int need = Notepad_TextClsVerdict(dir, text, thr, maxLen, NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_TextClsVerdict(dir, text, thr, maxLen, aResultToken.marker, need + 1);
	aResultToken.symbol = SYM_STRING;
}

BIF_DECL(BIF_TextCls)
{
	LPTSTR dir = ParamIndexToString(0);
	LPTSTR text = ParamIndexToString(1);
	int maxLen = ML_ARG_INT(2, 128);
	int need = Notepad_TextCls(dir, text, maxLen, NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_TextCls(dir, text, maxLen, aResultToken.marker, need + 1);
	aResultToken.symbol = SYM_STRING;
}

BIF_DECL(BIF_TextClsLabels)
{
	LPTSTR dir = ParamIndexToString(0);
	int need = Notepad_TextClsLabels(dir, NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_TextClsLabels(dir, aResultToken.marker, need + 1);
	aResultToken.symbol = SYM_STRING;
}

BIF_DECL(BIF_TextClsLastError)
{
	int need = Notepad_TextClsLastError(NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_TextClsLastError(aResultToken.marker, need + 1);
	aResultToken.symbol = SYM_STRING;
}

// ---- TextNN ---------------------------------------------------------------

BIF_DECL(BIF_TextNNAvailable)
{
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_TextNNAvailable();
}

BIF_DECL(BIF_TextNNTrain)
{
	LPTSTR backbone = ParamIndexToString(0);
	LPTSTR dataset = ParamIndexToString(1);
	LPTSTR model = ParamIndexToString(2);
	int epochs = ML_ARG_INT(3, 300);
	int hidden = ML_ARG_INT(4, 64);
	int maxlen = ML_ARG_INT(5, 64);
	int pool = ML_ARG_INT(6, 0);
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_TextNNTrain(backbone, dataset, model, epochs, hidden, maxlen, pool);
}

BIF_DECL(BIF_TextNNClassify)
{
	LPTSTR model = ParamIndexToString(0);
	LPTSTR text = ParamIndexToString(1);
	int topK = ML_ARG_INT(2, 3);
	int need = Notepad_TextNNClassify(model, text, topK, NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_TextNNClassify(model, text, topK, aResultToken.marker, need + 1);
	aResultToken.symbol = SYM_STRING;
}

BIF_DECL(BIF_TextNNLoad)
{
	LPTSTR model = ParamIndexToString(0);
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_TextNNLoad(model ? model : _T(""));
}

BIF_DECL(BIF_TextNNUnload)
{
	aResultToken.symbol = SYM_INTEGER;
	aResultToken.value_int64 = Notepad_TextNNUnload();
}

BIF_DECL(BIF_TextNNInfo)
{
	LPTSTR model = ParamIndexToString(0);
	int need = Notepad_TextNNInfo(model, NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_TextNNInfo(model, aResultToken.marker, need + 1);
	aResultToken.symbol = SYM_STRING;
}

BIF_DECL(BIF_TextNNLastError)
{
	int need = Notepad_TextNNLastError(NULL, 0);
	if (need <= 0) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
	if (!TokenSetResult(aResultToken, NULL, need)) return;
	Notepad_TextNNLastError(aResultToken.marker, need + 1);
	aResultToken.symbol = SYM_STRING;
}

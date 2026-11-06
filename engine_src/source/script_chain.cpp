// ============================================================================
//  AutoHotkey Web  -  script_chain.cpp
//  Native method chaining (no #Include, v1 syntax unchanged):
//
//    Strings :  "  hello world ".Trim().Title().Replace("World", "AHK")   -> "Hello AHK"
//               "a,b,,c".Split(",").Compact().Join("|")                 -> "a|b|c"
//    Arrays  :  [3, 1, 2].Sort("N").Reverse().Join("-")                 -> "3-2-1"
//               people.SortBy("age").Pluck("name").ToJson(false)
//
//  Precedence (nothing existing changes meaning):
//    * Strings: a script-defined "".base member (or "".base.__Call) still wins; only otherwise
//      does the native string method run.  Method names are case-insensitive as usual.
//    * Objects: keys, class methods and base objects are checked first, then the stock built-ins
//      (Length, Push, Count...), and only then these methods.
//  Array method names follow the NOTEPAD.dll NoteArr class so existing habits carry over.
// ============================================================================
#include "stdafx.h" // pre-compiled headers
#include "defines.h"
#include "globaldata.h"
#include "script.h"
#include "script_object.h"
#include "script_func_impl.h"
#include <shlwapi.h> // StrCmpLogicalW, StrStrIW
#include <math.h>

ResultType CallFunc(Func &aFunc, ExprTokenType &aResultToken, ExprTokenType *aParam[], int aParamCount);

// ---------------------------------------------------------------------------
// Small owned-result holder for calling script callbacks / functions.
// ---------------------------------------------------------------------------
struct ChainResult
{
	ExprTokenType tok;
	TCHAR buf[MAX_NUMBER_SIZE];
	ChainResult() { Reset(); }
	~ChainResult() { Free(); }
	void Reset()
	{
		tok.symbol = SYM_STRING;
		tok.marker = _T("");
		tok.mem_to_free = NULL;
		tok.buf = buf;
	}
	void Free()
	{
		if (tok.mem_to_free)
			free(tok.mem_to_free);
		if (tok.symbol == SYM_OBJECT)
			tok.object->Release();
		Reset();
	}
};

// Growable string buffer (no STL: the engine builds without C++ exceptions).
struct ChainStr
{
	LPTSTR p; size_t len, cap; bool fail;
	ChainStr() : p(NULL), len(0), cap(0), fail(false) {}
	~ChainStr() { free(p); }
	void Add(LPCTSTR s, size_t n)
	{
		if (fail) return;
		if (len + n + 1 > cap)
		{
			size_t nc = cap ? cap * 2 : 256;
			while (nc < len + n + 1) nc *= 2;
			LPTSTR np = (LPTSTR)realloc(p, nc * sizeof(TCHAR));
			if (!np) { fail = true; return; }
			p = np; cap = nc;
		}
		memcpy(p + len, s, n * sizeof(TCHAR));
		len += n;
		p[len] = '\0';
	}
	void Add(LPCTSTR s) { Add(s, _tcslen(s)); }
	void AddCh(TCHAR c) { Add(&c, 1); }
	LPTSTR Detach() { LPTSTR r = p; if (!r) { r = (LPTSTR)malloc(sizeof(TCHAR)); if (r) *r = 0; } p = NULL; len = cap = 0; return r; }
};

struct AhkWebChain
{
	// ---- object helpers ---------------------------------------------------------------
	static void *ObjectVtbl()
	{
		static void *vt = NULL;
		if (!vt)
		{
			Object *o = Object::Create();
			if (o) { vt = *(void **)o; o->Release(); }
		}
		return vt;
	}
	static Object *AsObject(IObject *aObj)
	{
		return (aObj && *(void **)aObj == ObjectVtbl()) ? (Object *)aObj : NULL;
	}
	static Object *TokenAsObject(ExprTokenType &aTok)
	{
		return AsObject(TokenToObject(aTok));
	}
	static bool IsLinear(Object *o)
	{
		if (o->mKeyOffsetObject != o->mFieldCount)
			return false;
		for (Object::IndexType i = 0; i < o->mFieldCount; ++i)
			if (o->mFields[i].key.i != i + 1)
				return false;
		return true;
	}
	static void KeyToken(Object *o, Object::IndexType i, ExprTokenType &aKey)
	{
		if (i < o->mKeyOffsetObject) { aKey.symbol = SYM_INTEGER; aKey.value_int64 = o->mFields[i].key.i; }
		else if (i < o->mKeyOffsetString) { aKey.symbol = SYM_OBJECT; aKey.object = o->mFields[i].key.p; }
		else { aKey.symbol = SYM_STRING; aKey.marker = o->mFields[i].key.s; }
	}
	static void ValToken(Object *o, Object::IndexType i, ExprTokenType &aVal)
	{
		o->mFields[i].ToToken(aVal);
	}
	static bool Push(Object *o, ExprTokenType &aVal)
	{
		ExprTokenType key;
		key.symbol = SYM_INTEGER;
		key.value_int64 = (o->mKeyOffsetObject ? o->mFields[o->mKeyOffsetObject - 1].key.i : 0) + 1;
		return o->SetItem(key, aVal);
	}
	static void ReturnObject(ExprTokenType &aResultToken, Object *o) // transfers o's reference
	{
		if (!o) { aResultToken.symbol = SYM_STRING; aResultToken.marker = _T(""); return; }
		aResultToken.symbol = SYM_OBJECT;
		aResultToken.object = o;
	}
	static void ReturnThis(ExprTokenType &aResultToken, Object *o)
	{
		o->AddRef();
		aResultToken.symbol = SYM_OBJECT;
		aResultToken.object = o;
	}
	static void ReturnToken(ExprTokenType &aResultToken, ExprTokenType &aVal0) // copies
	{
		ExprTokenType aVal;
		if (aVal0.symbol == SYM_VAR) aVal0.var->ToToken(aVal); else aVal = aVal0;
		switch (aVal.symbol)
		{
		case SYM_OBJECT:
			aVal.object->AddRef();
			aResultToken.symbol = SYM_OBJECT;
			aResultToken.object = aVal.object;
			break;
		case SYM_INTEGER:
		case SYM_FLOAT:
			aResultToken.symbol = aVal.symbol;
			aResultToken.value_int64 = aVal.value_int64;
			break;
		default:
		{
			TCHAR b[MAX_NUMBER_SIZE];
			LPTSTR s = TokenToString(aVal, b);
			aResultToken.symbol = SYM_STRING;
			aResultToken.marker = _T("");
			if (*s)
			{
				TokenSetResult(aResultToken, s);
				aResultToken.symbol = SYM_STRING;
			}
		}
		}
	}
	static void ReturnInt(ExprTokenType &aResultToken, __int64 n) { aResultToken.symbol = SYM_INTEGER; aResultToken.value_int64 = n; }
	static void ReturnStr(ExprTokenType &aResultToken, LPCTSTR s, size_t n = -1)
	{
		aResultToken.symbol = SYM_STRING;
		aResultToken.marker = _T("");
		if (n == (size_t)-1) n = _tcslen(s);
		if (n)
		{
			TokenSetResult(aResultToken, s, n);
			aResultToken.symbol = SYM_STRING;
		}
	}

	// ---- value helpers -------------------------------------------------------------------
	static bool ValEqual(ExprTokenType &a, ExprTokenType &b, bool aCS)
	{
		IObject *oa = TokenToObject(a), *ob = TokenToObject(b);
		if (oa || ob)
			return oa == ob;
		if (TokenIsPureNumeric(a) && TokenIsPureNumeric(b))
			return TokenToDouble(a) == TokenToDouble(b);
		TCHAR b1[MAX_NUMBER_SIZE], b2[MAX_NUMBER_SIZE];
		LPTSTR s1 = TokenToString(a, b1), s2 = TokenToString(b, b2);
		return aCS ? !_tcscmp(s1, s2) : !_tcsicmp(s1, s2);
	}
	// aVal[aProp] when aProp is non-empty and aVal is an object; otherwise aVal itself.
	static bool Prop(ExprTokenType &aVal, LPTSTR aProp, ExprTokenType &aOut)
	{
		if (!aProp || !*aProp) { aOut = aVal; return true; }
		Object *o = aVal.symbol == SYM_OBJECT ? AsObject(aVal.object) : NULL;
		if (!o || !o->GetItem(aOut, aProp))
		{
			aOut.symbol = SYM_STRING; aOut.marker = _T("");
			return false;
		}
		return true;
	}
	static bool NumberOf(ExprTokenType &aVal, double &aOut, bool &aIsInt)
	{
		SymbolType t = TokenIsPureNumeric(aVal);
		if (!t) return false;
		if (t == SYM_INTEGER) { aIsInt = true; aOut = (double)TokenToInt64(aVal); }
		else { aIsInt = false; aOut = TokenToDouble(aVal); }
		return true;
	}

	// ---- callbacks ---------------------------------------------------------------------------
	static ResultType Callback(ExprTokenType &aFn, ExprTokenType **aArgs, int aArgc, ChainResult &aOut)
	{
		aOut.Free();
		Func *func = TokenToFunc(aFn);
		ResultType r;
		if (func)
		{
			int n = aArgc;
			if (!func->mIsVariadic && n > func->mParamCount)
				n = func->mParamCount; // NoteArr rule: pass only as many args as the callback declares.
			r = CallFunc(*func, aOut.tok, aArgs, n);
		}
		else
		{
			IObject *obj = TokenToObject(aFn);
			if (!obj)
			{
				TCHAR b[MAX_NUMBER_SIZE];
				return g_script.ScriptError(_T("Callback must be a function, function name or function object."), TokenToString(aFn, b));
			}
			ExprTokenType this_tok, name_tok;
			this_tok.symbol = SYM_OBJECT; this_tok.object = obj;
			name_tok.symbol = SYM_STRING; name_tok.marker = _T("Call");
			ExprTokenType **p = (ExprTokenType **)_alloca((aArgc + 1) * sizeof(ExprTokenType *));
			p[0] = &name_tok;
			memcpy(p + 1, aArgs, aArgc * sizeof(ExprTokenType *));
			r = obj->Invoke(aOut.tok, this_tok, IT_CALL, p, aArgc + 1);
		}
		return (r == FAIL || r == EARLY_EXIT) ? r : OK;
	}
	// fn(value, key, arr)
	static ResultType CallVKA(ExprTokenType &aFn, ExprTokenType &aVal, ExprTokenType &aKey, ExprTokenType &aArr, ChainResult &aOut)
	{
		ExprTokenType *args[3] = { &aVal, &aKey, &aArr };
		return Callback(aFn, args, 3, aOut);
	}

	// ---- sorting -----------------------------------------------------------------------------
	struct SortCtx
	{
		bool numeric, desc, cs, natural;
		ExprTokenType *fn;
		LPTSTR by;
		ResultType result;
	};
	static int Compare(SortCtx &c, ExprTokenType &a0, ExprTokenType &b0)
	{
		if (c.result != OK) return 0;
		int r;
		if (c.fn)
		{
			ChainResult res;
			ExprTokenType *args[2] = { &a0, &b0 };
			c.result = Callback(*c.fn, args, 2, res);
			double d = TokenToDouble(res.tok);
			r = d < 0 ? -1 : d > 0 ? 1 : 0;
		}
		else
		{
			ExprTokenType a, b;
			Prop(a0, c.by, a); Prop(b0, c.by, b);
			if (c.numeric)
			{
				double da = TokenToDouble(a), db = TokenToDouble(b);
				r = da < db ? -1 : da > db ? 1 : 0;
			}
			else
			{
				TCHAR b1[MAX_NUMBER_SIZE], b2[MAX_NUMBER_SIZE];
				LPTSTR s1 = TokenToString(a, b1), s2 = TokenToString(b, b2);
				r = c.natural ? StrCmpLogicalW(s1, s2) : c.cs ? _tcscmp(s1, s2) : _tcsicmp(s1, s2);
				r = r < 0 ? -1 : r > 0 ? 1 : 0;
			}
		}
		return c.desc ? -r : r;
	}
	// Stable merge sort of index array over value tokens.
	static void MergeSort(SortCtx &c, ExprTokenType *v, int *idx, int *tmp, int n)
	{
		if (n < 2) return;
		int h = n / 2;
		MergeSort(c, v, idx, tmp, h);
		MergeSort(c, v, idx + h, tmp, n - h);
		int i = 0, j = h, k = 0;
		while (i < h && j < n)
			tmp[k++] = Compare(c, v[idx[j]], v[idx[i]]) < 0 ? idx[j++] : idx[i++];
		while (i < h) tmp[k++] = idx[i++];
		while (j < n) tmp[k++] = idx[j++];
		memcpy(idx, tmp, n * sizeof(int));
	}
	static void ParseSortOpts(SortCtx &c, ExprTokenType *aOpts)
	{
		c.numeric = c.desc = c.cs = c.natural = false;
		c.fn = NULL; c.by = NULL; c.result = OK;
		if (!aOpts || aOpts->symbol == SYM_MISSING) return;
		if (TokenToFunc(*aOpts)) { c.fn = aOpts; return; }
		Object *o = TokenAsObject(*aOpts);
		if (o)
		{
			ExprTokenType t;
			if (o->GetItem(t, (LPTSTR)_T("fn"))) { static ExprTokenType sFn; sFn = t; c.fn = &sFn; }
			if (o->GetItem(t, (LPTSTR)_T("desc"))) c.desc = TokenToBOOL(t) != 0;
			if (o->GetItem(t, (LPTSTR)_T("numeric"))) c.numeric = TokenToBOOL(t) != 0;
			if (o->GetItem(t, (LPTSTR)_T("natural"))) c.natural = TokenToBOOL(t) != 0;
			if (o->GetItem(t, (LPTSTR)_T("cs"))) c.cs = TokenToBOOL(t) != 0;
			if (o->GetItem(t, (LPTSTR)_T("by")) && t.symbol == SYM_OPERAND) c.by = t.marker;
			return;
		}
		if (aOpts->symbol == SYM_OBJECT) { c.fn = aOpts; return; } // BoundFunc or other function object
		TCHAR b[MAX_NUMBER_SIZE];
		LPTSTR s = TokenToString(*aOpts, b);
		for (LPTSTR cp = s; *cp; )
		{
			while (*cp == ' ' || *cp == '\t' || *cp == ',') ++cp;
			if (!*cp) break;
			LPTSTR w = cp;
			while (*cp && *cp != ' ' && *cp != '\t' && *cp != ',') ++cp;
			size_t n = cp - w;
			if (n == 3 && !_tcsnicmp(w, _T("NAT"), 3)) c.natural = true;
			else if (n == 1 && (*w == 'N' || *w == 'n')) c.numeric = true;
			else if (n == 1 && (*w == 'D' || *w == 'd' || *w == 'R' || *w == 'r')) c.desc = true;
			else if (n == 1 && (*w == 'C' || *w == 'c')) c.cs = true;
		}
	}
	// New array holding o's values sorted.  aProp: element property (SortBy).
	static ResultType SortedCopy(Object *o, ExprTokenType *aOpts, LPTSTR aProp, ExprTokenType &aResultToken)
	{
		Object *snap = o->Clone();
		if (!snap) return OK;
		int n = (int)snap->mFieldCount;
		ExprTokenType *v = (ExprTokenType *)malloc((n ? n : 1) * sizeof(ExprTokenType));
		int *idx = (int *)malloc((n ? n : 1) * sizeof(int) * 2);
		if (!v || !idx) { free(v); free(idx); snap->Release(); return OK; }
		for (int i = 0; i < n; ++i) { ValToken(snap, i, v[i]); idx[i] = i; }
		SortCtx c;
		ParseSortOpts(c, aOpts);
		if (aProp && *aProp) c.by = aProp;
		MergeSort(c, v, idx, idx + n, n);
		Object *out = NULL;
		if (c.result == OK && (out = Object::Create()))
			for (int i = 0; i < n; ++i)
				Push(out, v[idx[i]]);
		free(v); free(idx);
		snap->Release();
		if (c.result != OK) { if (out) out->Release(); return c.result; }
		ReturnObject(aResultToken, out);
		return OK;
	}

	// ---- JSON writer ---------------------------------------------------------------------------
	static void JsonString(ChainStr &w, LPCTSTR s)
	{
		w.AddCh('"');
		for (; *s; ++s)
		{
			TCHAR c = *s;
			switch (c)
			{
			case '"': w.Add(_T("\\\""), 2); break;
			case '\\': w.Add(_T("\\\\"), 2); break;
			case '\n': w.Add(_T("\\n"), 2); break;
			case '\r': w.Add(_T("\\r"), 2); break;
			case '\t': w.Add(_T("\\t"), 2); break;
			case '\b': w.Add(_T("\\b"), 2); break;
			case '\f': w.Add(_T("\\f"), 2); break;
			default:
				if (c < 0x20)
				{
					TCHAR u[8];
					_stprintf_s(u, _T("\\u%04X"), (unsigned)c);
					w.Add(u, 6);
				}
				else
					w.AddCh(c);
			}
		}
		w.AddCh('"');
	}
	static void JsonNewline(ChainStr &w, LPCTSTR aIndent, int aDepth)
	{
		if (!*aIndent) return;
		w.AddCh('\n');
		for (int i = 0; i < aDepth; ++i) w.Add(aIndent);
	}
	static void JsonValue(ChainStr &w, ExprTokenType &v, LPCTSTR aIndent, int aDepth)
	{
		switch (v.symbol)
		{
		case SYM_INTEGER:
		{
			TCHAR b[MAX_NUMBER_SIZE];
			_i64tot(v.value_int64, b, 10);
			w.Add(b);
			return;
		}
		case SYM_FLOAT:
		{
			double d = v.value_double;
			if (d != d || d > 1.7e308 || d < -1.7e308) { w.Add(_T("null")); return; }
			TCHAR b[64];
			_stprintf_s(b, _T("%.15g"), d);
			w.Add(b);
			return;
		}
		case SYM_OBJECT:
		{
			Object *o = AsObject(v.object);
			if (!o || aDepth > 64) { w.Add(_T("null")); return; }
			bool arr = IsLinear(o);
			w.AddCh(arr ? '[' : '{');
			int written = 0;
			for (Object::IndexType i = 0; i < o->mFieldCount; ++i)
			{
				if (!arr && i >= o->mKeyOffsetObject && i < o->mKeyOffsetString)
					continue; // object keys have no JSON form
				if (written++) w.AddCh(',');
				JsonNewline(w, aIndent, aDepth + 1);
				if (!arr)
				{
					if (i < o->mKeyOffsetObject)
					{
						TCHAR b[MAX_NUMBER_SIZE];
						_i64tot(o->mFields[i].key.i, b, 10);
						JsonString(w, b);
					}
					else
						JsonString(w, o->mFields[i].key.s);
					w.AddCh(':');
					if (*aIndent) w.AddCh(' ');
				}
				ExprTokenType val;
				ValToken(o, i, val);
				JsonValue(w, val, aIndent, aDepth + 1);
			}
			if (written) JsonNewline(w, aIndent, aDepth);
			w.AddCh(arr ? ']' : '}');
			return;
		}
		case SYM_VAR:
		{
			if (IObject *obj = TokenToObject(v))
			{
				ExprTokenType t; t.symbol = SYM_OBJECT; t.object = obj;
				JsonValue(w, t, aIndent, aDepth);
				return;
			}
			ExprTokenType t;
			v.var->ToToken(t);
			if (t.symbol == SYM_INTEGER || t.symbol == SYM_FLOAT) { JsonValue(w, t, aIndent, aDepth); return; }
			break;
		}
		}
		TCHAR b[MAX_NUMBER_SIZE];
		LPTSTR s = TokenToString(v, b);
		// v1 stores literals such as 1.5 as strings; like other v1 JSON libraries, a value that
		// is a canonical JSON number is written as a number ("007", "+1", "0x10" stay strings).
		if (JsonNumberLike(s))
			w.Add(s);
		else
			JsonString(w, s);
	}
	static bool JsonNumberLike(LPCTSTR s)
	{
		LPCTSTR p = s;
		if (*p == '-') ++p;
		if (*p == '0') ++p;
		else if (*p >= '1' && *p <= '9') { while (*p >= '0' && *p <= '9') ++p; }
		else return false;
		if (*p == '.')
		{
			++p;
			if (!(*p >= '0' && *p <= '9')) return false;
			while (*p >= '0' && *p <= '9') ++p;
		}
		if (*p == 'e' || *p == 'E')
		{
			++p;
			if (*p == '+' || *p == '-') ++p;
			if (!(*p >= '0' && *p <= '9')) return false;
			while (*p >= '0' && *p <= '9') ++p;
		}
		return !*p && p - s <= 24;
	}

	// ---- path helpers: "a.b.1.c" -------------------------------------------------------------
	static bool PathSegment(LPTSTR &aCp, LPTSTR aBuf, size_t aBufSize)
	{
		if (!*aCp) return false;
		size_t n = 0;
		while (*aCp && *aCp != '.' && n + 1 < aBufSize) aBuf[n++] = *aCp++;
		aBuf[n] = '\0';
		if (*aCp == '.') ++aCp;
		return true;
	}
	static void SegKey(LPTSTR aSeg, ExprTokenType &aKey)
	{
		if (IsPureNumeric(aSeg, TRUE, FALSE) == PURE_INTEGER) { aKey.symbol = SYM_INTEGER; aKey.value_int64 = ATOI64(aSeg); }
		else { aKey.symbol = SYM_STRING; aKey.marker = aSeg; }
	}
	static bool PathGet(Object *o, LPTSTR aPath, ExprTokenType &aOut)
	{
		TCHAR seg[256];
		LPTSTR cp = aPath;
		ExprTokenType cur; cur.symbol = SYM_OBJECT; cur.object = o;
		while (PathSegment(cp, seg, _countof(seg)))
		{
			Object *co = cur.symbol == SYM_OBJECT ? AsObject(cur.object) : NULL;
			if (!co) return false;
			ExprTokenType key; SegKey(seg, key);
			if (!co->GetItem(cur, key)) return false;
		}
		aOut = cur;
		return true;
	}
	static bool PathSet(Object *o, LPTSTR aPath, ExprTokenType &aValue)
	{
		TCHAR seg[256];
		LPTSTR cp = aPath;
		Object *co = o;
		while (PathSegment(cp, seg, _countof(seg)))
		{
			ExprTokenType key; SegKey(seg, key);
			if (!*cp) return co->SetItem(key, aValue);
			ExprTokenType next;
			Object *no = co->GetItem(next, key) && next.symbol == SYM_OBJECT ? AsObject(next.object) : NULL;
			if (!no)
			{
				if (!(no = Object::Create())) return false;
				ExprTokenType t; t.symbol = SYM_OBJECT; t.object = no;
				co->SetItem(key, t);
				no->Release(); // now owned by co
			}
			co = no;
		}
		return false;
	}
	static Object *DeepCopy(Object *o, int aDepth)
	{
		Object *c = o->Clone();
		if (!c) return NULL;
		if (o->mBase) (c->mBase = o->mBase)->AddRef();
		if (aDepth > 64) return c;
		for (Object::IndexType i = 0; i < c->mFieldCount; ++i)
		{
			Object::FieldType &f = c->mFields[i];
			Object *child = f.symbol == SYM_OBJECT ? AsObject(f.object) : NULL;
			if (child)
			{
				Object *cc = DeepCopy(child, aDepth + 1);
				if (cc) { f.object->Release(); f.object = cc; }
			}
		}
		return c;
	}
	static void Flatten(Object *aOut, Object *aSrc, int aDepth)
	{
		for (Object::IndexType i = 0; i < aSrc->mFieldCount; ++i)
		{
			ExprTokenType v; ValToken(aSrc, i, v);
			Object *child = (aDepth != 0 && v.symbol == SYM_OBJECT) ? AsObject(v.object) : NULL;
			if (child) Flatten(aOut, child, aDepth - 1);
			else Push(aOut, v);
		}
	}

	static ResultType ObjCall(Object *aObj, LPCTSTR aName, ExprTokenType &aResultToken, ExprTokenType &aThisToken, ExprTokenType *aParam[], int aParamCount);
	static ResultType StrInvoke(ExprTokenType &aResultToken, ExprTokenType &aThisToken, int aFlags, ExprTokenType *aParam[], int aParamCount);
};


// ============================================================================
//  Array / object methods
// ============================================================================
enum ChainObjMethod
{
	OM_Join = 1, OM_ToJson, OM_Sort, OM_SortBy, OM_Reverse, OM_Shuffle, OM_Unique, OM_Slice, OM_Chunk, OM_Flatten,
	OM_Map, OM_Filter, OM_Reduce, OM_Each, OM_Find, OM_FindKey, OM_FindIndex, OM_Any, OM_All, OM_CountIf,
	OM_Pluck, OM_Where, OM_GroupBy, OM_Pick, OM_Omit, OM_Diff, OM_Intersect, OM_Union, OM_Compact,
	OM_Has, OM_IndexOf, OM_KeyOf, OM_Len, OM_First, OM_Last, OM_Keys, OM_Values, OM_IsEmpty, OM_IsLinear,
	OM_Get, OM_Set, OM_HasPath, OM_Append, OM_Prepend, OM_Clear, OM_Copy, OM_Concat, OM_At,
	OM_Sum, OM_Min, OM_Max, OM_Avg, OM_Median, OM_Stdev, OM_ToLines, OM_ToQuery, OM_Top
};
struct ChainName { LPCTSTR name; int id; };
static const ChainName sObjMethods[] =
{
	{_T("Join"), OM_Join}, {_T("ToJson"), OM_ToJson}, {_T("Json"), OM_ToJson}, {_T("toString"), OM_Join},
	{_T("Sort"), OM_Sort}, {_T("toSorted"), OM_Sort}, {_T("SortBy"), OM_SortBy},
	{_T("Reverse"), OM_Reverse}, {_T("toReversed"), OM_Reverse}, {_T("Shuffle"), OM_Shuffle},
	{_T("Unique"), OM_Unique}, {_T("Distinct"), OM_Unique}, {_T("Slice"), OM_Slice}, {_T("Chunk"), OM_Chunk},
	{_T("Flatten"), OM_Flatten}, {_T("flat"), OM_Flatten},
	{_T("Map"), OM_Map}, {_T("Filter"), OM_Filter}, {_T("FindAll"), OM_Filter}, {_T("Reduce"), OM_Reduce},
	{_T("Each"), OM_Each}, {_T("forEach"), OM_Each}, {_T("Find"), OM_Find}, {_T("FindKey"), OM_FindKey},
	{_T("findIndex"), OM_FindIndex}, {_T("Any"), OM_Any}, {_T("some"), OM_Any}, {_T("All"), OM_All},
	{_T("every"), OM_All}, {_T("CountIf"), OM_CountIf}, {_T("Pluck"), OM_Pluck}, {_T("Column"), OM_Pluck},
	{_T("Where"), OM_Where}, {_T("GroupBy"), OM_GroupBy}, {_T("Pick"), OM_Pick}, {_T("Omit"), OM_Omit},
	{_T("Diff"), OM_Diff}, {_T("Intersect"), OM_Intersect}, {_T("Union"), OM_Union}, {_T("Compact"), OM_Compact},
	{_T("Has"), OM_Has}, {_T("includes"), OM_Has}, {_T("IndexOf"), OM_IndexOf}, {_T("KeyOf"), OM_KeyOf},
	{_T("Len"), OM_Len}, {_T("First"), OM_First}, {_T("Last"), OM_Last}, {_T("Keys"), OM_Keys},
	{_T("Values"), OM_Values}, {_T("IsEmpty"), OM_IsEmpty}, {_T("IsLinear"), OM_IsLinear},
	{_T("Get"), OM_Get}, {_T("Set"), OM_Set}, {_T("HasPath"), OM_HasPath},
	{_T("Append"), OM_Append}, {_T("Prepend"), OM_Prepend}, {_T("unshift"), OM_Prepend}, {_T("Clear"), OM_Clear},
	{_T("Copy"), OM_Copy}, {_T("concat"), OM_Concat}, {_T("at"), OM_At},
	{_T("Sum"), OM_Sum}, {_T("Min"), OM_Min}, {_T("Max"), OM_Max}, {_T("Avg"), OM_Avg}, {_T("Mean"), OM_Avg},
	{_T("Median"), OM_Median}, {_T("Stdev"), OM_Stdev}, {_T("ToLines"), OM_ToLines}, {_T("ToQuery"), OM_ToQuery},
	{_T("Top"), OM_Top},
};

static int ChainFind(const ChainName *aTable, int aCount, LPCTSTR aName)
{
	for (int i = 0; i < aCount; ++i)
		if (!_tcsicmp(aTable[i].name, aName))
			return aTable[i].id;
	return 0;
}

#define ARG_STR(i, def) (((i) < aParamCount && aParam[i]->symbol != SYM_MISSING) ? TokenToString(*aParam[i], argbuf[i]) : (LPTSTR)(def))
#define ARG_INT(i, def) (((i) < aParamCount && aParam[i]->symbol != SYM_MISSING) ? TokenToInt64(*aParam[i]) : (__int64)(def))
#define ARG_BOOL(i, def) (((i) < aParamCount && aParam[i]->symbol != SYM_MISSING) ? (TokenToBOOL(*aParam[i]) != 0) : (def))
#define NEED_ARGS(n) if (aParamCount < (n)) return g_script.ScriptError(ERR_TOO_FEW_PARAMS, aName)

ResultType AhkWebChain::ObjCall(Object *aObj, LPCTSTR aName, ExprTokenType &aResultToken, ExprTokenType &aThisToken, ExprTokenType *aParam[], int aParamCount)
{
	int id = ChainFind(sObjMethods, _countof(sObjMethods), aName);
	if (!id)
		return INVOKE_NOT_HANDLED;

	TCHAR argbuf[4][MAX_NUMBER_SIZE];
	ExprTokenType self; self.symbol = SYM_OBJECT; self.object = aObj; // for callbacks' "arr" param
	(void)aThisToken;

	switch (id)
	{
	// ------------------------------------------------------------- serialize
	case OM_Join:
	{
		LPTSTR sep = ARG_STR(0, _T(","));
		LPTSTR kvsep = ARG_STR(1, _T("="));
		bool linear = IsLinear(aObj);
		ChainStr w;
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			if (i) w.Add(sep);
			TCHAR b[MAX_NUMBER_SIZE];
			if (!linear)
			{
				ExprTokenType k; KeyToken(aObj, i, k);
				w.Add(k.symbol == SYM_OBJECT ? _T("") : TokenToString(k, b));
				w.Add(kvsep);
			}
			ExprTokenType v; ValToken(aObj, i, v);
			w.Add(v.symbol == SYM_OBJECT ? _T("") : TokenToString(v, b));
		}
		ReturnStr(aResultToken, w.p ? w.p : _T(""), w.len);
		return OK;
	}
	case OM_ToJson:
	{
		bool pretty = ARG_BOOL(0, true);
		ChainStr w;
		JsonValue(w, self, pretty ? _T("  ") : _T(""), 0);
		ReturnStr(aResultToken, w.p ? w.p : _T(""), w.len);
		return OK;
	}
	case OM_ToLines:
	{
		ChainStr w;
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			if (i) w.Add(_T("\r\n"), 2);
			TCHAR b[MAX_NUMBER_SIZE];
			ExprTokenType v; ValToken(aObj, i, v);
			w.Add(v.symbol == SYM_OBJECT ? _T("") : TokenToString(v, b));
		}
		ReturnStr(aResultToken, w.p ? w.p : _T(""), w.len);
		return OK;
	}
	case OM_ToQuery:
	{
		ChainStr w;
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			ExprTokenType k, v; KeyToken(aObj, i, k); ValToken(aObj, i, v);
			if (k.symbol == SYM_OBJECT || v.symbol == SYM_OBJECT) continue;
			if (w.len) w.AddCh('&');
			TCHAR b1[MAX_NUMBER_SIZE], b2[MAX_NUMBER_SIZE];
			ExprTokenType *p1[1] = { &k }, *p2[1] = { &v };
			ExprTokenType ks, vs; ks.symbol = SYM_STRING; ks.marker = TokenToString(k, b1); vs.symbol = SYM_STRING; vs.marker = TokenToString(v, b2);
			p1[0] = &ks; p2[0] = &vs;
			ChainResult r1, r2;
			AhkWebExt_Call(_T("UrlEncode"), r1.tok, p1, 1);
			AhkWebExt_Call(_T("UrlEncode"), r2.tok, p2, 1);
			w.Add(r1.tok.marker); w.AddCh('='); w.Add(r2.tok.marker);
		}
		ReturnStr(aResultToken, w.p ? w.p : _T(""), w.len);
		return OK;
	}
	// ------------------------------------------------------------- new arrays
	case OM_Sort:
		return SortedCopy(aObj, aParamCount > 0 ? aParam[0] : NULL, NULL, aResultToken);
	case OM_SortBy:
		NEED_ARGS(1);
		return SortedCopy(aObj, aParamCount > 1 ? aParam[1] : NULL, ARG_STR(0, _T("")), aResultToken);
	case OM_Reverse:
	{
		Object *out = Object::Create();
		if (out)
			for (Object::IndexType i = aObj->mFieldCount - 1; i >= 0; --i)
			{
				ExprTokenType v; ValToken(aObj, i, v);
				Push(out, v);
			}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Shuffle:
	{
		Object *out = aObj->Clone();
		if (!out) return OK;
		Object *res = Object::Create();
		int n = (int)out->mFieldCount;
		int *idx = (int *)malloc((n ? n : 1) * sizeof(int));
		if (res && idx)
		{
			for (int i = 0; i < n; ++i) idx[i] = i;
			LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
			unsigned __int64 seed = (unsigned __int64)qpc.QuadPart ^ ((unsigned __int64)(UINT_PTR)idx << 16) ^ 0x9E3779B97F4A7C15ULL;
			for (int i = n - 1; i > 0; --i)
			{
				seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; // xorshift64
				unsigned int rnd = (unsigned int)(seed >> 32);
				int j = (int)(rnd % (unsigned int)(i + 1));
				int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
			}
			for (int i = 0; i < n; ++i) { ExprTokenType v; ValToken(out, idx[i], v); Push(res, v); }
		}
		free(idx);
		out->Release();
		ReturnObject(aResultToken, res);
		return OK;
	}
	case OM_Unique:
	{
		LPTSTR prop = ARG_STR(0, _T(""));
		Object *out = Object::Create();
		if (!out) return OK;
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			ExprTokenType v, pv; ValToken(aObj, i, v); Prop(v, prop, pv);
			bool dup = false;
			for (Object::IndexType j = 0; j < out->mFieldCount && !dup; ++j)
			{
				ExprTokenType u, pu; ValToken(out, j, u); Prop(u, prop, pu);
				dup = ValEqual(pv, pu, false);
			}
			if (!dup) Push(out, v);
		}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Slice:
	{
		// Slice(start := 1, stop := 0): 1-based, inclusive; 0 = to the end; negatives count from the end.
		__int64 n = aObj->mFieldCount;
		__int64 start = ARG_INT(0, 1), stop = ARG_INT(1, 0);
		if (start < 0) start = n + start + 1;
		if (start < 1) start = 1;
		if (stop <= 0) stop = n + stop;
		if (stop > n) stop = n;
		Object *out = Object::Create();
		if (out)
			for (__int64 i = start; i <= stop; ++i)
			{
				ExprTokenType v; ValToken(aObj, (Object::IndexType)(i - 1), v);
				Push(out, v);
			}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Chunk:
	{
		__int64 size = ARG_INT(0, 2);
		if (size < 1) size = 1;
		Object *out = Object::Create(), *cur = NULL;
		if (!out) return OK;
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			if (!cur || cur->mFieldCount >= size)
			{
				if (!(cur = Object::Create())) break;
				ExprTokenType t; t.symbol = SYM_OBJECT; t.object = cur;
				Push(out, t);
				cur->Release();
			}
			ExprTokenType v; ValToken(aObj, i, v);
			Push(cur, v);
		}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Flatten:
	{
		Object *out = Object::Create();
		if (out) Flatten(out, aObj, (int)ARG_INT(0, -1));
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Compact:
	{
		bool zero_too = ARG_BOOL(0, false);
		Object *out = Object::Create();
		if (!out) return OK;
		bool linear = IsLinear(aObj);
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			ExprTokenType v; ValToken(aObj, i, v);
			if (v.symbol != SYM_OBJECT)
			{
				if (TokenIsEmptyString(v)) continue;
				if (zero_too && TokenIsPureNumeric(v) && TokenToDouble(v) == 0) continue;
			}
			if (linear) Push(out, v);
			else { ExprTokenType k; KeyToken(aObj, i, k); out->SetItem(k, v); }
		}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Pluck:
	{
		NEED_ARGS(1);
		LPTSTR prop = ARG_STR(0, _T(""));
		Object *out = Object::Create();
		if (out)
			for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
			{
				ExprTokenType v, pv; ValToken(aObj, i, v);
				Prop(v, prop, pv);
				Push(out, pv);
			}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Where:
	{
		// Where(prop, value := "", op := "=")   op: = == != <> > >= < <= contains startswith
		NEED_ARGS(1);
		LPTSTR prop = ARG_STR(0, _T(""));
		LPTSTR op = ARG_STR(2, _T("="));
		ExprTokenType empty; empty.symbol = SYM_STRING; empty.marker = _T("");
		ExprTokenType &want = aParamCount > 1 && aParam[1]->symbol != SYM_MISSING ? *aParam[1] : empty;
		Object *out = Object::Create();
		if (!out) return OK;
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			ExprTokenType v, pv; ValToken(aObj, i, v);
			Prop(v, prop, pv);
			bool keep;
			if (!_tcscmp(op, _T("=")) ) keep = ValEqual(pv, want, false);
			else if (!_tcscmp(op, _T("=="))) keep = ValEqual(pv, want, true);
			else if (!_tcscmp(op, _T("!=")) || !_tcscmp(op, _T("<>"))) keep = !ValEqual(pv, want, false);
			else if (!_tcsicmp(op, _T("contains")) || !_tcsicmp(op, _T("startswith")))
			{
				TCHAR b1[MAX_NUMBER_SIZE], b2[MAX_NUMBER_SIZE];
				LPTSTR s = TokenToString(pv, b1), w2 = TokenToString(want, b2);
				keep = !_tcsicmp(op, _T("contains")) ? StrStrIW(s, w2) != NULL : !_tcsnicmp(s, w2, _tcslen(w2));
			}
			else
			{
				double a = TokenToDouble(pv), b = TokenToDouble(want);
				keep = !_tcscmp(op, _T(">")) ? a > b : !_tcscmp(op, _T(">=")) ? a >= b
					: !_tcscmp(op, _T("<")) ? a < b : !_tcscmp(op, _T("<=")) ? a <= b : false;
			}
			if (keep) Push(out, v);
		}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Top:
	{
		// Top(n := 10, prop := "", desc := true): sort numerically then take n.
		__int64 n = ARG_INT(0, 10);
		LPTSTR prop = ARG_STR(1, _T(""));
		bool desc = ARG_BOOL(2, true);
		ExprTokenType opts; opts.symbol = SYM_STRING; opts.marker = desc ? _T("N D") : _T("N");
		ChainResult sorted;
		ResultType r = SortedCopy(aObj, &opts, prop, sorted.tok);
		if (r != OK) return r;
		Object *s = sorted.tok.symbol == SYM_OBJECT ? AsObject(sorted.tok.object) : NULL;
		Object *out = Object::Create();
		if (s && out)
			for (Object::IndexType i = 0; i < s->mFieldCount && i < n; ++i)
			{
				ExprTokenType v; ValToken(s, i, v);
				Push(out, v);
			}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Keys:
	case OM_Values:
	{
		Object *out = Object::Create();
		if (out)
			for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
			{
				ExprTokenType t;
				if (id == OM_Keys) KeyToken(aObj, i, t); else ValToken(aObj, i, t);
				Push(out, t);
			}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Copy:
	{
		bool deep = ARG_BOOL(0, true);
		Object *c;
		if (deep) c = DeepCopy(aObj, 0);
		else if ((c = aObj->Clone()) && aObj->mBase) (c->mBase = aObj->mBase)->AddRef();
		ReturnObject(aResultToken, c);
		return OK;
	}
	case OM_Concat:
	case OM_Union:
	{
		Object *out = Object::Create();
		if (!out) return OK;
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i) { ExprTokenType v; ValToken(aObj, i, v); Push(out, v); }
		for (int a = 0; a < aParamCount; ++a)
		{
			Object *other = TokenAsObject(*aParam[a]);
			if (other)
				for (Object::IndexType i = 0; i < other->mFieldCount; ++i) { ExprTokenType v; ValToken(other, i, v); Push(out, v); }
			else if (aParam[a]->symbol != SYM_MISSING)
				Push(out, *aParam[a]);
		}
		if (id == OM_Union)
		{
			ChainResult u;
			ExprTokenType ot; ot.symbol = SYM_OBJECT; ot.object = out;
			ObjCall(out, _T("Unique"), u.tok, ot, NULL, 0);
			out->Release();
			if (u.tok.symbol == SYM_OBJECT) { u.tok.object->AddRef(); ReturnObject(aResultToken, (Object *)u.tok.object); }
			return OK;
		}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Diff:
	case OM_Intersect:
	{
		NEED_ARGS(1);
		Object *other = TokenAsObject(*aParam[0]);
		Object *out = Object::Create();
		if (!out) return OK;
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			ExprTokenType v; ValToken(aObj, i, v);
			bool found = false;
			if (other)
				for (Object::IndexType j = 0; j < other->mFieldCount && !found; ++j)
				{
					ExprTokenType u; ValToken(other, j, u);
					found = ValEqual(v, u, false);
				}
			if (found == (id == OM_Intersect)) Push(out, v);
		}
		ReturnObject(aResultToken, out);
		return OK;
	}
	case OM_Pick:
	case OM_Omit:
	{
		Object *out = Object::Create();
		if (!out) return OK;
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			ExprTokenType k; KeyToken(aObj, i, k);
			bool listed = false;
			for (int a = 0; a < aParamCount && !listed; ++a)
			{
				Object *keys = TokenAsObject(*aParam[a]);
				if (keys)
					for (Object::IndexType j = 0; j < keys->mFieldCount && !listed; ++j)
					{ ExprTokenType u; ValToken(keys, j, u); listed = ValEqual(k, u, false); }
				else
					listed = ValEqual(k, *aParam[a], false);
			}
			if (listed == (id == OM_Pick))
			{
				ExprTokenType v; ValToken(aObj, i, v);
				out->SetItem(k, v);
			}
		}
		ReturnObject(aResultToken, out);
		return OK;
	}
	// ------------------------------------------------------------- callbacks
	case OM_Map:
	case OM_Filter:
	case OM_Each:
	case OM_Find:
	case OM_FindKey:
	case OM_FindIndex:
	case OM_Any:
	case OM_All:
	case OM_CountIf:
	case OM_GroupBy:
	{
		NEED_ARGS(1);
		ExprTokenType &fn = *aParam[0];
		Object *snap = aObj->Clone();
		if (!snap) return OK;
		bool linear = IsLinear(snap);
		Object *out = (id == OM_Map || id == OM_Filter || id == OM_GroupBy) ? Object::Create() : NULL;
		__int64 count = 0;
		ResultType result = OK;
		bool done = false;
		// GroupBy accepts a property name instead of a function.
		LPTSTR group_prop = NULL;
		if (id == OM_GroupBy && !TokenToFunc(fn) && fn.symbol != SYM_OBJECT)
			group_prop = TokenToString(fn, argbuf[0]);
		for (Object::IndexType i = 0; i < snap->mFieldCount && !done; ++i)
		{
			ExprTokenType v, k; ValToken(snap, i, v); KeyToken(snap, i, k);
			ChainResult r;
			if (!group_prop)
			{
				result = CallVKA(fn, v, k, self, r);
				if (result != OK) break;
			}
			bool truth = group_prop ? false : TokenToBOOL(r.tok) != 0;
			switch (id)
			{
			case OM_Map:
				if (out) { if (linear) Push(out, r.tok); else out->SetItem(k, r.tok); }
				break;
			case OM_Filter:
				if (truth && out) { if (linear) Push(out, v); else out->SetItem(k, v); }
				break;
			case OM_Find:
				if (truth) { ReturnToken(aResultToken, v); done = true; }
				break;
			case OM_FindKey:
			case OM_FindIndex:
				if (truth) { ReturnToken(aResultToken, k); done = true; }
				break;
			case OM_Any:
				if (truth) { ReturnInt(aResultToken, 1); done = true; }
				break;
			case OM_All:
				if (!truth) { ReturnInt(aResultToken, 0); done = true; }
				break;
			case OM_CountIf:
				if (truth) ++count;
				break;
			case OM_GroupBy:
				if (out)
				{
					ExprTokenType gk;
					if (group_prop) Prop(v, group_prop, gk); else gk = r.tok;
					TCHAR gb[MAX_NUMBER_SIZE];
					ExprTokenType gks; gks.symbol = SYM_STRING; gks.marker = gk.symbol == SYM_OBJECT ? _T("") : TokenToString(gk, gb);
					ExprTokenType bucket_tok;
					Object *bucket = out->GetItem(bucket_tok, gks) && bucket_tok.symbol == SYM_OBJECT ? AsObject(bucket_tok.object) : NULL;
					if (!bucket && (bucket = Object::Create()))
					{
						ExprTokenType t; t.symbol = SYM_OBJECT; t.object = bucket;
						out->SetItem(gks, t);
						bucket->Release();
					}
					if (bucket) Push(bucket, v);
				}
				break;
			}
		}
		snap->Release();
		if (result != OK) { if (out) out->Release(); return result; }
		switch (id)
		{
		case OM_Map: case OM_Filter: case OM_GroupBy: ReturnObject(aResultToken, out); break;
		case OM_Each: ReturnThis(aResultToken, aObj); break;
		case OM_Find: if (!done && aParamCount > 1) ReturnToken(aResultToken, *aParam[1]); break; // Find(fn, default)
		case OM_FindIndex: if (!done) ReturnInt(aResultToken, 0); break;
		case OM_Any: if (!done) ReturnInt(aResultToken, 0); break;
		case OM_All: if (!done) ReturnInt(aResultToken, 1); break;
		case OM_CountIf: ReturnInt(aResultToken, count); break;
		}
		return OK;
	}
	case OM_Reduce:
	{
		// Reduce(fn, init := ""): fn(acc, value, key)
		NEED_ARGS(1);
		ExprTokenType &fn = *aParam[0];
		Object *snap = aObj->Clone();
		if (!snap) return OK;
		Object *holder = Object::Create(); // keeps the accumulator alive between calls
		if (!holder) { snap->Release(); return OK; }
		ExprTokenType acc_key; acc_key.symbol = SYM_INTEGER; acc_key.value_int64 = 1;
		ExprTokenType init; init.symbol = SYM_STRING; init.marker = _T("");
		holder->SetItem(acc_key, aParamCount > 1 && aParam[1]->symbol != SYM_MISSING ? *aParam[1] : init);
		ResultType result = OK;
		for (Object::IndexType i = 0; i < snap->mFieldCount; ++i)
		{
			ExprTokenType acc, v, k;
			ValToken(holder, 0, acc); ValToken(snap, i, v); KeyToken(snap, i, k);
			ExprTokenType *args[3] = { &acc, &v, &k };
			ChainResult r;
			if ((result = Callback(fn, args, 3, r)) != OK) break;
			holder->SetItem(acc_key, r.tok);
		}
		if (result == OK) { ExprTokenType acc; ValToken(holder, 0, acc); ReturnToken(aResultToken, acc); }
		holder->Release();
		snap->Release();
		return result;
	}
	// ------------------------------------------------------------- lookups
	case OM_Has:
	case OM_IndexOf:
	case OM_KeyOf:
	{
		NEED_ARGS(1);
		bool cs = ARG_BOOL(1, false);
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			ExprTokenType v; ValToken(aObj, i, v);
			if (ValEqual(v, *aParam[0], cs))
			{
				if (id == OM_Has) ReturnInt(aResultToken, 1);
				else { ExprTokenType k; KeyToken(aObj, i, k); ReturnToken(aResultToken, k); }
				return OK;
			}
		}
		if (id == OM_KeyOf) ReturnStr(aResultToken, _T("")); else ReturnInt(aResultToken, 0);
		return OK;
	}
	case OM_Len: ReturnInt(aResultToken, aObj->mFieldCount); return OK;
	case OM_IsEmpty: ReturnInt(aResultToken, aObj->mFieldCount == 0); return OK;
	case OM_IsLinear: ReturnInt(aResultToken, IsLinear(aObj)); return OK;
	case OM_First:
	case OM_Last:
	case OM_At:
	{
		__int64 n = aObj->mFieldCount, pos;
		if (id == OM_First) pos = 1;
		else if (id == OM_Last) pos = n;
		else { NEED_ARGS(1); pos = ARG_INT(0, 1); if (pos < 0) pos = n + pos + 1; }
		if (pos >= 1 && pos <= n) { ExprTokenType v; ValToken(aObj, (Object::IndexType)(pos - 1), v); ReturnToken(aResultToken, v); }
		else ReturnStr(aResultToken, _T(""));
		return OK;
	}
	case OM_Get:
	case OM_HasPath:
	{
		NEED_ARGS(1);
		ExprTokenType v;
		bool found = PathGet(aObj, ARG_STR(0, _T("")), v);
		if (id == OM_HasPath) ReturnInt(aResultToken, found);
		else if (found) ReturnToken(aResultToken, v);
		else if (aParamCount > 1) ReturnToken(aResultToken, *aParam[1]);
		else ReturnStr(aResultToken, _T(""));
		return OK;
	}
	// ------------------------------------------------------------- in place (return this)
	case OM_Set:
		NEED_ARGS(2);
		PathSet(aObj, ARG_STR(0, _T("")), *aParam[1]);
		ReturnThis(aResultToken, aObj);
		return OK;
	case OM_Append:
		for (int a = 0; a < aParamCount; ++a)
			if (aParam[a]->symbol != SYM_MISSING) Push(aObj, *aParam[a]);
		ReturnThis(aResultToken, aObj);
		return OK;
	case OM_Prepend:
		if (aParamCount && !aObj->InsertAt(0, 1, aParam, aParamCount))
			return g_script.ScriptError(ERR_OUTOFMEM);
		ReturnThis(aResultToken, aObj);
		return OK;
	case OM_Clear:
	{
		while (aObj->mFieldCount)
		{
			Object::IndexType i = --aObj->mFieldCount;
			if (i >= aObj->mKeyOffsetString) free(aObj->mFields[i].key.s);
			else if (i >= aObj->mKeyOffsetObject) aObj->mFields[i].key.p->Release();
			aObj->mFields[i].Free();
		}
		aObj->mKeyOffsetObject = aObj->mKeyOffsetString = 0;
		ReturnThis(aResultToken, aObj);
		return OK;
	}
	// ------------------------------------------------------------- aggregates
	case OM_Sum:
	case OM_Avg:
	case OM_Min:
	case OM_Max:
	case OM_Median:
	case OM_Stdev:
	{
		LPTSTR prop = ARG_STR(0, _T(""));
		int n = 0; bool all_int = true;
		double sum = 0, mn = 0, mx = 0;
		double *vals = (double *)malloc(((size_t)aObj->mFieldCount + 1) * sizeof(double));
		if (!vals) return OK;
		for (Object::IndexType i = 0; i < aObj->mFieldCount; ++i)
		{
			ExprTokenType v, pv; ValToken(aObj, i, v); Prop(v, prop, pv);
			double d; bool is_int;
			if (!NumberOf(pv, d, is_int)) continue;
			if (!is_int) all_int = false;
			if (!n || d < mn) mn = d;
			if (!n || d > mx) mx = d;
			sum += d;
			vals[n++] = d;
		}
		double out = 0; bool as_int = false;
		switch (id)
		{
		case OM_Sum: out = sum; as_int = all_int; break;
		case OM_Min: out = mn; as_int = all_int; break;
		case OM_Max: out = mx; as_int = all_int; break;
		case OM_Avg: out = n ? sum / n : 0; break;
		case OM_Median:
		{
			for (int i = 1; i < n; ++i) { double t = vals[i]; int j = i - 1; while (j >= 0 && vals[j] > t) { vals[j + 1] = vals[j]; --j; } vals[j + 1] = t; }
			out = !n ? 0 : (n & 1) ? vals[n / 2] : (vals[n / 2 - 1] + vals[n / 2]) / 2;
			break;
		}
		case OM_Stdev:
		{
			bool sample = ARG_BOOL(1, true);
			double mean = n ? sum / n : 0, ss = 0;
			for (int i = 0; i < n; ++i) ss += (vals[i] - mean) * (vals[i] - mean);
			int div = sample ? n - 1 : n;
			out = div > 0 ? sqrt(ss / div) : 0;
			break;
		}
		}
		free(vals);
		if (!n && (id == OM_Min || id == OM_Max)) { ReturnStr(aResultToken, _T("")); return OK; }
		if (as_int) ReturnInt(aResultToken, (__int64)out);
		else { aResultToken.symbol = SYM_FLOAT; aResultToken.value_double = out; }
		return OK;
	}
	}
	return INVOKE_NOT_HANDLED;
}


// ============================================================================
//  String methods ("abc".Method()) - dispatched from MetaObject::Invoke
// ============================================================================
enum ChainStrKind
{
	SK_FN = 1, SK_LEN, SK_CONTAINS, SK_STARTS, SK_ENDS, SK_LEFT, SK_RIGHT, SK_LINES, SK_ISEMPTY,
	SK_ISNUMBER, SK_ISINT, SK_TONUM, SK_QUOTE, SK_JSONPARSE, SK_CONCAT, SK_EQUALS, SK_WRAP, SK_CHARS
};
struct ChainStrMethod { LPCTSTR name; int kind; LPCTSTR func; };
static const ChainStrMethod sStrMethods[] =
{
	// stock v1 functions with the string as first parameter
	{_T("Trim"), SK_FN, _T("Trim")}, {_T("LTrim"), SK_FN, _T("LTrim")}, {_T("RTrim"), SK_FN, _T("RTrim")},
	{_T("SubStr"), SK_FN, _T("SubStr")}, {_T("Sub"), SK_FN, _T("SubStr")},
	{_T("InStr"), SK_FN, _T("InStr")}, {_T("Find"), SK_FN, _T("InStr")}, {_T("IndexOf"), SK_FN, _T("InStr")},
	{_T("Replace"), SK_FN, _T("StrReplace")}, {_T("StrReplace"), SK_FN, _T("StrReplace")},
	{_T("RegExReplace"), SK_FN, _T("RegExReplace")}, {_T("ReReplace"), SK_FN, _T("RegExReplace")},
	{_T("RegExMatch"), SK_FN, _T("RegExMatch")},
	{_T("Split"), SK_FN, _T("StrSplit")}, {_T("StrSplit"), SK_FN, _T("StrSplit")},
	{_T("Format"), SK_FN, _T("Format")}, {_T("Ord"), SK_FN, _T("Ord")},
	// toolkit functions (script_ext.cpp)
	{_T("Upper"), SK_FN, _T("StrUpper")}, {_T("Lower"), SK_FN, _T("StrLower")}, {_T("Title"), SK_FN, _T("StrTitle")},
	{_T("Repeat"), SK_FN, _T("StrRepeat")}, {_T("Reverse"), SK_FN, _T("StrReverse")},
	{_T("PadLeft"), SK_FN, _T("StrPadLeft")}, {_T("PadRight"), SK_FN, _T("StrPadRight")}, {_T("Count"), SK_FN, _T("StrCount")},
	{_T("Chosung"), SK_FN, _T("HangulChosung")}, {_T("HalfWidth"), SK_FN, _T("ToHalfWidth")},
	{_T("Slug"), SK_FN, _T("Slugify")}, {_T("Slugify"), SK_FN, _T("Slugify")},
	{_T("UrlEncode"), SK_FN, _T("UrlEncode")}, {_T("UrlDecode"), SK_FN, _T("UrlDecode")},
	{_T("HtmlEncode"), SK_FN, _T("HtmlEncode")}, {_T("HtmlDecode"), SK_FN, _T("HtmlDecode")},
	{_T("Base64"), SK_FN, _T("Base64Encode")}, {_T("Base64Encode"), SK_FN, _T("Base64Encode")},
	{_T("Base64Decode"), SK_FN, _T("Base64Decode")}, {_T("Base64UrlEncode"), SK_FN, _T("Base64UrlEncode")},
	{_T("Base64UrlDecode"), SK_FN, _T("Base64UrlDecode")},
	{_T("Hex"), SK_FN, _T("HexEncode")}, {_T("HexEncode"), SK_FN, _T("HexEncode")}, {_T("HexDecode"), SK_FN, _T("HexDecode")},
	{_T("SHA256"), SK_FN, _T("SHA256")}, {_T("SHA1"), SK_FN, _T("SHA1")}, {_T("SHA384"), SK_FN, _T("SHA384")},
	{_T("SHA512"), SK_FN, _T("SHA512")}, {_T("MD5"), SK_FN, _T("MD5")}, {_T("CRC32"), SK_FN, _T("CRC32")},
	{_T("HMAC"), SK_FN, _T("HMAC")}, {_T("HMAC512"), SK_FN, _T("HMAC512")},
	{_T("Encrypt"), SK_FN, _T("EncryptPassword")}, {_T("Decrypt"), SK_FN, _T("DecryptPassword")},
	{_T("Compress"), SK_FN, _T("Compress")}, {_T("Decompress"), SK_FN, _T("Decompress")},
	{_T("JsonGet"), SK_FN, _T("JsonGet")}, {_T("JsonPretty"), SK_FN, _T("JsonPretty")}, {_T("JsonValid"), SK_FN, _T("JsonValid")},
	{_T("Similarity"), SK_FN, _T("SimilarityRatio")}, {_T("Levenshtein"), SK_FN, _T("Levenshtein")},
	{_T("JaroWinkler"), SK_FN, _T("JaroWinkler")}, {_T("NaturalCompare"), SK_FN, _T("NaturalCompare")},
	{_T("ToBase"), SK_FN, _T("ToBase")}, {_T("FromBase"), SK_FN, _T("FromBase")},
	// native
	{_T("Length"), SK_LEN, NULL}, {_T("Len"), SK_LEN, NULL},
	{_T("Contains"), SK_CONTAINS, NULL}, {_T("includes"), SK_CONTAINS, NULL},
	{_T("StartsWith"), SK_STARTS, NULL}, {_T("EndsWith"), SK_ENDS, NULL},
	{_T("Left"), SK_LEFT, NULL}, {_T("Right"), SK_RIGHT, NULL}, {_T("Lines"), SK_LINES, NULL}, {_T("Chars"), SK_CHARS, NULL},
	{_T("IsEmpty"), SK_ISEMPTY, NULL}, {_T("IsNumber"), SK_ISNUMBER, NULL}, {_T("IsInteger"), SK_ISINT, NULL},
	{_T("ToNumber"), SK_TONUM, NULL}, {_T("Num"), SK_TONUM, NULL},
	{_T("Quote"), SK_QUOTE, NULL}, {_T("JsonParse"), SK_JSONPARSE, NULL},
	{_T("Concat"), SK_CONCAT, NULL}, {_T("Append"), SK_CONCAT, NULL},
	{_T("Equals"), SK_EQUALS, NULL}, {_T("Wrap"), SK_WRAP, NULL},
};

ResultType AhkWebChain::StrInvoke(ExprTokenType &aResultToken, ExprTokenType &aThisToken, int aFlags, ExprTokenType *aParam[], int aParamCount)
{
	if (!aParamCount)
		return INVOKE_NOT_HANDLED;
	bool is_call = IS_INVOKE_CALL != 0;
	if (!is_call && !(IS_INVOKE_GET && aParamCount == 1))
		return INVOKE_NOT_HANDLED;
	TCHAR namebuf[MAX_NUMBER_SIZE];
	LPTSTR name = TokenToString(*aParam[0], namebuf);
	const ChainStrMethod *m = NULL;
	for (int i = 0; i < _countof(sStrMethods); ++i)
		if (!_tcsicmp(sStrMethods[i].name, name)) { m = &sStrMethods[i]; break; }
	if (!m)
		return INVOKE_NOT_HANDLED;
	if (!is_call && m->kind != SK_LEN) // only "str.Length" works without parentheses
		return INVOKE_NOT_HANDLED;

	ExprTokenType **args = aParam + 1;
	int argc = aParamCount - 1;
	TCHAR thisbuf[MAX_NUMBER_SIZE], argbuf[4][MAX_NUMBER_SIZE];
	LPTSTR s = TokenToString(aThisToken, thisbuf);
	size_t slen = _tcslen(s);
	#define SARG(i, def) ((i) < argc && args[i]->symbol != SYM_MISSING ? TokenToString(*args[i], argbuf[i]) : (LPTSTR)(def))
	#define BARG(i, def) ((i) < argc && args[i]->symbol != SYM_MISSING ? (TokenToBOOL(*args[i]) != 0) : (def))
	#define IARG(i, def) ((i) < argc && args[i]->symbol != SYM_MISSING ? TokenToInt64(*args[i]) : (__int64)(def))

	switch (m->kind)
	{
	case SK_FN:
	{
		Func *f = g_script.FindFunc(m->func);
		if (!f) return INVOKE_NOT_HANDLED;
		ExprTokenType **p = (ExprTokenType **)_alloca((argc + 1) * sizeof(ExprTokenType *));
		ExprTokenType this_str; this_str.symbol = SYM_STRING; this_str.marker = s;
		p[0] = &this_str;
		memcpy(p + 1, args, argc * sizeof(ExprTokenType *));
		ResultType r = CallFunc(*f, aResultToken, p, argc + 1);
		// A string result from a built-in may live in this_str's buffer; make it persistent.
		if (aResultToken.symbol == SYM_STRING && s == thisbuf
			&& aResultToken.marker >= thisbuf && aResultToken.marker < thisbuf + _countof(thisbuf))
		{
			TCHAR copy[MAX_NUMBER_SIZE];
			tcslcpy(copy, aResultToken.marker, _countof(copy));
			ReturnStr(aResultToken, copy);
		}
		return (r == FAIL || r == EARLY_EXIT) ? r : OK;
	}
	case SK_LEN: ReturnInt(aResultToken, (__int64)slen); return OK;
	case SK_CONTAINS:
	{
		LPTSTR n = SARG(0, _T(""));
		bool cs = BARG(1, false);
		ReturnInt(aResultToken, cs ? _tcsstr(s, n) != NULL : StrStrIW(s, n) != NULL);
		return OK;
	}
	case SK_STARTS:
	case SK_ENDS:
	{
		LPTSTR n = SARG(0, _T(""));
		bool cs = BARG(1, false);
		size_t nl = _tcslen(n);
		bool ok = false;
		if (nl <= slen)
		{
			LPTSTR at = m->kind == SK_STARTS ? s : s + slen - nl;
			ok = cs ? !_tcsncmp(at, n, nl) : !_tcsnicmp(at, n, nl);
		}
		ReturnInt(aResultToken, ok);
		return OK;
	}
	case SK_LEFT:
	case SK_RIGHT:
	{
		__int64 n = IARG(0, 1);
		if (n <= 0) { ReturnStr(aResultToken, _T("")); return OK; }
		if ((size_t)n > slen) n = slen;
		ReturnStr(aResultToken, m->kind == SK_LEFT ? s : s + slen - (size_t)n, (size_t)n);
		return OK;
	}
	case SK_LINES:
	case SK_CHARS:
	{
		Func *f = g_script.FindFunc(_T("StrSplit"));
		if (!f) return INVOKE_NOT_HANDLED;
		ExprTokenType t0, t1, t2;
		t0.symbol = SYM_STRING; t0.marker = s;
		t1.symbol = SYM_STRING; t1.marker = m->kind == SK_LINES ? _T("\n") : _T("");
		t2.symbol = SYM_STRING; t2.marker = m->kind == SK_LINES ? _T("\r") : _T("");
		ExprTokenType *p[3] = { &t0, &t1, &t2 };
		ResultType r = CallFunc(*f, aResultToken, p, 3);
		return (r == FAIL || r == EARLY_EXIT) ? r : OK;
	}
	case SK_ISEMPTY: ReturnInt(aResultToken, !*s); return OK;
	case SK_ISNUMBER: ReturnInt(aResultToken, IsPureNumeric(s, TRUE, FALSE, TRUE) != PURE_NOT_NUMERIC); return OK;
	case SK_ISINT: ReturnInt(aResultToken, IsPureNumeric(s, TRUE, FALSE, FALSE) == PURE_INTEGER); return OK;
	case SK_TONUM:
	{
		switch (IsPureNumeric(s, TRUE, TRUE, TRUE))
		{
		case PURE_INTEGER: ReturnInt(aResultToken, ATOI64(s)); break;
		case PURE_FLOAT: aResultToken.symbol = SYM_FLOAT; aResultToken.value_double = ATOF(s); break;
		default: ReturnInt(aResultToken, 0);
		}
		return OK;
	}
	case SK_QUOTE:
	{
		ChainStr w;
		JsonString(w, s);
		ReturnStr(aResultToken, w.p ? w.p : _T(""), w.len);
		return OK;
	}
	case SK_JSONPARSE:
		AhkWebExt_JsonParse(s, aResultToken);
		return OK;
	case SK_CONCAT:
	{
		ChainStr w;
		w.Add(s, slen);
		for (int i = 0; i < argc; ++i)
		{
			TCHAR b[MAX_NUMBER_SIZE];
			w.Add(TokenToString(*args[i], b));
		}
		ReturnStr(aResultToken, w.p ? w.p : _T(""), w.len);
		return OK;
	}
	case SK_EQUALS:
	{
		LPTSTR o = SARG(0, _T(""));
		ReturnInt(aResultToken, BARG(1, false) ? !_tcscmp(s, o) : !_tcsicmp(s, o));
		return OK;
	}
	case SK_WRAP:
	{
		LPTSTR l = SARG(0, _T("\""));
		LPTSTR r = argc > 1 ? SARG(1, l) : l;
		ChainStr w;
		w.Add(l); w.Add(s, slen); w.Add(r);
		ReturnStr(aResultToken, w.p ? w.p : _T(""), w.len);
		return OK;
	}
	}
	#undef SARG
	#undef BARG
	#undef IARG
	return INVOKE_NOT_HANDLED;
}


// ============================================================================
//  Entry points used by script_object.cpp / script_ext.cpp
// ============================================================================
ResultType AhkWebChain_ObjCall(Object *aObj, LPCTSTR aName, ExprTokenType &aResultToken, ExprTokenType &aThisToken, ExprTokenType *aParam[], int aParamCount)
{
	return AhkWebChain::ObjCall(aObj, aName, aResultToken, aThisToken, aParam, aParamCount);
}

ResultType AhkWebChain_StrInvoke(ExprTokenType &aResultToken, ExprTokenType &aThisToken, int aFlags, ExprTokenType *aParam[], int aParamCount)
{
	return AhkWebChain::StrInvoke(aResultToken, aThisToken, aFlags, aParam, aParamCount);
}

LPTSTR AhkWebChain_ToJson(ExprTokenType &aValue, LPCTSTR aIndent)
{
	ChainStr w;
	AhkWebChain::JsonValue(w, aValue, aIndent ? aIndent : _T(""), 0);
	return w.fail ? NULL : w.Detach();
}

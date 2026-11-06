; AutoHotkey Web - native chaining / toolkit self-test.  Run: AutoHotkeyWeb.exe /ErrorStdOut ext_chain_test.ahk
#NoEnv
#Warn, All, Off
FileEncoding, UTF-8
SetWorkingDir %A_ScriptDir%
global pass := 0, fail := 0, out := ""

T(name, got, want) {
    if (got == want)
        pass++, out .= "ok   " name "`n"
    else
        fail++, out .= "FAIL " name "  got=[" got "] want=[" want "]`n"
}
IsEven(v) {
    return Mod(v, 2) = 0
}
Dbl(v) {
    return v * 2
}
Add(acc, v) {
    return acc + v
}
ByAge(a, b) {
    return a.age - b.age
}
class MyList {
    Sort() {
        return "user-sort"
    }
}

; ---------------- string chaining
T("trim.title", "  hello world ".Trim().Title(), "Hello World")
T("trim.upper.replace", "  hello world ".Trim().Upper().Replace("WORLD", "AHK"), "HELLO AHK")
T("split.compact.join", "a,b,,c".Split(",").Compact().Join("|"), "a|b|c")
s := "abc"
T("len-prop", s.Length, 3)
T("len()", s.Length(), 3)
T("reverse", s.Upper().Reverse(), "CBA")
n := 5
T("number.repeat", n.Repeat(3), "555")
T("chosung", "한글 테스트".Chosung(), "ㅎㄱ ㅌㅅㅌ")
T("startswith", "Hello".StartsWith("he"), 1)
T("endswith-cs", "Hello".EndsWith("LO", true), 0)
T("left/right", "Hello".Left(2) "|" "Hello".Right(3), "He|llo")
T("contains", "Hello".Contains("ELL"), 1)
T("isnumber", "1.5".IsNumber() "" "x1".IsNumber(), "10")
T("quote", "a""b".Quote(), """a\""b""")
T("lines", "a`r`nb`nc".Lines().Length(), 3)
T("format", "{1}-{2}".Format("x", "y"), "x-y")
T("padleft", "7".PadLeft(3, "0"), "007")
T("sub", "abcdef".Sub(2, 3), "bcd")
T("sha256-method", "abc".SHA256(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
T("base64 roundtrip", "한글 OK".Base64().Base64Decode(), "한글 OK")
T("jsonparse.get", "{""a"":[1,2,{""b"":""c""}]}".JsonParse().Get("a.3.b"), "c")

; ---------------- array chaining
T("sort.reverse.join", [3, 1, 2].Sort("N").Reverse().Join("-"), "3-2-1")
T("filter.map.sum", [1, 2, 3, 4].Filter("IsEven").Map(Func("Dbl")).Sum(), 12)
T("reduce", [1, 2, 3].Reduce("Add", 10), 16)
people := [{name: "Kim", age: 30}, {name: "Lee", age: 20}, {name: "Park", age: 40}]
T("sortby.pluck", people.SortBy("age").Pluck("name").Join(), "Lee,Kim,Park")
T("sort fn", people.Sort("ByAge").First().name, "Lee")
T("where", people.Where("age", 25, ">").Len(), 2)
T("top", people.Top(1, "age").First().name, "Park")
g := people.GroupBy("age")
T("groupby", g["30"].First().name, "Kim")
T("unique", [1, "1", 2, 2, 3].Unique().Join(), "1,2,3")
T("slice", [1, 2, 3, 4, 5].Slice(2, 4).Join(), "2,3,4")
T("slice neg", [1, 2, 3, 4, 5].Slice(-2).Join(), "4,5")
T("chunk.json", [1, 2, 3].Chunk(2).ToJson(false), "[[1,2],[3]]")
T("flatten", [1, [2, [3, 4]]].Flatten().Join(), "1,2,3,4")
a567 := [5, 6, 7]
T("has/indexof", a567.Has(6) a567.IndexOf(7), "13")
T("first/last/at", a567.First() a567.Last() a567.at(-2), "576")
a426 := [4, 2, 6]
T("min/max/avg", a426.Min() "/" a426.Max() "/" a426.Avg(), "2/6/4.000000")
T("median", [5, 1, 3, 2].Median(), "2.500000")
o := {}
o.Set("cfg.window.width", 800)
T("set/get path", o.Get("cfg.window.width") "|" o.HasPath("cfg.x") "|" o.Get("cfg.x", "def"), "800|0|def")
kv := {b: 2, a: 1}
T("keys/values", kv.Keys().Join() "|" kv.Values().Join(), "a,b|1,2")
T("each returns this", [1, 2].Each("Dbl").Length(), 2)
x1 := [1, 3, 4], x2 := [1, 3], x3 := [2, 4, 5]
T("any/all/countif", x1.Any("IsEven") x2.All("IsEven") x3.CountIf("IsEven"), "102")
a123 := [1, 2, 3], a12 := [1, 2]
T("diff/intersect/union", a123.Diff([2]).Join() "|" a123.Intersect([2, 3, 9]).Join() "|" a12.Union([2, 3]).Join(), "1,3|2,3|1,2,3")
m3 := {a: 1, b: 2, c: 3}
T("pick/omit", m3.Pick("a", "c").Join(",", ":") "|" m3.Omit("a", "c").Join(",", ":"), "a:1,c:3|b:2")
T("append/prepend", [2].Append(3, 4).Prepend(1).Join(), "1,2,3,4")
c := [1, [2]], d := c.Copy()
d[2][1] := 9
T("deep copy", c[2][1], 2)
T("shuffle keeps items", [1, 2, 3, 4].Shuffle().Sort("N").Join(), "1,2,3,4")
; precedence: class method and stock built-ins still win
ml := new MyList
T("class method wins", ml.Sort(), "user-sort")
T("stock Length", [1, 2, 3].Length(), 3)
m2 := {a: 1, b: 2}
T("stock Count", m2.Count(), 2)
; precedence: "".base override still wins
MyTrim(this) {
    return "custom"
}
"".base.Trim := Func("MyTrim")
T("default-base override wins", " x ".Trim(), "custom")
"".base.Delete("Trim")
T("override removed", " x ".Trim(), "x")

; ---------------- JSON
j := JsonStringify({name: "홍길동", tags: ["a", "b"], n: 1.5, z: 0})
T("stringify", j, "{""n"":1.5,""name"":""홍길동"",""tags"":[""a"",""b""],""z"":0}")
r := JsonParse(j)
T("parse roundtrip", r.name "|" r.tags[2] "|" r.n, "홍길동|b|1.5")
T("stringify pretty", JsonStringify([1, {a: 2}], 2), "[`n  1,`n  {`n    ""a"": 2`n  }`n]")
T("json escapes", JsonStringify("a`nb""c\"), """a\nb\""c\\""")

; ---------------- crypto / encoding
T("SHA256", SHA256("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
T("MD5", MD5("abc"), "900150983cd24fb0d6963f7d28e17f72")
T("SHA1", SHA1("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d")
T("CRC32", CRC32("abc"), "352441c2")
T("HMAC", HMAC("The quick brown fox jumps over the lazy dog", "key"), "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8")
T("Hex roundtrip", HexDecode(HexEncode("한글")), "한글")
T("UUID len", StrLen(UUID()), 36)
T("RandomBase64", StrLen(RandomBase64(12)) > 0, 1)
tok := EncryptPassword("비밀 메시지", "pw1234")
T("AES-GCM roundtrip", DecryptPassword(tok, "pw1234"), "비밀 메시지")
T("AES wrong pw", DecryptPassword(tok, "wrong"), "")
h := HashPassword("secret", 20000)
T("password hash verify", VerifyPassword("secret", h) "" VerifyPassword("nope", h), "10")
T("DPAPI roundtrip", DpapiUnprotect(DpapiProtect("내 토큰")), "내 토큰")
kp := StrSplit(EcdsaKeyPair(), ":")
sig := EcdsaSign("문서", kp[1])
T("ECDSA verify", EcdsaVerify("문서", sig, kp[2]) "" EcdsaVerify("변조", sig, kp[2]), "10")
FileDelete, %A_Temp%\ahkw_plain.txt
FileAppend, 파일 암호화 테스트, %A_Temp%\ahkw_plain.txt
T("EncryptFile", EncryptFile(A_Temp "\ahkw_plain.txt", A_Temp "\ahkw_plain.enc", "pw"), 1)
T("DecryptFile", DecryptFile(A_Temp "\ahkw_plain.enc", A_Temp "\ahkw_plain.dec", "pw"), 1)
FileRead, dec, %A_Temp%\ahkw_plain.dec
T("file roundtrip", dec, "파일 암호화 테스트")
T("SHA256File", StrLen(SHA256File(A_Temp "\ahkw_plain.txt")), 64)

; ---------------- compression / zip / encoding
T("Compress roundtrip", Decompress(Compress("반복 반복 반복 반복")), "반복 반복 반복 반복")
FileDelete, %A_Temp%\ahkw_test.zip
T("ZipAddFile", ZipAddFile(A_Temp "\ahkw_test.zip", A_Temp "\ahkw_plain.txt", "plain.txt"), 1)
T("ZipList", InStr(ZipList(A_Temp "\ahkw_test.zip"), "plain.txt") > 0, 1)
T("ZipExtractFile", ZipExtractFile(A_Temp "\ahkw_test.zip", "plain.txt", A_Temp "\ahkw_unzip.txt"), 1)
T("DetectEncoding", DetectEncoding(A_Temp "\ahkw_plain.txt") != "", 1)
T("UrlEncode", UrlEncode("a b&c"), "a%20b%26c")
T("FormatBytes", FormatBytes("1536"), "1.50 KB")

; ---------------- image / qr / screen
FileDelete, %A_Temp%\ahkw_qr.png
T("QrEncode", QrEncode("https://autohotkey.com", A_Temp "\ahkw_qr.png", 4, 2), 1)
T("ImageWidth(qr)", ImageWidth(A_Temp "\ahkw_qr.png") > 0, 1)
FileDelete, %A_Temp%\ahkw_cap.png
T("CaptureRegion", CaptureRegion(0, 0, 120, 80, A_Temp "\ahkw_cap.png"), 1)
T("ImageWidth(cap)", ImageWidth(A_Temp "\ahkw_cap.png") "x" ImageHeight(A_Temp "\ahkw_cap.png"), "120x80")
T("ImageRotate", ImageRotate(A_Temp "\ahkw_cap.png", A_Temp "\ahkw_rot.png", "90", false), 1)
T("ImageWidth(rot)", ImageWidth(A_Temp "\ahkw_rot.png"), 80)

; ---------------- OCR
langs := OcrLanguages()
out .= "info OcrLanguages=" langs "`n"
img := A_ScriptDir "\..\..\examples\04_AI_한글OCR\sample.png"
if FileExist(img) {
    t0 := A_TickCount
    txt := OcrImageAI(img)
    out .= "info OcrImageAI(" (A_TickCount - t0) "ms)=" SubStr(StrReplace(txt, "`n", " / "), 1, 120) " err=" OcrAIError() "`n"
    T("OcrImageAI non-empty", txt != "", 1)
    if (langs != "") {
        small := A_Temp "\ahkw_ocr_small.png"
        ImageResize(img, small, 1200, 1200)
        out .= "info sample=" ImageWidth(img) "x" ImageHeight(img) "`n"
        w := OcrImage(small, "ko")
        out .= "info OcrImage(ko)=" SubStr(StrReplace(w, "`n", " / "), 1, 120) "`n"
    }
}

; ---------------- HTTP (network)
body := HttpGet("https://example.com/")
out .= "info HttpGet status=" HttpLastStatus() " len=" StrLen(body) "`n"

FileAppend, % out "`nPASS=" pass " FAIL=" fail "`n", *
ExitApp

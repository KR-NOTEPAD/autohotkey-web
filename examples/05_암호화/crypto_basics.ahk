; ============================================================================
;  AutoHotkey Web 예제 5-1 - 암호화 기초 (전부 엔진 내장, #Include / DLL 불필요)
;  해시 · HMAC · 인코딩 · AES-256-GCM 문자열 암호화 · 비밀번호 해시 · DPAPI ·
;  ECDSA 전자서명 · 보안 난수를 한 화면에서 확인합니다.
;  실행: AutoHotkeyWeb.exe crypto_basics.ahk
; ============================================================================
#NoEnv
#SingleInstance Force

msg := "안녕하세요, AutoHotkey Web!"
r := ""

; ---- 1) 해시 / 체크섬 (함수형과 메소드 체이닝 둘 다 가능) -------------------------
r .= "[해시]`r`n"
r .= "SHA256  : " SHA256(msg) "`r`n"
r .= "SHA512  : " SubStr(SHA512(msg), 1, 64) "...`r`n"
r .= "MD5     : " msg.MD5() "`r`n"                       ; 문자열 메소드
r .= "CRC32   : " CRC32(msg) "`r`n"
r .= "HMAC256 : " HMAC(msg, "비밀키") "`r`n`r`n"

; ---- 2) 인코딩 --------------------------------------------------------------------
r .= "[인코딩]`r`n"
b64 := msg.Base64()
r .= "Base64     : " b64 "`r`n"
r .= "Base64 복원: " b64.Base64Decode() "`r`n"
r .= "Hex        : " SubStr(HexEncode(msg), 1, 40) "...`r`n"
r .= "URL        : " UrlEncode("a b&c=한글") "`r`n`r`n"

; ---- 3) AES-256-GCM (비밀번호 기반, 위변조 검출 포함) -------------------------------
r .= "[AES-256-GCM 문자열 암호화]`r`n"
token := EncryptPassword(msg, "my-password")          ; "v1:salt:nonce:tag:cipher"
r .= "암호문 : " SubStr(token, 1, 60) "...`r`n"
r .= "복호화 : " DecryptPassword(token, "my-password") "`r`n"
r .= "틀린 비번 → " (DecryptPassword(token, "wrong") = "" ? "복호화 거부(정상)" : "??") "`r`n"
memo := "짧은 메모"
r .= "체이닝 : " memo.Encrypt("pw").Decrypt("pw") "`r`n`r`n"

; ---- 4) 비밀번호 저장용 해시 (PBKDF2) -----------------------------------------------
r .= "[비밀번호 해시]`r`n"
h := HashPassword("p@ssw0rd")                          ; 저장할 값 (평문 저장 금지)
r .= "저장값 : " SubStr(h, 1, 50) "...`r`n"
r .= "검증   : 맞는 비번=" VerifyPassword("p@ssw0rd", h) ", 틀린 비번=" VerifyPassword("x", h) "`r`n`r`n"

; ---- 5) DPAPI (이 PC / 이 사용자에서만 풀리는 자동 암호화) ------------------------------
r .= "[DPAPI]`r`n"
blob := DpapiProtect("API_KEY=abc123")
r .= "보호값 : " SubStr(blob, 1, 50) "...`r`n"
r .= "해제   : " DpapiUnprotect(blob) "`r`n`r`n"

; ---- 6) ECDSA P-256 전자서명 ----------------------------------------------------------
r .= "[전자서명 ECDSA]`r`n"
kp := StrSplit(EcdsaKeyPair(), ":")                    ; [1]=개인키(비밀) [2]=공개키(배포)
sig := EcdsaSign("계약서 v1", kp[1])
r .= "서명     : " SubStr(sig, 1, 50) "...`r`n"
r .= "원문 검증: " EcdsaVerify("계약서 v1", sig, kp[2]) "  (1=유효)`r`n"
r .= "변조 검증: " EcdsaVerify("계약서 v2", sig, kp[2]) "  (0=위조 탐지)`r`n`r`n"

; ---- 7) 보안 난수 ------------------------------------------------------------------------
r .= "[보안 난수]`r`n"
r .= "UUID         : " UUID() "`r`n"
r .= "토큰(32B)    : " RandomBase64(32) "`r`n"
r .= "주사위 1~6   : " RandInt(1, 6) "`r`n"
r .= "엔트로피(abc): " StrEntropy("abcabcabc") "`r`n"

Gui, Font, s10, Consolas
Gui, Add, Edit, w860 h560 ReadOnly, %r%
Gui, Show,, AutoHotkey Web - 암호화 기초
return

GuiClose:
ExitApp

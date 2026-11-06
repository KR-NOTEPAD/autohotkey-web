# Interactive-path tests (needs an elevated shell where UAC elevates without prompting):
#  1) code signing via certificate subject and via .pfx (temporary self-signed cert)
#  2) setup wizard clicked through page by page (SetupScope ask -> current user), GUI uninstall
#  3) same wizard launched non-elevated, "all users" chosen -> elevated relaunch installs to Program Files
#  4) RequireAdmin exe launched non-elevated runs elevated
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path "$here\..\.."
$a2e  = "$root\compiler\AhkWeb2Exe.exe"
$tmp  = "$env:TEMP\ahkw2e_wiz"
# stale wizards/uninstallers from an aborted run keep dialogs with the same title open -> close them first
Get-Process | Where-Object { $_.ProcessName -like 'uninstall*' -or $_.ProcessName -like 'AhkWebUninst*' -or $_.ProcessName -like 'Bundle Test_Setup*' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $tmp | Out-Null
$fail = 0
function Check($name, $cond) { if ($cond) { "PASS  $name" } else { "FAIL  $name"; $script:fail++ } }
function Compile([string[]]$a) {
    $p = Start-Process -FilePath $a2e -ArgumentList $a -RedirectStandardOutput "$tmp\o.txt" -RedirectStandardError "$tmp\e.txt" -Wait -PassThru
    Get-Content "$tmp\o.txt", "$tmp\e.txt" | ForEach-Object { Write-Host "    | $_" }
    return $p.ExitCode
}
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class U {
  [DllImport("user32")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32", CharSet=CharSet.Unicode)] static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
  public static string Text(IntPtr h) { var sb = new StringBuilder(1024); SendMessageW(h, 0x0D, (IntPtr)1024, sb); return sb.ToString(); }
  [DllImport("kernel32", CharSet=CharSet.Unicode)] static extern IntPtr LoadLibraryExW(string f, IntPtr h, uint fl);
  [DllImport("kernel32")] static extern bool FreeLibrary(IntPtr h);
  [DllImport("kernel32", CharSet=CharSet.Unicode)] static extern IntPtr FindResourceW(IntPtr h, string name, IntPtr type);
  [DllImport("kernel32")] static extern IntPtr LoadResource(IntPtr h, IntPtr r);
  [DllImport("kernel32")] static extern IntPtr LockResource(IntPtr g);
  [DllImport("kernel32")] static extern uint SizeofResource(IntPtr h, IntPtr r);
  public static string Cfg(string f) {
    IntPtr m = LoadLibraryExW(f, IntPtr.Zero, 0x22); if (m == IntPtr.Zero) return "";
    try { IntPtr r = FindResourceW(m, ">SETUP CFG<", (IntPtr)10); if (r == IntPtr.Zero) return "";
      byte[] b = new byte[SizeofResource(m, r)]; Marshal.Copy(LockResource(LoadResource(m, r)), b, 0, b.Length);
      return Encoding.Unicode.GetString(b); } finally { FreeLibrary(m); } } }
'@
$WM_COMMAND = 0x111; $BM_CLICK = 0xF5; $BM_SETCHECK = 0xF1; $WM_SETTEXT = 0x0C
$ID_NEXT = 101; $ID_AGREE = 108; $ID_DIR = 111; $ID_DESK = 114; $ID_START = 115; $ID_RUN = 118; $ID_SCOPE_U = 119; $ID_SCOPE_M = 120
function WaitWin([string]$title, [int]$sec = 15) {
    for ($i = 0; $i -lt $sec * 10; $i++) { $h = [U]::FindWindowW('AhkWebSetupWnd', $title); if ($h -ne [IntPtr]::Zero) { return $h }; Start-Sleep -Milliseconds 100 }
    return [IntPtr]::Zero
}
function WaitDialog([string]$title, [int]$sec = 15) {
    for ($i = 0; $i -lt $sec * 10; $i++) { $h = [U]::FindWindowW('#32770', $title); if ($h -ne [IntPtr]::Zero) { return $h }; Start-Sleep -Milliseconds 100 }
    return [IntPtr]::Zero
}
function Cmd($h, $id) { [U]::SendMessageW($h, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 250 }
function Click($h, $id) { [U]::SendMessageW([U]::GetDlgItem($h, $id), $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 200 }
function SetCheck($h, $id, $on) { [U]::SendMessageW([U]::GetDlgItem($h, $id), $BM_SETCHECK, [IntPtr]$on, [IntPtr]::Zero) | Out-Null }
function WaitFinish($h, [int]$sec = 60) {
    for ($i = 0; $i -lt $sec * 10; $i++) { if (-not [U]::IsWindow($h)) { return $false }; if ([U]::IsWindowVisible([U]::GetDlgItem($h, $ID_RUN))) { return $true }; Start-Sleep -Milliseconds 100 }
    return $false
}

"== 1) code signing =="
$subject = 'AhkWeb2Exe Test Signing'
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$subject" -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddDays(2)
try {
    [IO.File]::WriteAllText("$tmp\sign1.ahk", ";@AhkWeb-SignSubject $subject`r`nExitApp`r`n")
    Check "sign by subject: compile" ((Compile @('/in', "`"$tmp\sign1.ahk`"")) -eq 0)
    $sig = Get-AuthenticodeSignature "$tmp\sign1.exe"
    Check "sign by subject: signed with test cert" ($sig.SignerCertificate -and $sig.SignerCertificate.Thumbprint -eq $cert.Thumbprint)
    Check "sign by subject: timestamped" ($sig.TimeStamperCertificate -ne $null)
    $pw = ConvertTo-SecureString 'Test1234!' -AsPlainText -Force
    Export-PfxCertificate -Cert $cert -FilePath "$tmp\test.pfx" -Password $pw | Out-Null
    [IO.File]::WriteAllText("$tmp\sign2.ahk", ";@AhkWeb-Sign test.pfx, Test1234!`r`n;@AhkWeb-Setup`r`n;@AhkWeb-SetupShortcuts none`r`nExitApp`r`n")
    Check "sign by pfx (+setup): compile" ((Compile @('/in', "`"$tmp\sign2.ahk`"")) -eq 0)
    Check "sign by pfx: exe signed"   ((Get-AuthenticodeSignature "$tmp\sign2.exe").SignerCertificate.Thumbprint -eq $cert.Thumbprint)
    Check "sign by pfx: Setup.exe signed" ((Get-AuthenticodeSignature "$tmp\sign2_Setup.exe").SignerCertificate.Thumbprint -eq $cert.Thumbprint)
    [IO.File]::WriteAllText("$tmp\sign3.ahk", ";@AhkWeb-Sign test.pfx, wrong`r`nExitApp`r`n")
    Check "wrong pfx password -> exit 2" ((Compile @('/in', "`"$tmp\sign3.ahk`"")) -eq 2)
} finally {
    Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -Force -ErrorAction SilentlyContinue
}

"== 2) wizard click-through, scope=ask -> current user =="
$src = "$tmp\app"; Copy-Item "$here\bundle_test" $src -Recurse
$main = Get-Content "$src\main.ahk" -Raw
$main = $main -replace ';@AhkWeb-SetupScope user', ';@AhkWeb-SetupScope ask'
[IO.File]::WriteAllText("$src\main.ahk", $main, (New-Object Text.UTF8Encoding $true))
Check "compile setup (ask)" ((Compile @('/in', "`"$src\main.ahk`"", '/out', "`"$tmp\out\BundleTest.exe`"", '/setup')) -eq 0)
$setup = "$tmp\out\Bundle Test_Setup.exe"
Check "setup cfg scope=ask" ([U]::Cfg($setup) -match 'scope=ask')
$inst = "$tmp\inst_user"
Start-Process $setup
$h = WaitWin 'Bundle Test 설치'
if ($h -eq [IntPtr]::Zero) { $h = WaitWin 'Bundle Test Setup' 2 }
Check "wizard window opened" ($h -ne [IntPtr]::Zero)
Cmd $h $ID_NEXT                                  # welcome -> license
Check "license page: Next disabled until agree" (-not [U]::IsWindowEnabled([U]::GetDlgItem($h, $ID_NEXT)))
Click $h $ID_AGREE
Check "license page: Next enabled after agree" ([U]::IsWindowEnabled([U]::GetDlgItem($h, $ID_NEXT)))
Cmd $h $ID_NEXT                                  # license -> folder
Check "scope radios shown (ask)" ([U]::IsWindowVisible([U]::GetDlgItem($h, $ID_SCOPE_U)) -and [U]::IsWindowVisible([U]::GetDlgItem($h, $ID_SCOPE_M)))
Click $h $ID_SCOPE_U
[U]::SendMessageW([U]::GetDlgItem($h, $ID_DIR), $WM_SETTEXT, [IntPtr]::Zero, $inst) | Out-Null
SetCheck $h $ID_DESK 0; SetCheck $h $ID_START 0
Cmd $h $ID_NEXT                                  # Install
Check "installs and reaches finish page" (WaitFinish $h)
SetCheck $h $ID_RUN 0; Cmd $h $ID_NEXT           # Finish
Start-Sleep -Milliseconds 500
Check "wizard closed"            (-not [U]::IsWindow($h))
Check "files installed"          ((Test-Path "$inst\BundleTest.exe") -and (Test-Path "$inst\data\a.txt"))
$keyU = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AhkWeb_Bundle_Test'
Check "HKCU entry (current user)" (Test-Path $keyU)
Check "uninstall.exe records scope=user" ([U]::Cfg("$inst\uninstall.exe") -match 'scope=user')
# GUI uninstall: confirm "Yes", then "OK" on the result box
Start-Process "$inst\uninstall.exe"
$d = WaitDialog 'Bundle Test'
Check "uninstall confirm dialog" ($d -ne [IntPtr]::Zero)
# answer "Yes" until that box is really gone (a message posted before it finishes initialising is dropped)
for ($i = 0; $i -lt 40 -and [U]::IsWindow($d); $i++) { Start-Sleep -Milliseconds 300; [U]::PostMessageW($d, $WM_COMMAND, [IntPtr]6, [IntPtr]::Zero) | Out-Null }
$d2 = [IntPtr]::Zero
for ($i = 0; $i -lt 150 -and $d2 -eq [IntPtr]::Zero; $i++) { $c = [U]::FindWindowW('#32770', 'Bundle Test'); if ($c -ne [IntPtr]::Zero -and $c -ne $d) { $d2 = $c } else { Start-Sleep -Milliseconds 100 } }
Check "uninstall result dialog" ($d2 -ne [IntPtr]::Zero)
for ($i = 0; $i -lt 40 -and [U]::IsWindow($d2); $i++) { Start-Sleep -Milliseconds 300; [U]::PostMessageW($d2, $WM_COMMAND, [IntPtr]1, [IntPtr]::Zero) | Out-Null }   # IDOK
Start-Sleep -Seconds 5
Check "GUI uninstall removed folder"   (-not (Test-Path $inst))
Check "GUI uninstall removed registry" (-not (Test-Path $keyU))

"== 3) non-elevated wizard -> all users -> elevated relaunch =="
Start-Process explorer.exe -ArgumentList "`"$setup`""          # medium integrity, like a double-click
$h = WaitWin 'Bundle Test 설치'
if ($h -eq [IntPtr]::Zero) { $h = WaitWin 'Bundle Test Setup' 2 }
Check "wizard (non-elevated) opened" ($h -ne [IntPtr]::Zero)
Cmd $h $ID_NEXT; Click $h $ID_AGREE; Cmd $h $ID_NEXT
Click $h $ID_SCOPE_M
$dirText = [U]::Text([U]::GetDlgItem($h, $ID_DIR))
Check "all-users folder = Program Files" ($dirText -like "$env:ProgramFiles\*")
SetCheck $h $ID_DESK 0; SetCheck $h $ID_START 0
$uacOn = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System').EnableLUA -ne 0
Cmd $h $ID_NEXT
Start-Sleep -Seconds 1
if ($uacOn) {
    Check "non-elevated wizard handed off and closed" (-not [U]::IsWindow($h))
    $h2 = WaitWin 'Bundle Test 설치' 20
    if ($h2 -eq [IntPtr]::Zero) { $h2 = WaitWin 'Bundle Test Setup' 2 }
    Check "elevated wizard appeared" ($h2 -ne [IntPtr]::Zero)
} else {
    "SKIP  elevation hand-off (UAC is off on this PC: every process is already admin)"
    $h2 = $h
}
Check "elevated install reaches finish" (WaitFinish $h2)
SetCheck $h2 $ID_RUN 0; Cmd $h2 $ID_NEXT
$keyM = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AhkWeb_Bundle_Test'
Check "installed to Program Files" (Test-Path "$dirText\BundleTest.exe")
Check "HKLM entry (all users)"     (Test-Path $keyM)
Check "uninstall.exe records scope=machine" ([U]::Cfg("$dirText\uninstall.exe") -match 'scope=machine')
$q = (Get-ItemProperty $keyM).QuietUninstallString
Start-Process cmd.exe -ArgumentList "/c $q" -Wait
Start-Sleep -Seconds 5
Check "silent uninstall (all users) removed folder"   (-not (Test-Path $dirText))
Check "silent uninstall (all users) removed registry" (-not (Test-Path $keyM))

"== 4) RequireAdmin exe from a normal launch =="
[IO.File]::WriteAllText("$tmp\adm.ahk", ";@AhkWeb-RequireAdmin`r`nFileAppend, %A_IsAdmin%, %A_ScriptDir%\adm.txt`r`nExitApp`r`n")
Check "compile RequireAdmin" ((Compile @('/in', "`"$tmp\adm.ahk`"")) -eq 0)
Start-Process explorer.exe -ArgumentList "`"$tmp\adm.exe`""
for ($i = 0; $i -lt 100 -and -not (Test-Path "$tmp\adm.txt"); $i++) { Start-Sleep -Milliseconds 100 }
Check "runs elevated (A_IsAdmin=1)" ((Get-Content "$tmp\adm.txt" -ErrorAction SilentlyContinue) -eq '1')

if ($fail) { "$fail check(s) failed"; exit 1 } else { "all checks passed" }

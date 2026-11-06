# End-to-end: bundle_test\main.ahk -> single exe (includes, Lib, FileInstall, bundled files),
# then EXE + Setup.exe (silent install, run, silent uninstall), RequireAdmin manifest, syntax error.
# Executables run from %TEMP% (cloud-synced drives refuse to launch them).
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path "$here\..\.."
$a2e  = "$root\compiler\AhkWeb2Exe.exe"
$src  = "$here\bundle_test\main.ahk"
$tmp  = "$env:TEMP\ahkw2e_bundle"
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $tmp | Out-Null
$fail = 0
function Check($name, $cond) { if ($cond) { "PASS  $name" } else { "FAIL  $name"; $script:fail++ } }
function Compile([string[]]$a) {
    $p = Start-Process -FilePath $a2e -ArgumentList $a -RedirectStandardOutput "$tmp\o.txt" -RedirectStandardError "$tmp\e.txt" -Wait -PassThru
    Get-Content "$tmp\o.txt", "$tmp\e.txt" | ForEach-Object { Write-Host "    | $_" }
    return $p.ExitCode
}
function Result($dir) { $r = @{}; Get-Content "$dir\result.txt" -Encoding UTF8 -ErrorAction SilentlyContinue | ForEach-Object { $k, $v = $_ -split '=', 2; $r[$k] = $v }; $r }

"== single exe =="
$exe = "$tmp\single\BundleTest.exe"
Check "compile exit 0" ((Compile @('/in', "`"$src`"", '/out', "`"$exe`"")) -eq 0)
$p = Start-Process $exe -Wait -PassThru
$r = Result "$tmp\single"
Check "runs (exit 0)"            ($p.ExitCode -eq 0)
Check "#Include merged"          ($r['helper'] -eq 'helper-ok')
Check "Lib auto-include merged"  ($r['lib'] -eq 'lib-ok')
Check "FileInstall embedded"     ($r['fileinstall'] -eq 'from-fileinstall')
Check "bundled folder extracted" ($r['files'] -eq 'alpha|{"x":1}')
Check "bundled file -> docs\"    ($r['docs'] -eq 'yes')
Check "IgnoreBegin/End + Keep"   ($r['mode'] -eq 'compiled')
Check "temp files cleaned on exit" (-not (Test-Path "$env:TEMP\AhkWeb_$($p.Id)"))

"== EXE + setup wizard =="
$exe2 = "$tmp\setup\BundleTest.exe"; $inst = "$tmp\installed"
Check "compile /setup exit 0" ((Compile @('/in', "`"$src`"", '/out', "`"$exe2`"", '/setup')) -eq 0)
$setup = "$tmp\setup\Bundle Test_Setup.exe"
Check "Setup.exe created" (Test-Path $setup)
$v = [Diagnostics.FileVersionInfo]::GetVersionInfo($setup)
Check "setup version info" ($v.ProductName -eq 'Bundle Test' -and $v.ProductVersion -eq '2.0.1')
$p = Start-Process $setup -ArgumentList '/S', "/D=$inst" -Wait -PassThru
Check "silent install exit 0" ($p.ExitCode -eq 0)
Check "installed exe + files" ((Test-Path "$inst\BundleTest.exe") -and (Test-Path "$inst\data\deep\b.json") -and (Test-Path "$inst\docs\readme.md"))
Check "uninstaller written"   ((Test-Path "$inst\uninstall.exe") -and (Test-Path "$inst\uninstall.log"))
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AhkWeb_Bundle_Test'
$reg = Get-ItemProperty $key -ErrorAction SilentlyContinue
Check "Apps & features entry" ($reg -and $reg.DisplayName -eq 'Bundle Test' -and $reg.DisplayVersion -eq '2.0.1')
Check "uninstaller has no payload" ((Get-Item "$inst\uninstall.exe").Length -lt (Get-Item $setup).Length / 2)
$p = Start-Process "$inst\BundleTest.exe" -Wait -PassThru
$r = Result $inst
Check "installed app runs, sees files" ($p.ExitCode -eq 0 -and $r['files'] -eq 'alpha|{"x":1}' -and $r['lib'] -eq 'lib-ok')
Remove-Item "$inst\result.txt" -ErrorAction SilentlyContinue
$p = Start-Process "$inst\uninstall.exe" -ArgumentList '/S' -Wait -PassThru
Start-Sleep -Seconds 5   # uninstall.exe + folder are removed by a short delayed cleanup
Check "silent uninstall: folder removed" (-not (Test-Path $inst))
Check "silent uninstall: registry removed" (-not (Test-Path $key))

"== RequireAdmin manifest =="
$adm = "$tmp\admin.ahk"
[IO.File]::WriteAllText($adm, ";@AhkWeb-RequireAdmin`r`nExitApp`r`n")
Check "compile exit 0" ((Compile @('/in', "`"$adm`"")) -eq 0)
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class Man {
  [DllImport("kernel32", CharSet=CharSet.Unicode)] static extern IntPtr LoadLibraryExW(string f, IntPtr h, uint fl);
  [DllImport("kernel32")] static extern bool FreeLibrary(IntPtr h);
  [DllImport("kernel32")] static extern IntPtr FindResourceW(IntPtr h, IntPtr name, IntPtr type);
  [DllImport("kernel32")] static extern IntPtr LoadResource(IntPtr h, IntPtr r);
  [DllImport("kernel32")] static extern IntPtr LockResource(IntPtr g);
  [DllImport("kernel32")] static extern uint SizeofResource(IntPtr h, IntPtr r);
  public static string Get(string f) {
    IntPtr m = LoadLibraryExW(f, IntPtr.Zero, 0x22); if (m == IntPtr.Zero) return "";
    try { IntPtr r = FindResourceW(m, (IntPtr)1, (IntPtr)24); if (r == IntPtr.Zero) return "";
      byte[] b = new byte[SizeofResource(m, r)]; Marshal.Copy(LockResource(LoadResource(m, r)), b, 0, b.Length);
      return Encoding.UTF8.GetString(b); } finally { FreeLibrary(m); } } }
'@
Check "manifest requireAdministrator" ((([Man]::Get("$tmp\admin.exe")) -match 'level="requireAdministrator"'))

"== syntax error is reported =="
$bad = "$tmp\bad.ahk"
[IO.File]::WriteAllText($bad, "x := (1 + `r`nMsgBox`r`n")
$rc = Compile @('/in', "`"$bad`"")
Check "bad script -> exit 2" ($rc -eq 2 -and -not (Test-Path "$tmp\bad.exe"))

if ($fail) { "$fail check(s) failed"; exit 1 } else { "all checks passed" }

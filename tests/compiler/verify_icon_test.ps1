# Compiles icon_test.ahk with AhkWeb2Exe and verifies icon groups, version info and that it runs.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path "$here\..\.."
$res  = "$root\engine_src\source\resources"
$exe  = "$here\out\icon_test.exe"

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Res {
  [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryExW(string f, IntPtr h, uint fl);
  [DllImport("kernel32")] static extern bool FreeLibrary(IntPtr h);
  [DllImport("kernel32")] static extern IntPtr FindResourceW(IntPtr h, IntPtr name, IntPtr type);
  [DllImport("kernel32")] static extern IntPtr LoadResource(IntPtr h, IntPtr r);
  [DllImport("kernel32")] static extern IntPtr LockResource(IntPtr g);
  [DllImport("kernel32")] static extern uint SizeofResource(IntPtr h, IntPtr r);
  static byte[] Get(IntPtr m, int type, int id) {
    IntPtr r = FindResourceW(m, (IntPtr)id, (IntPtr)type); if (r == IntPtr.Zero) return null;
    uint n = SizeofResource(m, r); byte[] b = new byte[n]; Marshal.Copy(LockResource(LoadResource(m, r)), b, 0, (int)n); return b;
  }
  // Returns true when icon group <grp> in <file> holds exactly the images of <ico>.
  public static bool GroupMatches(string file, int grp, byte[] ico) {
    IntPtr m = LoadLibraryExW(file, IntPtr.Zero, 0x22); if (m == IntPtr.Zero) return false;
    try {
      byte[] g = Get(m, 14, grp); if (g == null) return false;
      int cnt = BitConverter.ToUInt16(g, 4); if (cnt != BitConverter.ToUInt16(ico, 4)) return false;
      for (int i = 0; i < cnt; i++) {
        int id = BitConverter.ToUInt16(g, 6 + 14*i + 12);
        int len = BitConverter.ToInt32(ico, 6 + 16*i + 8), off = BitConverter.ToInt32(ico, 6 + 16*i + 12);
        byte[] img = Get(m, 3, id); if (img == null || img.Length != len) return false;
        for (int k = 0; k < len; k++) if (img[k] != ico[off + k]) return false;
      }
      return true;
    } finally { FreeLibrary(m); }
  }
}
'@

Remove-Item "$here\out" -Recurse -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$root\compiler\AhkWeb2Exe.exe" -ArgumentList "/in `"$here\icon_test.ahk`"" `
     -RedirectStandardOutput "$env:TEMP\ahkw2e_out.txt" -RedirectStandardError "$env:TEMP\ahkw2e_err.txt" -Wait -PassThru
Get-Content "$env:TEMP\ahkw2e_out.txt", "$env:TEMP\ahkw2e_err.txt"
$fail = 0
function Check($name, $cond) { if ($cond) { "PASS  $name" } else { "FAIL  $name"; $script:fail++ } }

Check "exit code 0"           ($p.ExitCode -eq 0)
Check "ExeName -> out\icon_test.exe" (Test-Path $exe)
Check "main icon (159) = icon_suspend.ico" ([Res]::GroupMatches($exe, 159, [IO.File]::ReadAllBytes("$res\icon_suspend.ico")))
Check "AddResource 207 = icon_pause.ico"   ([Res]::GroupMatches($exe, 207, [IO.File]::ReadAllBytes("$res\icon_pause.ico")))
Check "tray suspend icon (206) untouched"  ([Res]::GroupMatches($exe, 206, [IO.File]::ReadAllBytes("$res\icon_suspend.ico")))
$v = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
Check "FileVersion 1.2.3.4"   ($v.FileVersion -eq '1.2.3.4' -and $v.FileMajorPart -eq 1 -and $v.FilePrivatePart -eq 4)
Check "ProductVersion 1.2.3.4" ($v.ProductVersion -eq '1.2.3.4')
Check "FileDescription"       ($v.FileDescription -eq '아이콘 테스트 앱')
Check "LegalCopyright"        ($v.LegalCopyright -eq '(c) 2026 KR-NOTEPAD')
Check "CompanyName"           ($v.CompanyName -eq 'KR-NOTEPAD')
Check "ProductName"           ($v.ProductName -eq 'AhkWeb Icon Test')
# Run a copy from %TEMP%: cloud-synced drives (e.g. Google Drive) refuse to launch executables.
$run = "$env:TEMP\ahkw2e_test"; Remove-Item $run -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$run\out" | Out-Null; Copy-Item $exe "$run\icon_test.exe"
$r = Start-Process -FilePath "$run\icon_test.exe" -Wait -PassThru
Check "compiled exe runs"     ($r.ExitCode -eq 0 -and (Get-Content "$run\out\ran.txt" -ErrorAction SilentlyContinue) -eq 'ok')
if ($fail) { "$fail check(s) failed"; exit 1 } else { "all checks passed" }

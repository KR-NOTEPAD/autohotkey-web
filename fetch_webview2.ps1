# ============================================================================
#  AutoHotkey Web - fetch the Microsoft.Web.WebView2 SDK (headers + static lib)
#  into third_party\webview2. Run once before building the engine.
# ============================================================================
param([string]$Version = "1.0.2792.45")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$tp   = Join-Path $root "third_party"
New-Item -ItemType Directory -Force $tp | Out-Null

$zip = Join-Path $tp "webview2.nupkg.zip"
$dst = Join-Path $tp "webview2"

if (Test-Path (Join-Path $dst "build\native\include\WebView2.h")) {
    Write-Host "[fetch_webview2] already present at $dst"
    exit 0
}

$url = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$Version"
Write-Host "[fetch_webview2] downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing -TimeoutSec 180

Write-Host "[fetch_webview2] extracting"
Expand-Archive -Path $zip -DestinationPath $dst -Force

if (Test-Path (Join-Path $dst "build\native\include\WebView2.h")) {
    Write-Host "[fetch_webview2] OK: $dst\build\native"
} else {
    Write-Error "[fetch_webview2] extraction did not produce WebView2.h"
    exit 1
}

# styled-qr-c on Windows: build the preview tool, render the examples, check them.
#
#   .\build.ps1
#   .\build.ps1 -Text "WIFI:T:WPA;S:my-ap;P:secret;;" -Verbose2
#
# Exit code is the number of examples that failed the scanner-side check, so it
# is usable as a gate.

param(
    [string]$OutDir = "assets/examples",
    [string]$Text = "https://github.com/MRobertEvers/styled-qr-c",
    [switch]$Verbose2
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

# gcc on PATH, or HOST_CC, or a vendored MinGW. Say which were tried rather
# than failing with "not recognized".
$Candidates = @()
$Cc = Get-Command gcc -ErrorAction SilentlyContinue
if ($Cc) { $Candidates += $Cc.Source }
if ($env:HOST_CC) { $Candidates += $env:HOST_CC }
$Candidates += (Join-Path $Root "..\oldschool-clientc\toolchains\mingw64\bin\gcc.exe")

$CcPath = $Candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $CcPath) {
    throw ("no C compiler. Put gcc on PATH or set HOST_CC. Tried:`n  " +
           ($Candidates -join "`n  "))
}

New-Item -ItemType Directory -Force -Path (Join-Path $Root "build") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Root $OutDir) | Out-Null
$Exe = Join-Path $Root "build\preview.exe"

& $CcPath -O2 -std=c99 -Wall -Wextra `
    -I (Join-Path $Root "src") `
    -I (Join-Path $Root "third_party\qrcodegen") `
    -I (Join-Path $Root "examples") `
    -o $Exe `
    (Join-Path $Root "third_party\qrcodegen\qrcodegen.c") `
    (Join-Path $Root "src\qrstyle.c") `
    (Join-Path $Root "examples\preview.c")
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

if ($Verbose2) { & $Exe (Join-Path $Root $OutDir) $Text -v }
else { & $Exe (Join-Path $Root $OutDir) $Text }
exit $LASTEXITCODE

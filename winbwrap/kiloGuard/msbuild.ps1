param(
    [switch]$d,
    [switch]$r
)

$cfg = if ($d) { "Debug" } else { "Release" }
$proj = "$PSScriptRoot\KiloGuard.vcxproj"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath 2>&1
if (-not $vs) { throw "Visual Studio not found" }

$msbuild = "$vs\MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) { throw "MSBuild.exe not found at $msbuild" }

Write-Host "=== Building $cfg (x64) ==="
& $msbuild $proj /p:Configuration=$cfg /p:Platform=x64 /t:Build /p:WarningLevel=0 /p:WarningsNotAsErrors=4266 /p:SkipPackageVerification=true 2>&1
if ($LASTEXITCODE -ne 0) { throw "BUILD FAILED" }

$out = "$PSScriptRoot\x64\KiloGuard.sys"
if (Test-Path $out) {
    $f = Get-Item $out
    Write-Host "=== Build OK: $out ($($f.Length) bytes) ==="
} else {
    throw "Output not found: $out"
}
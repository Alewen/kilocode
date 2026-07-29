param(
    [switch]$Clean
)

Set-Location $PSScriptRoot

if ($Clean) {
    Write-Host "Cleaning build artifacts..."
    $artifacts = @("UseCom.exe", "UseCom.obj", "UseCom.pdb", "UseCom.ilk", "vc140.pdb")
    $removed = 0
    foreach ($f in $artifacts) {
        if (Test-Path $f) {
            Remove-Item $f -Force
            Write-Host "  Removed $f"
            $removed++
        }
    }
    if ($removed -eq 0) {
        Write-Host "  Nothing to clean."
    } else {
        Write-Host "Clean complete: removed $removed file(s)."
    }
    exit 0
}

$vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$sdkVer = "10.0.26100.0"
$sdkPath = "C:\Program Files (x86)\Windows Kits\10"

$vcTools = Get-ChildItem "$vsPath\VC\Tools\MSVC" | Sort-Object Name -Descending | Select-Object -First 1
if (-not $vcTools) {
    Write-Error "VC Tools not found under $vsPath\VC\Tools\MSVC"
    exit 1
}
$vcDir = $vcTools.FullName
Write-Host "VC Tools: $vcDir"

$clPath = "$vcDir\bin\Hostx64\x64\cl.exe"
if (-not (Test-Path $clPath)) {
    Write-Error "cl.exe not found at $clPath"
    exit 1
}

$inc = @(
    "$vcDir\include",
    "$sdkPath\Include\$sdkVer\ucrt",
    "$sdkPath\Include\$sdkVer\um",
    "$sdkPath\Include\$sdkVer\shared"
) -join ";"

$lib = @(
    "$vcDir\lib\x64",
    "$sdkPath\Lib\$sdkVer\ucrt\x64",
    "$sdkPath\Lib\$sdkVer\um\x64"
) -join ";"

$env:PATH = "$vcDir\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE = $inc
$env:LIB = $lib

Write-Host "Compiling UseCom.cpp..."
& cl.exe /nologo /EHsc /W3 /O2 /D_UNICODE /DUNICODE UseCom.cpp `
    /link ole32.lib oleaut32.lib advapi32.lib shell32.lib wbemuuid.lib taskschd.lib

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nBuild succeeded: UseCom.exe"
} else {
    Write-Error "Build failed (exit code $LASTEXITCODE)"
    exit $LASTEXITCODE
}

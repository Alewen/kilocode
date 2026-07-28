
$Src = $PSScriptRoot
$Platform = "x64"

$TargetDir = Join-Path $Src "..\..\packages\opencode\bin\winbwrap"
if (Test-Path -LiteralPath $TargetDir) {
    Copy-Item -LiteralPath "$Src\$Platform\bwrap.exe" -Destination (Join-Path $TargetDir "bwrap.exe") -Force
    Copy-Item -LiteralPath "$Src\$Platform\KiloHook.dll" -Destination (Join-Path $TargetDir "KiloHook.dll") -Force
    Write-Host "Copied to $TargetDir" -ForegroundColor Cyan
}


$Src = $PSScriptRoot
$Platform = "x64"

$extensionBin = Join-Path $env:USERPROFILE ".vscode\extensions\kilocode.kilo-code-7.4.5\bin"
if (Test-Path -LiteralPath $extensionBin) {
    Copy-Item -LiteralPath "$Src\$Platform\bwrap.exe" -Destination (Join-Path $extensionBin "bwrap.exe") -Force
    Copy-Item -LiteralPath "$Src\$Platform\KiloHook.dll" -Destination (Join-Path $extensionBin "KiloHook.dll") -Force
    Write-Host "Copied to $extensionBin" -ForegroundColor Cyan
}

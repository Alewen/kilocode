param(
    [string]$Src = $PSScriptRoot,
    [string]$Config = 'Release',
    [switch]$Clean
)

# Normalize shorthand: r -> Release, d -> Debug
$ConfigMap = @{ 'r' = 'Release'; 'd' = 'Debug' }
if ($ConfigMap.ContainsKey($Config.ToLower())) { $Config = $ConfigMap[$Config.ToLower()] }
if ($Config -notin @('Debug','Release')) { throw "Invalid -Config: use Debug/Release or d/r" }

Write-Host @"

How to build this project:

  .\build.ps1                        # Build Release x64 (default)
  .\build.ps1 -Clean                 # Clean Release x64 (default)
  .\build.ps1 -Config Debug          # Build Debug x64
  .\build.ps1 -Config Debug -Clean   # Clean Debug x64
  .\build.ps1 -Co d                  # Build Debug x64 (shorthand)
  .\build.ps1 -Co r                  # Build Release x64 (shorthand)

"@

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: $vswhere" }
$VS = & $vswhere -latest -products * -property installationPath 2>&1
if (-not $VS) { throw "No Visual Studio installation found by vswhere" }
$MSBuild = "$VS\MSBuild\Current\Bin\amd64\MSBuild.exe"
if (-not (Test-Path $MSBuild)) { $MSBuild = "$VS\MSBuild\Current\Bin\MSBuild.exe" }
if (-not (Test-Path $MSBuild)) { throw "MSBuild.exe not found: $MSBuild" }
Write-Host "VS       = $VS"
Write-Host "MSBuild  = $MSBuild"
Write-Host ""
$Project = "$Src\bwrap.vcxproj"
$HookProject = "$Src\KiloHook.vcxproj"
$Platform = "x64"

if ($Clean) {
    Write-Host "==================== Cleaning bwrap.exe ($Config) ====================" -ForegroundColor Yellow
    & $MSBuild $Project /p:Configuration=$Config /p:Platform=$Platform /t:Clean /nologo
    & $MSBuild $HookProject /p:Configuration=$Config /p:Platform=$Platform /t:Clean /nologo

    $OutDir = "$Src\$Platform"
    if (Test-Path -LiteralPath $OutDir) {
        Remove-Item -LiteralPath $OutDir -Recurse -Force
        Write-Host "Removed $OutDir"
    }

    Write-Host "==================== Clean successful ====================" -ForegroundColor Green
    return
}

Write-Host "==================== Building KiloHook.dll ($Config) ====================" -ForegroundColor Yellow
& $MSBuild $HookProject /p:Configuration=$Config /p:Platform=$Platform /t:Rebuild /nologo
if ($LASTEXITCODE -ne 0) { throw "KiloHook BUILD FAILED" }
Get-Item "$Src\$Platform\KiloHook.dll" | Select-Object LastWriteTime, Length, FullName

Write-Host "==================== Building bwrap.exe ($Config) ====================" -ForegroundColor Yellow
& $MSBuild $Project /p:Configuration=$Config /p:Platform=$Platform /t:Rebuild /nologo
if ($LASTEXITCODE -ne 0) { throw "bwrap BUILD FAILED" }
Get-Item "$Src\$Platform\bwrap.exe" | Select-Object LastWriteTime, Length, FullName

Write-Host "==================== Build successful ====================" -ForegroundColor Green

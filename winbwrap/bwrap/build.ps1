param(
    [string]$Src = $PSScriptRoot,
    [string]$Config = 'Release',
    [string]$Arch = 'x64',
    [switch]$Clean
)

# Normalize shorthand: r→Release, d→Debug
$ConfigMap = @{ 'r' = 'Release'; 'd' = 'Debug' }
if ($ConfigMap.ContainsKey($Config.ToLower())) { $Config = $ConfigMap[$Config.ToLower()] }
if ($Config -notin @('Debug','Release')) { throw "Invalid -Config: use Debug/Release or d/r" }

# Normalize shorthand: w→Win32, x→x64
$ArchMap = @{ 'w' = 'Win32'; 'x' = 'x64' }
if ($ArchMap.ContainsKey($Arch.ToLower())) { $Arch = $ArchMap[$Arch.ToLower()] }
if ($Arch -notin @('Win32','x64')) { throw "Invalid -Arch: use Win32/x64 or w/x" }

Write-Host @"

How to build this project:

  .\build.ps1                                    # Build Release x64 (default)
  .\build.ps1 -Clean                             # Clean Release x64 (default)
  .\build.ps1 -Config Debug -Arch Win32          # Build Debug Win32
  .\build.ps1 -Config Debug -Arch Win32 -Clean   # Clean Debug Win32
  .\build.ps1 -C d -A w                          # Build Debug Win32 (shorthand)
  .\build.ps1 -C r -A x                          # Build Release x64 (shorthand)

"@

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: $vswhere" }
$VS = & $vswhere -latest -products * -property installationPath 2>&1
if (-not $VS) { throw "No Visual Studio installation found by vswhere" }
$MSBuild = "$VS\MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $MSBuild)) { throw "MSBuild.exe not found: $MSBuild" }
Write-Host "VS       = $VS"
Write-Host "MSBuild  = $MSBuild"
Write-Host ""
$Project = "$Src\bwrap.vcxproj"
$HookProject = "$Src\KiloHook.vcxproj"
$Platform = $Arch

if ($Clean) {
    Write-Host "==================== Cleaning bwrap.exe ($Arch $Config) ====================" -ForegroundColor Yellow
    & $MSBuild $Project /p:Configuration=$Config /p:Platform=$Platform /t:Clean /nologo
    & $MSBuild $HookProject /p:Configuration=$Config /p:Platform=$Platform /t:Clean /nologo

    $OutDir = "$Src\$Platform\$Config"
    if (Test-Path -LiteralPath $OutDir) {
        Remove-Item -LiteralPath $OutDir -Recurse -Force
        Write-Host "Removed $OutDir"
    }

    # If the other config folder under the same platform does not exist, remove the empty platform folder
    $OtherConfig = if ($Config -eq 'Release') { 'Debug' } else { 'Release' }
    $OtherDir = "$Src\$Platform\$OtherConfig"
    if (-not (Test-Path -LiteralPath $OtherDir)) {
        $PlatformDir = "$Src\$Platform"
        Remove-Item -LiteralPath $PlatformDir -Recurse -Force
        Write-Host "Removed empty $PlatformDir"
    }

    Write-Host "==================== Clean successful ====================" -ForegroundColor Green
    return
}

Write-Host "==================== Building KiloHook.dll ($Arch $Config) ====================" -ForegroundColor Yellow
& $MSBuild $HookProject /p:Configuration=$Config /p:Platform=$Platform /t:Rebuild /nologo
if ($LASTEXITCODE -ne 0) { throw "KiloHook BUILD FAILED" }
Get-Item "$Src\$Platform\$Config\KiloHook.dll" | Select-Object LastWriteTime, Length, FullName

Write-Host "==================== Building bwrap.exe ($Arch $Config) ====================" -ForegroundColor Yellow
& $MSBuild $Project /p:Configuration=$Config /p:Platform=$Platform /t:Rebuild /nologo
if ($LASTEXITCODE -ne 0) { throw "bwrap BUILD FAILED" }
Get-Item "$Src\$Platform\$Config\bwrap.exe" | Select-Object LastWriteTime, Length, FullName

$TargetDir = Join-Path $Src "..\..\packages\opencode\bin\winbwrap"
if (Test-Path -LiteralPath $TargetDir) {
    Copy-Item -LiteralPath "$Src\$Platform\$Config\bwrap.exe" -Destination (Join-Path $TargetDir "bwrap.exe") -Force
    Copy-Item -LiteralPath "$Src\$Platform\$Config\KiloHook.dll" -Destination (Join-Path $TargetDir "KiloHook.dll") -Force
    Write-Host "Copied to $TargetDir" -ForegroundColor Cyan
}

Write-Host "==================== Build successful ====================" -ForegroundColor Green

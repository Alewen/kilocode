param(
    [string]$NewVersion
)

$ScriptDir = Split-Path -Parent -Path $MyInvocation.MyCommand.Path

$VsWhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath

if (-not $VsPath) {
    Write-Host "未检测到 Visual Studio (含 C++ 工具链)" -ForegroundColor Red
    exit 1
}

$VsVer = & $VsWhere -latest -products * -property catalog_productLineVersion
$VsDisplay = switch ($VsVer) {
    17 { "2022" }
    16 { "2019" }
    15 { "2017" }
    default { $VsVer }
}

Write-Host "检测到 VS $VsDisplay : $VsPath" -ForegroundColor Green

$DevShellModule = Join-Path $VsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Test-Path $DevShellModule)) {
    Write-Host "未找到 DevShell 模块: $DevShellModule" -ForegroundColor Red
    exit 1
}

$InstanceId = & $VsWhere -latest -products * -property instanceId

$PwshPath = (Get-Command pwsh -ErrorAction SilentlyContinue).Source
if (-not $PwshPath) {
    Write-Host "未检测到 PowerShell 7 (pwsh)，请先安装" -ForegroundColor Red
    exit 1
}

$CompileScript = Join-Path $ScriptDir "kilo-compile-windows-x64-vsix.ps1"
$VersionArg = if ($NewVersion) { " '$NewVersion'" } else { "" }

Write-Host "正在启动 VS $VsDisplay 编译环境 (PowerShell 7)..." -ForegroundColor Cyan

$PwshArgs = @(
    "-NoExit"
    "-Command"
    "Import-Module '$DevShellModule'; Enter-VsDevShell $InstanceId; & '$CompileScript'$VersionArg"
)

Start-Process -FilePath $PwshPath -ArgumentList $PwshArgs
Stop-Process -Id $PID

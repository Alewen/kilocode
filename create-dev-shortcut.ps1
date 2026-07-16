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

$WshShell = New-Object -ComObject WScript.Shell
$ShortcutPath = Join-Path (Get-Location) "VS Dev PS.lnk"
$Shortcut = $WshShell.CreateShortcut($ShortcutPath)
$Shortcut.TargetPath = $PwshPath
$Shortcut.Arguments = "-NoExit -Command `"Import-Module '$DevShellModule'; Enter-VsDevShell $InstanceId`""
$Shortcut.WorkingDirectory = (Get-Location).Path
$Shortcut.Description = "VS $VsDisplay Developer PowerShell (PowerShell 7)"
$Shortcut.Save()

Write-Host "快捷方式已创建: $ShortcutPath" -ForegroundColor Green

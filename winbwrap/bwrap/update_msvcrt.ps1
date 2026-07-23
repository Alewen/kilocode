#Requires -RunAsAdministrator

$src = "E:\Microsoft Visual Studio\2022\Professional\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT"
$sys = "C:\Windows\System32"

$dlls = @("msvcp140.dll", "vcruntime140.dll", "concrt140.dll")

foreach ($d in $dlls)
{
    $old = "$sys\$d"
    $bak = "$sys\$d.bak"
    $new = "$src\$d"

    if ((Get-Item $old).VersionInfo.FileVersion -eq (Get-Item $new).VersionInfo.FileVersion)
    {
        Write-Host "$d 已经是正确版本，跳过" -ForegroundColor Green
        continue
    }

    Rename-Item -LiteralPath $old -NewName "$d.bak" -Force
    Write-Host "已备份 $old -> $bak" -ForegroundColor Yellow

    Copy-Item -LiteralPath $new -Destination $old -Force
    Write-Host "已替换 $old <- $new" -ForegroundColor Green
}

Write-Host "`n替换完成，当前版本：" -ForegroundColor Cyan
Get-ChildItem $sys\msvcp140.dll, $sys\vcruntime140.dll, $sys\concrt140.dll |
    ForEach-Object { "$($_.Name) = $($_.VersionInfo.FileVersion)" }
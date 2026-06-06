<#
.SYNOPSIS
win-hex-file-viewer.ps1 - 以十六进制格式查看文件内容的 PowerShell 脚本

.DESCRIPTION
该脚本读取指定文件的二进制内容，并以十六进制格式显示，
同时显示偏移地址，方便用户查看和分析文件的原始二进制数据。

.PARAMETER File
要查看的文件路径。必填参数，不能包含通配符。

.PARAMETER BytesPerLine
每行显示的字节数。可选参数，范围为 10-50 之间的整数，默认值为 20。

.EXAMPLE
.\win-hex-file-viewer.ps1 "C:\test\example.bin"
使用默认设置（每行 20 字节）查看 example.bin 文件

.EXAMPLE
.\win-hex-file-viewer.ps1 "C:\test\example.bin" 32
每行显示 32 字节查看 example.bin 文件

.NOTES
作者：Kilo
日期：2026-05-27
#>

param(
    [Parameter(Mandatory=$false, Position=0, HelpMessage="文件路径")]
    [string]$File,

    [Parameter(Mandatory=$false, Position=1, HelpMessage="每行显示字节数")]
    [int]$BytesPerLine = 20
)

# 检查是否包含通配符
if ([System.Management.Automation.WildcardPattern]::ContainsWildcardCharacters($File)) {
    Write-Host "错误：参数一 [ $File ] 不能包含通配符" -ForegroundColor Red
    return
}

# 参数检查
if ([string]::IsNullOrWhiteSpace($File)) {
    Write-Host "错误：参数一 [ $File ] 是无效的空值或空格" -ForegroundColor Red
    Write-Host "用法: win-hex-file-viewer <文件> [每行字节数]"
    Write-Host "       每行字节数: 10-50 之间的整数，默认 20"
    return
}

# 文件检查
if (-not (Test-Path -LiteralPath $File)) {
    Write-Host "错误: 文件 [ $File ] 不存在" -ForegroundColor Red
    return
}

$File = Resolve-Path $File

# 验证每行字节数参数
if ($BytesPerLine -lt 10 -or $BytesPerLine -gt 50) {
    Write-Host "错误: 每行字节数必须在 10-50 之间"
    return
}

# 获取文件大小
$fileSize = (Get-Item $File).Length

# 读取文件二进制数据
$bytes = [System.IO.File]::ReadAllBytes($File)
$offset = 0

# 处理并显示十六进制数据
while ($offset -lt $bytes.Length)
{
    $remainingBytes = $bytes.Length - $offset
    $bytesToShow = [Math]::Min($BytesPerLine, $remainingBytes)
    $lineBytes = $bytes[$offset..($offset + $bytesToShow - 1)]
    $hexStr = ""
    foreach ($b in $lineBytes)
    {
        $hexStr += "{0:X2} " -f $b
    }
    Write-Host ("{0:X8}: {1}" -f $offset, $hexStr.Trim())
    $offset += $BytesPerLine
}

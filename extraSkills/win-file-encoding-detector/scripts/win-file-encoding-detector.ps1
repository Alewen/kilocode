<#
.SYNOPSIS
win-file-encoding-detector.ps1 - 文件编码检测工具（独立版本）

.DESCRIPTION
检测文本文件的编码格式和行尾标志，支持 UTF-8、UTF-16、UTF-32、ASCII、GB2312、GBK、GB18030 等多种编码格式
通过 BOM 检测、编码验证等方式精准识别文件编码，同时可检测行尾格式（LF/CRLF/Mixed）

.PARAMETER FilePath
要检测的文件完整路径（必需参数），不能包含通配符

.PARAMETER CheckLineEnding
是否检测并返回行尾格式（可选开关），默认不检测

.EXAMPLE
.\win-file-encoding-detector.ps1 -FilePath "C:\test\example.txt"
检测文件编码格式，返回 JSON 结果

.EXAMPLE
.\win-file-encoding-detector.ps1 -FilePath "C:\test\example.txt" -CheckLineEnding
检测文件编码格式和行尾格式，返回 JSON 结果

.NOTES
支持的编码格式：
- UTF-8 编码（带 BOM: UTF-8-BOM / 不带 BOM: UTF-8）
- UTF-16 LE（带 BOM: UTF-16LE-BOM / 不带 BOM: UTF-16LE）
- UTF-16 BE（带 BOM: UTF-16BE-BOM / 不带 BOM: UTF-16BE）
- UTF-32 LE（带 BOM: UTF-32LE-BOM / 不带 BOM: UTF-32LE）
- UTF-32 BE（带 BOM: UTF-32BE-BOM / 不带 BOM: UTF-32BE）
- ASCII 编码
- GB 系列编码（GB2312/GBK/GB18030）

文件大小限制：5 字节 - 10 MB

输出格式：JSON 结构化数据
#>

# ================================================
# FileSize-Format
# 将文件大小(以字节为单位)转化为以 K/M 为单位的字符串表示
# 参数：
#   $sizeInBytes - [long] 文件大小（字节数）
# 返回值：
#   [string] 格式化后的文件大小，如 "1.23 K"、"4.56 M"、"789 B"
# ================================================
function FileSize-Format
{
    param([long]$sizeInBytes)

    if ($sizeInBytes -ge 1MB)
    {
        return "{0:N2} M" -f ($sizeInBytes / 1MB)
    }
    elseif ($sizeInBytes -ge 1KB)
    {
        return "{0:N2} K" -f ($sizeInBytes / 1KB)
    }
    else
    {
        return "$sizeInBytes B"
    }
}

# ================================================
# Detect-BOM
# 检测文件的 BOM（字节顺序标记）类型
# 参数：
#   $FilePath - [string] 要检测的文件完整路径
# 返回值：
#   [string] 可能值：
#     "UTF-8-BOM" - UTF-8 编码带 BOM
#     "UTF-16LE-BOM" - UTF-16 小端序带 BOM
#     "UTF-16BE-BOM" - UTF-16 大端序带 BOM
#     "UTF-32LE-BOM" - UTF-32 小端序带 BOM
#     "UTF-32BE-BOM" - UTF-32 大端序带 BOM
#     "NO_BOM" - 没有检测到 BOM
#     "UNKNOWN" - 检测过程出错
# ================================================
function Detect-BOM
{
    param([string]$FilePath)

    try
    {
        $stream = [System.IO.File]::OpenRead($FilePath)
        $bytes = New-Object byte[] 4
        $bytesRead = $stream.Read($bytes, 0, 4)
        $stream.Close()

        if ($bytesRead -ge 3)
        {
            # UTF-8 with BOM: EF BB BF
            if ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
            {
                return "UTF-8-BOM"
            }
        }

        if ($bytesRead -ge 2)
        {
            # UTF-16 LE with BOM: FF FE
            if ($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE)
            {
                return "UTF-16LE-BOM"
            }
            # UTF-16 BE with BOM: FE FF
            if ($bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF)
            {
                return "UTF-16BE-BOM"
            }
        }

        if ($bytesRead -ge 4)
        {
            # UTF-32 LE with BOM: FF FE 00 00
            if ($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE -and $bytes[2] -eq 0x00 -and $bytes[3] -eq 0x00) {
                return "UTF-32LE-BOM"
            }
            # UTF-32 BE with BOM: 00 00 FE FF
            if ($bytes[0] -eq 0x00 -and $bytes[1] -eq 0x00 -and $bytes[2] -eq 0xFE -and $bytes[3] -eq 0xFF) {
                return "UTF-32BE-BOM"
            }
        }
        return "NO_BOM"
    }
    catch
    {
        return "UNKNOWN"
    }
}

# ================================================
# Is-ASCII-File
# 检测文件是否为 ASCII 编码
# 参数：
#   $FilePath - [string] 要检测的文件完整路径
# 返回值：
#   [bool] $true - 文件符合 ASCII 编码规范
#          $false - 不符合或检测出错
# ================================================
function Is-ASCII-File
{
    param([string]$FilePath)

    try
    {
        # 读取文件字节
        $bytes = [System.IO.File]::ReadAllBytes($FilePath)

        # 检查是否有高位字节 (0x80-0xFF)
        foreach ($byte in $bytes)
        {
            if ($byte -ge 0x80)
            {
                return $false
            }
        }

        # 检查禁止的控制字符
        # 允许: 0x09(tab), 0x0A(LF), 0x0D(CR), 0x20(space)
        # 禁止: 0x00-0x08, 0x0B-0x0C, 0x0E-0x1F, 0x7F
        $forbiddenRanges = @(
            @{Start=0x00; End=0x08},
            @{Start=0x0B; End=0x0C},
            @{Start=0x0E; End=0x1F},
            @{Start=0x7F; End=0x7F}
        )

        foreach ($byte in $bytes)
        {
            foreach ($range in $forbiddenRanges)
            {
                if ($byte -ge $range.Start -and $byte -le $range.End)
                {
                    return $false
                }
            }
        }
        
        return $true
    }
    catch
    {
        return $false
    }
}

# ================================================
# Is-UTF8-WithoutBom-File
# 检测文件是否为 UTF-8 编码（无 BOM）
# 参数：
#   $FilePath - [string] 要检测的文件完整路径
# 返回值：
#   [bool] $true - 文件符合 UTF-8（无 BOM）编码规范
#          $false - 不符合或检测出错
# ================================================
function Is-UTF8-WithoutBom-File
{
    param([string]$FilePath)

    try
    {
        # 读取原始文件字节
        $bytes = [System.IO.File]::ReadAllBytes($FilePath)
        # 使用UTF-8无BOM编码
        $utf8NoBOM = [System.Text.UTF8Encoding]::new($false)

        # 尝试解码
        $content = $utf8NoBOM.GetString($bytes)

        # 检查是否有替换字符 (U+FFFD) - 真正的解码错误
        $replacementChar = [char]0xFFFD
        if ($content.Contains($replacementChar))
        {
            return $false
        }

        # 重新编码
        $reencodedBytes = $utf8NoBOM.GetBytes($content)

        # 比较字节长度
        if ($bytes.Length -ne $reencodedBytes.Length)
        {
            return $false
        }

        # 逐个比较字节
        for ($i = 0; $i -lt $bytes.Length; $i++)
        {
            if ($bytes[$i] -ne $reencodedBytes[$i])
            {
                return $false
            }
        }

        return $true
    }
    catch # [System.Text.DecoderFallbackException]
    {
        # UTF-8解码失败
        return $false
    }
}

# ================================================
# Is-GB2312-Bytes
# 严格检测字节数组是否符合 GB2312 编码规范（不含备用区）
# 参数：
#   $bytes - [byte[]] 要检测的字节数组
# 返回值：
#   [bool] $true - 字节符合 GB2312 规范
#          $false - 不符合或包含 BOM
# ================================================
function Is-GB2312-Bytes
{
    param(
        [byte[]]$bytes
    )

    $len = $bytes.Length
    # 空文件视为合法 GB2312
    if ($len -eq 0) {
        return $true
    }

    # --- 1. 检测常见 BOM（纯 GB2312 不应有任何 BOM）---
    if ($len -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        # UTF-8 BOM
        return $false
    }

    if ($len -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        # UTF-16 LE BOM
        return $false
    }
    if ($len -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) {
        # UTF-16 BE BOM (FE FF)，第一个字节 0xFE 会在后面被判非法，这里提前拦截
        return $false
    }

    # --- 2. 遍历所有字节进行严格 GB2312 解码 ---
    $i = 0
    while ($i -lt $len) {
        $b = $bytes[$i]

        if ($b -le 0x7F) {
            # 单字节 ASCII（含控制字符）
            $i++
        }
        elseif (($b -ge 0xA1 -and $b -le 0xA9) -or ($b -ge 0xB0 -and $b -le 0xF7)) {
            # 双字节字符的第一字节（1-9 区 或 16-87 区）
            if ($i + 1 -ge $len) {
                # 文件末尾截断，缺少第二字节
                return $false
            }
            $next = $bytes[$i + 1]
            if ($next -lt 0xA1 -or $next -gt 0xFE) {
                # 第二字节必须在 0xA1-0xFE 之间
                return $false
            }
            $i += 2
        }
        else {
            # 非法字节：0x80-0xA0, 0xAA-0xAF（备用区 10-15 区）, 0xF8-0xFF（备用区 88-94 区及其它）
            return $false
        }
    }

    # 全部通过检测
    return $true
}

# ================================================
# Is-GB-File
# 检测文件是否为 GB 系列编码（GB2312/GBK/GB18030）
# 参数：
#   $FilePath - [string] 要检测的文件完整路径
# 返回值：
#   [string] 检测到的编码名称："GB2312" / "GBK" / "GB18030"
#   [null] 未检测到符合的 GB 编码或出错
# ================================================
function Is-GB-File
{
    param([string]$FilePath)

    $encodings = @(
        @{Name="GB2312"; Encoding=[System.Text.Encoding]::GetEncoding("GB2312")},
        @{Name="GBK"; Encoding=[System.Text.Encoding]::GetEncoding("GBK")},
        @{Name="GB18030"; Encoding=[System.Text.Encoding]::GetEncoding("GB18030")}
    )

    try
    {
        $bytes = [System.IO.File]::ReadAllBytes($FilePath)

        foreach ($enc in $encodings)
        {
            try
            {
                # 使用检测到的编码读取
                $text = $enc.Encoding.GetString($bytes)

                # 检查是否有替换字符
                if ($text.Contains([char]0xFFFD))
                {
                    continue
                }

                # 重新编码验证
                $reencodedBytes = $enc.Encoding.GetBytes($text)

                # 比较字节序列
                if ($bytes.Length -ne $reencodedBytes.Length)
                {
                    continue
                }

                $match = $true
                for ($i = 0; $i -lt $bytes.Length; $i++)
                {
                    if ($bytes[$i] -ne $reencodedBytes[$i])
                    {
                        $match = $false
                        break
                    }
                }

                if ($match)
                {
                    # 如果是 GB2312，额外检查字节是否是真正的 GB2312（而不是 GBK 扩展）
                    if ($enc.Name -eq "GB2312" -and -not (Is-GB2312-Bytes $bytes))
                    {
                        continue  # 继续检测下一个编码
                    }
                    return $enc.Name
                }
            }
            catch
            {
                # 编码失败，尝试下一个
                continue
            }
        }

        return $null
    }
    catch
    {
        return $null
    }
}

# ================================================
# Detect-LineEnding
# 检测文件的行尾格式
# 参数：
#   $FilePath - [string] 要检测的文件完整路径
# 返回值：
#   [hashtable] 包含：
#     Conclusion - [string] 行尾结论："LF" / "CRLF" / "Mixed" / "NoEOL" / "unknown"
#     TotalLines - [int] 总行数
#     LFLines - [int] LF 结尾的行数
#     CRLFLines - [int] CRLF 结尾的行数
# ================================================
function Detect-LineEnding
{
    param(
        [string]$FilePath
    )

    try
    {
        $bytes = [System.IO.File]::ReadAllBytes($FilePath)

        $conclusion = "NoEOL"
        $totalLines = 0
        $crlfLines = 0
        $lfLines = 0

        $lastLineEndingPosition = 0
        $i = 0

        while ($i -lt $bytes.Length)
        {
            if ($i + 1 -lt $bytes.Length -and $bytes[$i] -eq 0x0D -and $bytes[$i + 1] -eq 0x0A) # CRLF
            {
                $totalLines++
                $crlfLines++
                $lastLineEndingPosition = $i + 1
                $i += 2
            }
            elseif ($bytes[$i] -eq 0x0A) # LF
            {
                $totalLines++
                $lfLines++
                $lastLineEndingPosition = $i
                $i++
            }
            else
            {
                $i++
            }
        }

        if ($lastLineEndingPosition -ne $bytes.Length - 1)
        {
            $totalLines++
        }

        if ($totalLines -eq 0)
        {
            $conclusion = "NoEOL"
        }
        elseif ($totalLines -eq 1)
        {
            if ($lfLines -eq 0 -and $crlfLines -eq 0)
            {
                $conclusion = "NoEOL"
            }
            elseif ($lfLines -eq 0)
            {
                $conclusion = "CRLF"
            }
            elseif ($crlfLines -eq 0)
            {
                $conclusion = "LF"
            }
        }
        else
        {
            if ($lfLines -eq 0)
            {
                $conclusion = "CRLF"
            }
            elseif ($crlfLines -eq 0)
            {
                $conclusion = "LF"
            }
            else
            {
                $conclusion = "Mixed"
            }
        }

        # 返回结果对象
        return @{
            Conclusion = $conclusion
            TotalLines = $totalLines
            LFLines = $lfLines
            CRLFLines = $crlfLines
        }
    }
    catch
    {
        return @{
            Conclusion = "unknown"
            TotalLines = 0
            LFLines = 0
            CRLFLines = 0
        }
    }
}



# ================================================
# Check-FileFormat
# 主函数：检测文件编码格式和行尾标志
# 参数：
#   $FilePath - [string] 必需，要检测的文件完整路径
#   $CheckLineEnding - [switch] 可选，是否检测并返回行尾格式
# 返回值：
#   [hashtable] 结构化对象，包含：
#     Success - [bool] 是否检测成功
#     FilePath - [string] 检测的文件完整路径
#     FileSize - [string] 人类可读的文件大小（带单位）
#     FileSizeBytes - [long] 文件大小（字节数）
#     Encoding - [string] 检测到的文件编码
#     LineEnding - [string] 行尾格式（"skip" 表示未检测）
#     ErrorMessage - [string] 错误信息（成功为空字符串）
# ================================================
function Check-FileFormat
{
    param(
        [Parameter(Mandatory=$true, HelpMessage="要检测的文件路径")]
        [string]$FilePath,

        [Parameter(HelpMessage="是否统计行尾标志")]
        [switch]$CheckLineEnding
    )

    # 初始化结果对象
    $result = @{
        Success = $false
        FilePath = ""
        FileSize = ""
        FileSizeBytes = 0
        Encoding = ""
        LineEnding = "skip"
        ErrorMessage = ""
    }

    # 检查是否包含通配符
    if ([System.Management.Automation.WildcardPattern]::ContainsWildcardCharacters($FilePath)) {
        $result.ErrorMessage = "路径不能包含通配符"
        return $result
    }

    if ([string]::IsNullOrWhiteSpace($FilePath)) {
        $result.ErrorMessage = "文件路径不能为空"
        return $result
    }

    # 检查是否为普通文件
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf))
    {
        $result.ErrorMessage = "不是有效文件或文件不存在"
        return $result
    }

    $FilePath = Resolve-Path $FilePath
    $result.FilePath = $FilePath

    # 检查文件是否可读并获取大小
    try
    {
        $fileItem = Get-Item $FilePath -Force
        $fileSize = $fileItem.Length
        $result.FileSizeBytes = $fileSize
        $result.FileSize = FileSize-Format $fileSize
    }
    catch
    {
        $result.ErrorMessage = "文件不可读"
        return $result
    }

    # 检查文件大小限制
    $MIN_FILE_SIZE_LIMITED = 5
    $MAX_FILE_SIZE_LIMITED = 10MB
    if ($fileSize -lt $MIN_FILE_SIZE_LIMITED)
    {
        $result.ErrorMessage = "文件太小"
        return $result
    }

    if ($fileSize -gt $MAX_FILE_SIZE_LIMITED)
    {
        $result.ErrorMessage = "文件太大"
        return $result
    }

    # 检查BOM
    $bomResult = Detect-BOM $FilePath
    if ($bomResult -ne "NO_BOM" -and $bomResult -ne "UNKNOWN")
    {
        $result.Encoding = $bomResult
        if ($CheckLineEnding)
        {
            $leInfo = Detect-LineEnding $FilePath
            $result.LineEnding = $leInfo.Conclusion
        }
        $result.Success = $true
        return $result
    }

    # 检查ASCII
    if (Is-ASCII-File $FilePath)
    {
        $result.Encoding = "ASCII"
        if ($CheckLineEnding)
        {
            $leInfo = Detect-LineEnding $FilePath
            $result.LineEnding = $leInfo.Conclusion
        }
        $result.Success = $true
        return $result
    }

    # 检查UTF-8 (无BOM)
    if (Is-UTF8-WithoutBom-File $FilePath)
    {
        $result.Encoding = "UTF-8"
        if ($CheckLineEnding)
        {
            $leInfo = Detect-LineEnding $FilePath
            $result.LineEnding = $leInfo.Conclusion
        }
        $result.Success = $true
        return $result
    }

    # 检查GB系列编码
    $gbResult = Is-GB-File $FilePath
    if ($null -ne $gbResult)
    {
        $result.Encoding = $gbResult
        if ($CheckLineEnding)
        {
            $leInfo = Detect-LineEnding $FilePath
            $result.LineEnding = $leInfo.Conclusion
        }
        $result.Success = $true
        return $result
    }

    # 未知编码
    $result.Encoding = "----"
    if ($CheckLineEnding)
    {
        $leInfo = Detect-LineEnding $FilePath
        $result.LineEnding = $leInfo.Conclusion
    }
    $result.Success = $true
    return $result
}

# ================================================
# 脚本入口点
# ================================================
if ($MyInvocation.InvocationName -ne '.')
{
    $result = Check-FileFormat @args
    $result | ConvertTo-Json -Depth 10
}

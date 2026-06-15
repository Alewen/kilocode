<#
.SYNOPSIS
win-file-encoding-converter.ps1 - 文件字符编码转换工具

.DESCRIPTION
将文本文件从一种编码格式转换为另一种编码格式，支持 UTF-8、UTF-16、UTF-32、ASCII、GB2312、GBK 等多种编码
具备源编码验证、转换结果验证和原子操作保护机制，确保文件安全转换

.PARAMETER FilePath
要转换的文件完整路径（必需参数，位置0），不能包含通配符

.PARAMETER SourceEncoding
源文件的编码格式（必需参数，位置1）

.PARAMETER TargetEncoding
目标编码格式（必需参数，位置2）

.PARAMETER Quiet
静默模式（可选开关，位置3），不显示输出信息

.PARAMETER LineEnding
行尾符转换（可选参数，位置4），可选值：LF（Unix格式）、CRLF（Windows格式）。指定后，转换编码后将统一替换为指定行尾符

.EXAMPLE
.\win-file-encoding-converter.ps1 "C:\test\example.txt" "GB2312" "UTF-8" "CRLF"
将文件从 GB2312 转换为 UTF-8，并统一替换为 CRLF 行尾符

.EXAMPLE
.\win-file-encoding-converter.ps1 "C:\test\example.txt" "UTF-8" "UTF-8-BOM" -Quiet -LineEnding "LF"
将文件从 UTF-8（不带 BOM）转换为 UTF-8（带 BOM），静默模式，并统一替换为 LF 行尾符

.NOTES
支持的编码格式：
- UTF-8 / UTF8: UTF-8 编码（不带 BOM）
- UTF-8-BOM / UTF8-BOM / UTF8BOM: UTF-8 编码（带 BOM）
- UTF-16LE / UTF16LE: UTF-16 小端序（不带 BOM）
- UTF-16LE-BOM / UTF16LE-BOM / UTF16LEBOM / UNICODE: UTF-16 小端序（带 BOM）
- UTF-16BE / UTF16BE: UTF-16 大端序（不带 BOM）
- UTF-16BE-BOM / UTF16BE-BOM / UTF16BEBOM: UTF-16 大端序（带 BOM）
- UTF-32LE / UTF32LE: UTF-32 小端序（不带 BOM）
- UTF-32LE-BOM / UTF32LE-BOM / UTF32LEBOM: UTF-32 小端序（带 BOM）
- UTF-32BE / UTF32BE: UTF-32 大端序（不带 BOM）
- UTF-32BE-BOM / UTF32BE-BOM / UTF32BEBOM: UTF-32 大端序（带 BOM）
- ASCII: ASCII 编码
- GB2312: GB2312 编码
- GBK: GBK 编码
- DEFAULT: 系统默认编码

转换流程：
1. 验证源编码是否正确（通过临时文件对比）
2. 读取源文件内容
3. 写入临时文件（使用目标编码）
4. 验证转换结果（对比内容）
5. 原子操作替换原文件
6. （可选）转换行尾符为 LF 或 CRLF

输出格式：JSON 结构化数据，包含 Success 和 ErrorMessage 字段
#>
$SCRIPTNAME = $MyInvocation.MyCommand.Name

# 支持的编码格式列表
$SUPPORTED_ENCODINGS = @(
    "UTF-8", "UTF8",
    "UTF-8-BOM", "UTF8-BOM", "UTF8BOM",                     # UTF-8
    "UTF-16LE", "UTF16LE",
    "UTF-16LE-BOM", "UTF16LE-BOM", "UTF16LEBOM", "UNICODE", # UTF-16 LE
    "UTF-16BE", "UTF16BE",
    "UTF-16BE-BOM", "UTF16BE-BOM", "UTF16BEBOM",            # UTF-16 BE
    "ASCII",                                                # 单字节编码
    "GB2312", "GBK",                                        # 中文字符集
    "UTF-32LE", "UTF32LE",
    "UTF-32LE-BOM", "UTF32LE-BOM", "UTF32LEBOM",            # UTF-32 LE
    "UTF-32BE", "UTF32BE",
    "UTF-32BE-BOM", "UTF32BE-BOM", "UTF32BEBOM",            # UTF-32 BE
    "DEFAULT"
)

# =======================================================
# 函数：Is-Supported-Encoding
# 功能：判断指定编码格式是否支持
# 参数：
#   - Encoding: string (必需) - 编码格式名称
# 返回值：结构化对象
#   - IsSupported: bool - true=支持，false=不支持
#   - EncodingCode: int - 不支持时=0，支持时=1-13
# =======================================================
function Is-Supported-Encoding
{
    param(
        [Parameter(Mandatory=$true)]
        [string]$Encoding
    )

    foreach ($enc in $SUPPORTED_ENCODINGS)
    {
        if ($enc.ToUpper() -eq $Encoding.ToUpper())
        {
            switch ($Encoding.ToUpper())
            {
                { $_ -in @("UTF-8", "UTF8") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 1
                    }
                }
                { $_ -in @("UTF-8-BOM", "UTF8-BOM", "UTF8BOM") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 2
                    }
                }
                { $_ -in @("UTF-16LE", "UTF16LE") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 3
                    }
                }
                { $_ -in @("UTF-16LE-BOM", "UTF16LE-BOM", "UTF16LEBOM", "UNICODE") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 4
                    }
                }
                { $_ -in @("UTF-16BE", "UTF16BE") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 5
                    }
                }
                { $_ -in @("UTF-16BE-BOM", "UTF16BE-BOM", "UTF16BEBOM") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 6
                    }
                }
                { $_ -in @("ASCII") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 7
                    }
                }
                { $_ -in @("GB2312", "GBK") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 8
                    }
                }
                { $_ -in @("UTF-32LE", "UTF32LE") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 9
                    }
                }
                { $_ -in @("UTF-32LE-BOM", "UTF32LE-BOM", "UTF32LEBOM") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 10
                    }
                }
                { $_ -in @("UTF-32BE", "UTF32BE") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 11
                    }
                }
                { $_ -in @("UTF-32BE-BOM", "UTF32BE-BOM", "UTF32BEBOM") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 12
                    }
                }
                { $_ -in @("DEFAULT") } { 
                    return [PSCustomObject]@{
                        IsSupported = $true
                        EncodingCode = 13
                    }
                }
            }
        }
    }
    return [PSCustomObject]@{
        IsSupported = $false
        EncodingCode = 0
    }
}

# =======================================================
# 函数：Get-EncodingObject
# 功能：将编码名称转换为 .NET Encoding 对象
# 参数：
#   - EncodingName: string (必需) - 编码格式名称
# 返回值：结构化对象
#   - Success: bool - true=成功，false=失败
#   - Encoding: object - 成功时=.NET Encoding对象，失败时=$null
#   - ErrorMessage: string - 失败时=错误信息，成功时=空字符串
# =======================================================
function Get-EncodingObject
{
    param(
        [Parameter(Mandatory=$true)]
        [string]$EncodingName
    )

    $checkResult = Is-Supported-Encoding $EncodingName
    
    if (-not $checkResult.IsSupported) {
        return [PSCustomObject]@{
            Success = $false
            Encoding = $null
            ErrorMessage = "不支持的编码格式: $EncodingName"
        }
    }
    
    $encodingCode = $checkResult.EncodingCode
    $encodingObj = $null

    switch ($encodingCode)
    {
        "1" { $encodingObj = [System.Text.UTF8Encoding]::new($false) }
        "2" { $encodingObj = [System.Text.UTF8Encoding]::new($true) }

        # UTF-16 LE 不带 BOM
        # 参数一：true 表示大端序 UTF-16 BE，false 表示小端序 UTF-16 LE，参数二：是否带 BOM
        "3" { $encodingObj = [System.Text.UnicodeEncoding]::new($false, $false, $true) }

        # UTF-16 LE 带 BOM
        "4" { $encodingObj = [System.Text.UnicodeEncoding]::new($false, $true, $true) }

        # UTF-16 BE 不带 BOM
        "5" { $encodingObj = [System.Text.UnicodeEncoding]::new($true, $false, $true) }

        # UTF-16 BE 带 BOM
        "6" { $encodingObj = [System.Text.UnicodeEncoding]::new($true, $true, $true) }

        "7" { $encodingObj = [System.Text.Encoding]::ASCII }
        "8" { $encodingObj = [System.Text.Encoding]::GetEncoding(936) }

        # UTF-32 LE without BOM
        "9" { $encodingObj = [System.Text.UTF32Encoding]::new($false, $false, $false) }

        # UTF-32 LE with BOM
        "10" { $encodingObj = [System.Text.UTF32Encoding]::new($false, $true, $true) }

        # UTF-32 BE without BOM
        "11" { $encodingObj = [System.Text.UTF32Encoding]::new($true, $false, $true) }

        # UTF-32 BE with BOM
        "12" { $encodingObj = [System.Text.UTF32Encoding]::new($true, $true, $true) }

        # DEFAULT / 系统默认编码
        "13" { $encodingObj = [System.Text.Encoding]::Default }

        default {
            $encodingObj = [System.Text.Encoding]::Default
        }
    }

    return [PSCustomObject]@{
        Success = $true
        Encoding = $encodingObj
        ErrorMessage = ""
    }
}

# =======================================================
# 函数：Get-ContentWithoutBom
# 功能：读取字节数组，去除可能的 BOM 头，解码为字符串
# 参数：
#   - Bytes: byte[] (必需) - 文件原始字节数组
#   - EncodingName: string (必需) - 编码格式名称
# 返回值：结构化对象
#   - Success: bool - true=成功，false=失败
#   - Content: string - 成功时=解码后的字符串，失败时=空字符串
#   - ErrorMessage: string - 失败时=错误信息，成功时=空字符串
# =======================================================
function Get-ContentWithoutBom
{
    param(
        [Parameter(Mandatory=$true)]
        [byte[]]$Bytes,

        [Parameter(Mandatory=$true)]
        [string]$EncodingName
    )

    try {
        if ($null -eq $Bytes -or $Bytes.Length -eq 0) {
            return [PSCustomObject]@{
                Success = $false
                Content = ""
                ErrorMessage = "字节数组为空或 null"
            }
        }

        $checkResult = Is-Supported-Encoding $EncodingName
        if (-not $checkResult.IsSupported) {
            return [PSCustomObject]@{
                Success = $false
                Content = ""
                ErrorMessage = "不支持的编码格式: $EncodingName"
            }
        }
        $encodingCode = $checkResult.EncodingCode

        $offset = 0

        if ($Bytes.Length -ge 4 -and $Bytes[0] -eq 0xFF -and $Bytes[1] -eq 0xFE -and $Bytes[2] -eq 0x00 -and $Bytes[3] -eq 0x00) {
            $offset = 4
        }
        elseif ($Bytes.Length -ge 4 -and $Bytes[0] -eq 0x00 -and $Bytes[1] -eq 0x00 -and $Bytes[2] -eq 0xFE -and $Bytes[3] -eq 0xFF) {
            $offset = 4
        }
        elseif ($Bytes.Length -ge 3 -and $Bytes[0] -eq 0xEF -and $Bytes[1] -eq 0xBB -and $Bytes[2] -eq 0xBF) {
            $offset = 3
        }
        elseif ($Bytes.Length -ge 2 -and $Bytes[0] -eq 0xFF -and $Bytes[1] -eq 0xFE) {
            $offset = 2
        }
        elseif ($Bytes.Length -ge 2 -and $Bytes[0] -eq 0xFE -and $Bytes[1] -eq 0xFF) {
            $offset = 2
        }

        if ($offset -gt 0) {
            $contentBytes = $Bytes[$offset..($Bytes.Length - 1)]
        } else {
            $contentBytes = $Bytes
        }

        switch ($encodingCode)
        {
            "1" { $decoder = [System.Text.UTF8Encoding]::new($false) }
            "2" { $decoder = [System.Text.UTF8Encoding]::new($false) }
            "3" { $decoder = [System.Text.UnicodeEncoding]::new($false, $false, $true) }
            "4" { $decoder = [System.Text.UnicodeEncoding]::new($false, $false, $true) }
            "5" { $decoder = [System.Text.UnicodeEncoding]::new($true, $false, $true) }
            "6" { $decoder = [System.Text.UnicodeEncoding]::new($true, $false, $true) }
            "7" { $decoder = [System.Text.Encoding]::ASCII }
            "8" { $decoder = [System.Text.Encoding]::GetEncoding(936) }
            "9" { $decoder = [System.Text.UTF32Encoding]::new($false, $false, $true) }
            "10" { $decoder = [System.Text.UTF32Encoding]::new($false, $false, $true) }
            "11" { $decoder = [System.Text.UTF32Encoding]::new($true, $false, $true) }
            "12" { $decoder = [System.Text.UTF32Encoding]::new($true, $false, $true) }
            default {
                $decoder = [System.Text.Encoding]::Default
            }
        }

        $content = $decoder.GetString($contentBytes)
        return [PSCustomObject]@{
            Success = $true
            Content = $content
            ErrorMessage = ""
        }
    }
    catch {
        return [PSCustomObject]@{
            Success = $false
            Content = ""
            ErrorMessage = "解码失败: $($_.Exception.Message)"
        }
    }
}

# =======================================================
# 函数：Write-Silent
# 功能：静默模式输出函数（给人类用户看的辅助输出）
# 参数：
#   - Message: string - 要输出的消息
#   - ForegroundColor: ConsoleColor - 输出文字颜色（默认Yellow）
# 返回值：无
# 说明：依赖父作用域的 $Quiet 变量判断是否输出
# =======================================================
function Write-Silent
{
    param(
        [string]$Message,
        [System.ConsoleColor]$ForegroundColor = [System.ConsoleColor]::Yellow
    )

    if (-not $Quiet)
    {
        Write-Host $Message -ForegroundColor $ForegroundColor
    }
}

# =======================================================
# 函数：Validate-SourceEncodingWithTempFile
# 功能：验证源文件编码是否正确
# 参数：
#   - FilePath: string (必需) - 文件路径
#   - SourceEncoding: string (必需) - 源文件编码
# 返回值：结构化对象
#   - Success: bool - true=验证过程成功完成，false=验证过程发生异常
#   - IsValid: bool - true=源编码正确，false=源编码可能不正确
#   - ErrorMessage: string - 失败时=错误信息，成功时=空字符串
# =======================================================
function Validate-SourceEncodingWithTempFile
{
    param(
        [Parameter(Mandatory=$true, Position=0)]
        [string]$FilePath,

        [Parameter(Mandatory=$true, Position=1)]
        [string]$SourceEncoding
    )

    try {
        $encResult = Get-EncodingObject $SourceEncoding
        if (-not $encResult.Success) {
            Write-Silent "错误: $($encResult.ErrorMessage)" -ForegroundColor Red
            return [PSCustomObject]@{
                Success = $false
                IsValid = $false
                ErrorMessage = $encResult.ErrorMessage
            }
        }
        $srcEncodingObj = $encResult.Encoding
        Write-Silent "1. 正在验证源编码 [ $SourceEncoding ] 是否正确..." -ForegroundColor Gray

        $tempFileName = [System.IO.Path]::GetRandomFileName()
        $tempFilePath = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), $tempFileName)
        Write-Silent "   临时验证文件: [ $tempFilePath ]" -ForegroundColor Gray

        $originalContent = [System.IO.File]::ReadAllText($FilePath, $srcEncodingObj)
        [System.IO.File]::WriteAllText($tempFilePath, $originalContent, $srcEncodingObj)
        
        $originalBytes = [System.IO.File]::ReadAllBytes($FilePath)
        $tempBytes = [System.IO.File]::ReadAllBytes($tempFilePath)
        
        Write-Silent "   原始文件大小: $($originalBytes.Length) 字节" -ForegroundColor Gray
        Write-Silent "   临时文件大小: $($tempBytes.Length) 字节" -ForegroundColor Gray

        if ($originalBytes.Length -ne $tempBytes.Length)
        {
            Write-Silent "   大小差异: $($originalBytes.Length - $tempBytes.Length) 字节" -ForegroundColor Yellow
            Write-Silent "   可能原因: BOM处理不一致或编码转换损失" -ForegroundColor Yellow

            $originalHasBOM = $false
            $tempHasBOM = $false

            # 检测原始文件的 BOM（先 4 字节、再 3 字节、再 2 字节）
            if ($originalBytes.Length -ge 4 -and $originalBytes[0] -eq 0xFF -and $originalBytes[1] -eq 0xFE -and $originalBytes[2] -eq 0x00 -and $originalBytes[3] -eq 0x00) {
                $originalHasBOM = $true
            }
            elseif ($originalBytes.Length -ge 4 -and $originalBytes[0] -eq 0x00 -and $originalBytes[1] -eq 0x00 -and $originalBytes[2] -eq 0xFE -and $originalBytes[3] -eq 0xFF) {
                $originalHasBOM = $true
            }
            elseif ($originalBytes.Length -ge 3 -and $originalBytes[0] -eq 0xEF -and $originalBytes[1] -eq 0xBB -and $originalBytes[2] -eq 0xBF) {
                $originalHasBOM = $true
            }
            elseif ($originalBytes.Length -ge 2 -and $originalBytes[0] -eq 0xFF -and $originalBytes[1] -eq 0xFE) {
                $originalHasBOM = $true
            }
            elseif ($originalBytes.Length -ge 2 -and $originalBytes[0] -eq 0xFE -and $originalBytes[1] -eq 0xFF) {
                $originalHasBOM = $true
            }

            # 检测临时文件的 BOM（先 4 字节、再 3 字节、再 2 字节）
            if ($tempBytes.Length -ge 4 -and $tempBytes[0] -eq 0xFF -and $tempBytes[1] -eq 0xFE -and $tempBytes[2] -eq 0x00 -and $tempBytes[3] -eq 0x00) {
                $tempHasBOM = $true
            }
            elseif ($tempBytes.Length -ge 4 -and $tempBytes[0] -eq 0x00 -and $tempBytes[1] -eq 0x00 -and $tempBytes[2] -eq 0xFE -and $tempBytes[3] -eq 0xFF) {
                $tempHasBOM = $true
            }
            elseif ($tempBytes.Length -ge 3 -and $tempBytes[0] -eq 0xEF -and $tempBytes[1] -eq 0xBB -and $tempBytes[2] -eq 0xBF) {
                $tempHasBOM = $true
            }
            elseif ($tempBytes.Length -ge 2 -and $tempBytes[0] -eq 0xFF -and $tempBytes[1] -eq 0xFE) {
                $tempHasBOM = $true
            }
            elseif ($tempBytes.Length -ge 2 -and $tempBytes[0] -eq 0xFE -and $tempBytes[1] -eq 0xFF) {
                $tempHasBOM = $true
            }

            if ($originalHasBOM -ne $tempHasBOM) {
                Write-Silent "   BOM处理不一致: 原始文件$(if ($originalHasBOM) { "有" } else { "无" })BOM" -ForegroundColor Yellow
                Write-Silent "   BOM处理不一致: 临时文件$(if ($tempHasBOM) { "有" } else { "无" })BOM" -ForegroundColor Yellow
            }
            
            if (Test-Path -LiteralPath $tempFilePath) {
                Remove-Item -Path $tempFilePath -Force -ErrorAction SilentlyContinue
            }
            
            return [PSCustomObject]@{
                Success = $true
                IsValid = $false
                ErrorMessage = "文件大小不匹配，可能是 BOM 处理不一致"
            }
        }

        $allMatch = $true
        $mismatchCount = 0
        
        for ($i = 0; $i -lt $originalBytes.Length; $i++) {
            if ($originalBytes[$i] -ne $tempBytes[$i]) {
                $allMatch = $false
                $mismatchCount++

                if ($mismatchCount -le 5)
                {
                    Write-Silent "   字节位置 $i 不匹配: 原始 0x$($originalBytes[$i].ToString('X2'))" -ForegroundColor Yellow
                    Write-Silent "   字节位置 $i 不匹配: 临时 0x$($tempBytes[$i].ToString('X2'))" -ForegroundColor Yellow
                }
            }
        }
        
        if (-not $allMatch) {
            Write-Silent "   错误: 发现 [ $mismatchCount ] 处字节不匹配" -ForegroundColor Red
            Write-Silent "   说明: 源编码 [ $SourceEncoding ] 可能不正确" -ForegroundColor Yellow
            Write-Silent "   临时验证文件已保留: [ $tempFilePath ]" -ForegroundColor Yellow
            
            return [PSCustomObject]@{
                Success = $true
                IsValid = $false
                ErrorMessage = "发现 $mismatchCount 处字节不匹配，源编码可能不正确"
            }
        }

        Remove-Item -Path $tempFilePath -Force -ErrorAction SilentlyContinue
        Write-Silent "   验证通过: 源编码正确" -ForegroundColor Green
        
        return [PSCustomObject]@{
            Success = $true
            IsValid = $true
            ErrorMessage = ""
        }
    }
    catch [System.Text.DecoderFallbackException]
    {
        Write-Silent "   错误: 解码失败 - 文件包含无法用 '$SourceEncoding' 解码的字节" -ForegroundColor Red
        Write-Silent "   说明: 源文件很可能不是 '$SourceEncoding' 编码" -ForegroundColor Yellow
        if (Test-Path -LiteralPath $tempFilePath)
        {
            Remove-Item -Path $tempFilePath -Force -ErrorAction SilentlyContinue
        }
        
        return [PSCustomObject]@{
            Success = $false
            IsValid = $false
            ErrorMessage = "解码失败: 文件包含无法用 '$SourceEncoding' 解码的字节"
        }
    }
    catch
    {
        Write-Silent "   错误: 验证过程中发生异常: $($_.Exception.Message)" -ForegroundColor Red
        if (Test-Path -LiteralPath $tempFilePath)
        {
            Remove-Item -Path $tempFilePath -Force -ErrorAction SilentlyContinue
        }
        
        return [PSCustomObject]@{
            Success = $false
            IsValid = $false
            ErrorMessage = "验证异常: $($_.Exception.Message)"
        }
    }
}

# =======================================================
# 函数：FileEncodingConverter
# 功能：主函数 - 转换文件编码格式
# 参数：
#   - FilePath: string (必需) - 文件路径
#   - SourceEncoding: string (必需) - 源文件编码
#   - TargetEncoding: string (必需) - 目标编码
#   - Quiet: switch (可选) - 静默模式，不显示输出信息
# 返回值：结构化对象
#   - Success: bool - true=转换成功，false=转换失败
#   - ErrorMessage: string - 失败时=错误信息，成功时=空字符串
# =======================================================
function FileEncodingConverter
{
    param(
        [Parameter(Mandatory=$true, Position=0)]
        [string]$FilePath,

        [Parameter(Mandatory=$true, Position=1)]
        [string]$SourceEncoding,

        [Parameter(Mandatory=$true, Position=2)]
        [string]$TargetEncoding,

        [Parameter(Mandatory=$false)]
        [switch]$Quiet,

        [Parameter(Mandatory=$false, Position=3)]
        [ValidateSet("LF", "CRLF")]
        [string]$LineEnding
    )

    if ($FilePath -eq "/?")
    {
        return [PSCustomObject]@{
            Success = $false
            ErrorMessage = "显示帮助信息"
        }
    }

    if ([System.Management.Automation.WildcardPattern]::ContainsWildcardCharacters($FilePath)) {
        Write-Silent "错误：参数一 [ $FilePath ] 不能包含通配符" -ForegroundColor Red
        return [PSCustomObject]@{
            Success = $false
            ErrorMessage = "文件路径不能包含通配符"
        }
    }

    $FilePath = Resolve-Path $FilePath
    if (-not (Test-Path -LiteralPath $FilePath))
    {
        Write-Silent "错误: 文件不存在 - $FilePath" -ForegroundColor Red
        return [PSCustomObject]@{
            Success = $false
            ErrorMessage = "文件不存在: $FilePath"
        }
    }

    $sourceResult = Is-Supported-Encoding $SourceEncoding
    if (-not $sourceResult.IsSupported)
    {
        Write-Silent "错误: 不支持的源编码格式: $SourceEncoding" -ForegroundColor Red
        Write-Silent "支持的源编码格式: $($SUPPORTED_ENCODINGS -join ', ')" -ForegroundColor Green
        return [PSCustomObject]@{
            Success = $false
            ErrorMessage = "不支持的源编码格式: $SourceEncoding"
        }
    }
    $sourceCode = $sourceResult.EncodingCode

    $targetResult = Is-Supported-Encoding $TargetEncoding
    if (-not $targetResult.IsSupported)
    {
        Write-Silent "错误: 不支持的目标编码格式: $TargetEncoding" -ForegroundColor Red
        Write-Silent "支持的目标编码格式: $($SUPPORTED_ENCODINGS -join ', ')" -ForegroundColor Green
        return [PSCustomObject]@{
            Success = $false
            ErrorMessage = "不支持的目标编码格式: $TargetEncoding"
        }
    }
    $targetCode = $targetResult.EncodingCode

    if ($sourceCode -eq $targetCode) {
        if ($null -eq $LineEnding -or $LineEnding -eq "") {
            Write-Silent "提醒: 源编码和目标编码相同，无需转换" -ForegroundColor Yellow
            return [PSCustomObject]@{
                Success = $true
                ErrorMessage = ""
            }
        } else {
            Write-Silent "源编码和目标编码相同，仅转换行尾符..." -ForegroundColor Gray
            # 继续执行行尾转换（跳过编码转换部分）
            try
            {
                $encResult = Get-EncodingObject $TargetEncoding
                if (-not $encResult.Success) {
                    throw $encResult.ErrorMessage
                }
                $tgtEncodingObj = $encResult.Encoding
                
                $originalBytes = [System.IO.File]::ReadAllBytes($FilePath)
                $contentResult = Get-ContentWithoutBom $originalBytes $SourceEncoding
                if (-not $contentResult.Success) {
                    throw $contentResult.ErrorMessage
                }
                $originalContent = $contentResult.Content
                
                # 行尾符转换
                if ($LineEnding -eq "CRLF")
                {
                    $originalContent = $originalContent -replace "`r`n", "`n" -replace "`n", "`r`n"
                }
                elseif ($LineEnding -eq "LF")
                {
                    $originalContent = $originalContent -replace "`r`n", "`n"
                }
                
                [System.IO.File]::WriteAllText($FilePath, $originalContent, $tgtEncodingObj)
                Write-Silent "行尾符转换完成" -ForegroundColor Green
                
                return [PSCustomObject]@{
                    Success = $true
                    ErrorMessage = ""
                }
            }
            catch
            {
                Write-Silent "行尾符转换失败: $_" -ForegroundColor Red
                return [PSCustomObject]@{
                    Success = $false
                    ErrorMessage = $_.Exception.Message
                }
            }
        }
    }

    try
    {
        $validateResult = Validate-SourceEncodingWithTempFile $FilePath $SourceEncoding
        if (-not $validateResult.Success -or -not $validateResult.IsValid)
        {
            Write-Silent "   错误: 源编码验证失败，无法使用 [ $SourceEncoding ] 正确读取文件" -ForegroundColor Red
            Write-Silent "   提示: 请使用文件编码检测工具检查文件的实际编码" -ForegroundColor Red
            Write-Silent "   详细信息: $($validateResult.ErrorMessage)" -ForegroundColor Yellow
            return [PSCustomObject]@{
                Success = $false
                ErrorMessage = $validateResult.ErrorMessage
            }
        }

        Write-Silent "2. 开始转换文件编码..." -ForegroundColor Gray
        Write-Silent "   文　件: [ $FilePath ]" -ForegroundColor Gray
        Write-Silent "   源编码: [ $SourceEncoding ] -> 目标编码: [ $TargetEncoding ]" -ForegroundColor Red
        Write-Silent "   读取源文件内容..." -ForegroundColor Gray

        $tempFilePath = $FilePath + ".orig"

        if (Test-Path -LiteralPath $tempFilePath)
        {
            Remove-Item -Path $tempFilePath -Force -ErrorAction SilentlyContinue
            Write-Silent "   删除已存在的临时文件: $tempFilePath" -ForegroundColor Gray
        }

        $originalBytes = [System.IO.File]::ReadAllBytes($FilePath)
        $contentResult = Get-ContentWithoutBom $originalBytes $SourceEncoding
        if (-not $contentResult.Success) {
            throw $contentResult.ErrorMessage
        }
        $originalContent = $contentResult.Content

        $originalCharCount = $originalContent.Length
        Write-Silent "   原始文件大小: $($originalBytes.Length) 字节" -ForegroundColor Gray
        Write-Silent "   原始文件字符数: $originalCharCount 个" -ForegroundColor Gray
        Write-Silent "3. 写入临时文件（使用目标编码）..." -ForegroundColor Gray

        $encResult = Get-EncodingObject $TargetEncoding
        if (-not $encResult.Success) {
            throw $encResult.ErrorMessage
        }
        $tgtEncodingObj = $encResult.Encoding
        [System.IO.File]::WriteAllText($tempFilePath, $originalContent, $tgtEncodingObj)

        if (-not (Test-Path -LiteralPath $tempFilePath))
        {
            throw "临时文件创建失败: $tempFilePath"
        }
        else
        {
            Write-Silent "   写入临时文件成功 $tempFilePath 使用编码 [ $TargetEncoding ]" -ForegroundColor Gray
        }

        Write-Silent "4. 验证转换结果..." -ForegroundColor Gray

        $tempFileSize = (Get-Item $tempFilePath).Length
        $tempBytes = [System.IO.File]::ReadAllBytes($tempFilePath)
        $contentResult = Get-ContentWithoutBom $tempBytes $TargetEncoding
        if (-not $contentResult.Success) {
            throw $contentResult.ErrorMessage
        }
        $tempContent = $contentResult.Content
        Write-Silent "   临时文件大小: $tempFileSize 字节" -ForegroundColor Gray
        Write-Silent "   临时文件字符数: $($tempContent.Length) 个" -ForegroundColor Gray
        Write-Silent "   现在开始比较原文件的内容和临时文件的内容 (去除了 BOM 且解码之后的内容)..." -ForegroundColor Gray

        if ($originalContent -eq $tempContent)
        {
            Write-Silent "   验证通过: 内容一致，转换结果正确" -ForegroundColor Green
        }
        else
        {
            if (-not $Quiet)
            {
                Write-Silent "  内容不同，开始详细比较..." -ForegroundColor Yellow
                
                $minLength = [Math]::Min($originalContent.Length, $tempContent.Length)
                $diffIndex = -1
                for ($i = 0; $i -lt $minLength; $i++) {
                    if ($originalContent[$i] -ne $tempContent[$i]) {
                        $diffIndex = $i
                        break
                    }
                }

                if ($diffIndex -ge 0) {
                    Write-Silent "   第一个不同字符在位置 $diffIndex" -ForegroundColor Yellow
                    Write-Silent "   原始: '$($originalContent[$diffIndex])' (U+$([int]$originalContent[$diffIndex]))" -ForegroundColor Gray
                    Write-Silent "   临时: '$($tempContent[$diffIndex])' (U+$([int]$tempContent[$diffIndex]))" -ForegroundColor Gray

                    $startIndex = [Math]::Max(0, $diffIndex - 10)
                    $endIndex = [Math]::Min($originalContent.Length - 1, $diffIndex + 10)
                    Write-Silent "   周围文本 (原始): '$($originalContent.Substring($startIndex, $endIndex - $startIndex + 1))'" -ForegroundColor Gray
                    Write-Silent "   周围文本 (临时): '$($tempContent.Substring($startIndex, $endIndex - $startIndex + 1))'" -ForegroundColor Gray
                } else {
                    Write-Silent "   字符长度不同: 原始 $($originalContent.Length) 字符, 临时 $($tempContent.Length) 字符" -ForegroundColor Yellow
                }

                $debugOriginalPath = $FilePath + ".orig_content.txt"
                $debugTempPath = $FilePath + ".temp_content.txt"

                [System.IO.File]::WriteAllText($debugOriginalPath, $originalContent, [System.Text.UTF8Encoding]::new($false))
                [System.IO.File]::WriteAllText($debugTempPath, $tempContent, [System.Text.UTF8Encoding]::new($false))

                Write-Silent "   原始文件解码后的内容被保存至: $debugOriginalPath" -ForegroundColor Gray
                Write-Silent "   临时文件解码后的内容被保存至: $debugTempPath" -ForegroundColor Gray
            }
            throw "验证失败: 去除BOM后内容不同"
        }

        Write-Silent "5. 执行原子操作: 替换原文件..." -ForegroundColor Gray

        try
        {
            Remove-Item -Path $FilePath -Force
            Write-Silent "   已删除原文件: $FilePath" -ForegroundColor Gray

            Move-Item -Path $tempFilePath -Destination $FilePath -Force
            Write-Silent "   已重命名文件: $tempFilePath -->" -ForegroundColor Gray
            Write-Silent "   已重命名文件: $FilePath <--" -ForegroundColor Gray
            Write-Silent "   转换成功完成!" -ForegroundColor Green

            if ((-not $Quiet) -and (Test-Path -LiteralPath $FilePath))
            {
                $finalSize = (Get-Item $FilePath).Length
                Write-Silent "6. 最终文件大小: $finalSize 字节" -ForegroundColor Gray
            }

            # 行尾符转换（如果指定了 LineEnding 参数）
            if ($null -ne $LineEnding -and $LineEnding -ne "")
            {
                Write-Silent "7. 转换行尾符为 [$LineEnding]..." -ForegroundColor Gray
                try
                {
                    $fileContent = [System.IO.File]::ReadAllText($FilePath, $tgtEncodingObj)
                    if ($LineEnding -eq "CRLF")
                    {
                        # 将 LF 转换为 CRLF
                        $fileContent = $fileContent -replace "`r`n", "`n" -replace "`n", "`r`n"
                    }
                    elseif ($LineEnding -eq "LF")
                    {
                        # 将 CRLF 转换为 LF
                        $fileContent = $fileContent -replace "`r`n", "`n"
                    }
                    [System.IO.File]::WriteAllText($FilePath, $fileContent, $tgtEncodingObj)
                    Write-Silent "   行尾符转换完成" -ForegroundColor Green
                }
                catch
                {
                    Write-Silent "   行尾符转换失败: $_" -ForegroundColor Yellow
                }
            }
            
            return [PSCustomObject]@{
                Success = $true
                ErrorMessage = ""
            }
        }
        catch
        {
            Write-Silent "  原子操作失败: $_" -ForegroundColor Red
            if (Test-Path -LiteralPath $tempFilePath)
            {
                Write-Silent "尝试恢复临时文件..." -ForegroundColor Yellow
                if (-not (Test-Path -LiteralPath $FilePath))
                {
                    Move-Item -Path $tempFilePath -Destination $FilePath -Force
                    Write-Silent "已恢复临时文件为原文件" -ForegroundColor Green
                }
            }
            throw "原子操作失败: $_"
        }
    }
    catch
    {
        if (-not $Quiet)
        {
            Write-Silent "转换失败: $_" -ForegroundColor Red
            if (Test-Path -LiteralPath $tempFilePath)
            {
                Write-Silent "转换产生的临时文件已保留: $tempFilePath" -ForegroundColor Yellow
            }
        }
        return [PSCustomObject]@{
            Success = $false
            ErrorMessage = $_.Exception.Message
        }
    }
}

# =======================================================
# 脚本入口点
# =======================================================
# 功能：作为独立脚本执行时的入口点
# 说明：
#   - 将所有命令行参数传递给 FileEncodingConverter 主函数
#   - 输出 JSON 格式的结果，便于 AI Agent 解析
#   - 设置退出码：0=成功，1=失败
# =======================================================
if ($MyInvocation.InvocationName -ne '.')
{
    if ($args.Count -eq 0) {
        exit 1
    }
    
    # 构建参数字典，兼容 switch 参数和命名参数
    $paramArgs = @{}
    $posArgs = @()
    $i = 0
    while ($i -lt $args.Count) {
        if ($args[$i] -eq '-Quiet') {
            $paramArgs['Quiet'] = $true
        } elseif ($args[$i] -eq '-LineEnding' -and ($i + 1 -lt $args.Count)) {
            $paramArgs['LineEnding'] = $args[$i + 1]
            $i++
        } elseif ($args[$i] -match '^-') {
            # 其他命名参数跳过
        } else {
            $posArgs += $args[$i]
        }
        $i++
    }
    
    # 按位置填充必需参数
    if ($posArgs.Count -ge 1) { $paramArgs['FilePath'] = $posArgs[0] }
    if ($posArgs.Count -ge 2) { $paramArgs['SourceEncoding'] = $posArgs[1] }
    if ($posArgs.Count -ge 3) { $paramArgs['TargetEncoding'] = $posArgs[2] }
    if ($posArgs.Count -ge 4) { $paramArgs['LineEnding'] = $posArgs[3] }
    
    $result = FileEncodingConverter @paramArgs
    
    $result | ConvertTo-Json -Depth 10
    
    if ($result.Success) {
        exit 0
    } else {
        exit 1
    }
}

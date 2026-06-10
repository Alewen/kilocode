param(
    [Parameter(Mandatory=$true)]
    [string]$SessionId,
    
    [Parameter(Mandatory=$true)]
    [string]$StartTime,
    
    [Parameter(Mandatory=$true)]
    [string]$EndTime
)

$KILO_DB = "$env:USERPROFILE\.local\share\kilo\kilo.db"

$now = Get-Date
$currentYear = $now.Year
$currentMonth = "{0:D2}" -f $now.Month
$currentDate = "{0:yyyy-MM-dd}" -f $now

function Normalize-Time {
    param([string]$Raw)
    
    $spaces = ($Raw.ToCharArray() | Where-Object { $_ -eq ' ' }).Count
    
    if ($spaces -eq 1) {
        $parts = $Raw -split ' ', 2
        $datePart = $parts[0]
        $timePart = $parts[1]
        $dashes = ($datePart.ToCharArray() | Where-Object { $_ -eq '-' }).Count
        
        if ($dashes -eq 2) {
            # 完整 "YYYY-MM-DD HH:MM:SS"
            return "$Raw"
        } elseif ($dashes -eq 1) {
            # "MM-DD HH:MM:SS" → 补年份
            return "${currentYear}-${Raw}"
        } else {
            return "$Raw"
        }
    } elseif ($spaces -eq 0) {
        # "HH:MM:SS" → 补年月日
        return "${currentDate} ${Raw}"
    } else {
        # 其他情况：提取 day 和 time 补年月
        $firstSpace = $Raw.IndexOf(' ')
        $dayPart = $Raw.Substring(0, $firstSpace)
        $timePart = $Raw.Substring($firstSpace + 1)
        return "${currentYear}-${currentMonth}-${dayPart} ${timePart}"
    }
}

$startNormalized = Normalize-Time -Raw $StartTime
$endNormalized = Normalize-Time -Raw $EndTime

try {
    $startDt = Get-Date $startNormalized -ErrorAction Stop
    $startTs = [long][Math]::Floor($startDt.ToUniversalTime().Subtract([DateTime]::UnixEpoch).TotalSeconds)
} catch {
    Write-Host "错误: 开始时间格式无效: $StartTime → $startNormalized"
    exit 1
}

try {
    $endDt = Get-Date $endNormalized -ErrorAction Stop
    $endTs = [long][Math]::Floor($endDt.ToUniversalTime().Subtract([DateTime]::UnixEpoch).TotalSeconds)
} catch {
    Write-Host "错误: 结束时间格式无效: $EndTime → $endNormalized"
    exit 1
}

Write-Host "=== Kilo Session Messages: $SessionId ==="
Write-Host "时间范围: $startNormalized ~ $endNormalized"
Write-Host ""

sqlite3.exe -header -column $KILO_DB "SELECT id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, datetime(time_updated/1000, 'unixepoch', 'localtime') as updated, substr(data, 1, 200) as preview FROM message WHERE session_id = '$SessionId' AND time_updated/1000 >= $startTs AND time_updated/1000 <= $endTs ORDER BY time_updated ASC;"

Write-Host ""
Write-Host "查询完成"

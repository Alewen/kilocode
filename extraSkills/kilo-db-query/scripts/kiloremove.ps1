# 删除 Kilo 数据库中指定 session_id 相关的全部信息
# 用法: .\kiloremove.ps1 -SessionId <session_id>

param(
    [Parameter(Mandatory=$true)]
    [string]$SessionId
)

$KILO_DB = "$env:USERPROFILE\.local\share\kilo\kilo.db"

# 检查数据库文件是否存在
if (-not (Test-Path $KILO_DB)) {
    Write-Host "错误: 数据库文件不存在: $KILO_DB"
    exit 1
}

# 开始删除操作
Write-Host "正在删除 session_id: $SessionId 的数据..."

# 先删除 part 表
$PartCount = sqlite3.exe $KILO_DB "SELECT COUNT(*) FROM part WHERE session_id = '$SessionId';"
sqlite3.exe $KILO_DB "DELETE FROM part WHERE session_id = '$SessionId';"
Write-Host "已删除 part 表记录: $PartCount 条"

# 删除 session_message 表
$SessionMessageCount = sqlite3.exe $KILO_DB "SELECT COUNT(*) FROM session_message WHERE session_id = '$SessionId';"
sqlite3.exe $KILO_DB "DELETE FROM session_message WHERE session_id = '$SessionId';"
Write-Host "已删除 session_message 表记录: $SessionMessageCount 条"

# 删除 session_share 表
$SessionShareCount = sqlite3.exe $KILO_DB "SELECT COUNT(*) FROM session_share WHERE session_id = '$SessionId';"
sqlite3.exe $KILO_DB "DELETE FROM session_share WHERE session_id = '$SessionId';"
Write-Host "已删除 session_share 表记录: $SessionShareCount 条"

# 删除 todo 表
$TodoCount = sqlite3.exe $KILO_DB "SELECT COUNT(*) FROM todo WHERE session_id = '$SessionId';"
sqlite3.exe $KILO_DB "DELETE FROM todo WHERE session_id = '$SessionId';"
Write-Host "已删除 todo 表记录: $TodoCount 条"

# 再删除 message 表
$MessageCount = sqlite3.exe $KILO_DB "SELECT COUNT(*) FROM message WHERE session_id = '$SessionId';"
sqlite3.exe $KILO_DB "DELETE FROM message WHERE session_id = '$SessionId';"
Write-Host "已删除 message 表记录: $MessageCount 条"

# 最后删除 session 表
$SessionCount = sqlite3.exe $KILO_DB "SELECT COUNT(*) FROM session WHERE id = '$SessionId';"
sqlite3.exe $KILO_DB "DELETE FROM session WHERE id = '$SessionId';"
Write-Host "已删除 session 表记录: $SessionCount 条"

Write-Host "删除操作完成！"

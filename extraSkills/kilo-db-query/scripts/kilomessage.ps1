param(
    [Parameter(Mandatory=$true)]
    [string]$SessionId,
    
    [int]$Limit
)

$KILO_DB = "$HOME/.local/share/kilo/kilo.db"

if ($Limit) {
    Write-Host "=== Kilo Session Messages: $SessionId (Last $Limit) ==="
    Write-Host ""
    sqlite3 -header -column $KILO_DB "SELECT id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, datetime(time_updated/1000, 'unixepoch', 'localtime') as updated, data as data FROM message WHERE session_id = '$SessionId' ORDER BY time_created DESC LIMIT $Limit;"
    Write-Host ""
    Write-Host "Showing last $Limit messages"
} else {
    Write-Host "=== Kilo Session Messages: $SessionId ==="
    Write-Host ""
    sqlite3 -header -column $KILO_DB "SELECT id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, datetime(time_updated/1000, 'unixepoch', 'localtime') as updated, data as data FROM message WHERE session_id = '$SessionId' ORDER BY time_created DESC;"
    Write-Host ""
    Write-Host "Showing all messages"
}

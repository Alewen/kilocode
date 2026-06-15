param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$SessionId,
    
    [Parameter(Position=1)]
    [string]$Filter,
    
    [int]$Limit
)

$KILO_DB = "$HOME/.local/share/kilo/kilo.db"

if (-not $Filter) {
    if ($Limit) {
        Write-Host "=== Kilo Session Parts: $SessionId (Last $Limit) ==="
        Write-Host ""
        sqlite3 -header -column $KILO_DB "SELECT id, message_id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, data as data FROM part WHERE session_id = '$SessionId' ORDER BY time_created DESC LIMIT $Limit;"
        Write-Host ""
        Write-Host "Showing last $Limit parts"
    } else {
        Write-Host "=== Kilo Session Parts: $SessionId ==="
        Write-Host ""
        sqlite3 -header -column $KILO_DB "SELECT id, message_id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, data as data FROM part WHERE session_id = '$SessionId' ORDER BY time_created DESC;"
        Write-Host ""
        Write-Host "Showing all parts"
    }
} else {
    if ($Limit) {
        Write-Host "=== Kilo Session Parts: $SessionId (Filter: $Filter, Last $Limit) ==="
        Write-Host ""
        sqlite3 -header -column $KILO_DB "SELECT id, message_id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, data as data FROM part WHERE session_id = '$SessionId' AND data LIKE '%$Filter%' ORDER BY time_created DESC LIMIT $Limit;"
        Write-Host ""
        Write-Host "Showing last $Limit parts matching: $Filter"
    } else {
        Write-Host "=== Kilo Session Parts: $SessionId (Filter: $Filter) ==="
        Write-Host ""
        sqlite3 -header -column $KILO_DB "SELECT id, message_id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, data as data FROM part WHERE session_id = '$SessionId' AND data LIKE '%$Filter%' ORDER BY time_created DESC;"
        Write-Host ""
        Write-Host "Showing all parts matching: $Filter"
    }
}

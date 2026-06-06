param(
    [int]$Limit
)

$KILO_DB = "C:\Users\shaoke\.local\share\kilo\kilo.db"

if ($Limit) {
    Write-Host "=== Kilo Session List (Last $Limit) ==="
    Write-Host ""
    sqlite3.exe -header -column $KILO_DB "SELECT id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, datetime(time_updated/1000, 'unixepoch', 'localtime') as updated, directory, path FROM session ORDER BY time_updated DESC LIMIT $Limit;"
    Write-Host ""
    Write-Host "Showing last $Limit sessions"
} else {
    Write-Host "=== Kilo Session List ==="
    Write-Host ""
    sqlite3.exe -header -column $KILO_DB "SELECT id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, datetime(time_updated/1000, 'unixepoch', 'localtime') as updated, directory, path FROM session ORDER BY time_updated DESC;"
    Write-Host ""
    Write-Host "Showing all sessions"
}

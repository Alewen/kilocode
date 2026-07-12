Write-Host "=== Registered Minifilter Altitudes ===" -ForegroundColor Cyan
Write-Host ""

$entries = Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Services\*\Instances\*" -ErrorAction SilentlyContinue | ForEach-Object {
    $val = Get-ItemProperty -Path $_.PSPath -Name "Altitude" -ErrorAction SilentlyContinue
    if ($val -and $val.Altitude) {
        [PSCustomObject]@{
            Service  = ($_.PSParentPath -split '\\')[-2]
            Instance = $_.PSChildName
            Altitude = [double]$val.Altitude
        }
    }
} | Sort-Object Altitude

if ($entries.Count -eq 0) {
    Write-Host "  (none found)" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Suggested altitude: 360000" -ForegroundColor Green
    exit 0
}

$categories = @(
    @{ Name = "A (Anti-Virus)";        Min = 320000; Max = 339999 },
    @{ Name = "B (Compression)";       Min = 340000; Max = 349999 },
    @{ Name = "C (Encryption)";        Min = 350000; Max = 359999 },
    @{ Name = "D (General/Undefined)"; Min = 360000; Max = 389999 },
    @{ Name = "E (File System)";       Min = 400000; Max = 409999 },
    @{ Name = "F (File System)";       Min = 420000; Max = 429999 },
    @{ Name = "G (File System)";       Min = 440000; Max = 449999 },
    @{ Name = "H (File System)";       Min = 460000; Max = 469999 },
    @{ Name = "I (File System)";       Min = 480000; Max = 489999 },
    @{ Name = "J (File System)";       Min = 500000; Max = 509999 },
    @{ Name = "K (File System)";       Min = 520000; Max = 529999 },
    @{ Name = "L (File System)";       Min = 540000; Max = 549999 },
    @{ Name = "M (File System)";       Min = 560000; Max = 569999 },
    @{ Name = "N (File System)";       Min = 580000; Max = 589999 },
    @{ Name = "O (File System)";       Min = 600000; Max = 609999 },
    @{ Name = "P (File System)";       Min = 620000; Max = 629999 },
    @{ Name = "Q (File System)";       Min = 640000; Max = 649999 },
    @{ Name = "R (File System)";       Min = 660000; Max = 669999 },
    @{ Name = "S (File System)";       Min = 680000; Max = 689999 },
    @{ Name = "T (File System)";       Min = 700000; Max = 709999 },
    @{ Name = "U (File System)";       Min = 720000; Max = 729999 },
    @{ Name = "V (File System)";       Min = 740000; Max = 749999 },
    @{ Name = "W (File System)";       Min = 760000; Max = 769999 },
    @{ Name = "X (File System)";       Min = 780000; Max = 789999 },
    @{ Name = "Y (File System)";       Min = 800000; Max = 809999 }
)

foreach ($cat in $categories) {
    $inRange = $entries | Where-Object { $_.Altitude -ge $cat.Min -and $_.Altitude -le $cat.Max }
    if ($inRange) {
        Write-Host ("  {0,-30} {1}" -f "$($cat.Name):", ($inRange.Altitude -join ', '))
    }
}

$other = $entries | Where-Object {
    $inAny = $false
    foreach ($cat in $categories) {
        if ($_.Altitude -ge $cat.Min -and $_.Altitude -le $cat.Max) { $inAny = $true; break }
    }
    -not $inAny
}
if ($other) {
    Write-Host ("  {0,-30} {1}" -f "Other:", ($other.Altitude -join ', '))
}

Write-Host ""
Write-Host "Total: $($entries.Count)" -ForegroundColor Gray
Write-Host ""

$base = 360000.0
$used = $entries.Altitude
$next = $base
while ($used -contains $next) {
    $next += 0.1
}
Write-Host "Suggested altitude: $next" -ForegroundColor Green

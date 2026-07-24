param(
    [Parameter(HelpMessage="Minifilter altitude (default 360000)")]
    [double]$Altitude = 360000
)

$name = "KiloGuard"
$src = "$PSScriptRoot\x64\$name.sys"
$dst = "$env:SystemRoot\System32\drivers\$name.sys"
$cer = "$PSScriptRoot\x64\$name.cer"

$admin = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "Please run as Administrator" }

if (-not (Test-Path $src)) { throw "Driver not found: $src. Run msbuild.ps1 first." }

$svc = Get-Service -Name $name -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "FAILED: Driver service '$name' already exists (state: $($svc.Status))." -ForegroundColor Red
    Write-Host "  Run mscleanup.ps1 first to remove the existing driver." -ForegroundColor Yellow
    throw "Driver already exists"
}

Write-Host "=== KiloGuard Deploy ==="

# Copy driver
Write-Host " [1] Copying driver ..."
Copy-Item -Path $src -Destination $dst -Force

$srcHash = (Get-FileHash -Path $src -Algorithm SHA256).Hash
$dstHash = (Get-FileHash -Path $dst -Algorithm SHA256).Hash
if ($srcHash -ne $dstHash) { throw "Binary mismatch" }
Write-Host "  OK"

# Install certificate
if (Test-Path $cer) {
    Write-Host " [2] Installing certificate ..."
    certutil -addstore -f Root $cer 2>&1 | Out-Null
    Write-Host "  OK"
} else { Write-Host " [2] No certificate file (skip)" }

# Check test signing
$startOpts = (Get-ItemPropertyValue -Path "HKLM:\SYSTEM\CurrentControlSet\Control" -Name "SystemStartOptions" -ErrorAction SilentlyContinue)
if ($startOpts -notmatch "TESTSIGNING") {
    Write-Host " WARNING: Test Signing Mode is OFF. Run: bcdedit /set testsigning on" -ForegroundColor Yellow
    Write-Host "  Press Enter to continue or Ctrl+C to abort ..."
    $null = Read-Host
}

# Create service
Write-Host " [3] Creating service ..."
$svc = Get-Service -Name $name -ErrorAction SilentlyContinue
if (-not $svc) {
    sc.exe create $name type=kernel binPath=$dst 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Service create failed" }
    reg add "HKLM\SYSTEM\CurrentControlSet\Services\$name\Instances" /v DefaultInstance /t REG_SZ /d "$name Instance" /f 2>&1 | Out-Null
    reg add "HKLM\SYSTEM\CurrentControlSet\Services\$name\Instances\$name Instance" /v Altitude /t REG_SZ /d $Altitude /f 2>&1 | Out-Null
    reg add "HKLM\SYSTEM\CurrentControlSet\Services\$name\Instances\$name Instance" /v Flags /t REG_DWORD /d 0 /f 2>&1 | Out-Null
    Write-Host "  OK"
} else { Write-Host "  Already exists" }

# Start driver
Write-Host " [4] Starting driver ..."
sc.exe start $name 2>&1 | Out-Null
Start-Sleep -Seconds 1
$svc = Get-Service -Name $name -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq 'Running') {
    Write-Host "  OK"
} else {
    Write-Host "  FAILED (check: sc.exe query $name)" -ForegroundColor Red
    exit 1
}

# Verify
Write-Host "=== Verification ==="
fltmc filters | Select-String -Pattern $name -SimpleMatch | ForEach-Object { Write-Host "  $_" }
Write-Host "=== Deploy OK ==="
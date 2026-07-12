Write-Host "========================================"
Write-Host " KiloGuard v5 Cleanup"
Write-Host "========================================"
Write-Host ""

$admin = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    throw "Please run as Administrator"
}
Write-Host " [*] Administrator check OK"
Write-Host ""

# Step 1: Clean build artifacts first (no admin needed, no side effects)
Write-Host " [1] Cleaning build artifacts ..."
Remove-Item -Path "$PSScriptRoot\*.obj", "$PSScriptRoot\*.pdb" -Force -ErrorAction SilentlyContinue
Write-Host "OK"
Write-Host ""

# Step 2: Check if driver exists
Write-Host " [2] Checking if driver exists ..."
$svc = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
if (-not $svc) {
    $fileExists = Test-Path "$env:SystemRoot\System32\drivers\KiloGuard.sys"
    if (-not $fileExists) {
        Write-Host "  (not installed, nothing to clean)"
        Write-Host ""
        Write-Host "========================================"
        Write-Host " Cleanup complete (nothing to do)."
        Write-Host "========================================"
        exit 0
    }
    Write-Host "  Service not found, but driver file exists. Will clean up."
} else {
    Write-Host "  Found (state: $($svc.Status))"
}
Write-Host ""

# Step 3: Unload minifilter (先摘除文件系统栈，再停服务)
Write-Host " [3] Unloading minifilter ..."
$fltBefore = fltmc filters 2>&1 | Select-String -Pattern "KiloGuard" -SimpleMatch
if ($fltBefore) {
    fltmc unload KiloGuard
    $fltAfter = fltmc filters 2>&1 | Select-String -Pattern "KiloGuard" -SimpleMatch
    if ($fltAfter) { Write-Host "  NEEDS REBOOT (instances attached)" } else { Write-Host "OK" }
} else {
    Write-Host "  (not loaded)"
}
Write-Host ""

# Step 4: Stop service
Write-Host " [4] Stopping service ..."
$stillRunning = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
if ($stillRunning -and ($stillRunning.Status -eq 'Running')) {
    sc.exe stop KiloGuard
    Start-Sleep -Seconds 2
    $stillRunning = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
    if ($stillRunning -and ($stillRunning.Status -eq 'Running')) {
        Write-Host "  NEEDS REBOOT (service did not stop)"
    } else {
        Write-Host "OK"
    }
} else {
    Write-Host "  (not running)"
}
Write-Host ""

# Step 5: Delete service
Write-Host " [5] Deleting service ..."
$svcDelete = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
if ($svcDelete) {
    sc.exe delete KiloGuard
    $svcDelete = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
    if ($svcDelete) { Write-Host "  NEEDS REBOOT (pending deletion)" } else { Write-Host "OK" }
} else {
    Write-Host "  (not installed)"
}
Write-Host ""

# Step 6: Remove registry
Write-Host " [6] Removing registry ..."
$regBefore = Test-Path HKLM:\SYSTEM\CurrentControlSet\Services\KiloGuard
if ($regBefore) {
    reg delete HKLM\SYSTEM\CurrentControlSet\Services\KiloGuard /f
    $regAfter = Test-Path HKLM:\SYSTEM\CurrentControlSet\Services\KiloGuard
    if ($regAfter) { Write-Host "  FAILED" } else { Write-Host "OK" }
} else {
    Write-Host "  (no registry)"
}
Write-Host ""

# Step 7: Delete driver file
Write-Host " [7] Deleting driver file ..."
if (Test-Path "$env:SystemRoot\System32\drivers\KiloGuard.sys") {
    try {
        Remove-Item -Path "$env:SystemRoot\System32\drivers\KiloGuard.sys" -Force -ErrorAction Stop
        Write-Host "OK"
    } catch {
        Write-Host "  NEEDS REBOOT (file in use)"
    }
} else {
    Write-Host "  (not found)"
}
Write-Host ""

Write-Host "========================================"
Write-Host " Verification"
Write-Host "========================================"

$svc = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "  Service: NEEDS REBOOT"
} else {
    Write-Host "  Service: CLEAN"
}

$flt = fltmc filters 2>&1 | Select-String -Pattern "KiloGuard" -SimpleMatch
if ($flt) {
    Write-Host "  Filter:  NEEDS REBOOT"
} else {
    Write-Host "  Filter:  CLEAN"
}

if (Test-Path "$env:SystemRoot\System32\drivers\KiloGuard.sys") {
    Write-Host "  Driver:  NEEDS REBOOT"
} else {
    Write-Host "  Driver:  CLEAN"
}

Write-Host "========================================"
Write-Host " Cleanup complete."
Write-Host "========================================"

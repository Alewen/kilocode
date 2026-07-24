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

# Step 1: Check if driver is installed
Write-Host " [1] Checking if driver is installed ..."
$svc = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
$fileExists = Test-Path "$env:SystemRoot\System32\drivers\KiloGuard.sys"
$driverInstalled = $svc -or $fileExists
if (-not $driverInstalled) {
    Write-Host "  (not installed, skipping driver cleanup)"
    Write-Host ""
} else {
    Write-Host "  Found (state: $($svc.Status))"
    Write-Host ""
}

# Step 2: Unload minifilter (先摘除文件系统栈，再停服务)
Write-Host " [2] Unloading minifilter ..."
if ($driverInstalled) {
    $flt = fltmc filters 2>&1 | Select-String -Pattern "KiloGuard" -SimpleMatch
    if ($flt) {
        fltmc unload KiloGuard
        $flt = fltmc filters 2>&1 | Select-String -Pattern "KiloGuard" -SimpleMatch
        if ($flt) { Write-Host "  NEEDS REBOOT (instances attached)" } else { Write-Host "  OK" }
    } else {
        Write-Host "  (not loaded)"
    }
} else {
    Write-Host "  (skipped)"
}
Write-Host ""

# Step 3: Stop service
Write-Host " [3] Stopping service ..."
if ($driverInstalled) {
    $svc = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
    if ($svc -and ($svc.Status -eq 'Running')) {
        sc.exe stop KiloGuard
        Start-Sleep -Seconds 2
        $svc = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
        if ($svc -and ($svc.Status -eq 'Running')) {
            Write-Host "  NEEDS REBOOT (service did not stop)"
        } else {
            Write-Host "  OK"
        }
    } else {
        Write-Host "  (not running)"
    }
} else {
    Write-Host "  (skipped)"
}
Write-Host ""

# Step 4: Delete service
Write-Host " [4] Deleting service ..."
if ($driverInstalled) {
    $svc = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
    if ($svc) {
        sc.exe delete KiloGuard
        $svc = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
        if ($svc) { Write-Host "  NEEDS REBOOT (pending deletion)" } else { Write-Host "  OK" }
    } else {
        Write-Host "  (not installed)"
    }
} else {
    Write-Host "  (skipped)"
}
Write-Host ""

# Step 5: Remove registry
Write-Host " [5] Removing registry ..."
if ($driverInstalled) {
    if (Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\KiloGuard") {
        reg delete "HKLM\SYSTEM\CurrentControlSet\Services\KiloGuard" /f
        if (Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\KiloGuard") {
            Write-Host "  FAILED"
        } else {
            Write-Host "  OK"
        }
    } else {
        Write-Host "  (no registry)"
    }
} else {
    Write-Host "  (skipped)"
}
Write-Host ""

# Step 6: Delete driver file
Write-Host " [6] Deleting driver file ..."
if ($driverInstalled) {
    $sys = "$env:SystemRoot\System32\drivers\KiloGuard.sys"
    if (Test-Path $sys) {
        try {
            Remove-Item -Path $sys -Force -ErrorAction Stop
            Write-Host "  OK"
        } catch {
            Write-Host "  NEEDS REBOOT (file in use)"
        }
    } else {
        Write-Host "  (not found)"
    }
} else {
    Write-Host "  (skipped)"
}
Write-Host ""

# Step 7: Delete x64 output folder
Write-Host " [7] Deleting x64 output folder ..."
$x64 = "$PSScriptRoot\x64"
if (Test-Path $x64) {
    Remove-Item -Path $x64 -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $x64) {
        Write-Host "  NEEDS REBOOT (files in use)"
    } else {
        Write-Host "  OK"
    }
} else {
    Write-Host "  (not found)"
}
Write-Host ""

# Verification summary
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

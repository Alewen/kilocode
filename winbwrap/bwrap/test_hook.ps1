$ErrorActionPreference = "Stop"

function Run-Test($name, $script) {
    Write-Host "=== $name ==="
    try {
        & $script
        Write-Host "RESULT: SUCCESS (unexpected if this is a block test)"
    } catch {
        Write-Host "RESULT: BLOCKED/FAILED - $($_.Exception.Message)"
    }
    Write-Host ""
}

Run-Test "1. WMI (Get-WmiObject)" {
    Get-WmiObject Win32_Process | Select-Object -First 1 Name | Out-Null
    Write-Host "WMI query succeeded"
}

Run-Test "2. WMI (Get-CimInstance)" {
    Get-CimInstance Win32_Process | Select-Object -First 1 Name | Out-Null
    Write-Host "CIM query succeeded"
}

Run-Test "3. WScript.Shell COM" {
    $wsh = New-Object -ComObject WScript.Shell
    Write-Host "WScript.Shell created"
}

Run-Test "4. Shell.Application COM" {
    $sa = New-Object -ComObject Shell.Application
    Write-Host "Shell.Application created"
}

Run-Test "5. Schedule.Service COM" {
    $sched = New-Object -ComObject Schedule.Service
    Write-Host "Schedule.Service created"
}

Write-Host "=== 6. UAC runas ==="
try {
    Start-Process pwsh -Verb RunAs -ArgumentList "-NoProfile","-Command","exit 0" -Wait -ErrorAction Stop
    Write-Host "RESULT: SUCCESS (UAC elevation leaked through!)"
} catch {
    Write-Host "RESULT: BLOCKED - $($_.Exception.Message)"
}
Write-Host ""

Run-Test "7. Control: Scripting.FileSystemObject (should work)" {
    $fso = New-Object -ComObject Scripting.FileSystemObject
    $tmp = $fso.GetTempName()
    Write-Host "FileSystemObject OK, temp name: $tmp"
}

Write-Host "=== 8. Control: Basic PS (should work) ==="
Write-Output "hello world"
Write-Output (1 + 1)
Write-Host "RESULT: OK"
Write-Host ""

Write-Host "=== All tests done ==="

# ========================================================================================
# 这两种命令的本质区别在于它们触发的卸载类型不同，这直接影响驱动在卸载时能否“自我保护”。
#
# fltmc unload：非强制卸载（友好协商）
#       当你使用 fltmc unload 时，系统会调用驱动的 FilterUnloadCallback 回调函数。
#       这个函数可以让驱动在卸载前执行一些清理工作，并且可以选择拒绝卸载。
#       如果回调函数返回一个错误或警告状态（例如 STATUS_FLT_DO_NOT_DETACH），筛选器管理器（Filter Manager）就不会卸载该驱动。
#       这是一种“协商”式的卸载，驱动可以因为“正在处理任务”等原因说“不”。
#       fltmc unload $name # 2>&1 | Out-Null
#       if ($LASTEXITCODE -eq 0) {
#       } else {
#       }
#
# sc.exe stop：强制卸载（强制执行）
#       当你使用 sc.exe stop 时，同样会触发 FilterUnloadCallback 回调函数。
#       但关键区别在于，即使回调函数返回了拒绝卸载的错误代码，系统仍然会强制将该驱动从内存中卸载。
#       这意味着 sc.exe stop 拥有更高的权限，可以无视驱动的“反对意见”强制执行卸载。
#       不过，驱动开发者可以通过在注册时设置 FLTFL_REGISTRATION_DO_NOT_SUPPORT_SERVICE_STOP 标志，
#       来彻底禁用被 sc.exe stop 强制卸载的能力
# ========================================================================================
<#
.SYNOPSIS
    Manage the KiloGuard filter driver service.

.DESCRIPTION
    Start, stop, or change the startup type of the KiloGuard driver.
    Uses sc.exe (forceful unload) for stopping the driver.

.PARAMETER Start
    Start the driver.

.PARAMETER Stop
    Stop the driver using sc.exe stop (forceful unload).

.PARAMETER ManualStart
    Set the driver startup type to demand (manual). The driver will not start on boot.

.PARAMETER AutoStart
    Set the driver startup type to auto. The driver will start on boot.

.EXAMPLE
    .\manage.ps1 -Start

.EXAMPLE
    .\manage.ps1 -Stop

.EXAMPLE
    .\manage.ps1 -AutoStart
#>
[CmdletBinding(DefaultParameterSetName = "Start")]
param(
    [Parameter(ParameterSetName="Start")]
    [switch]$Start,
    [Parameter(ParameterSetName="Stop")]
    [switch]$Stop,
    [Parameter(ParameterSetName="ManualStart")]
    [switch]$ManualStart,
    [Parameter(ParameterSetName="AutoStart")]
    [switch]$AutoStart
)

$name = "KiloGuard"

$admin = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    throw "Please run as Administrator"
}

if (-not ($Start -or $Stop -or $ManualStart -or $AutoStart)) {
    Write-Host "=============================================================="
    Write-Host "Usage: .\manage.ps1 -Start       Start the driver"
    Write-Host "       .\manage.ps1 -Stop        Stop the driver"
    Write-Host "       .\manage.ps1 -ManualStart Set start type to demand (stopped after reboot)"
    Write-Host "       .\manage.ps1 -AutoStart   Set start type to auto (starts on boot)"
    Write-Host "=============================================================="
    sc.exe query $name
    Write-Host ""
    exit 0
}

if ($Start) {
    sc.exe start $name # 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "OK: $name started"
        Write-Host ""
        exit 0
    } else {
        $s = sc.exe query $name # 2>&1 | Select-String "RUNNING"
        if ($s) {
            Write-Host "OK: $name already running"
            Write-Host ""
        } else {
            Write-Host "FAILED: start $name (error $LASTEXITCODE)";
            Write-Host ""
            exit 1
        }
    }
}

if ($Stop) {
    sc.exe stop $name # 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "OK: $name stopped"
        Write-Host ""
        exit 0
    } else {
        $s = sc.exe query $name # 2>&1 | Select-String "STOPPED"
        if ($s) {
            Write-Host "OK: $name stopped"
            Write-Host ""
        } else {
            Write-Host "WARN: $name may need reboot to fully stop"
            Write-Host ""
        }
    }
}

if ($ManualStart) {
    sc.exe config $name start=demand # 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Host "OK: $name set to demand start (stopped after reboot)" }
    else { Write-Host "FAILED: set $name to demand (error $LASTEXITCODE)"; exit 1 }
}

if ($AutoStart) {
    sc.exe config $name start=auto # 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Host "OK: $name set to auto start (starts on boot)" }
    else { Write-Host "FAILED: set $name to auto (error $LASTEXITCODE)"; exit 1 }
}

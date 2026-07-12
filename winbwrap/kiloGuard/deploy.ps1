param(
    [string]$Src = $PSScriptRoot,
    [switch]$Auto,
    [double]$Altitude = 360000
    )

$sysFile   = "$Src\KiloGuard.sys"
$SYS       = "$env:SystemRoot\System32\drivers\KiloGuard.sys"
$CERT_PASS = "test"
$PFX       = "$Src\KiloGuardTest.pfx"

Write-Host "========================================"
Write-Host " KiloGuard Deploy"
Write-Host " 重要说明，在机上为部署这个驱动，必须执行 bcdedit /set testsigning on"
Write-Host " 而这个设置是否生效，可能被 BIOS 的安全启动(Security Boot)限制，需要将其关闭"
Write-Host "========================================"

$admin = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    throw "Please run as Administrator"
}
Write-Host " [1] Check run WindowsPrincipal OK"

if (-not (Test-Path $sysFile)) {
    Write-Host "KiloGuard.sys not found - please run build.ps1 first." -ForegroundColor Yellow
    Write-Host "Then run this script again."
    exit 1
}
Write-Host " [2] $sysFile is OK"

$svc = Get-Service -Name KiloGuard -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "FAILED: Driver service 'KiloGuard' already exists (state: $($svc.Status))." -ForegroundColor Red
    Write-Host "  Run cleanup.ps1 first to remove the existing driver." -ForegroundColor Yellow
    throw "Driver already exists"
}
Write-Host " [3] Driver service 'KiloGuard' is not exist OK"

#$existingFile = Test-Path "$SYS"
#if ($existingFile) {
#    Write-Host "FAILED: Driver file already exists at $SYS" -ForegroundColor Red
#    Write-Host "  Run cleanup.ps1 first to remove the existing driver." -ForegroundColor Yellow
#    throw "Driver file [ $SYS ] already exists"
#}
#Write-Host " [4] $SYS is not exist OK"

Copy-Item -Path $sysFile -Destination $SYS -Force
Write-Host " [4] Copying driver to $SYS OK"

$srcHash = (Get-FileHash -Path $sysFile -Algorithm SHA256).Hash
$dstHash = (Get-FileHash -Path $SYS -Algorithm SHA256).Hash
Write-Host "  $srcHash -- $sysFile"
Write-Host "  $dstHash -- $SYS"
if ($srcHash -ne $dstHash) {
    Write-Host "FAILED: Binary mismatch!"
    throw "Deployment aborted: binary mismatch between source and target"
}
Write-Host " [5] SHA256 match: $srcHash OK"

certutil -f -p $CERT_PASS -importpfx $PFX 2>&1 | Out-Null
certutil -addstore -f Root $PFX 2>&1 | Out-Null
Write-Host " [6] Installing certificate OK ==="

# 检查 Test Signing Mode（64 位系统加载测试签名驱动需要）
$startOpts = (Get-ItemPropertyValue -Path "HKLM:\SYSTEM\CurrentControlSet\Control" -Name "SystemStartOptions" -ErrorAction SilentlyContinue)
if (-not ($startOpts -match "TESTSIGNING")) {
    Write-Host ""
    Write-Host " WARNING: Test Signing Mode is OFF." -ForegroundColor Yellow
    Write-Host "  64-bit Windows 加载测试签名的驱动需要开启测试签名模式。" -ForegroundColor Yellow
    Write-Host "  请以管理员身份运行以下命令并重启：" -ForegroundColor Yellow
    Write-Host "    bcdedit /set testsigning on" -ForegroundColor Yellow
    Write-Host "    shutdown /r /t 0" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  [按回车键继续尝试加载驱动，或 Ctrl+C 中止]" -ForegroundColor Gray
    $null = Read-Host
}
Write-Host " [7] Test Signing Mode check OK"

$startType = if ($Auto) { "start=auto" } else { "start=demand" }
$r = sc.exe create KiloGuard type=kernel $startType binPath=$SYS 2>&1
if ($LASTEXITCODE -ne 0) { throw "FAILED: Service create error ($($r -join ' '))" }
reg add HKLM\SYSTEM\CurrentControlSet\Services\KiloGuard\Instances /v DefaultInstance /t REG_SZ /d "KiloGuard Instance" /f 2>&1 | Out-Null
reg add "HKLM\SYSTEM\CurrentControlSet\Services\KiloGuard\Instances\KiloGuard Instance" /v Altitude /t REG_SZ /d $Altitude /f 2>&1 | Out-Null
reg add "HKLM\SYSTEM\CurrentControlSet\Services\KiloGuard\Instances\KiloGuard Instance" /v Flags /t REG_DWORD /d 0 /f 2>&1 | Out-Null
Write-Host " [8] Creating service and altitude registry (altitude = $Altitude) OK"

$r = sc.exe start KiloGuard 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: Start error"
    sc.exe query KiloGuard
    throw "Start failed"
}
Write-Host " [9] Starting driver OK"

Write-Host "========================================"
Write-Host " Verification"
Write-Host "========================================"
sc.exe query KiloGuard
fltmc filters

Write-Host "========================================"
Write-Host " KiloGuard deployed successfully!"
Write-Host "========================================"

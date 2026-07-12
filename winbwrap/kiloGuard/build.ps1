param(
    [string]$Src = $PSScriptRoot,
    [switch]$Debug
    )

$WDK     = "C:\Program Files (x86)\Windows Kits\10"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: $vswhere" }
$VS = & $vswhere -latest -products * -property installationPath 2>&1
if (-not $VS) { throw "No Visual Studio installation found by vswhere" }

$msvcDir = Get-ChildItem "$VS\VC\Tools\MSVC" -Directory -ErrorAction Stop |
           Sort-Object Name -Descending | Select-Object -First 1
if (-not $msvcDir) { throw "No MSVC tools found under $VS\VC\Tools\MSVC" }
$MSVC = $msvcDir.FullName

$wdkVerDir = Get-ChildItem "$WDK\Include" -Directory -ErrorAction Stop |
             Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
             Sort-Object Name -Descending | Select-Object -First 1
if (-not $wdkVerDir) { throw "No WDK version found under $WDK\Include" }
$WDK_VER = $wdkVerDir.Name

$SIGNTOOL = "$WDK\bin\$WDK_VER\x64\signtool.exe"
if (-not (Test-Path $SIGNTOOL)) { throw "signtool.exe not found: $SIGNTOOL" }

Write-Host "=========== 原来固定设置 ==========="
Write-Host "VS        = C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
Write-Host "MSVC      = C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
Write-Host "WDK       = C:\Program Files (x86)\Windows Kits\10"
Write-Host "WDK_VER   = 10.0.26100.0"
Write-Host "SIGNTOOL  = C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
Write-Host "=========== 现在动态查找 ==========="
Write-Host "VS        = $VS"
Write-Host "MSVC      = $MSVC"
Write-Host "WDK       = $WDK"
Write-Host "WDK_VER   = $WDK_VER"
Write-Host "SIGNTOOL  = $SIGNTOOL"
Write-Host ""

$CERT_NAME = "CN=KiloGuard Test Cert"
$CERT_PASS = "test"
$PFX       = "$Src\KiloGuardTest.pfx"
$outName   = "KiloGuard.sys"

$env:Path    = "$MSVC\bin\Hostx64\x64;$env:Path"
$env:INCLUDE = "$WDK\Include\$WDK_VER\km;$WDK\Include\$WDK_VER\shared;$WDK\Include\$WDK_VER\um;$WDK\Include\$WDK_VER\ucrt;$MSVC\include"
$env:LIB     = "$WDK\Lib\$WDK_VER\km\x64"

$cflags = @(
    "/nologo", "/c", "/kernel", "/GS", "/sdl", "/W4", "/WX",
    "/wd4100", "/wd4189", "/wd4201", "/wd4324",
    "/guard:cf", "/guard:ehcont", "/Qspectre", "/O2", "/Ob2", "/Oi",
    "/GF", "/Gy", "/GR-", "/EHs-c-",
    "/DKERNEL_MODE", "/D_WIN64", "/D_AMD64_"
)

if ($Debug) {
    $cflags += "/DDEBUG", "/Od", "/Zi"
}

$lflags = @(
    "/nologo", "/subsystem:native", "/driver",
    "/DYNAMICBASE", "/INTEGRITYCHECK", "/CETCOMPAT",
    "/GUARD:CF", "/GUARD:EHCONT", "/NXCOMPAT",
    "/HIGHENTROPYVA", "/OPT:REF", "/OPT:ICF",
    "/DEBUG", "/INCREMENTAL:NO", "/nodefaultlib",
    "/entry:DriverEntry"
)

Write-Host "=== Building $outName ($(if ($Debug) {'Debug'} else {'Release'})) ==="

& cl.exe @cflags /Fo"$Src\FileGuard.obj" "$Src\FileGuard.c"
if ($LASTEXITCODE -ne 0) { throw "COMPILE FAILED" }

& cl.exe @cflags /Fo"$Src\Pidmap.obj" "$Src\Pidmap.c"
if ($LASTEXITCODE -ne 0) { throw "COMPILE FAILED" }

& cl.exe @cflags /Fo"$Src\Domain.obj" "$Src\Domain.c"
if ($LASTEXITCODE -ne 0) { throw "COMPILE FAILED" }

& cl.exe @cflags /Fo"$Src\ProcessGuard.obj" "$Src\ProcessGuard.c"
if ($LASTEXITCODE -ne 0) { throw "COMPILE FAILED" }

& cl.exe @cflags /Fo"$Src\PidPortMap.obj" "$Src\PidPortMap.c"
if ($LASTEXITCODE -ne 0) { throw "COMPILE FAILED" }

& cl.exe @cflags /Fo"$Src\RegGuard.obj" "$Src\RegGuard.c"
if ($LASTEXITCODE -ne 0) { throw "COMPILE FAILED" }

Write-Host "=== Linking KiloGuard.sys ==="
& link.exe @lflags fltMgr.lib fwpkclnt.lib ntoskrnl.lib hal.lib wdmsec.lib BufferOverflowK.lib /out:"$Src\$outName" "$Src\FileGuard.obj" "$Src\Pidmap.obj" "$Src\Domain.obj" "$Src\ProcessGuard.obj" "$Src\PidPortMap.obj" "$Src\RegGuard.obj"
if ($LASTEXITCODE -ne 0) { throw "LINK FAILED" }

Write-Host "=== [1] Build successful OK ==="

if (Test-Path $PFX) {
    Write-Host "  Certificate exists, skipping generation"
} else {
    Write-Host "  Generating self-signed certificate ..."
    $cert = New-SelfSignedCertificate -Subject $CERT_NAME -CertStoreLocation Cert:\LocalMachine\My -KeyUsage DigitalSignature -Type CodeSigningCert -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3") -NotAfter ([DateTime]::Now.AddYears(1))
    $pwd = ConvertTo-SecureString -String $CERT_PASS -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $PFX -Password $pwd | Out-Null
    if (-not (Test-Path $PFX)) { throw "FAILED: Certificate generation" }
}
Write-Host "=== [2] certificate OK ==="

Write-Host "=== [3] Signing drivers ==="
$r = & $SIGNTOOL sign /f $PFX /p $CERT_PASS /fd sha256 "$Src\$outName" 2>&1
if ($LASTEXITCODE -ne 0) { throw "FAILED: Sign KiloGuard error" }
Write-Host "=== [3] Signing driver OK ==="

Get-Item "$Src\$outName" | Select-Object Name, Length, LastWriteTime

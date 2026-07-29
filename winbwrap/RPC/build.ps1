param(
    [string]$Example = "",
    [switch]$Clean,
    [switch]$Run
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found. Visual Studio 2022 may not be installed."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    Write-Error "No VS2022 installation with VC tools found."
}
Write-Host "Found VS at: $vsPath" -ForegroundColor Cyan

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Write-Error "vcvars64.bat not found at $vcvars"
}

function Invoke-VCCommand {
    param([string]$Command)
    $full = "`"$vcvars`" > nul 2>&1 && $Command"
    & cmd /c $full
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Command failed with exit code $LASTEXITCODE`: $Command"
    }
}

function Build-Example {
    param([string]$DirName)

    $dir = Join-Path $ScriptDir $DirName
    if (-not (Test-Path $dir)) {
        Write-Warning "Directory not found: $dir"
        return
    }

    $outDir = Join-Path $dir "out"
    if ($Clean) {
        if (Test-Path $outDir) {
            Write-Host "Cleaning $DirName..." -ForegroundColor Yellow
            Remove-Item -Recurse -Force $outDir
        }
        return
    }

    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "Building: $DirName" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan

    New-Item -ItemType Directory -Path $outDir -Force | Out-Null

    $idlFiles = Get-ChildItem -Path $dir -Filter "*.idl"
    if (-not $idlFiles) {
        Write-Warning "No .idl file found in $dir"
        return
    }
    $idlBase = $idlFiles[0].BaseName

    Write-Host "[1/3] Generating RPC stubs from $idlBase.idl ..." -ForegroundColor Gray
    $idlPath = Join-Path $dir "$idlBase.idl"
    $acfPath = Join-Path $dir "$idlBase.acf"
    Push-Location $outDir
    try {
        Invoke-VCCommand "midl /nologo /x64 /header $idlBase.h /cstub $idlBase`_c.c /sstub $idlBase`_s.c `"$idlPath`" /acf `"$acfPath`" /out `"$outDir`""
    } finally {
        Pop-Location
    }
    $hFile = Join-Path $outDir "$idlBase.h"
    $cFile = Join-Path $outDir "$idlBase`_c.c"
    $sFile = Join-Path $outDir "$idlBase`_s.c"
    if (-not (Test-Path $hFile) -or -not (Test-Path $cFile) -or -not (Test-Path $sFile)) {
        Write-Error "MIDL output files not found. Check midl errors above."
    }
    Write-Host "  -> Generated: $idlBase.h, $idlBase`_c.c, $idlBase`_s.c" -ForegroundColor Green

    Write-Host "[2/3] Compiling server ..." -ForegroundColor Gray
    $serverStub = Join-Path $outDir "$idlBase`_s.c"
    $serverSrc = @(
        "`"$serverStub`"",
        "`"$(Join-Path $dir server.c)`"",
        "`"$(Join-Path $dir server_main.c)`""
    ) -join " "
    # If server_main.c doesn't exist, just use server.c (which may contain main)
    if (-not (Test-Path (Join-Path $dir "server_main.c"))) {
        $serverSrc = @(
            "`"$serverStub`"",
            "`"$(Join-Path $dir server.c)`""
        ) -join " "
    }
    Invoke-VCCommand "cl /nologo /W3 /Od /Zi /Fd`"$outDir\server.pdb`" /I `"$outDir`" /Fe`"$outDir\server.exe`" $serverSrc rpcrt4.lib"
    Write-Host "  -> server.exe built" -ForegroundColor Green

    Write-Host "[3/3] Compiling client ..." -ForegroundColor Gray
    $clientStub = Join-Path $outDir "$idlBase`_c.c"
    $clientSrc = @(
        "`"$clientStub`"",
        "`"$(Join-Path $dir client.c)`""
    ) -join " "
    Invoke-VCCommand "cl /nologo /W3 /Od /Zi /Fd`"$outDir\client.pdb`" /I `"$outDir`" /Fe`"$outDir\client.exe`" $clientSrc rpcrt4.lib"
    Write-Host "  -> client.exe built" -ForegroundColor Green

    Write-Host ""
    Write-Host "Build OK: $DirName" -ForegroundColor Green
    Write-Host "  Server: $outDir\server.exe"
    Write-Host "  Client: $outDir\client.exe"
    Write-Host ""
}

$allExamples = @(
    "01_BasicHelloWorld",
    "02_StructAndArray",
    "03_CallbackRPC"
)

if ($Example) {
    $matched = $allExamples | Where-Object { $_ -like "*$Example*" }
    if (-not $matched) {
        Write-Error "No example matched '$Example'. Available: $($allExamples -join ', ')"
    }
    foreach ($ex in $matched) { Build-Example $ex }
} else {
    foreach ($ex in $allExamples) { Build-Example $ex }
}

Write-Host "========================================" -ForegroundColor Green
Write-Host "All builds completed." -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green

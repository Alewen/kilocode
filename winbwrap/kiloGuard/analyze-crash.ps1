# .\analyze-crash.ps1
# .\analyze-crash.ps1 -File "C:\Windows\Minidump\xxx.dmp"
# .\analyze-crash.ps1 -Download
# .\analyze-crash.ps1 -File "C:\Windows\Minidump\xxx.dmp" -Download -o xxx.txt
param(
    [string]$File = "",         # 指定要分析的 dump 文件，不填则自动选最新的
    [string]$OutFile = "",      # 输出文件
    [switch]$Download           # 是否下载 PDB
)

$ErrorActionPreference = "Continue"

# C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe
# C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe
# C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\kd.exe
$kd = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe"
if (-not (Test-Path $kd)) {
    Write-Error "kd.exe not found at $kd"
    exit 1
}

if ([string]::IsNullOrEmpty($File)) {
    $latest = Get-ChildItem "C:\Windows\Minidump\*.dmp" -ErrorAction Stop |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $latest) {
        Write-Error "No dump file found in C:\Windows\Minidump\"
        exit 1
    }
    $File = $latest.FullName
}

Write-Host "Analyzing: $File"
if ($Download) {
    Write-Host "Mode: full analysis with symbol download (may take several minutes)"
} else {
    Write-Host "Mode: fast analysis (no symbol download)"
}
Write-Host "===================="

if ($Download) {
    $symPath = "srv*c:\symbols*https://msdl.microsoft.com/download/symbols"
    $commands = @(
        ".symfix c:\symbols",
        ".reload",
        "!analyze -v",
        "kv",
        "lmvm kiloguard",
        "q"
    ) -join ";"
    if (-not [string]::IsNullOrEmpty($OutFile)) {
        & $kd -z $File -y $symPath -c $commands 2>&1 | Out-File -FilePath $OutFile -Encoding UTF8
    } else {
        & $kd -z $File -y $symPath -c $commands 2>&1
    }
} else {
    $commands = @(
        ".symopt- 1",
        "!sysinfo machineid",
        "!analyze -show",
        "kv",
        "r",
        "lm k",
        "q"
    ) -join ";"
    if (-not [string]::IsNullOrEmpty($OutFile)) {
        & $kd -z $File -y "" -n -c $commands 2>&1 | Out-File -FilePath $OutFile -Encoding UTF8
    } else {
        & $kd -z $File -y "" -n -c $commands 2>&1
    }
}

if (-not [string]::IsNullOrEmpty($OutFile)) {
    Write-Host "Done. Report: $OutFile"
    Get-Content $OutFile
}
Write-Host "===================="

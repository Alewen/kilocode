$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent -Path $MyInvocation.MyCommand.Path
$ExtBinDir = "$env:USERPROFILE\.vscode\extensions\kilocode.kilo-code-7.4.5\bin"
$SrcDir = Join-Path $ScriptDir "winbwrap\bwrap\x64"

function Write-GroupHeader {
    param([string]$Name)
    if ($script:LastGroup -ne $Name) {
        if ($script:LastGroup) { Write-Host "" }
        Write-Host "  [$Name]" -ForegroundColor Cyan
        $script:LastGroup = $Name
    }
}

function Write-FileHash {
    param(
        [string]$DisplayName,
        [string]$FilePath
    )
    if (-not (Test-Path -LiteralPath $FilePath)) {
        Write-Host "    [缺失] $DisplayName" -ForegroundColor Red
        return
    }
    $Hash = Get-FileHash -LiteralPath $FilePath -Algorithm SHA256
    Write-Host "    $($Hash.Hash)  $DisplayName"
}

Write-Host "=========================================="
Write-Host " 计算 bwrap.exe / KiloHook.dll 的 SHA256"
Write-Host " 根目录: $ScriptDir"
Write-Host "=========================================="
Write-Host ""

$script:LastGroup = ""

$Targets = @(
    @{ Name = "bwrap.exe"; Items = @(
        @{ Display = "winbwrap\bwrap\x64\bwrap.exe"; Path = Join-Path $SrcDir "bwrap.exe"; SkipDir = $SrcDir },
        @{ Display = "packages\opencode\bin\winbwrap\bwrap.exe"; Path = Join-Path $ScriptDir "packages\opencode\bin\winbwrap\bwrap.exe" },
        @{ Display = "packages\opencode\dist\@kilocode\cli-windows-x64\bin\bwrap.exe"; Path = Join-Path $ScriptDir "packages\opencode\dist\@kilocode\cli-windows-x64\bin\bwrap.exe" },
        @{ Display = "packages\kilo-vscode\bin\bwrap.exe"; Path = Join-Path $ScriptDir "packages\kilo-vscode\bin\bwrap.exe" },
        @{ Display = "ext-bin\bwrap.exe"; Path = Join-Path $ExtBinDir "bwrap.exe"; SkipDir = $ExtBinDir }
    )},
    @{ Name = "KiloHook.dll"; Items = @(
        @{ Display = "winbwrap\bwrap\x64\KiloHook.dll"; Path = Join-Path $SrcDir "KiloHook.dll"; SkipDir = $SrcDir },
        @{ Display = "packages\opencode\bin\winbwrap\KiloHook.dll"; Path = Join-Path $ScriptDir "packages\opencode\bin\winbwrap\KiloHook.dll" },
        @{ Display = "packages\opencode\dist\@kilocode\cli-windows-x64\bin\KiloHook.dll"; Path = Join-Path $ScriptDir "packages\opencode\dist\@kilocode\cli-windows-x64\bin\KiloHook.dll" },
        @{ Display = "packages\kilo-vscode\bin\KiloHook.dll"; Path = Join-Path $ScriptDir "packages\kilo-vscode\bin\KiloHook.dll" },
        @{ Display = "ext-bin\KiloHook.dll"; Path = Join-Path $ExtBinDir "KiloHook.dll"; SkipDir = $ExtBinDir }
    )}
)

foreach ($Group in $Targets) {
    Write-GroupHeader $Group.Name
    foreach ($Item in $Group.Items) {
        if ($Item.SkipDir -and -not (Test-Path -LiteralPath $Item.SkipDir)) {
            continue
        }
        Write-FileHash -DisplayName $Item.Display -FilePath $Item.Path
    }
}

Write-Host ""
Write-Host "=========================================="

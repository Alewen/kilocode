
# 该脚本支持编译的 kilo 源码来自 gitclone https://github.com/Kilo-Org/kilocode.git v7.3.1
# 整个编译 vsix 的耗时大约 260 秒，前提是 bun install 已经在本地有缓存
# 用法: .\kilo-compile-windows-x64-vsix.ps1 [version]
#   不带参数：使用现有版本号编译
#   带参数：修改版本号后编译（例如: .\kilo-compile-windows-x64-vsix.ps1 7.3.16.1）
param(
    [string]$NewVersion
)

$ErrorActionPreference = "Stop"

# 获取脚本所在目录（源码根目录）
$ScriptDir = Split-Path -Parent -Path $MyInvocation.MyCommand.Path

<#
.SYNOPSIS
    同步 package.json 版本号。

.DESCRIPTION
    将指定 package.json 文件中的 version 字段更新为用户传入的新版本号，并使用 UTF-8 无 BOM 写回。

.PARAMETER FilePath
    需要更新的 package.json 文件路径。
#>
function Update-PackageVersion {
    param(
        [string]$FilePath
    )

    $Content = [System.IO.File]::ReadAllText($FilePath)
    $Updated = $Content -replace '"version": "[^"]*"', "`"version`": `"$NewVersion`""
    $Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($FilePath, $Updated, $Utf8NoBom)
}

# 如果提供了版本号参数，同步修改包版本
if ($NewVersion) {
    Write-Host "=========================================="
    Write-Host " 正在更新版本号到: $NewVersion"
    Write-Host "=========================================="

    Update-PackageVersion -FilePath (Join-Path -Path $ScriptDir -ChildPath "package.json")
    Write-Host "  更新: package.json"

    # 所有需要同步的包列表
    $Pkgs = @(
        "core",
        "kilo-web-ui",
        "llm",
        "plugin-atomic-chat",
        "http-recorder",
        "kilo-console",
        "kilo-vscode",
        "opencode",
        "kilo-gateway",
        "kilo-telemetry",
        "kilo-i18n",
        "kilo-ui",
        "kilo-indexing",
        "kilo-jetbrains",
        "ui",
        "sdk/js",
        "plugin",
        "script",
        "storybook",
        "kilo-docs",
        "kilo-vscode/tests",
        "upstream"
    )

    foreach ($Pkg in $Pkgs) {
        if ($Pkg -eq "upstream") {
            $PackagePath = Join-Path -Path $ScriptDir -ChildPath "script\upstream\package.json"
            if (Test-Path -Path $PackagePath) {
                Update-PackageVersion -FilePath $PackagePath
                Write-Host "  更新: script/upstream/package.json"
            }
            continue
        }

        $PackagePath = Join-Path -Path $ScriptDir -ChildPath "packages\$Pkg\package.json"
        if (Test-Path -Path $PackagePath) {
            Update-PackageVersion -FilePath $PackagePath
            Write-Host "  更新: packages/$Pkg/package.json"
        }
    }

    Write-Host "  版本同步完成"
}

Write-Host "=========================================="
Write-Host " 源码目录: $ScriptDir 打包 Kilo VS Code 扩展 (Windows-x64)"
Write-Host "=========================================="

# 步骤 1: 检查并安装根目录依赖
Write-Host "[1/5] 检查根目录依赖..."
Set-Location -Path $ScriptDir
Write-Host "  当前目录: $(Get-Location)"
Write-Host ""
if (-not (Test-Path -Path "node_modules")) {
    Write-Host "  正在安装依赖..."
    bun install
} else {
    Write-Host "  依赖已存在"
}

# 步骤 2: 编译 CLI 二进制
Write-Host "[2/5] 编译 CLI 二进制..."
Set-Location -Path (Join-Path -Path $ScriptDir -ChildPath "packages\opencode")
Write-Host "  当前目录: $(Get-Location)"
Write-Host ""
# 从根目录 package.json 获取版本号
$Version = (Select-String -Path (Join-Path -Path $ScriptDir -ChildPath "package.json") -Pattern '"version":').Line |
    Select-Object -First 1
$Version = ($Version -split '"')[3]
$env:KILO_RELEASE = "true"
$env:KILO_VERSION = $Version
$env:KILO_CHANNEL = "latest"
bun run script/build.ts --single

# 步骤 3: 编译 VS Code 扩展
Write-Host "[3/5] 编译扩展代码..."
Set-Location -Path (Join-Path -Path $ScriptDir -ChildPath "packages\kilo-vscode")
Write-Host "  当前目录: $(Get-Location)"
Write-Host ""
bun run prepare:cli-binary && bun run rebuild-sdk && bun run typecheck && bun run lint && node esbuild.js

# 步骤 4: 确保 CLI 二进制权限正确（Windows 不需要 chmod）
Write-Host "[4/5] 检查 CLI 二进制..."
Write-Host "  跳过权限设置（Windows 平台无需 chmod）"

# 步骤 5: 打包 VSIX
Write-Host "[5/5] 打包 VSIX..."
Write-Host "  当前目录: $(Get-Location)"
Write-Host ""
$VscePath = Join-Path -Path "node_modules" -ChildPath ".bin" | Join-Path -ChildPath "vsce"
if (Test-Path -Path $VscePath) {
    & $VscePath package --no-dependencies --skip-license --target win32-x64
} else {
    & .\node_modules\.bin\vsce package --no-dependencies --skip-license --target win32-x64
}

Write-Host "=========================================="
Write-Host " ? 打包完成！"
$VsixFiles = Get-ChildItem -Filter "kilo-code-*.vsix"
if ($VsixFiles) {
    foreach ($File in $VsixFiles) {
        Write-Host " ? 文件位置: $($File.FullName)"
    }
} else {
    Write-Host "  请在当前目录查找生成的 VSIX 文件"
}
Write-Host "=========================================="

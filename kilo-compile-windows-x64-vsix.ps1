
# 该脚本支持编译的 kilo 源码来自 gitclone https://github.com/Kilo-Org/kilocode.git v7.3.1
# 整个编译 vsix 的耗时大约 260 秒，前提是 bun install 已经在本地有缓存
$ErrorActionPreference = "Stop"

# 获取脚本所在目录（源码根目录）
$ScriptDir = Split-Path -Parent -Path $MyInvocation.MyCommand.Path

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
$env:KILO_RELEASE = "true"
bun run script/build.ts --single

# 步骤 3: 编译 VS Code 扩展
Write-Host "[3/5] 编译扩展代码..."
Set-Location -Path (Join-Path -Path $ScriptDir -ChildPath "packages\kilo-vscode")
Write-Host "  当前目录: $(Get-Location)"
Write-Host ""
bun run compile

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

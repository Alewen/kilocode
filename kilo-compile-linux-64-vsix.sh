#!/usr/bin/env bash
# 该脚本支持编译的 kilo 源码来自 gitclone https://github.com/Kilo-Org/kilocode.git v7.3.1
# 整个编译 vsix 的耗时大约 260 秒，前提是 bun install 已经在本地有缓存
set -e

# 获取脚本所在目录（源码根目录）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================="
echo " 源码目录: $SCRIPT_DIR 打包 Kilo VS Code 扩展 (Linux-x64)"
echo "=========================================="

# 步骤 1: 检查并安装根目录依赖
echo "[1/5] 检查根目录依赖..."
cd "$SCRIPT_DIR"
echo "  当前目录: $(pwd)"
echo ""
if [ ! -d "node_modules" ]; then
  echo "  正在安装依赖..."
  bun install
else
  echo "  依赖已存在"
fi

# 步骤 2: 编译 CLI 二进制
echo "[2/5] 编译 CLI 二进制..."
cd "$SCRIPT_DIR/packages/opencode"
echo "  当前目录: $(pwd)"
echo ""
# 从根目录 package.json 获取版本号
VERSION=$(grep -E '"version":' "$SCRIPT_DIR/package.json" | head -1 | cut -d'"' -f4)
export KILO_RELEASE=true
export KILO_VERSION=$VERSION
export KILO_CHANNEL=latest
bun run script/build.ts --single

# 步骤 3: 编译 VS Code 扩展
echo "[3/5] 编译扩展代码..."
cd "$SCRIPT_DIR/packages/kilo-vscode"
echo "  当前目录: $(pwd)"
echo ""
bun run compile

# 步骤 4: 确保 CLI 二进制权限正确
echo "[4/5] 检查 CLI 二进制..."
cd "$SCRIPT_DIR/packages/kilo-vscode"
echo "  当前目录: $(pwd)"
echo ""
chmod +x bin/kilo

# 步骤 5: 打包 VSIX
echo "[5/5] 打包 VSIX..."
cd "$SCRIPT_DIR/packages/kilo-vscode"
echo "  当前目录: $(pwd)"
echo ""
./node_modules/.bin/vsce package --no-dependencies --skip-license --target linux-x64

echo "=========================================="
echo " ✅ 打包完成！"
echo -n " ✅ 文件位置: $(pwd)/"
echo $(ls kilo-code-*.vsix 2>/dev/null || echo "  请在当前目录查找生成的 VSIX 文件")
echo "=========================================="

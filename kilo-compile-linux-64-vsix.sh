#!/usr/bin/env bash
# 该脚本支持编译的 kilo 源码来自 gitclone https://github.com/Kilo-Org/kilocode.git
# 用法: ./kilo-compile-linux-64-vsix.sh [version]
#   不带参数：使用现有版本号编译
#   带参数：修改版本号后编译（例如: ./kilo-compile-linux-64-vsix.sh 7.3.16.1）
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 如果提供了版本号参数，同步修改包版本
if [ -n "$1" ]; then
    NEW_VERSION="$1"
    echo "=========================================="
    echo " 正在更新版本号到: $NEW_VERSION"
    echo "=========================================="
    # 修改根目录版本
    sed -i "s/\"version\": \"[^\"]*\"/\"version\": \"$NEW_VERSION\"/" "$SCRIPT_DIR/package.json" && echo "  更新: package.json"
    # 所有需要同步的包列表
    PKGS="core kilo-vscode opencode kilo-gateway kilo-telemetry kilo-i18n kilo-ui kilo-indexing kilo-jetbrains ui sdk/js plugin script storybook kilo-docs kilo-vscode/tests upstream"
    for pkg in $PKGS; do
        if [ -f "$SCRIPT_DIR/packages/$pkg/package.json" ]; then
            sed -i "s/\"version\": \"[^\"]*\"/\"version\": \"$NEW_VERSION\"/" "$SCRIPT_DIR/packages/$pkg/package.json" && echo "  更新: packages/$pkg/package.json"
        elif [ "$pkg" = "upstream" ]; then
            sed -i "s/\"version\": \"[^\"]*\"/\"version\": \"$NEW_VERSION\"/" "$SCRIPT_DIR/script/upstream/package.json" 2>/dev/null && echo "  更新: script/upstream/package.json"
        fi
    done
    echo "  版本同步完成"
fi

echo "=========================================="
echo " 源码目录: $SCRIPT_DIR 打包 Kilo VS Code 扩展 (Linux-x64)"
echo "=========================================="

# 步骤 1: 检查并安装根目录依赖
echo "[1/4] 检查根目录依赖..."
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
#echo "[2/5] 编译 CLI 二进制..."
#cd "$SCRIPT_DIR/packages/opencode"
#echo "  当前目录: $(pwd)"
#echo ""
# 从根目录 package.json 获取版本号
VERSION=$(grep -E '"version":' "$SCRIPT_DIR/package.json" | head -1 | cut -d'"' -f4)
export KILO_RELEASE=true
export KILO_VERSION=$VERSION
export KILO_CHANNEL=latest
#bun run script/build.ts --single

# 步骤 2: 编译 VS Code 扩展
echo "[2/4] 编译扩展代码..."
cd "$SCRIPT_DIR/packages/kilo-vscode"
echo "  当前目录: $(pwd)"
echo ""
bun run compile

# 步骤 3: 确保 CLI 二进制权限正确
echo "[3/4] 检查 CLI 二进制..."
cd "$SCRIPT_DIR/packages/kilo-vscode"
echo "  当前目录: $(pwd)"
echo ""
chmod +x bin/kilo

# 步骤 4: 打包 VSIX
echo "[4/4] 打包 VSIX..."
cd "$SCRIPT_DIR/packages/kilo-vscode"
echo "  当前目录: $(pwd)"
echo ""
./node_modules/.bin/vsce package --no-dependencies --skip-license --target linux-x64

echo "=========================================="
echo " ✅ 打包完成！"
echo -n " ✅ 文件位置: $(pwd)/"
echo $(ls kilo-code-*.vsix 2>/dev/null || echo "  请在当前目录查找生成的 VSIX 文件")
echo "=========================================="

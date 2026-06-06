#!/bin/bash
# kilo-edit.sh - 模拟 kilo 插件执行 edit 工具调用
# 用法: ./kilo-edit.sh <edit.json> [level]
#   level: 1-9, 控制匹配策略的宽松程度 (默认 1 = 仅精确匹配, 9 = 完全模拟 kilo)
# 支持通过软链接调用，自动解析真实路径
# JSON 格式: { "filePath": "/path/to/file", "oldString": "...", "newString": "...", "replaceAll": false }

# 解析脚本真实路径（支持软链接）
if command -v realpath &>/dev/null; then
  SCRIPT_REAL="$(realpath "$0")"
elif command -v readlink &>/dev/null && readlink -f "$0" &>/dev/null 2>&1; then
  SCRIPT_REAL="$(readlink -f "$0")"
else
  SCRIPT_REAL="$0"
fi
SCRIPT_DIR="$(dirname "$SCRIPT_REAL")"

if [ $# -lt 1 ]; then
  echo "用法: $0 <edit.json> [level]"
  echo "  level: 1-9, 匹配策略宽松程度 (默认 1)"
  echo "    1 = 仅精确匹配"
  echo "    2 = 精确 + 行尾空格容忍"
  echo "    3 = + 首尾锚点匹配"
  echo "    4 = + 空白归一化"
  echo "    5 = + 忽略缩进差异"
  echo "    6 = + 转义字符处理"
  echo "    7 = + 边界 trim"
  echo "    8 = + 上下文感知"
  echo "    9 = 完全模拟 kilo (全部 9 种策略)"
  echo "JSON 格式: { \"filePath\": \"/path/to/file\", \"oldString\": \"...\", \"newString\": \"...\" }"
  exit 1
fi

# 将 JSON 文件转为绝对路径（处理软链接场景下相对路径的问题）
JSON_FILE="$1"
if [ "${JSON_FILE:0:1}" != "/" ]; then
  JSON_FILE="$(cd "$(dirname "$JSON_FILE")" 2>/dev/null && pwd)/$(basename "$JSON_FILE")"
fi
LEVEL="${2:-1}"

if [ ! -f "$JSON_FILE" ]; then
  echo "错误: 找不到文件: $JSON_FILE"
  exit 1
fi

bun run "$SCRIPT_DIR/kilo-edit-impl.js" "$JSON_FILE" "$LEVEL"

#!/bin/bash
# kilo-write.sh - 模拟 kilo 插件执行 write 工具调用
# 用法: ./kilo-write.sh <write.json>
# 支持通过软链接调用，自动解析真实路径
# JSON 格式: { "filePath": "/path/to/file", "content": "content" }

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
  echo "用法: $0 <write.json>"
  echo "JSON 格式: { \"filePath\": \"/path/to/file\", \"content\": \"content\" }"
  exit 1
fi

# 将 JSON 文件转为绝对路径（处理软链接场景下相对路径的问题）
JSON_FILE="$1"
if [ "${JSON_FILE:0:1}" != "/" ]; then
  JSON_FILE="$(cd "$(dirname "$JSON_FILE")" 2>/dev/null && pwd)/$(basename "$JSON_FILE")"
fi

if [ ! -f "$JSON_FILE" ]; then
  echo "错误: 找不到文件: $JSON_FILE"
  exit 1
fi

# 优先用 python3，否则用 bun
if command -v python3 &>/dev/null; then
  exec python3 "$SCRIPT_DIR/kilo-write.py" "$JSON_FILE"
elif command -v bun &>/dev/null; then
  exec bun run "$SCRIPT_DIR/kilo-write-impl.js" "$JSON_FILE"
else
  echo "错误: 需要 python3 或 bun 运行时"
  exit 1
fi

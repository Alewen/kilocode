#!/bin/bash
# kilo-write.sh - 模拟 kilo 插件执行 write 工具调用
# 用法: ./kilo-write.sh <write.json>
# JSON 格式: { "filePath": "/path/to/file", "content": "content" }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

if [ $# -lt 1 ]; then
  echo "用法: $0 <write.json>"
  echo "JSON 格式: { \"filePath\": \"/path/to/file\", \"content\": \"content\" }"
  exit 1
fi

JSON_FILE="$1"

if [ ! -f "$JSON_FILE" ]; then
  echo "错误: 找不到文件: $JSON_FILE"
  exit 1
fi

bun run "$SCRIPT_DIR/kilo-write-impl.js" "$JSON_FILE"

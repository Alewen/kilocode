#!/bin/bash
# kilo-write-py.sh - Python 版本的 write 工具（独立，不依赖 bun/Node.js）
# 用法: ./kilo-write-py.sh <write.json>
SCRIPT_REAL="$(realpath "$0" 2>/dev/null || readlink -f "$0" 2>/dev/null || echo "$0")"
SCRIPT_DIR="$(dirname "$SCRIPT_REAL")"
JSON_FILE="$1"
[ "${JSON_FILE:0:1}" != "/" ] && JSON_FILE="$(cd "$(dirname "$JSON_FILE")" 2>/dev/null && pwd)/$(basename "$JSON_FILE")"
exec python3 "$SCRIPT_DIR/kilo-write.py" "$JSON_FILE"

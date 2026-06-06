#!/bin/bash

# 查询 Kilo 会话消息
# 用法: ./kilomessage.sh <session_id> [-L <limit>]

if [ $# -lt 1 ]; then
    echo "错误: 必须提供 session_id"
    echo "用法: $0 <session_id> [-L <limit>]"
    exit 1
fi

SESSION_ID="$1"
LIMIT=""

# 解析剩余参数
shift
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -L|--Limit) LIMIT="$2"; shift ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
    shift
done

# Kilo 数据库路径 (Linux)
KILO_DB="$HOME/.local/share/kilo/kilo.db"

if [ -n "$LIMIT" ]; then
    echo "=== Kilo Session Messages: $SESSION_ID (Last $LIMIT) ==="
    echo ""
    sqlite3 -header -column "$KILO_DB" "SELECT id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, datetime(time_updated/1000, 'unixepoch', 'localtime') as updated, data as data FROM message WHERE session_id = '$SESSION_ID' ORDER BY time_created DESC LIMIT $LIMIT;"
    echo ""
    echo "Showing last $LIMIT messages"
else
    echo "=== Kilo Session Messages: $SESSION_ID ==="
    echo ""
    sqlite3 -header -column "$KILO_DB" "SELECT id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, datetime(time_updated/1000, 'unixepoch', 'localtime') as updated, data as data FROM message WHERE session_id = '$SESSION_ID' ORDER BY time_created DESC;"
    echo ""
    echo "Showing all messages"
fi

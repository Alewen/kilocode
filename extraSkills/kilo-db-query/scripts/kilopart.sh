#!/bin/bash

# 查询 Kilo 会话 parts
# 用法: ./kilopart.sh <session_id> [filter] [-L <limit>]

if [ $# -lt 1 ]; then
    echo "错误: 必须提供 session_id"
    echo "用法: $0 <session_id> [filter] [-L <limit>]"
    exit 1
fi

SESSION_ID="$1"
FILTER=""
LIMIT=""

# 解析剩余参数
shift
while [[ "$#" -gt 0 ]]; do
    if [ "$1" = "-L" ] || [ "$1" = "--Limit" ]; then
        LIMIT="$2"
        shift
    else
        FILTER="$1"
    fi
    shift
done

# Kilo 数据库路径 (Linux)
KILO_DB="$HOME/.local/share/kilo/kilo.db"

if [ -z "$FILTER" ]; then
    if [ -n "$LIMIT" ]; then
        echo "=== Kilo Session Parts: $SESSION_ID (Last $LIMIT) ==="
        echo ""
        sqlite3 -header -column "$KILO_DB" "SELECT id, message_id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, data as data FROM part WHERE session_id = '$SESSION_ID' ORDER BY time_created DESC LIMIT $LIMIT;"
        echo ""
        echo "Showing last $LIMIT parts"
    else
        echo "=== Kilo Session Parts: $SESSION_ID ==="
        echo ""
        sqlite3 -header -column "$KILO_DB" "SELECT id, message_id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, data as data FROM part WHERE session_id = '$SESSION_ID' ORDER BY time_created DESC;"
        echo ""
        echo "Showing all parts"
    fi
else
    if [ -n "$LIMIT" ]; then
        echo "=== Kilo Session Parts: $SESSION_ID (Filter: $FILTER, Last $LIMIT) ==="
        echo ""
        sqlite3 -header -column "$KILO_DB" "SELECT id, message_id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, data as data FROM part WHERE session_id = '$SESSION_ID' AND data LIKE '%$FILTER%' ORDER BY time_created DESC LIMIT $LIMIT;"
        echo ""
        echo "Showing last $LIMIT parts matching: $FILTER"
    else
        echo "=== Kilo Session Parts: $SESSION_ID (Filter: $FILTER) ==="
        echo ""
        sqlite3 -header -column "$KILO_DB" "SELECT id, message_id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, data as data FROM part WHERE session_id = '$SESSION_ID' AND data LIKE '%$FILTER%' ORDER BY time_created DESC;"
        echo ""
        echo "Showing all parts matching: $FILTER"
    fi
fi

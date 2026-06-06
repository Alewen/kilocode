#!/bin/bash

# 查询 Kilo 会话列表
# 用法: ./kilosessions.sh [-L <limit>]

LIMIT=""

# 解析命令行参数
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
    echo "=== Kilo Session List (Last $LIMIT) ==="
    echo ""
    sqlite3 -header -column "$KILO_DB" "SELECT id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, datetime(time_updated/1000, 'unixepoch', 'localtime') as updated, directory, path FROM session ORDER BY time_updated DESC LIMIT $LIMIT;"
    echo ""
    echo "Showing last $LIMIT sessions"
else
    echo "=== Kilo Session List ==="
    echo ""
    sqlite3 -header -column "$KILO_DB" "SELECT id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, datetime(time_updated/1000, 'unixepoch', 'localtime') as updated, directory, path FROM session ORDER BY time_updated DESC;"
    echo ""
    echo "Showing all sessions"
fi

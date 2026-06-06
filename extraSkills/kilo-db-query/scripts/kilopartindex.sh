#!/bin/bash

# 查询 Kilo 会话 parts
# 用法: ./kilopartindex.sh <session_id> [index]
#   - 只输入 session_id: 显示该会话的记录条数
#   - 输入 session_id + 数字 N: 输出按时间排序的第 N 条数据的 JSON 格式

KILO_DB="$HOME/.local/share/kilo/kilo.db"

if [ $# -eq 1 ]; then
    # 一个参数：显示记录条数
    SESSION_ID="$1"
    COUNT=$(sqlite3 "$KILO_DB" "SELECT COUNT(*) FROM part WHERE session_id = '$SESSION_ID';")
    echo "$COUNT"
elif [ $# -eq 2 ]; then
    # 两个参数：输出第 N 条数据的 JSON
    SESSION_ID="$1"
    INDEX="$2"
    
    # 验证第二个参数是否为正整数
    if ! [[ "$INDEX" =~ ^[1-9][0-9]*$ ]]; then
        echo "错误: 第二个参数必须是正整数" >&2
        exit 1
    fi
    
    # 计算偏移量（SQLite LIMIT/OFFSET 从 0 开始）
    OFFSET=$((INDEX - 1))
    
    # 查询数据并输出为 JSON 格式
    sqlite3 "$KILO_DB" "SELECT data FROM part WHERE session_id = '$SESSION_ID' ORDER BY time_created ASC LIMIT 1 OFFSET $OFFSET;"
else
    echo "用法: $0 <session_id> [index]"
    echo "  - 只输入 session_id: 显示该会话的记录条数"
    echo "  - 输入 session_id + 数字 N: 输出按时间排序的第 N 条数据的 JSON 格式"
    exit 1
fi

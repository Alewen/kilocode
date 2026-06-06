#!/bin/bash

# 删除 Kilo 数据库中指定 session_id 相关的全部信息
# 用法: ./kiloremove.sh <session_id>

if [ $# -lt 1 ]; then
    echo "错误: 必须提供 session_id"
    echo "用法: $0 <session_id>"
    exit 1
fi

SESSION_ID="$1"

# Kilo 数据库路径 (Linux)
KILO_DB="$HOME/.local/share/kilo/kilo.db"

# 检查数据库文件是否存在
if [ ! -f "$KILO_DB" ]; then
    echo "错误: 数据库文件不存在: $KILO_DB"
    exit 1
fi

# 开始删除操作
echo "正在删除 session_id: $SESSION_ID 的数据..."

# 先删除 part 表
PART_COUNT=$(sqlite3 "$KILO_DB" "SELECT COUNT(*) FROM part WHERE session_id = '$SESSION_ID';")
sqlite3 "$KILO_DB" "DELETE FROM part WHERE session_id = '$SESSION_ID';"
echo "已删除 part 表记录: $PART_COUNT 条"

# 删除 session_message 表
SESSION_MESSAGE_COUNT=$(sqlite3 "$KILO_DB" "SELECT COUNT(*) FROM session_message WHERE session_id = '$SESSION_ID';")
sqlite3 "$KILO_DB" "DELETE FROM session_message WHERE session_id = '$SESSION_ID';"
echo "已删除 session_message 表记录: $SESSION_MESSAGE_COUNT 条"

# 删除 session_share 表
SESSION_SHARE_COUNT=$(sqlite3 "$KILO_DB" "SELECT COUNT(*) FROM session_share WHERE session_id = '$SESSION_ID';")
sqlite3 "$KILO_DB" "DELETE FROM session_share WHERE session_id = '$SESSION_ID';"
echo "已删除 session_share 表记录: $SESSION_SHARE_COUNT 条"

# 删除 todo 表
TODO_COUNT=$(sqlite3 "$KILO_DB" "SELECT COUNT(*) FROM todo WHERE session_id = '$SESSION_ID';")
sqlite3 "$KILO_DB" "DELETE FROM todo WHERE session_id = '$SESSION_ID';"
echo "已删除 todo 表记录: $TODO_COUNT 条"

# 再删除 message 表
MESSAGE_COUNT=$(sqlite3 "$KILO_DB" "SELECT COUNT(*) FROM message WHERE session_id = '$SESSION_ID';")
sqlite3 "$KILO_DB" "DELETE FROM message WHERE session_id = '$SESSION_ID';"
echo "已删除 message 表记录: $MESSAGE_COUNT 条"

# 最后删除 session 表
SESSION_COUNT=$(sqlite3 "$KILO_DB" "SELECT COUNT(*) FROM session WHERE id = '$SESSION_ID';")
sqlite3 "$KILO_DB" "DELETE FROM session WHERE id = '$SESSION_ID';"
echo "已删除 session 表记录: $SESSION_COUNT 条"

echo "删除操作完成！"

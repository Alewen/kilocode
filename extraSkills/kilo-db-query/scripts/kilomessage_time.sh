#!/bin/bash

# 查询 Kilo 会话消息（按时间范围过滤）
# 用法: ./kilomessage_time.sh <session_id> <start_time> <end_time>
# 时间格式: "2026-06-05 12:00:01" / "06-05 12:00:01" / "05 12:00:01" / "12:00:01"

if [ $# -lt 3 ]; then
    echo "错误: 需要 3 个参数"
    echo "用法: $0 <session_id> <start_time> <end_time>"
    echo "示例: $0 ses_xxx \"2026-06-05 12:00:01\" \"2026-06-05 14:00:00\""
    echo "简写: $0 ses_xxx \"06-05 12:00:01\" \"14:00:00\""
    exit 1
fi

SESSION_ID="$1"
START_RAW="$2"
END_RAW="$3"
KILO_DB="$HOME/.local/share/kilo/kilo.db"

# 将时间字符串补全为完整 "YYYY-MM-DD HH:MM:SS" 格式
normalize_time() {
    local raw="$1"
    local now_date
    now_date=$(date +"%Y-%m-%d")
    local now_year=$(date +"%Y")
    local now_month=$(date +"%m")

    # 统计空格数量来判断格式
    local spaces
    spaces=$(echo "$raw" | tr -cd ' ' | wc -c)

    if [ "$spaces" -eq 1 ]; then
        # 格式: "YYYY-MM-DD HH:MM:SS" 或 "MM-DD HH:MM:SS"
        local date_part="${raw%% *}"
        local time_part="${raw##* }"
        local dashes
        dashes=$(echo "$date_part" | tr -cd '-' | wc -c)
        if [ "$dashes" -eq 2 ]; then
            # 完整 "YYYY-MM-DD HH:MM:SS"
            echo "$raw"
        elif [ "$dashes" -eq 1 ]; then
            # "MM-DD HH:MM:SS" → 补年份
            echo "${now_year}-${raw}"
        else
            echo "$raw"
        fi
    elif [ "$spaces" -eq 0 ]; then
        # 格式: "HH:MM:SS" → 补年月日
        echo "${now_date} ${raw}"
    else
        # 格式: "DD HH:MM:SS" → 补年月
        local day_part="${raw%% *}"
        local time_part="${raw#* }"
        echo "${now_year}-${now_month}-${day_part} ${time_part}"
    fi
}

START_TIME=$(normalize_time "$START_RAW")
END_TIME=$(normalize_time "$END_RAW")

# 验证时间格式
if ! date -d "$START_TIME" +%s >/dev/null 2>&1; then
    echo "错误: 开始时间格式无效: $START_RAW → $START_TIME"
    exit 1
fi
if ! date -d "$END_TIME" +%s >/dev/null 2>&1; then
    echo "错误: 结束时间格式无效: $END_RAW → $END_TIME"
    exit 1
fi

START_TS=$(date -d "$START_TIME" +%s)
END_TS=$(date -d "$END_TIME" +%s)

echo "=== Kilo Session Messages: $SESSION_ID ==="
echo "时间范围: $START_TIME ~ $END_TIME"
echo ""

sqlite3 -header -column "$KILO_DB" \
  "SELECT id, datetime(time_created/1000, 'unixepoch', 'localtime') as created, datetime(time_updated/1000, 'unixepoch', 'localtime') as updated, substr(data, 1, 200) as preview FROM message WHERE session_id = '$SESSION_ID' AND time_updated/1000 >= $START_TS AND time_updated/1000 <= $END_TS ORDER BY time_updated ASC;"

echo ""
echo "查询完成"

#!/usr/bin/env python3
import sqlite3
import json
import sys
import os

KILO_DB = os.path.expanduser("~/.local/share/kilo/kilo.db")

def is_valid_json(data):
    try:
        json.loads(data)
        return True
    except:
        return False

def check_table(table_name):
    conn = sqlite3.connect(KILO_DB)
    cursor = conn.cursor()
    
    cursor.execute(f"SELECT id, data FROM {table_name}")
    rows = cursor.fetchall()
    
    print(f"\n=== 检查 {table_name} 表 ===")
    print(f"✅ 总记录数: {len(rows)}")

    invalid = []
    for row in rows:
        record_id, data = row
        if not is_valid_json(data):
            invalid.append(record_id)
    
    if invalid:
        print(f"❌ 发现 {len(invalid)} 条无效 JSON 记录:")
        for record_id in invalid:
            print(f"   - {record_id}")
    else:
        print(f"✅ 所有记录的 data 字段都是有效 JSON")
    
    conn.close()

if __name__ == "__main__":
    check_table("message")
    check_table("part")


#!/usr/bin/env python3
"""Kilo 对话历史查看器 - 后端 API 服务

通过 SQLite 查询 Kilo 数据库，提供 REST API 给前端页面使用。
无需额外依赖，仅使用 Python 标准库。
"""

import http.server
import json
import os
import sqlite3
import urllib.parse
from pathlib import Path

KILO_DB = os.environ.get("KILO_DB_PATH") or os.path.expanduser("~/.local/share/kilo/kilo.db")
STATIC_DIR = Path(__file__).parent


def db():
    conn = sqlite3.connect(KILO_DB)
    conn.row_factory = sqlite3.Row
    return conn


def get_sessions():
    """获取所有 session 列表，按时间倒序"""
    conn = db()
    rows = conn.execute("""
        SELECT id, title, directory, time_created, time_updated, parent_id
        FROM session
        ORDER BY time_updated DESC
    """).fetchall()
    conn.close()
    return [
        {
            "id": r["id"],
            "title": r["title"],
            "directory": r["directory"],
            "created": r["time_created"],
            "updated": r["time_updated"],
            "parent_id": r["parent_id"],
        }
        for r in rows
    ]


def get_messages(session_id):
    """获取指定 session 的所有消息及其 parts，按时间正序"""
    conn = db()
    msgs = conn.execute("""
        SELECT id, data, time_created
        FROM message
        WHERE session_id = ?
        ORDER BY time_created ASC
    """, (session_id,)).fetchall()
    conn.close()

    result = []
    for m in msgs:
        parsed = json.loads(m["data"])
        role = parsed.get("role", "unknown")
        # 处理模型信息：用户消息在 model.modelID 中，AI消息在 modelID 中
        model = ""
        if "model" in parsed and isinstance(parsed["model"], dict):
            model = parsed["model"].get("modelID", "")
        if not model and "modelID" in parsed:
            model = parsed.get("modelID", "")
        msg_data = {
            "id": m["id"],
            "role": role,
            "time_created": m["time_created"],
            "model": model,
            "agent": parsed.get("agent", ""),
            "parts": get_parts(m["id"]),
        }
        # 添加 error 和 finish 字段
        if "error" in parsed:
            msg_data["error"] = parsed["error"]
        if "finish" in parsed:
            msg_data["finish"] = parsed["finish"]
        result.append(msg_data)
    return result


def get_parts(message_id):
    """获取指定消息的所有 parts，按时间正序"""
    conn = db()
    parts = conn.execute("""
        SELECT id, data, time_created
        FROM part
        WHERE message_id = ?
        ORDER BY time_created ASC
    """, (message_id,)).fetchall()
    conn.close()

    result = []
    for p in parts:
        parsed = json.loads(p["data"])
        ptype = parsed.get("type", "unknown")
        entry = {
            "id": p["id"],
            "type": ptype,
            "time_created": p["time_created"],
        }

        if ptype == "text":
            entry["text"] = parsed.get("text", "")
        elif ptype == "reasoning":
            entry["text"] = parsed.get("text", "")
        elif ptype == "tool":
            entry["tool"] = parsed.get("tool", "")
            state = parsed.get("state", {})
            entry["status"] = state.get("status", "")
            entry["error"] = state.get("error", "")
            entry["reason"] = parsed.get("reason", "")
            inp = state.get("input", {})
            if isinstance(inp, dict) and len(inp) > 0:
                entry["toolInput"] = json.dumps(inp, ensure_ascii=False, indent=2)
            if entry["tool"] == "question":
                questions = inp.get("questions", [])
                meta = state.get("metadata", {})
                answers = meta.get("answers", [])
                entry["questions"] = questions
                entry["answers"] = answers
            if entry["tool"] == "task":
                meta = state.get("metadata", {})
                if "sessionId" in meta:
                    entry["taskId"] = meta["sessionId"]
                if "subagent_type" in inp:
                    entry["subagentType"] = inp["subagent_type"]
            if entry["tool"] == "todowrite":
                meta = state.get("metadata", {})
                if "todos" in meta:
                    entry["todos"] = meta["todos"]
                elif "todos" in inp:
                    entry["todos"] = inp["todos"]
            if "command" in inp:
                entry["command"] = inp.get("command", "")
            if "description" in inp:
                entry["description"] = inp.get("description", "")
            if "filePath" in inp:
                entry["filePath"] = inp.get("filePath", "")
            if "offset" in inp:
                entry["offset"] = inp.get("offset")
            if "limit" in inp:
                entry["limit"] = inp.get("limit")
            if "content" in inp:
                entry["content"] = inp.get("content", "")
            if "pattern" in inp:
                entry["pattern"] = inp.get("pattern", "")
            if "path" in inp:
                entry["path"] = inp.get("path", "")
            if "include" in inp:
                entry["include"] = inp.get("include", "")
            if "type" in inp and entry["tool"] == "grep":
                entry["grepType"] = inp.get("type", "")
            out = state.get("output", "")
            if out:
                entry["output"] = out
            meta = state.get("metadata", {})
            if "diff" in meta:
                entry["diff"] = meta["diff"]
            filediff = meta.get("filediff", {})
            if "additions" in filediff:
                entry["additions"] = filediff["additions"]
            if "deletions" in filediff:
                entry["deletions"] = filediff["deletions"]
            if "diff" in parsed:
                entry["diff"] = parsed["diff"]
            if "title" in parsed:
                entry["title"] = parsed["title"]
            if "title" in state:
                entry["title"] = state["title"]
        elif ptype == "step-start":
            entry["text"] = "开始执行..."
        elif ptype == "step-finish":
            entry["reason"] = parsed.get("reason", "")
            entry["tokens"] = parsed.get("tokens", {})
        elif ptype == "compaction":
            entry["text"] = "[对话压缩]"

        result.append(entry)
    return result


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(STATIC_DIR), **kwargs)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/api/sessions":
            self.send_json(get_sessions())
        elif path.startswith("/api/sessions/") and path.endswith("/messages"):
            session_id = path.split("/")[3]
            self.send_json(get_messages(session_id))
        else:
            super().do_GET()

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()

    def send_json(self, data):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


def main():
    port = 8080
    print(f"Kilo Viewer API 服务启动: http://0.0.0.0:{port}")
    print(f"数据库路径: {KILO_DB}")
    server = http.server.HTTPServer(("0.0.0.0", port), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()

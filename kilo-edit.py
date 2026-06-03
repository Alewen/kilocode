#!/usr/bin/env python3
"""
kilo-edit.py - 模拟 kilo 插件 edit 工具的核心实现 (纯 Python)
用法: python3 kilo-edit.py <edit.json> [level]
  level: 1-9, 匹配策略宽松程度 (默认 1)
JSON 格式: { "filePath": "/path/to/file", "oldString": "...", "newString": "...", "replaceAll": false }
"""

import json
import sys
import os
import re
import chardet

DEFAULT = "utf-8"
UTF8_BOM = "utf-8-bom"
BOM_CODE = 0xFEFF
BOM_CHAR = chr(BOM_CODE)

BOMS = {
    "utf-8-bom": b'\xef\xbb\xbf',
    "utf-16le": b'\xff\xfe',
    "utf-16be": b'\xfe\xff',
    "utf-32le": b'\xff\xfe\x00\x00',
    "utf-32be": b'\x00\x00\xfe\xff',
}

ENCODING_NORMALIZE = {
    "utf8": "utf-8", "utf16le": "utf-16le", "utf16be": "utf-16be",
    "utf32le": "utf-32le", "utf32be": "utf-32be",
    "iso88591": "iso-8859-1", "iso88592": "iso-8859-2",
    "iso88595": "iso-8859-5", "iso88597": "iso-8859-7",
    "iso88598": "iso-8859-8", "iso88599": "iso-8859-9",
    "windows1250": "windows-1250", "windows1251": "windows-1251",
    "windows1252": "windows-1252", "windows1253": "windows-1253",
    "windows1255": "windows-1255",
    "shiftjis": "Shift_JIS", "eucjp": "euc-jp",
    "iso2022jp": "iso-2022-jp", "euckr": "euc-kr",
    "iso2022kr": "iso-2022-kr", "big5": "big5",
    "gb18030": "gb18030", "koi8r": "koi8-r",
}

SINGLE_CANDIDATE_SIMILARITY_THRESHOLD = 0.0
MULTIPLE_CANDIDATES_SIMILARITY_THRESHOLD = 0.3


# ====== 编码检测 ======

def normalize(name):
    lower = name.lower().replace(" ", "").replace("-", "").replace("_", "")
    return ENCODING_NORMALIZE.get(lower, name)


def has_utf8_bom(data):
    return data[:3] == BOMS["utf-8-bom"]


def is_utf8(data):
    try:
        data.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def detect(data):
    if len(data) == 0:
        return DEFAULT
    if is_utf8(data):
        return UTF8_BOM if has_utf8_bom(data) else DEFAULT
    result = chardet.detect(data)
    if not result or not result.get("encoding"):
        return DEFAULT
    enc = normalize(result["encoding"])
    try:
        "test".encode(enc)
        return enc
    except (LookupError, UnicodeEncodeError):
        return DEFAULT


def read_file(path):
    with open(path, "rb") as f:
        data = f.read()
    encoding = detect(data)
    if encoding == UTF8_BOM:
        text = data.decode("utf-8-sig")
    else:
        text = data.decode(encoding, errors="replace")
    return text, encoding


def write_file(path, text, encoding=DEFAULT):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if encoding == UTF8_BOM:
        body = text[1:] if text and ord(text[0]) == BOM_CODE else text
        with open(path, "wb") as f:
            f.write(BOMS["utf-8-bom"])
            f.write(body.encode("utf-8"))
        return
    bom = BOMS.get(encoding.lower())
    if bom:
        body = text[1:] if text and ord(text[0]) == BOM_CODE else text
        with open(path, "wb") as f:
            f.write(bom)
            f.write(body.encode(encoding.lower()))
        return
    with open(path, "w", encoding=encoding, errors="replace") as f:
        f.write(text)


# ====== BOM 处理 ======

def bom_split(text):
    if text and ord(text[0]) == BOM_CODE:
        return True, text[1:]
    return False, text


def bom_join(text, bom):
    _, stripped = bom_split(text)
    if not bom:
        return stripped
    return BOM_CHAR + stripped


# ====== 换行符处理 ======

def normalize_line_endings(text):
    return text.replace("\r\n", "\n")


def detect_line_ending(text):
    return "\r\n" if "\r\n" in text else "\n"


def convert_to_line_ending(text, ending):
    if ending == "\n":
        return text
    return text.replace("\n", "\r\n")


# ====== Levenshtein 距离 ======

def levenshtein(a, b):
    if a == "" or b == "":
        return max(len(a), len(b))
    m, n = len(a), len(b)
    prev = list(range(n + 1))
    for i in range(1, m + 1):
        curr = [i] + [0] * n
        for j in range(1, n + 1):
            cost = 0 if a[i - 1] == b[j - 1] else 1
            curr[j] = min(prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost)
        prev = curr
    return prev[n]


# ====== 9 种 Replacer ======

def simple_replacer(content, find):
    yield find


def line_trimmed_replacer(content, find):
    original_lines = content.split("\n")
    search_lines = find.split("\n")
    if search_lines and search_lines[-1] == "":
        search_lines.pop()
    for i in range(len(original_lines) - len(search_lines) + 1):
        matches = True
        for j in range(len(search_lines)):
            if original_lines[i + j].strip() != search_lines[j].strip():
                matches = False
                break
        if matches:
            match_start = sum(len(original_lines[k]) + 1 for k in range(i))
            match_end = match_start
            for k in range(len(search_lines)):
                match_end += len(original_lines[i + k])
                if k < len(search_lines) - 1:
                    match_end += 1
            yield content[match_start:match_end]


def block_anchor_replacer(content, find):
    original_lines = content.split("\n")
    search_lines = find.split("\n")
    if len(search_lines) < 3:
        return
    if search_lines and search_lines[-1] == "":
        search_lines.pop()

    first_line_search = search_lines[0].strip()
    last_line_search = search_lines[-1].strip()
    search_block_size = len(search_lines)

    candidates = []
    for i in range(len(original_lines)):
        if original_lines[i].strip() != first_line_search:
            continue
        for j in range(i + 2, len(original_lines)):
            if original_lines[j].strip() == last_line_search:
                candidates.append((i, j))
                break
    if not candidates:
        return

    if len(candidates) == 1:
        start_line, end_line = candidates[0]
        actual_block_size = end_line - start_line + 1
        similarity = 0
        lines_to_check = min(search_block_size - 2, actual_block_size - 2)
        if lines_to_check > 0:
            for j in range(1, min(search_block_size - 1, actual_block_size - 1)):
                orig_line = original_lines[start_line + j].strip()
                search_line = search_lines[j].strip()
                max_len = max(len(orig_line), len(search_line))
                if max_len == 0:
                    continue
                distance = levenshtein(orig_line, search_line)
                similarity += (1 - distance / max_len) / lines_to_check
                if similarity >= SINGLE_CANDIDATE_SIMILARITY_THRESHOLD:
                    break
        else:
            similarity = 1.0
        if similarity >= SINGLE_CANDIDATE_SIMILARITY_THRESHOLD:
            match_start = sum(len(original_lines[k]) + 1 for k in range(start_line))
            match_end = match_start
            for k in range(start_line, end_line + 1):
                match_end += len(original_lines[k])
                if k < end_line:
                    match_end += 1
            yield content[match_start:match_end]
        return

    best_match = None
    max_similarity = -1
    for start_line, end_line in candidates:
        actual_block_size = end_line - start_line + 1
        similarity = 0
        lines_to_check = min(search_block_size - 2, actual_block_size - 2)
        if lines_to_check > 0:
            for j in range(1, min(search_block_size - 1, actual_block_size - 1)):
                orig_line = original_lines[start_line + j].strip()
                search_line = search_lines[j].strip()
                max_len = max(len(orig_line), len(search_line))
                if max_len == 0:
                    continue
                distance = levenshtein(orig_line, search_line)
                similarity += 1 - distance / max_len
            similarity /= lines_to_check
        else:
            similarity = 1.0
        if similarity > max_similarity:
            max_similarity = similarity
            best_match = (start_line, end_line)
    if max_similarity >= MULTIPLE_CANDIDATES_SIMILARITY_THRESHOLD and best_match:
        start_line, end_line = best_match
        match_start = sum(len(original_lines[k]) + 1 for k in range(start_line))
        match_end = match_start
        for k in range(start_line, end_line + 1):
            match_end += len(original_lines[k])
            if k < end_line:
                match_end += 1
        yield content[match_start:match_end]


def whitespace_normalized_replacer(content, find):
    def normalize_ws(text):
        return re.sub(r"\s+", " ", text).strip()

    normalized_find = normalize_ws(find)
    lines = content.split("\n")
    for i, line in enumerate(lines):
        if normalize_ws(line) == normalized_find:
            yield line
        else:
            normalized_line = normalize_ws(line)
            if normalized_find in normalized_line:
                words = find.strip().split()
                if words:
                    pattern = r"\s+".join(re.escape(w) for w in words)
                    try:
                        m = re.search(pattern, line)
                        if m:
                            yield m.group(0)
                    except re.error:
                        pass
    find_lines = find.split("\n")
    if len(find_lines) > 1:
        for i in range(len(lines) - len(find_lines) + 1):
            block = "\n".join(lines[i:i + len(find_lines)])
            if normalize_ws(block) == normalized_find:
                yield block


def indentation_flexible_replacer(content, find):
    def remove_indentation(text):
        lines = text.split("\n")
        non_empty = [l for l in lines if l.strip()]
        if not non_empty:
            return text
        min_indent = min(len(l) - len(l.lstrip()) for l in non_empty)
        return "\n".join(
            l if not l.strip() else l[min_indent:] for l in lines
        )

    normalized_find = remove_indentation(find)
    content_lines = content.split("\n")
    find_lines = find.split("\n")
    for i in range(len(content_lines) - len(find_lines) + 1):
        block = "\n".join(content_lines[i:i + len(find_lines)])
        if remove_indentation(block) == normalized_find:
            yield block


def escape_normalized_replacer(content, find):
    def unescape(s):
        result = []
        i = 0
        while i < len(s):
            if s[i] == "\\" and i + 1 < len(s):
                c = s[i + 1]
                if c == "n":
                    result.append("\n")
                elif c == "t":
                    result.append("\t")
                elif c == "r":
                    result.append("\r")
                elif c in ("'", '"', "`", "\\", "$"):
                    result.append(c)
                else:
                    result.append(s[i:i + 2])
                i += 2
            else:
                result.append(s[i])
                i += 1
        return "".join(result)

    unescaped_find = unescape(find)
    if unescaped_find in content:
        yield unescaped_find
    lines = content.split("\n")
    find_lines = unescaped_find.split("\n")
    for i in range(len(lines) - len(find_lines) + 1):
        block = "\n".join(lines[i:i + len(find_lines)])
        if unescape(block) == unescaped_find:
            yield block


def trimmed_boundary_replacer(content, find):
    trimmed_find = find.strip()
    if trimmed_find == find:
        return
    if trimmed_find in content:
        yield trimmed_find
    lines = content.split("\n")
    find_lines = find.split("\n")
    for i in range(len(lines) - len(find_lines) + 1):
        block = "\n".join(lines[i:i + len(find_lines)])
        if block.strip() == trimmed_find:
            yield block


def context_aware_replacer(content, find):
    find_lines = find.split("\n")
    if len(find_lines) < 3:
        return
    if find_lines and find_lines[-1] == "":
        find_lines.pop()
    content_lines = content.split("\n")
    first_line = find_lines[0].strip()
    last_line = find_lines[-1].strip()
    for i in range(len(content_lines)):
        if content_lines[i].strip() != first_line:
            continue
        for j in range(i + 2, len(content_lines)):
            if content_lines[j].strip() == last_line:
                block_lines = content_lines[i:j + 1]
                block = "\n".join(block_lines)
                if len(block_lines) == len(find_lines):
                    matching_lines = 0
                    total_non_empty = 0
                    for k in range(1, len(block_lines) - 1):
                        bl = block_lines[k].strip()
                        fl = find_lines[k].strip()
                        if bl or fl:
                            total_non_empty += 1
                            if bl == fl:
                                matching_lines += 1
                    if (total_non_empty == 0 or
                            matching_lines / total_non_empty >= 0.5):
                        yield block
                        break
                break


def multi_occurrence_replacer(content, find):
    start = 0
    while True:
        idx = content.find(find, start)
        if idx == -1:
            break
        yield find
        start = idx + len(find)


ALL_REPLACERS = [
    simple_replacer,
    line_trimmed_replacer,
    block_anchor_replacer,
    whitespace_normalized_replacer,
    indentation_flexible_replacer,
    escape_normalized_replacer,
    trimmed_boundary_replacer,
    context_aware_replacer,
    multi_occurrence_replacer,
]


# ====== replace 主函数 ======

def replace(content, old_string, new_string, replace_all, level):
    if old_string == new_string:
        raise ValueError(
            "No changes to apply: oldString and newString are identical."
        )

    replacers = ALL_REPLACERS[:level]
    not_found = True

    for replacer in replacers:
        for search in replacer(content, old_string):
            idx = content.find(search)
            if idx == -1:
                continue
            not_found = False
            if replace_all:
                return content.replace(search, new_string)
            last_idx = content.rfind(search)
            if idx != last_idx:
                continue
            return content[:idx] + new_string + content[idx + len(search):]

    if not_found:
        raise ValueError(
            "Could not find oldString in the file. "
            "It must match exactly, including whitespace, "
            "indentation, and line endings."
        )
    raise ValueError(
        "Found multiple matches for oldString. "
        "Provide more surrounding context to make the match unique."
    )


# ====== 主流程 ======

def main():
    if len(sys.argv) < 2:
        print("用法: python3 kilo-edit.py <edit.json> [level]")
        print("  level: 1-9, 匹配策略宽松程度 (默认 1)")
        sys.exit(1)

    json_path = sys.argv[1]
    level = int(sys.argv[2]) if len(sys.argv) > 2 else 1

    if not os.path.isfile(json_path):
        print(f"错误: 找不到文件: {json_path}", file=sys.stderr)
        sys.exit(1)

    with open(json_path, "r", encoding="utf-8") as f:
        params = json.load(f)

    if ("filePath" not in params or
            "oldString" not in params or
            "newString" not in params):
        print("错误: JSON 必须包含 filePath, oldString 和 newString 字段",
              file=sys.stderr)
        sys.exit(1)

    if params["oldString"] == params["newString"]:
        print("错误: oldString 和 newString 完全相同", file=sys.stderr)
        sys.exit(1)

    file_path = params["filePath"]
    if not file_path.startswith("/"):
        file_path = os.path.abspath(file_path)

    if not os.path.isfile(file_path):
        print(f"错误: 目标文件不存在: {file_path}", file=sys.stderr)
        sys.exit(1)

    old_content, source_encoding = read_file(file_path)
    source_bom = source_encoding == UTF8_BOM

    ending = detect_line_ending(old_content)
    old_str = convert_to_line_ending(
        normalize_line_endings(params["oldString"]), ending
    )
    new_str = convert_to_line_ending(
        normalize_line_endings(params["newString"]), ending
    )

    replaced = replace(
        old_content, old_str, new_str,
        params.get("replaceAll", False), level
    )
    to_write = bom_join(replaced, source_bom)
    write_file(file_path, to_write, source_encoding)

    print(f"OK: {file_path}")


if __name__ == "__main__":
    main()

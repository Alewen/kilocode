#!/usr/bin/env python3
"""
kilo-write.py - 模拟 kilo 插件 write 工具的核心实现 (纯 Python)
用法: python3 kilo-write.py <write.json>
JSON 格式: { "filePath": "/path/to/file", "content": "content" }
"""

import json
import sys
import os
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
    "utf8": "utf-8",
    "utf16le": "utf-16le",
    "utf16be": "utf-16be",
    "utf32le": "utf-32le",
    "utf32be": "utf-32be",
    "iso88591": "iso-8859-1",
    "iso88592": "iso-8859-2",
    "iso88595": "iso-8859-5",
    "iso88597": "iso-8859-7",
    "iso88598": "iso-8859-8",
    "iso88599": "iso-8859-9",
    "windows1250": "windows-1250",
    "windows1251": "windows-1251",
    "windows1252": "windows-1252",
    "windows1253": "windows-1253",
    "windows1255": "windows-1255",
    "shiftjis": "Shift_JIS",
    "eucjp": "euc-jp",
    "iso2022jp": "iso-2022-jp",
    "euckr": "euc-kr",
    "iso2022kr": "iso-2022-kr",
    "big5": "big5",
    "gb18030": "gb18030",
    "koi8r": "koi8-r",
}


def normalize(name):
    """规范化编码名称"""
    lower = name.lower().replace(" ", "").replace("-", "").replace("_", "")
    return ENCODING_NORMALIZE.get(lower, name)


def has_utf8_bom(data):
    """检查是否有 UTF-8 BOM"""
    return data[:3] == BOMS["utf-8-bom"]


def is_utf8(data):
    """检查是否为有效 UTF-8"""
    try:
        data.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def detect(data):
    """检测文件编码"""
    if len(data) == 0:
        return DEFAULT
    if is_utf8(data):
        return UTF8_BOM if has_utf8_bom(data) else DEFAULT
    result = chardet.detect(data)
    if not result or not result.get("encoding"):
        return DEFAULT
    enc = normalize(result["encoding"])
    # 验证编码是否可用
    try:
        "test".encode(enc)
        return enc
    except (LookupError, UnicodeEncodeError):
        return DEFAULT


def read_file(path):
    """读取文件并检测编码"""
    with open(path, "rb") as f:
        data = f.read()
    encoding = detect(data)
    if encoding == UTF8_BOM:
        text = data.decode("utf-8-sig")
    else:
        text = data.decode(encoding, errors="replace")
    return text, encoding


def write_file(path, text, encoding=DEFAULT):
    """以指定编码写入文件，自动创建父目录"""
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


def bom_split(text):
    """拆分 BOM"""
    if text and ord(text[0]) == BOM_CODE:
        return True, text[1:]
    return False, text


def bom_join(text, bom):
    """拼接 BOM"""
    _, stripped = bom_split(text)
    if not bom:
        return stripped
    return BOM_CHAR + stripped


def main():
    if len(sys.argv) < 2:
        print("用法: python3 kilo-write.py <write.json>")
        print('JSON 格式: { "filePath": "/path/to/file", "content": "content" }')
        sys.exit(1)

    json_path = sys.argv[1]
    if not os.path.isfile(json_path):
        print(f"错误: 找不到文件: {json_path}", file=sys.stderr)
        sys.exit(1)

    with open(json_path, "r", encoding="utf-8") as f:
        params = json.load(f)

    if "filePath" not in params or "content" not in params:
        print("错误: JSON 必须包含 filePath 和 content 字段", file=sys.stderr)
        sys.exit(1)

    file_path = params["filePath"]
    if not file_path.startswith("/"):
        file_path = os.path.abspath(file_path)

    exists = os.path.isfile(file_path)
    source_encoding = DEFAULT
    source_bom = False

    if exists:
        _, source_encoding = read_file(file_path)
        source_bom = source_encoding == UTF8_BOM

    has_bom, content_new = bom_split(params["content"])
    desired_bom = source_bom or has_bom

    write_file(file_path, bom_join(content_new, desired_bom),
               source_encoding if exists else DEFAULT)

    print(f"OK: {file_path}")


if __name__ == "__main__":
    main()

---
name: win-file-encoding-detector
description: This skill provides a PowerShell script to detect text file encoding formats and line ending styles. It identifies various encodings including UTF-8 (with/without BOM), UTF-16, UTF-32, ASCII, GB2312, GBK, and GB18030, while also detecting line endings (LF, CRLF, Mixed). This skill should be used when determining the encoding format or line ending style of a text file.
---

# File Encoding Detector

## Purpose

[Windows Only] Provide a standalone PowerShell script to detect the encoding format and line ending style of text files. BOM detection, byte-level validation, and .NET encoding round-trip verification are used for accurate identification.

Supports: UTF-8-BOM / UTF-8 / UTF-16LE-BOM / UTF-16BE-BOM / UTF-32LE-BOM / UTF-32BE-BOM / ASCII / GB2312 / GBK / GB18030.

## When to Use

Use this skill when:

- Determining the encoding format of a text file
- Checking the line ending style (LF / CRLF / Mixed / NoEOL) of a file
- Verifying whether a file uses a specific encoding (UTF-8, GB2312, etc.)

## Main Script

`scripts/win-file-encoding-detector.ps1` (636 lines)

Contains all detection functions:

| Function | Purpose |
|----------|---------|
| `Detect-BOM` | Read first 4 bytes to identify BOM (UTF-32 LE/BE → UTF-8 → UTF-16 LE/BE) |
| `Is-ASCII-File` | Verify all bytes are in ASCII printable range |
| `Is-UTF8-WithoutBom-File` | Decode/re-encode round-trip via `UTF8Encoding` |
| `Is-GB-File` | Try GB2312/GBK/GB18030 with round-trip check |
| `Is-GB2312-Bytes` | Strict byte-level GB2312 validation (excludes reserved zones) |
| `Detect-LineEnding` | Scan bytes for CRLF / LF and classify |
| `Check-FileFormat` | Main entry point, orchestrates detection pipeline |
| `FileSize-Format` | Format byte count to human-readable string |

Detection pipeline order: BOM → ASCII → UTF-8 (no BOM) → GB series → unknown.

## Parameters

### Check-FileFormat

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `-FilePath` | string | Yes | Full path of the file to detect (no wildcards) |

File size limits: 5 bytes minimum, 10 MB maximum.

## Return Value

Structured object (JSON when called from command line):

| Field | Type | Description |
|-------|------|-------------|
| `Success` | bool | Detection result |
| `FilePath` | string | Resolved full path of the detected file |
| `FileSize` | string | Human-readable size (e.g. "1.23 K", "4.56 M") |
| `FileSizeBytes` | long | Size in bytes |
| `Encoding` | string | Detected encoding (see table below) |
| `LineEnding` | string | Detected line ending (see table below) |
| `ErrorMessage` | string | Empty on success, error text on failure |

### Encoding Values

| Value | Description |
|-------|-------------|
| `UTF-8-BOM` | UTF-8 with BOM (`EF BB BF`) |
| `UTF-8` | UTF-8 without BOM |
| `UTF-16LE-BOM` | UTF-16 little-endian with BOM (`FF FE`) |
| `UTF-16BE-BOM` | UTF-16 big-endian with BOM (`FE FF`) |
| `UTF-32LE-BOM` | UTF-32 little-endian with BOM (`FF FE 00 00`) |
| `UTF-32BE-BOM` | UTF-32 big-endian with BOM (`00 00 FE FF`) |
| `ASCII` | Pure ASCII (no byte ≥ 0x80, no forbidden control chars) |
| `GB2312` | GB2312 (strict, excludes reserved zones) |
| `GBK` | GBK (superset of GB2312) |
| `GB18030` | GB18030 (superset of GBK) |
| `----` | Unknown encoding |

### Line Ending Values

| Value | Description |
|-------|-------------|
| `LF` | Unix-style line endings (`\n`) |
| `CRLF` | Windows-style line endings (`\r\n`) |
| `Mixed` | Mixed LF and CRLF |
| `NoEOL` | No line terminators found |
| `unknown` | Error during line ending detection |

## Usage

### From pwsh (PowerShell 7)

```powershell
# Detect encoding and line endings
pwsh -File "scripts/win-file-encoding-detector.ps1" -FilePath "path\to\your\file.txt"
```

### From PowerShell (Dot-Sourcing)

```powershell
# Load functions into current session
. "scripts/win-file-encoding-detector.ps1"

# Call main function
$result = Check-FileFormat -FilePath "path\to\your\file.txt"
$result | ConvertTo-Json -Depth 10
```

## Example Output

```json
{
    "Success": true,
    "FilePath": "F:\\Winscripts\\example.txt",
    "FileSize": "16.18 K",
    "FileSizeBytes": 16569,
    "Encoding": "GB2312",
    "LineEnding": "LF",
    "ErrorMessage": ""
}
```

## Error Conditions

- `"路径不能包含通配符"` — wildcard characters detected in path
- `"文件路径不能为空"` — empty or whitespace-only path
- `"不是有效文件或文件不存在"` — path does not point to a file
- `"文件不可读"` — file cannot be accessed
- `"文件太小"` — file size < 5 bytes
- `"文件太大"` — file size > 10 MB

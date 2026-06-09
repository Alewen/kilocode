---
name: win-file-encoding-detector
description: This skill provides a PowerShell script to detect text file encoding formats and line ending styles. It identifies various encodings including UTF-8 (with/without BOM), UTF-16, UTF-32, ASCII, GB2312, GBK, and GB18030, while also detecting line endings (LF, CRLF, Mixed). This skill should be used when determining the encoding format or line ending style of a text file.
---

# File Encoding Detector

## Purpose

[Windows Only] This skill provides a standalone PowerShell script to detect the encoding format and line ending style of text files. It supports detection of multiple encoding types and can be easily called by AI agents. This skill is only available on Windows systems as it uses PowerShell scripts.

## When to Use

This skill should be used when:
- Determining the encoding format of a text file
- Checking the line ending style (LF/CRLF/Mixed) of a file
- Verifying if a file uses a specific encoding (UTF-8, GB2312, etc.)

## How to Use

### Main Script Location

The main detection script is located at: `scripts/win-file-encoding-detector.ps1`

### Calling from PowerShell

```powershell
# Basic usage - detect encoding only
powershell -File "scripts/win-file-encoding-detector.ps1" -FilePath "path\to\your\file.txt"

# Detect encoding AND line endings
powershell -File "scripts/win-file-encoding-detector.ps1" -FilePath "path\to\your\file.txt" -CheckLineEnding
```

### Calling from PowerShell Directly (Dot-Sourcing)

```powershell
# Load the script
. "scripts/win-file-encoding-detector.ps1"

# Call the main function
$result = Check-FileFormat -FilePath "path\to\your\file.txt" -CheckLineEnding
$result | ConvertTo-Json -Depth 10
```

## Parameters

### Check-FileFormat Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `FilePath` | string | Yes | Full path of the file to detect |
| `CheckLineEnding` | switch | No | Whether to detect and return line ending style |

## Return Value

The script returns a structured object (JSON when called from command line) with the following fields:

| Field | Type | Description |
|-------|------|-------------|
| `Success` | bool | Indicates whether detection was successful |
| `FilePath` | string | Full path of the detected file |
| `FileSize` | string | Human-readable file size with units (e.g., "1.23 K", "4.56 M") |
| `FileSizeBytes` | long | File size in bytes |
| `Encoding` | string | Detected encoding format |
| `LineEnding` | string | Detected line ending style (or "skip" if not detected) |
| `ErrorMessage` | string | Error message (empty string if successful) |

### Possible Encoding Values

| Value | Description |
|-------|-------------|
| "UTF-8-BOM" | UTF-8 with BOM |
| "UTF-16LE-BOM" | UTF-16 little-endian with BOM |
| "UTF-16BE-BOM" | UTF-16 big-endian with BOM |
| "UTF-32LE-BOM" | UTF-32 little-endian with BOM |
| "UTF-32BE-BOM" | UTF-32 big-endian with BOM |
| "ASCII" | ASCII encoding |
| "UTF-8" | UTF-8 without BOM |
| "GB2312" | GB2312 encoding |
| "GBK" | GBK encoding |
| "GB18030" | GB18030 encoding |
| "----" | Unknown encoding |

### Possible Line Ending Values

| Value | Description |
|-------|-------------|
| "LF" | Unix-style line endings (\\n) |
| "CRLF" | Windows-style line endings (\\r\\n) |
| "Mixed" | Mixed line endings |
| "NoEOL" | No line endings detected |
| "skip" | Line ending detection was skipped |
| "unknown" | Error during line ending detection |

## File Size Limits

- Minimum: 5 bytes
- Maximum: 10 MB

Files outside this range will return an error.

## Example Usage

```powershell
# Example 1: Detect a file's encoding
$result = powershell -File "scripts/win-file-encoding-detector.ps1" -FilePath "document.txt"
# Output JSON with encoding information

# Example 2: Detect encoding and line endings
powershell -File "scripts/win-file-encoding-detector.ps1" -FilePath "code.py" -CheckLineEnding
```

## Example Output

```json
{
    "Success": true,
    "FilePath": "F:\\Winscripts\\ckFile_standalone.ps1",
    "FileSize": "16.18 K",
    "FileSizeBytes": 16569,
    "Encoding": "GB2312",
    "LineEnding": "LF",
    "ErrorMessage": ""
}
```

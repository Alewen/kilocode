---
name: win-file-encoding-converter
description: This skill provides a PowerShell script to convert text file encoding formats between various encodings including UTF-8 (with/without BOM), UTF-16, UTF-32, ASCII, GB2312, and GBK. It includes validation steps to ensure conversion accuracy and atomic operations for safe file replacement. This skill should be used when converting a file from one encoding format to another.
---

# File Encoding Converter

## Purpose

[Windows Only] This skill provides a standalone PowerShell script to convert the encoding format of text files. It supports conversion between multiple encoding types with validation steps to ensure accuracy and uses atomic operations for safe file replacement. This skill is only available on Windows systems as it uses PowerShell scripts.

## When to Use

This skill should be used when:
- Converting a text file from one encoding format to another
- Ensuring accurate encoding conversion with validation
- Safely replacing files using atomic operations
- Converting between UTF-8 (with/without BOM), UTF-16, UTF-32, ASCII, GB2312, or GBK

## How to Use

### Main Script Location

The main conversion script is located at: `scripts/win-file-encoding-converter.ps1`

### Calling from PowerShell

```powershell
# Basic usage
powershell -File "C:\Users\shaoke\.kilo\skills\win-file-encoding-converter\scripts\win-file-encoding-converter.ps1" "path\to\your\file.txt" "SourceEncoding" "TargetEncoding"

# With quiet mode (no output)
powershell -File "C:\Users\shaoke\.kilo\skills\win-file-encoding-converter\scripts\win-file-encoding-converter.ps1" "path\to\your\file.txt" "SourceEncoding" "TargetEncoding" -Quiet
```

### Calling from PowerShell Directly (Dot-Sourcing)

```powershell
# Load the script
. "C:\Users\shaoke\.kilo\skills\win-file-encoding-converter\scripts\win-file-encoding-converter.ps1"

# Call the main function
$result = FileEncodingConverter -FilePath "path\to\your\file.txt" -SourceEncoding "SourceEncoding" -TargetEncoding "TargetEncoding"
$result | ConvertTo-Json -Depth 10
```

## Parameters

### FileEncodingConverter Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `FilePath` | string | Yes | Full path of the file to convert |
| `SourceEncoding` | string | Yes | Source encoding format of the file |
| `TargetEncoding` | string | Yes | Target encoding format to convert to |
| `Quiet` | switch | No | Suppress output messages |

## Return Value

The script returns a structured object (JSON when called from command line) with the following fields:

| Field | Type | Description |
|-------|------|-------------|
| `Success` | bool | Indicates whether conversion was successful |
| `ErrorMessage` | string | Error message (empty string if successful) |

## Supported Encoding Formats

| Encoding Name | Aliases | Description |
|---------------|---------|-------------|
| `"UTF-8"` | `"UTF8"` | UTF-8 without BOM |
| `"UTF-8-BOM"` | `"UTF8-BOM"`, `"UTF8BOM"` | UTF-8 with BOM |
| `"UTF-16LE"` | `"UTF16LE"` | UTF-16 little-endian without BOM |
| `"UTF-16LE-BOM"` | `"UTF16LE-BOM"`, `"UTF16LEBOM"`, `"UNICODE"` | UTF-16 little-endian with BOM |
| `"UTF-16BE"` | `"UTF16BE"` | UTF-16 big-endian without BOM |
| `"UTF-16BE-BOM"` | `"UTF16BE-BOM"`, `"UTF16BEBOM"` | UTF-16 big-endian with BOM |
| `"UTF-32LE"` | `"UTF32LE"` | UTF-32 little-endian without BOM |
| `"UTF-32LE-BOM"` | `"UTF32LE-BOM"`, `"UTF32LEBOM"` | UTF-32 little-endian with BOM |
| `"UTF-32BE"` | `"UTF32BE"` | UTF-32 big-endian without BOM |
| `"UTF-32BE-BOM"` | `"UTF32BE-BOM"`, `"UTF32BEBOM"` | UTF-32 big-endian with BOM |
| `"ASCII"` | - | ASCII encoding |
| `"GB2312"` | - | GB2312 encoding |
| `"GBK"` | - | GBK encoding |
| `"DEFAULT"` | - | System default encoding |

## Conversion Process

The script follows these steps:
1. **Validate source encoding** - Verifies that the file can be correctly read with the specified source encoding
2. **Read file content** - Reads the file and removes BOM if present
3. **Write temporary file** - Writes content to a temporary file using target encoding
4. **Validate conversion** - Compares original content with converted content to ensure accuracy
5. **Atomic replacement** - Safely replaces the original file with the converted file

## Safety Features

- **Atomic file replacement** - Ensures original file is only replaced if conversion is successful
- **Temporary backup** - Creates `.orig` temporary file during conversion
- **Content validation** - Verifies that content remains unchanged after conversion
- **Error recovery** - Attempts to restore original file if atomic operation fails

## Example Usage

```powershell
# Example 1: Convert from GBK to UTF-8 (without BOM)
powershell -File "C:\Users\shaoke\.kilo\skills\win-file-encoding-converter\scripts\win-file-encoding-converter.ps1" "document.txt" "GBK" "UTF-8"

# Example 2: Convert from UTF-8 to UTF-8 with BOM
$result = FileEncodingConverter -FilePath "code.py" -SourceEncoding "UTF-8" -TargetEncoding "UTF-8-BOM"

# Example 3: Quiet mode conversion
powershell -File "C:\Users\shaoke\.kilo\skills\win-file-encoding-converter\scripts\win-file-encoding-converter.ps1" "config.json" "UTF-16LE-BOM" "UTF-8" -Quiet
```

## Example Output

```json
{
    "Success": true,
    "ErrorMessage": ""
}
```

```json
{
    "Success": false,
    "ErrorMessage": "发现 5 处字节不匹配，源编码可能不正确"
}
```

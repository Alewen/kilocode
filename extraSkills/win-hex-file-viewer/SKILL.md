---
name: win-hex-file-viewer
description: This skill should be used when you need to view the binary content of a file in hexadecimal format. It displays files with hexadecimal offsets on the left and hexadecimal byte values, making it easy for AI agents to analyze binary files.
---

# Hex File Viewer

## Overview

[Windows Only] This skill provides a PowerShell script to view file contents in hexadecimal format with hexadecimal offsets, optimized for AI agent readability. The script performs read-only operations and does not modify the viewed file. This skill is only available on Windows systems as it uses PowerShell scripts.

## Usage

To view a file in hexadecimal format:

1. Locate the script at `scripts/win-hex-file-viewer.ps1`
2. Run it using pwsh (PowerShell 7) with the file path as argument
3. Optionally specify bytes per line (10-50, default 20)

> **Note:** The script loads the entire file into memory via `ReadAllBytes`. For very large files (e.g., > 500 MB), consider using an alternative approach or truncating the file first.

### Example Usage

```powershell
pwsh -File scripts/win-hex-file-viewer.ps1 <file-path>
pwsh -File scripts/win-hex-file-viewer.ps1 <file-path> 32
```

### Output Format

The output shows:
- Left column: Hexadecimal starting offset (8 digits)
- Right column: Hexadecimal byte values (2 digits each, space-separated)

Example:
```
00000000: 48 65 6C 6C 6F 2C 20 77 6F 72 6C 64 21
0000000D: 20 54 68 69 73 20 69 73 20 61 20 74 65
```

## Resources

### scripts/
- `win-hex-file-viewer.ps1` - PowerShell script to display file in hexadecimal format

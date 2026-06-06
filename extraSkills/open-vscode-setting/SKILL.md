---
name: open-vscode-setting
description: This skill should be used when the user asks to "open settings" or "open settings file" - it opens the VSCode user settings.json file in the current VSCode instance.
---

# Open VSCode Setting

## Overview

This skill opens the VSCode user settings.json file in the current VSCode instance. Use it whenever the user asks to "open settings" or "open settings file".

## Usage

When the user requests to open settings or settings file, use the bundled Python script to open the settings.json file.

Execute the script using the bash tool:

```bash
python "C:\Users\shaoke\.kilo\skills\open-vscode-setting\scripts\open-vscode-setting.py"
```

The script automatically determines the correct settings file path based on the operating system and opens it in the current VSCode instance.

## Resources

### scripts/
- `open-vscode-setting.py` - Python script that opens the settings.json file in VSCode

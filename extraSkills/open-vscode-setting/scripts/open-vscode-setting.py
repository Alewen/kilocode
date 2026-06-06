#!/usr/bin/env python3
"""
Open VSCode Settings File

This script opens the VSCode user settings.json file in the current VSCode instance.
"""

import sys
import os
import platform
import subprocess


def get_settings_path():
    """Get the path to VSCode settings.json based on OS."""
    system = platform.system()
    
    if system == 'Windows':
        appdata = os.environ.get('APPDATA')
        if appdata:
            return os.path.join(appdata, 'Code', 'User', 'settings.json')
    elif system == 'Darwin':  # macOS
        home = os.environ.get('HOME')
        if home:
            return os.path.join(home, 'Library', 'Application Support', 'Code', 'User', 'settings.json')
    else:  # Linux
        home = os.environ.get('HOME')
        if home:
            return os.path.join(home, '.config', 'Code', 'User', 'settings.json')
    
    return None


def get_code_command():
    """Get the appropriate code command based on OS."""
    system = platform.system()
    
    if system == 'Windows':
        # On Windows, use code.cmd from PATH
        return "code.cmd"
    else:
        # macOS/Linux
        return "code"


def main():
    settings_path = get_settings_path()
    
    if not settings_path:
        print("Could not determine VSCode settings.json path")
        return 1
    
    # Check if file exists
    if not os.path.exists(settings_path):
        print(f"Settings file not found at: {settings_path}")
        return 1
    
    # Open with VSCode
    code_cmd = get_code_command()
    try:
        subprocess.run([code_cmd, '-r', settings_path], check=True)
        print(f"Opened settings file: {settings_path}")
        return 0
    except FileNotFoundError:
        print(f"'{code_cmd}' command not found. Make sure VSCode is installed.")
        return 1
    except Exception as e:
        print(f"Error opening settings file: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())

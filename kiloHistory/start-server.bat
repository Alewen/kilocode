@echo off
cd /d "%~dp0"
echo Starting Kilo API Server...
echo Database path: %USERPROFILE%\.local\share\kilo\kilo.db
python server.py
pause
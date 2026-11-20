@echo off
chcp 65001 >nul
cd /d "%~dp0"
python -c "import PyQt5, truck_telemetry" 2>nul || python -m pip install -r requirements.txt
start "" pythonw main.py %*

@echo off
cd /d "%~dp0"
python tools\pc_camera_server.py --host 0.0.0.0 --port 8000

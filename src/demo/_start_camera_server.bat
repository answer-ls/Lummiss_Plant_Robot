@echo off
cd /d "%~dp0"
python -c "import av, cv2" >nul 2>&1
if errorlevel 1 (
    echo First run: installing PC preview dependencies...
    python -m pip install -r tools\camera_server_requirements.txt
    if errorlevel 1 exit /b 1
)
python tools\pc_camera_server.py --host 0.0.0.0 --port 8000

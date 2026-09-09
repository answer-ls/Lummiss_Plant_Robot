@echo off
set IDF_PATH=D:\espidf5.5.5\.espressif\v5.5.5\esp-idf
set IDF_TOOLS_PATH=C:\Espressif
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v5.5.5\venv
rem The installer keeps the constraint file under C:\Espressif\tools.
set IDF_PYTHON_CHECK_CONSTRAINTS=no
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b 1
cd /d E:\Lummiss_Plant_Robot\src\demo
idf.py -B build_main_verified build
exit /b %errorlevel%

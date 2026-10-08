$ErrorActionPreference = 'Continue'
$env:IDF_PATH = 'D:\espidf5.5.5\.espressif\v5.5.5\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v5.5.5\venv'
$env:IDF_TARGET = 'esp32p4'
$env:ESP_IDF_VERSION = '5.5'
$env:IDF_COMPONENT_MANAGER = '1'
$env:PATH = 'C:\Espressif\tools\python\v5.5.5\venv\Scripts;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;' + $env:PATH
# 仅构建独立工程，构建日志统一留在项目根目录。
$log = Join-Path $PSScriptRoot '..\..\logs\xiaozhi_reference_build.log'
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" -C $PSScriptRoot build *> $log
exit $LASTEXITCODE

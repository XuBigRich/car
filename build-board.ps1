param([string]$Port, [switch]$Flash)
# 板载录制固件构建入口。默认只编译；明确指定 -Flash -Port COM5 才烧录。
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$env:IDF_PATH = 'D:\esp\v5.5.6\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v5.5.6\venv'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20260528'
# 仅本进程信任当前工程与本机 IDF，避免修改全局 Git 配置。
$env:GIT_CONFIG_COUNT = '2'
$env:GIT_CONFIG_KEY_0 = 'safe.directory'
$env:GIT_CONFIG_VALUE_0 = 'D:/c-project/Control_car'
$env:GIT_CONFIG_KEY_1 = 'safe.directory'
$env:GIT_CONFIG_VALUE_1 = 'D:/esp/v5.5.6/esp-idf'
$env:PATH = 'C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\python\v5.5.6\venv\Scripts;' + $env:PATH
$boardPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
& $boardPython "$env:IDF_PATH/tools/idf.py" -B build-android-clean -D SDKCONFIG=sdkconfig.android build
if ($LASTEXITCODE -ne 0) { throw '固件编译失败' }
if ($Flash) {
    if (!$Port) { throw '烧录必须指定 -Port，例如 COM5' }
    # 只写入编译产物指定的区域，不擦除整个 Flash，不覆盖 NVS 分区。
    & $boardPython "$env:IDF_PATH/tools/idf.py" -B build-android-clean -D SDKCONFIG=sdkconfig.android -p $Port -b 460800 flash
    if ($LASTEXITCODE -ne 0) { throw '固件烧录失败' }
}

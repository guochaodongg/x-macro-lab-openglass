<#
.SYNOPSIS
  ESP-IDF helper for the XIAO ESP32S3 blink demo.
  Preconfigured for THIS machine's ESP-IDF v6.1 layout.

.EXAMPLE
  .\build.ps1 set-target
  .\build.ps1 build
  .\build.ps1 flash -Port COM3
  .\build.ps1 monitor -Port COM3
  .\build.ps1 erase -Port COM3
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet("set-target", "build", "flash", "monitor", "fullflash", "menuconfig", "erase", "clean")]
    [string]$Action = "build",

    [string]$Port = "COM3",
    [string]$Target = "esp32s3"
)

$env:IDF_PATH = "D:\esp-idf\v6.1\esp-idf"
$env:IDF_PYTHON_ENV_PATH = "C:\Users\guochaodong\.espressif\python_env\idf6.1_py3.11_env"
$env:ESP_IDF_VERSION = "6.1.0"
$env:ESP_ROM_ELF_DIR = "D:\esp-idf\v6.1\tools\esp-rom-elfs\20241011"

$py = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
$idf = "$env:IDF_PATH\tools\idf.py"
$tools = "D:\esp-idf\v6.1\tools"

$env:PATH = "$env:IDF_PYTHON_ENV_PATH\Scripts;" +
            "$tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;" +
            "$tools\cmake\4.0.3\bin;" +
            "$tools\ninja\1.12.1;" +
            "$tools\ccache\4.12.1;" + $env:PATH

Set-Location $PSScriptRoot

switch ($Action) {
    "set-target" { & $py $idf set-target $Target }
    "build"      { & $py $idf build }
    "flash"      { & $py $idf -p $Port flash }
    "monitor"    { & $py $idf -p $Port monitor }
    "fullflash"  { & $py $idf -p $Port flash monitor }
    "menuconfig" { & $py $idf menuconfig }
    "erase"      { & $py $idf -p $Port erase_flash }
    "clean"      { & $py $idf clean }
}

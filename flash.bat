@echo off
chcp 65001 >nul
setlocal

REM ============================================================
REM  One-shot flash for Seeed Studio XIAO ESP32S3 (pre-built FW)
REM  Usage:  flash.bat COM3     (replace with your COM port)
REM          flash.bat          (defaults to COM3)
REM ============================================================

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM3"

set "ESPTOOL=D:\InstallationLocation\Python314\Scripts\esptool.exe"
set "FIRMWARE=%~dp0firmware.factory.bin"

echo.
echo  Target   : Seeed Studio XIAO ESP32S3 (ESP32-S3R8, 8MB flash)
echo  Port     : %PORT%
echo  Firmware : %FIRMWARE%
echo.

if not exist "%FIRMWARE%" (
  echo  [ERROR] firmware.factory.bin not found.
  echo          Rebuild it with:  .\build.ps1 build
  goto :end
)

if not exist "%ESPTOOL%" (
  echo  [ERROR] esptool not found at %ESPTOOL%
  goto :end
)

echo  Flashing... (hold BOOT if no port is detected)
"%ESPTOOL%" --chip esp32s3 --port %PORT% --baud 921600 write_flash -z 0x0 "%FIRMWARE%"

:end
echo.
pause

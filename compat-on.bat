@echo off
REM Enable Cristina/FTDI-compat mode: child PDOs will advertise as
REM VID_0403+PID_6015+CRxxxxxx in Win32_PnPEntity.DeviceID so apps that
REM scan WMI for FTDI hardware (Bosch Cristina, etc.) can discover them.
REM Reload the driver afterwards for the change to take effect.
reg add "HKLM\SYSTEM\CurrentControlSet\Services\vSerious\Parameters" /v CompatMode /t REG_DWORD /d 1 /f
echo CompatMode=1. Now run reload.bat (or sc stop/start vSerious + re-activate).

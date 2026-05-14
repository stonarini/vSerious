@echo off
REM Disable Cristina/FTDI-compat mode: child PDOs revert to vSerious\COMx
REM hardware/device IDs. Reload the driver afterwards.
reg add "HKLM\SYSTEM\CurrentControlSet\Services\vSerious\Parameters" /v CompatMode /t REG_DWORD /d 0 /f
echo CompatMode=0. Now run reload.bat (or sc stop/start vSerious + re-activate).

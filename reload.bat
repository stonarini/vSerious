@echo off
REM reload.bat - rebind the rebuilt vSerious driver on the test machine.
REM Run from a directory that contains vSerious.inf + vSerious.sys + vSerious.cat,
REM as Administrator. First-time install of Root\vSerious must still be done
REM via devcon or "Add legacy hardware" in Device Manager.

setlocal

net session >nul 2>&1
if errorlevel 1 (
    echo ERROR: must be run as Administrator.
    pause
    exit /b 1
)

if not exist vSerious.inf (
    echo ERROR: vSerious.inf not found in current directory ^(%CD%^).
    pause
    exit /b 1
)

echo === Removing previously installed vSerious driver packages ===
for /f "delims=" %%i in ('powershell -NoProfile -Command "Get-WindowsDriver -Online ^| Where-Object { $_.OriginalFileName -like '*\vSerious.inf' } ^| Select-Object -ExpandProperty Driver"') do (
    echo   pnputil /delete-driver %%i /uninstall /force
    pnputil /delete-driver "%%i" /uninstall /force
)

echo.
echo === Installing vSerious.inf ===
pnputil /add-driver vSerious.inf /install
if errorlevel 1 (
    echo ERROR: install failed. Check test-signing is on and the cat is valid.
    pause
    exit /b 1
)

echo.
echo === Triggering PnP rescan ===
pnputil /scan-devices

echo.
echo === Done. Verify in Device Manager ===
echo   - Software devices: "vSerious Virtual COM Port Controller"
echo   - Ports (COM ^& LPT^): appears after SetActive
pause

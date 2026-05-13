@echo off

cd C:\Users\Kraker\source\repos\vSerious\x64\Debug\vSerious

"C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe" update vSerious.inf Root\vSerious

REM Stage vSeriousPort.inf into the driver store so PnP matches it when
REM child PDOs (vSerious\Port) are enumerated. Class=Ports assignment is
REM the only purpose — no service, no files to copy.
pnputil /add-driver vSeriousPort.inf /install
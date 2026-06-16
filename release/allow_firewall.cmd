@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if not "%errorlevel%"=="0" (
    echo This script needs administrator rights.
    echo Right-click allow_firewall.cmd and choose "Run as administrator".
    pause
    exit /b 1
)

netsh advfirewall firewall delete rule name="Tarkov Voice Overlay P2P UDP 40771" >nul 2>&1
netsh advfirewall firewall add rule name="Tarkov Voice Overlay P2P UDP 40771" dir=in action=allow protocol=UDP localport=40771 profile=any enable=yes

echo Firewall rule added for direct UDP voice on port 40771.
pause

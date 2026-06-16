@echo off
setlocal
cd /d "%~dp0"

if not exist logs mkdir logs

start "TVO Coordinator" /min "%~dp0tvo-coordinator.exe" > "%~dp0logs\coordinator.out.log" 2> "%~dp0logs\coordinator.err.log"
timeout /t 2 /nobreak > nul
start "TVO Public Tunnel" /min "%~dp0cloudflared.exe" tunnel --url http://localhost:8080 --no-autoupdate > "%~dp0logs\cloudflared.out.log" 2> "%~dp0logs\cloudflared.err.log"

echo Local coordinator: ws://127.0.0.1:8080/ws
echo Public tunnel URL appears in logs\cloudflared.err.log after startup.


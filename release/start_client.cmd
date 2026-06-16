@echo off
setlocal
cd /d "%~dp0"

if "%TVO_COORDINATOR_URL%"=="" set TVO_COORDINATOR_URL=ws://185.244.51.198:8080/ws
set TVO_DIRECT_UDP_PORT=40771
if "%TVO_NICK%"=="" set TVO_NICK=Player

start "" "%~dp0tarkov_voice_overlay.exe"

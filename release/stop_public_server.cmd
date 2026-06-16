@echo off
taskkill /IM tvo-coordinator.exe /F > nul 2> nul
taskkill /IM cloudflared.exe /F > nul 2> nul
echo Tarkov Voice Overlay public server processes stopped.


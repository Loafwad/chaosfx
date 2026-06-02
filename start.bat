@echo off
set NODE=C:\Users\nathb\.nvm\versions\node\v21.2.0\bin\node.exe
set DIR=%~dp0controller

start "ChaosFX Bridge" cmd /k "cd /d "%DIR%" && "%NODE%" reward-bridge.js"
start "ChaosFX Twitch" cmd /k "cd /d "%DIR%" && "%NODE%" twitch-eventsub.js"
timeout /t 2 /nobreak >nul
start http://127.0.0.1:18244

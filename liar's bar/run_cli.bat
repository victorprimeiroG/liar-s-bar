@echo off
title LIARS BAR - JOGADOR 2 (CLIENTE)
cd /d "%~dp0"
timeout /t 3 >nul
liarsbar.exe --cli 127.0.0.1 5050
pause
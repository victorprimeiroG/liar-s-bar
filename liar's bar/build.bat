@echo off
cd /d "%~dp0"
"C:\msys64\ucrt64\bin\gcc.exe" -O2 -Wall -Wextra -std=c11 -static ^
  -o liarsbar.exe liarsbar.c net.c shm.c -lws2_32 -lpthread
if errorlevel 1 (echo FALHOU) else (echo OK - liarsbar.exe gerado)
pause
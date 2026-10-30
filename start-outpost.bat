@echo off
setlocal
cd /d "%~dp0"
dgmp-server.exe --config server-outpost.ini %*
pause

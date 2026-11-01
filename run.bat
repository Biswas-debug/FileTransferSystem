@echo off
cd /d "%~dp0"
title TCP File Transfer System

echo ========================================================
echo        STARTING TCP FILE TRANSFER SYSTEM
echo ========================================================
echo.
echo [1/2] Starting Server...
start "TCP File Transfer Server" server.exe

REM wait 1 second for server to initialize
ping 127.0.0.1 -n 2 >nul

echo [2/2] Starting GUI Client...
start "TCP File Transfer Client" gui_client.exe

echo.
echo ========================================================
echo  SUCCESS: Server and GUI Client have been launched!
echo.
echo  - Look for the 'TCP File Transfer Client' window on your screen
echo  - Look for the 'TCP File Transfer Server' black console window
echo.
echo  To connect a second client at the same time:
echo    Double-click gui_client.exe or client.exe in this folder!
echo ========================================================
echo.
pause

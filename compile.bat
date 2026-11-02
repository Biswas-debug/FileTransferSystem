@echo off
REM ──────────────────────────────────────────────────────────────────────
REM  Build script for TCP File Transfer System
REM  Requires: MSYS2  (C:\msys64\ucrt64\bin\gcc.exe)
REM ──────────────────────────────────────────────────────────────────────

set GCC=C:\msys64\ucrt64\bin\gcc.exe
set PATH=C:\msys64\ucrt64\bin;%PATH%

echo.
echo [1/3] Compiling server.c  ...
"%GCC%" -O2 server.c -o server.exe -lws2_32 -lurlmon -static-libgcc
if %errorlevel% neq 0 (
    echo   FAILED - server.exe not updated
) else (
    echo   OK    - server.exe
)

echo.
echo [2/3] Compiling client.c (CLI Client)  ...
"%GCC%" -O2 client.c -o client.exe -lws2_32 -lcomdlg32 -static-libgcc
if %errorlevel% neq 0 (
    echo   FAILED - client.exe not updated
) else (
    echo   OK    - client.exe
)

echo.
echo [3/3] Compiling gui_client.c (Win32 GUI Client)  ...
"%GCC%" -O2 gui_client.c -o gui_client.exe ^
    -lws2_32 -lcomdlg32 -lcomctl32 -mwindows -static-libgcc
if %errorlevel% neq 0 (
    echo   FAILED - gui_client.exe not updated
) else (
    echo   OK    - gui_client.exe
)

echo.
echo ========================================================
echo All binaries compiled successfully with -static-libgcc!
echo They can now be copied and run on any Windows PC.
echo ========================================================
echo.
pause

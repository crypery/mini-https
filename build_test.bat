@echo off
cd /d "%~dp0"
echo Building unified HTTP test harness...
gcc -O2 -Wall -Wextra -o http_test.exe -I. -I./zlib -I./crypery -I./utilery ./zlib/*.c ./crypery/*.c ./utilery/*.c http_test.c -lws2_32 -lz
if %errorlevel% neq 0 (
    echo Build FAILED
    exit /b 1
)
echo Build OK
echo.
echo Usage (server must be running):
echo   http_test.exe base https 127.0.0.1 443 all     REM server-base.exe, TLS
echo   http_test.exe web  https 127.0.0.1 443 all     REM server.exe, TLS
echo   http_test.exe base http  127.0.0.1 443 stress  REM plain HTTP
echo   http_test.exe web  http  127.0.0.1 443 unit    REM plain HTTP, unit only
exit /b 0

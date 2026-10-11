@echo off
cd /d "%~dp0"
echo Building web test harness...
gcc -O2 -Wall -Wextra -o web_test.exe -I. -I./zlib -I./crypery -I./utilery ./zlib/*.c ./crypery/*.c ./utilery/*.c web_test.c -lws2_32 -lz
if %errorlevel% neq 0 (
    echo Build FAILED
    exit /b 1
)
echo Build OK
echo.
echo Usage (server must be running):
echo   web_test.exe base https 127.0.0.1 443 all     REM server-base.exe, TLS
echo   web_test.exe web  https 127.0.0.1 443 all     REM server.exe, TLS
echo   web_test.exe base http  127.0.0.1 443 stress  REM plain HTTP
exit /b 0

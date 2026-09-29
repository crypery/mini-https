@echo off
echo Building unit tests...
gcc -O2 -o ssl_test.exe -I. -I./zlib -I./crypery -I./utilery ./zlib/*.c ./crypery/*.c ./utilery/*.c ssl_test.c -lws2_32 -lz
if %errorlevel% neq 0 (
    echo Build FAILED
    exit /b 1
)
echo Running unit tests...
ssl_test.exe https 127.0.0.1 443 stress
if %errorlevel% neq 0 (
    echo Tests FAILED
    exit /b 1
)
echo All tests passed.

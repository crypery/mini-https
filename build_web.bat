@echo off
echo Building Server with HTTPS support...
gcc -O2 -o server.exe -I. -I./zlib -I./crypery -I./utilery ./zlib/*.c ./crypery/*.c ./utilery/*.c server.c -lws2_32 -lz
if %errorlevel% neq 0 (
    echo Build FAILED
    exit /b 1
)

echo Build OK

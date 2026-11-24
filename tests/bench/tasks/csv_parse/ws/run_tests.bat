@echo off
gcc -std=c99 -Wall -Iinclude tests/test_visible.c src/strutil.c -o test_visible.exe
if errorlevel 1 exit /b 1
test_visible.exe

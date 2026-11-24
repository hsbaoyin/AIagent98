@echo off
gcc -std=c99 -Wall -Iinclude tests/test_visible.c src/idset.c src/idfmt.c -o test_visible.exe
if errorlevel 1 exit /b 1
test_visible.exe

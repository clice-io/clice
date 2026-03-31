@echo off
for /f "delims=" %%i in ('clang++ --print-resource-dir') do set "CLANG_RES=%%i"
set "PATH=%CLANG_RES%\lib\windows;%PATH%"

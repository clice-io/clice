@echo off
for /f "delims=" %%i in ('clang-cl --print-resource-dir') do set "CLANG_RES=%%i"
set "PATH=%CLANG_RES%\lib\windows;%PATH%"

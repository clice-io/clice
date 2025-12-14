@echo off
setlocal

for /f "usebackq delims=" %%i in (`clang-cl /clang:-print-resource-dir`) do set "CLANG_RES=%%i"

rem clang runtime libs/import libs live here (asan/ubsan/etc)
set "CLANG_RT=%CLANG_RES%\lib\windows"

rem Link-time: make link.exe/lld-link able to find clang_rt.*.lib
set "LIB=%CLANG_RT%;%LIB%"

rem Optional: also inject an explicit /LIBPATH for tools that only look at LINK flags
set "LINK=/LIBPATH:%CLANG_RT% %LINK%"

rem Run-time: make loader find clang_rt.*.dll
set "PATH=%CLANG_RT%;%PATH%"

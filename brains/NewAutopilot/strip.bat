@echo off
:: Strips debug calls from NewAutopilot Lua sources and writes cleaned
:: copies to opt/.  Run this after editing any .lua file when you want
:: to refresh the production (non-debug) brain used by WinBolo/WinBoloDS.
::
:: Usage: strip.bat [path\to\lua_strip.exe]
::   Default lua_strip location: ..\..\build\Release\lua_strip.exe

setlocal
set SCRIPT_DIR=%~dp0
set LUA_STRIP=%~1
if "%LUA_STRIP%"=="" set LUA_STRIP=%SCRIPT_DIR%..\..\build\Release\lua_strip.exe

if not exist "%LUA_STRIP%" (
    echo ERROR: lua_strip.exe not found at %LUA_STRIP%
    echo Build the lua_strip target first, or pass its path as an argument.
    exit /b 1
)

"%LUA_STRIP%" ^
    --strip print2 ^
    --strip "viz." ^
    --strip overlay_ ^
    --strip "io.open" ^
    "%SCRIPT_DIR%opt" ^
    "%SCRIPT_DIR%*.lua"

if %errorlevel% neq 0 (
    echo Stripping failed.
    exit /b %errorlevel%
)
echo Done. Stripped files written to %SCRIPT_DIR%opt\

@echo off
:: Strips debug calls from GoalHunter Lua sources and writes cleaned
:: copies to opt/.  Run this after editing any .lua file when you want
:: to refresh the production (non-debug) brain used by WinBolo/WinBoloDS.
::
:: Usage: strip.bat [path\to\lua_strip.exe]
::   Default lua_strip location: ..\..\build\Release\lua_strip.exe

setlocal enabledelayedexpansion
set SCRIPT_DIR=%~dp0
set LUA_STRIP=%~1
if "%LUA_STRIP%"=="" set LUA_STRIP=%SCRIPT_DIR%..\..\build\Release\lua_strip.exe

if not exist "%LUA_STRIP%" (
    echo ERROR: lua_strip.exe not found at %LUA_STRIP%
    echo Build the lua_strip target first, or pass its path as an argument.
    exit /b 1
)

:: lua_strip does not expand wildcards itself, so build the .lua file list
:: here and pass explicit paths.
set FILES=
for %%f in ("%SCRIPT_DIR%*.lua") do set FILES=!FILES! "%%f"

if "!FILES!"=="" (
    echo ERROR: no .lua files found in %SCRIPT_DIR%
    exit /b 1
)

"%LUA_STRIP%" ^
    --strip print2 ^
    --strip "viz." ^
    --strip overlay_ ^
    --strip-block "if BRAIN_DEBUG_MODE" ^
    --exclude los_stamp_cache.lua ^
    --exclude shield_stamp_cache.lua ^
    "%SCRIPT_DIR%opt" ^
    !FILES!

if %errorlevel% neq 0 (
    echo Stripping failed.
    exit /b %errorlevel%
)
echo Done. Stripped files written to %SCRIPT_DIR%opt\

@echo off
rem =====================================================================
rem Survival with full bot brain-debug recording: a dedicated server
rem (WinBoloDS) runs the scenario with -brain-debug. Only THIS server
rem records -- your regular single-player games are untouched.
rem
rem Connect manually from WinBolo: LAN screen -> join the Survival
rem server on port 27500.
rem
rem Every bot the server runs (the wave attackers and any defender bots
rem added in the lobby) records its pool breakdown, visualizer overlays,
rem print2 log and per-tick jsonl into:
rem
rem     build\debug_sessions\<timestamp>_survival\
rem
rem Load a session in BrainTest ("Load Session") to replay what each bot
rem was thinking. The scenario's live map edits (per-round forest fill,
rem base/pill flips) reach every bot through the normal delta stream, so
rem recordings show the world exactly as the bots saw it.
rem =====================================================================
setlocal
set PORT=27500
set WINBOLO_BRAINDBG_LABEL=survival
cd /d "%~dp0"
WinBoloDS.exe -map "data/maps/Survival.map" -port %PORT% -gametype scripted -ai yes -brain-debug -firstjoinhost -nowinbolonet -log
rem Keep the window open if the server exited with an error (port in use,
rem bad map, ...) so the message is readable instead of flashing away.
if errorlevel 1 pause
endlocal

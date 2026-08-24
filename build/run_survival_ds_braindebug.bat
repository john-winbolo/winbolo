@echo off
rem =====================================================================
rem Survival with full bot brain-debug recording, hosted the right way:
rem a dedicated server (WinBoloDS) runs the scenario with -brain-debug,
rem and a normal WinBolo client auto-joins it. Only THIS server records
rem -- your regular single-player games are untouched.
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
rem
rem In the client: you land in the server's lobby -- Ready when set.
rem =====================================================================
setlocal
set PORT=27500
set WINBOLO_BRAINDBG_LABEL=survival
cd /d "%~dp0"
start "Survival DS" WinBoloDS.exe -map "data/maps/Survival.map" -port %PORT% -gametype scripted -ai yes -brain-debug -nowinbolonet
rem give the server a moment to bind before the client knocks
timeout /t 2 /nobreak > nul
start "" WinBolo.exe +connect localhost:%PORT%
endlocal

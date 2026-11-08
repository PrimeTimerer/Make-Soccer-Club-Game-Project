@echo off
rem Same as run_logged.bat but with the old (legacy) D3D12 renderer, for comparison or rollback.
setlocal
set RSX_LIVE_DRAW=0
call "%~dp0run_logged.bat" %*

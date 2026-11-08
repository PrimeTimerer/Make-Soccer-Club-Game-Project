@echo off
rem Experiment: same as run_logged.bat (live renderer, F12 capture, logs\live_*.txt), but the
rem SPU task that builds each frame's RSX commands/data (gs_task) runs in the SPU interpreter
rem instead of its lifted code. Slower. If the match 3D looks right this way, the lifted task
rem is at fault.
setlocal
set SAKA_SPU_TASK_REGIONS=0
call "%~dp0run_logged.bat" %*

@echo off
rem Same as run.bat, but the console output goes to logs\live_out.txt / live_err.txt
rem (with file-access logging) so the run can be analysed afterwards.
setlocal
set "ROOT=%~dp0"
set "PS3_VFS_ROOT=%ROOT%game\dev_bdvd"
set "PS3_HDD0_ROOT=%ROOT%game\dev_hdd0"
set RT_DISPLAY_BY_SIZE=1
rem Renderer: the live-draw engine (the path the Killzone 2/3 ports use). Set RSX_LIVE_DRAW=0
rem before running this file to go back to the legacy D3D12 backend.
if not defined RSX_LIVE_DRAW set RSX_LIVE_DRAW=1
set PS3_FSLOG=1
rem Every 5 s: audio blocks/s, silent share, and periods held for a late producer.
set AUDIO_RATE=1
rem F12 in the game window saves the current frame (draw list, textures, screen) here.
set "RSX_CAP_DIR=%ROOT%logs\cap_live"
if not exist "%ROOT%logs" mkdir "%ROOT%logs"
rem Live renderer: F12 opens this gate file for 0.6 s; the engine then dumps every render
rem surface once (surf\*.ppm) and writes the per-draw CSV for those frames.
if not exist "%ROOT%logs\cap_live\surf" mkdir "%ROOT%logs\cap_live\surf"
set "SAKA_CAPTURE_GATE=%ROOT%logs\cap_live\gate.txt"
set "LD_SURF_DUMP=%ROOT%logs\cap_live\surf"
set "LD_SURF_DUMP_GATE=%ROOT%logs\cap_live\gate.txt"
set "RSX_DRAW_CSV=%ROOT%logs\cap_live\draws.csv"
set "RSX_DRAW_CSV_GATE=%ROOT%logs\cap_live\gate.txt"
if exist "%ROOT%logs\cap_live\gate.txt" del "%ROOT%logs\cap_live\gate.txt"
set "EXE=%ROOT%build-release\SakatsukuRecomp.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\SakatsukuRecomp.exe"
"%EXE%" "%ROOT%game\EBOOT.ELF" %* > "%ROOT%logs\live_out.txt" 2> "%ROOT%logs\live_err.txt"

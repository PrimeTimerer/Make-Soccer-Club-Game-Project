@echo off
rem Run the recompiled Sakatsuku 1.03 with the disc + HDD patch layout from PORT_1.03_RUNTIME.md.
rem   /dev_bdvd -> game\dev_bdvd (original disc, read-only)   /dev_hdd0 -> game\dev_hdd0 (1.03 patch, writable)
setlocal
set "ROOT=%~dp0"
set "PS3_VFS_ROOT=%ROOT%game\dev_bdvd"
set "PS3_HDD0_ROOT=%ROOT%game\dev_hdd0"
rem The game renders into a 1280x720 surface at 0x00F00000, not the registered display buffers.
set RT_DISPLAY_BY_SIZE=1
rem Renderer: the live-draw engine (the path the Killzone 2/3 ports use). Set RSX_LIVE_DRAW=0
rem before running this file to go back to the legacy D3D12 backend.
if not defined RSX_LIVE_DRAW set RSX_LIVE_DRAW=1
rem Optimized Release build (build_release.bat) when present, else the Debug build (build.bat).
set "EXE=%ROOT%build-release\SakatsukuRecomp.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\SakatsukuRecomp.exe"
"%EXE%" "%ROOT%game\EBOOT.ELF" %*

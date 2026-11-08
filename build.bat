@echo off
rem Sakatsuku PS3 (BLJM61064, 1.03) recomp build -- clang-cl + Ninja.
rem No CMAKE_BUILD_TYPE on purpose: the template then compiles the huge lifted
rem PPU files at -O1 (its intended setting); Release would force /O2 and is much slower.
rem Usage: build.bat [extra cmake --build args]   (delete build\ to reconfigure)
setlocal
set "ROOT=%~dp0"
rem ps3recomp's Python tools write UTF-8 text; the Korean Windows default (cp949) breaks them.
set PYTHONUTF8=1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin;%PATH%"
cd /d "%ROOT%"
rem Configure every time (a few seconds) so changed options and new source globs apply.
cmake -S . -B build -G Ninja ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  "-DPython3_EXECUTABLE=%ROOT%.venv\Scripts\python.exe" ^
  "-DPORT_SPU_INPUTS=%ROOT%game\EBOOT.ELF;%ROOT%game\firmware\libsre.prx;%ROOT%game\firmware\libspurs_jq.prx;%ROOT%spu_jobs\cri_atomdecoder.elf;%ROOT%spu_jobs\cri_asrvoice.elf;%ROOT%spu_jobs\cri_dsppack1.elf;%ROOT%spu_jobs\cri_dsppack2.elf;%ROOT%spu_jobs\cri_dsppitchshifter.elf;%ROOT%spu_jobs\cri_hcamixer.elf" || exit /b 1
rem 24 parallel jobs keeps the ~100k-line lifted files well inside 64 GB RAM.
cmake --build build -j 24 %* || exit /b 1

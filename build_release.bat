@echo off
rem Optimised build (Release: /O2, no D3D12 debug layer) in build-release\.
rem build.bat keeps the Debug build in build\ for diagnosis.
setlocal
set "ROOT=%~dp0"
set PYTHONUTF8=1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin;%PATH%"
cd /d "%ROOT%"
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  "-DPython3_EXECUTABLE=%ROOT%.venv\Scripts\python.exe" ^
  "-DPORT_SPU_INPUTS=%ROOT%game\EBOOT.ELF;%ROOT%game\firmware\libsre.prx;%ROOT%game\firmware\libspurs_jq.prx;%ROOT%spu_jobs\cri_atomdecoder.elf;%ROOT%spu_jobs\cri_asrvoice.elf;%ROOT%spu_jobs\cri_dsppack1.elf;%ROOT%spu_jobs\cri_dsppack2.elf;%ROOT%spu_jobs\cri_dsppitchshifter.elf;%ROOT%spu_jobs\cri_hcamixer.elf" || exit /b 1
rem 16 jobs: optimised compiles of the large lifted files use far more memory than Debug.
cmake --build build-release -j 16 %* || exit /b 1

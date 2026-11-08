@echo off
rem Builds "Sakatsuku Launcher.exe" (tools\launcher, WPF) next to run.bat with the C# compiler of the
rem .NET Framework 4 that is part of Windows: nothing to install. The launcher keeps its settings in
rem launcher.ini and starts build-release\SakatsukuRecomp.exe with them, the way run.bat does.
setlocal
set "ROOT=%~dp0"
set "FW=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319"
set "SRC=%ROOT%tools\launcher"
if not exist "%FW%\csc.exe" (
  echo %FW%\csc.exe not found: the launcher needs the .NET Framework 4, part of Windows.
  exit /b 1
)
if not exist "%SRC%\icon.ico" powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%\make_icon.ps1"
"%FW%\csc.exe" /nologo /target:winexe /optimize+ /codepage:65001 "/out:%ROOT%Sakatsuku Launcher.exe" "/win32icon:%SRC%\icon.ico" ^
  /r:System.Core.dll /r:System.Xaml.dll /r:System.Xml.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll ^
  /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Web.Extensions.dll /r:Microsoft.VisualBasic.dll ^
  "/r:%FW%\WPF\PresentationCore.dll" "/r:%FW%\WPF\PresentationFramework.dll" "/r:%FW%\WPF\WindowsBase.dll" ^
  "/resource:%SRC%\Theme.xaml,Theme.xaml" "%SRC%\*.cs"
if errorlevel 1 exit /b 1
echo Built "%ROOT%Sakatsuku Launcher.exe"

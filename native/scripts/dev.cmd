@echo off
rem Runs a command inside the MSVC x64 developer environment with Qt, FFmpeg and OpenColorIO located.
rem   scripts\dev.cmd cmake --preset debug
rem   scripts\dev.cmd cmake --build --preset debug
rem   scripts\dev.cmd ctest --preset debug
rem Override QT_ROOT / FFMPEG_ROOT / OCIO_ROOT in the environment if they live elsewhere.
setlocal
if not defined QT_ROOT set "QT_ROOT=C:\Qt\6.8.3\msvc2022_64"
if not defined FFMPEG_ROOT set "FFMPEG_ROOT=C:\dev\ffmpeg-8.1-gpl"
if not defined OCIO_ROOT set "OCIO_ROOT=C:\dev\ocio"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo MSVC x64 tools not found. Install Visual Studio 2022 Build Tools with the C++ workload.
  exit /b 1
)
rem vcvars64 runs vswhere itself and expects the installer folder on PATH
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%QT_ROOT%\bin;%FFMPEG_ROOT%\bin;%OCIO_ROOT%\bin;%PATH%"
cd /d "%~dp0.."
%*

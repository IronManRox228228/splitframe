@echo off
rem Builds SplitFrame (native, debug preset) and opens it. Double-click to run.
rem Paths for Qt, FFmpeg and OpenColorIO come from native\scripts\dev.cmd (override QT_ROOT / FFMPEG_ROOT / OCIO_ROOT there).
setlocal
set "NATIVE=%~dp0native"
if not defined QT_ROOT set "QT_ROOT=C:\Qt\6.8.3\msvc2022_64"
if not defined FFMPEG_ROOT set "FFMPEG_ROOT=C:\dev\ffmpeg-8.1-gpl"
if not defined OCIO_ROOT set "OCIO_ROOT=C:\dev\ocio"

if not exist "%NATIVE%\build\debug\CMakeCache.txt" (
  echo Configuring...
  call "%NATIVE%\scripts\dev.cmd" cmake --preset debug || goto :fail
)
echo Building...
call "%NATIVE%\scripts\dev.cmd" cmake --build --preset debug -j 8 || goto :fail

set "PATH=%QT_ROOT%\bin;%FFMPEG_ROOT%\bin;%OCIO_ROOT%\bin;%PATH%"
echo Starting SplitFrame...
start "" "%NATIVE%\build\debug\bin\splitframe.exe"
exit /b 0

:fail
echo.
echo Build failed. Scroll up for the first error.
pause
exit /b 1

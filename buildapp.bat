@echo off
rem ---------------------------------------------------------------------------
rem Build the desktop application FROM THIS TREE.
rem
rem The shell (Win32 + WebView2 + WebGPU canvas) lives in
rem "D:\mandelbrot view\Meter Mandelbrot"; the renderer lives here in src\.
rem This script drives that shell build with MB_CORE pointing at this tree and
rem drops the result into dist\, so the whole product can be produced from
rem C:\Users\harve\Desktop\mande\new without touching the old project by hand.
rem
rem   buildapp.bat            build + export to dist\
rem   buildapp.bat --no-build export the existing exe only
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

rem Everything is relative to this checkout: src\ is the renderer, desktop\
rem is the Win32/WebView2 shell, runtime\ holds the shipped GMP DLL.
set "CORE=%~dp0src"
set "SHELL=%~dp0desktop"
set "EXE=%~dp0Mandelbrot View.exe"
set "DLL=%~dp0runtime\libgmp-10.dll"

if not exist "%SHELL%\build.bat" (
  echo [app] shell not found under "%SHELL%"
  exit /b 1
)
if not exist "%CORE%\deep.cpp" (
  echo [app] core not found under "%CORE%"
  exit /b 1
)

if /I not "%1"=="--no-build" (
  echo [app] building the shell against "%CORE%" ...
  set "MB_CORE=%CORE%"
  call "%SHELL%\build.bat"
  if errorlevel 1 exit /b 1
)

if not exist dist mkdir dist
copy /y "%EXE%" "dist\Mandelbrot View.exe" >nul || exit /b 1
if exist "%DLL%" copy /y "%DLL%" "dist\libgmp-10.dll" >nul
rem The command-line engine belongs in the same folder: it is the thing that
rem can be scripted, verified against the oracle and used for batch renders.
if exist "%~dp0build\mset.exe" copy /y "%~dp0build\mset.exe" "dist\mset.exe" >nul
echo [app] exported to "%~dp0dist"

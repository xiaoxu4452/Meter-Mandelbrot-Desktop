@echo off
rem Build and run the applied-math unit checks (BigFloat elementary functions).
setlocal
cd /d "%~dp0"
call "%~dp0..\env-msvc.bat" || exit /b 1
if not exist "%~dp0..\build" mkdir "%~dp0..\build"
cl /nologo /std:c++20 /O2 /EHsc /utf-8 /W3 /I "%~dp0..\src" ^
  "%~dp0..\src\bigfloat.cpp" "%~dp0..\src\mathx\bigmath.cpp" ^
  "%~dp0test_bigmath.cpp" /Fo:"%~dp0..\build\\" /Fe:"%~dp0..\build\test_bigmath.exe"
if errorlevel 1 exit /b 1
"%~dp0..\build\test_bigmath.exe"

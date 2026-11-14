@echo off
setlocal
cd /d "%~dp0"
call "%~dp0env-msvc.bat" || exit /b 1
if not exist build mkdir build
cl /nologo /std:c++20 /O2 /EHsc /utf-8 /I src /Fe:build\b1.exe /Fo:build\ tests\b1\b1_view.cpp src\bigfloat.cpp src\reference.cpp
if errorlevel 1 exit /b 1
build\b1.exe

@echo off
setlocal
cd /d "%~dp0"
call "%~dp0env-msvc.bat" || exit /b 1
if not exist build mkdir build
set CORE=src\bigfloat.cpp src\gmp_bridge.cpp src\dive.cpp src\reference.cpp src\palette.cpp src\image.cpp src\render.cpp
if /I not "%1"=="f2test" set CORE=%CORE% src\deep.cpp
cl /nologo /std:c++20 /O2 /arch:AVX2 /EHsc /utf-8 /I src /Fo:build\ /Fe:build\%1.exe tests\b2\%1.cpp %CORE%
if errorlevel 1 exit /b 1
build\%1.exe %2 %3 %4 %5 %6 %7 %8 %9

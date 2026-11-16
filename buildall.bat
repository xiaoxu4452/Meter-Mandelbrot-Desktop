@echo off
rem Build the standalone engine (mset.exe) with cl directly.  cmake cannot run
rem on this machine (vcvars/cmake call reg.exe, which the security policy
rem blocks), so the translation-unit list is spelled out here.
setlocal
cd /d "%~dp0"
call "%~dp0env-msvc.bat" || exit /b 1
if not exist build mkdir build
set SRC=src\bigfloat.cpp src\mathx\bigmath.cpp src\mathx\expr.cpp src\mathx\families.cpp src\gmp_bridge.cpp src\dive.cpp src\reference.cpp src\deep.cpp src\render.cpp src\palette.cpp src\image.cpp src\workspace.cpp src\port\registry.cpp src\main.cpp
cl /nologo /std:c++20 /O2 /arch:AVX2 /GL /EHsc /utf-8 /W3 /I src /Fo:build\ /Fe:build\mset.exe %SRC% /link /LTCG
if errorlevel 1 exit /b 1
echo Built build\mset.exe

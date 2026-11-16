@echo off
setlocal
cd /d "%~dp0"
call "%~dp0..\env-msvc.bat" >nul 2>&1
cl /nologo /O2 /EHsc /utf-8 /std:c++17 /I "%~dp0..\src" ^
  "%~dp0test_expr.cpp" ^
  "%~dp0..\src\bigfloat.cpp" ^
  "%~dp0..\src\mathx\bigmath.cpp" ^
  "%~dp0..\src\mathx\expr.cpp" ^
  /Fe:"%~dp0test_expr.exe" > "%~dp0expr-build.log" 2>&1
if errorlevel 1 (
  echo [expr] build failed - see tests\expr-build.log
  type "%~dp0expr-build.log"
  exit /b 1
)
"%~dp0test_expr.exe"

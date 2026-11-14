@echo off
setlocal
rem ---------------------------------------------------------------------------
rem Boost <-> MPFR reference-orbit bridge diagnostic.
rem Boost is header-only here; no GMP/MPFR/OpenCL runtime is linked.
rem mb_hf.cpp is compiled because mb_mpf.cpp references the HF primitives.
rem ---------------------------------------------------------------------------
set "SRC=%~dp0"
call "%SRC%env-msvc.bat" || exit /b 1
if not exist "%SRC%build" mkdir "%SRC%build"
if not exist "%SRC%bin" mkdir "%SRC%bin"
cl /nologo /O2 /GL /MT /EHsc /std:c++17 /utf-8 /arch:AVX2 /DNDEBUG ^
  /I "%SRC%core" /I "%SRC%gpu" /I "%SRC%third_party" /I "%SRC%third_party\boost" ^
  /Fo"%SRC%build\\" ^
  "%SRC%core\mb_bridge.cpp" "%SRC%core\mb_mpf.cpp" ^
  "%SRC%core\mb_precision.cpp" "%SRC%core\mb_hf.cpp" ^
  "%SRC%gpu\mb_bridgecheck.cpp" ^
  /Fe"%SRC%bin\mb_bridgecheck.exe" ^
  /link /SUBSYSTEM:CONSOLE /LTCG
if errorlevel 1 (
  echo [bridgecheck] BUILD FAILED
  exit /b 1
)
echo [bridgecheck] OK
endlocal

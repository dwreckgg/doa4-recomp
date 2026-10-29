@echo off
rem Builds and runs the DPPS cross-CPU check with clang-cl at the game's CPU level (no CMake).
rem The exe (%TEMP%\doa4_dpps_test\dpps_host_test.exe) needs no install: copy it to another PC
rem and run it there to compare the "DPPS combined hash" line.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "OUT=%TEMP%\doa4_dpps_test"
if not exist "%OUT%" mkdir "%OUT%"
clang-cl /nologo /std:c++latest /EHsc /O2 /fp:strict /MT -march=x86-64-v3 "%~dp0dpps_host_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\dpps_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\dpps_host_test.exe"

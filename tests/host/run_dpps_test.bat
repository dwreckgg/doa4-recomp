@echo off
rem Builds and runs the DPPS cross-CPU check with clang-cl at the game's CPU level (no CMake).
rem The exe (%TEMP%\doa4_dpps_test\dpps_host_test.exe) needs no install: copy it to another PC
rem and run it there to compare the "DPPS combined hash" line (raw DPPS) and the
rem "DPPS canonical hash" line (every NaN lane made 0xFFC00000 by the SDK's own function).
rem The test includes <rex/ppc/dot_product.h> from the SDK the netplay build uses (REXSDK_DIR,
rem default sdk-np0 beside this worktree), so the include paths are that SDK's include folder and simde.
rem Optional first argument: an include folder searched BEFORE the SDK's (the mutation probe's copies).
setlocal
if "%REXSDK_DIR%"=="" set "REXSDK_DIR=%~dp0..\..\..\sdk-np0"
if not defined DOA4_VSDEVCMD set "DOA4_VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
call "%DOA4_VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul 2>&1
if not defined DOA4_LLVM_BIN set "DOA4_LLVM_BIN=C:\Program Files\LLVM\bin"
set "PATH=%DOA4_LLVM_BIN%;%PATH%"
if "%OUT%"=="" set "OUT=%TEMP%\doa4_dpps_test"
if not exist "%OUT%" mkdir "%OUT%"
set EXTRA=
if not "%~1"=="" set EXTRA=/I"%~1"
clang-cl /nologo /std:c++latest /EHsc /O2 /fp:strict /MT -march=x86-64-v3 %EXTRA% /I"%REXSDK_DIR%\include" /I"%REXSDK_DIR%\thirdparty\simde" "%~dp0dpps_host_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\dpps_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\dpps_host_test.exe"

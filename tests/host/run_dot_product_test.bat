@echo off
rem Builds and runs the dot-product NaN redirect host test with clang-cl at the game's CPU level (no CMake).
rem It includes <rex/ppc.h> from the SDK the netplay build uses (REXSDK_DIR, default sdk-np0) the way the
rem generated code does, so the include paths are the SDK's include folder and simde.
rem Prints "PASSED: N checks, 0 failures" (or FAILED) as its last line. Optional first argument: an
rem extra include folder searched BEFORE the SDK's (the mutation probe puts its header copies there).
setlocal
if "%REXSDK_DIR%"=="" set "REXSDK_DIR=C:\Users\fsop1\doa4\sdk-np0"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if "%OUT%"=="" set "OUT=%TEMP%\doa4_dot_product_test"
if not exist "%OUT%" mkdir "%OUT%"
set EXTRA=
if not "%~1"=="" set EXTRA=/I"%~1"
clang-cl /nologo /std:c++latest /EHsc /O2 /fp:strict /MT -march=x86-64-v3 -msse4.1 %EXTRA% /I"%REXSDK_DIR%\include" /I"%REXSDK_DIR%\thirdparty\simde" /I"%REXSDK_DIR%\thirdparty\fmt\include" /I"%REXSDK_DIR%\thirdparty\spdlog\include" /showIncludes "%~dp0dot_product_host_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\dot_product_host_test.exe" > "%OUT%\build.log" 2>&1
if errorlevel 1 (
  type "%OUT%\build.log"
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\dot_product_host_test.exe"

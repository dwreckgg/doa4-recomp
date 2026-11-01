@echo off
rem Builds and runs the online session core host test with clang-cl (no CMake): np_live.cpp over np_exec.cpp,
rem np_gekko.cpp, np_core.cpp, np_rules.cpp, np_wire.cpp and every third_party\GekkoNet\src\*.cpp, with the pair test's
rem flags (GEKKONET_STATIC GEKKONET_NO_ASIO, C++20, _MSVC_STL_HARDENING). No SDK headers, no socket, no file written
rem outside the output folder. The two sessions run as fibers on one thread over a fake clock (np_live_host_test.cpp).
rem Pass -v to print each check group before it runs.
rem NP_LIVE_OUT (optional) names the output folder; the mutation probes use it so that they never share one.
setlocal enabledelayedexpansion
if not defined DOA4_VSDEVCMD set "DOA4_VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
call "%DOA4_VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul 2>&1
if not defined DOA4_LLVM_BIN set "DOA4_LLVM_BIN=C:\Program Files\LLVM\bin"
set "PATH=%DOA4_LLVM_BIN%;%PATH%"
if not defined NP_LIVE_OUT set "NP_LIVE_OUT=%TEMP%\doa4_np_live_test"
set "OUT=%NP_LIVE_OUT%"
set "G=%~dp0..\..\third_party\GekkoNet"
set "N=%~dp0..\..\src\net"
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\np_live_host_test.exe" del "%OUT%\np_live_host_test.exe"
set "SRCS="
for %%f in ("%G%\src\*.cpp") do set SRCS=!SRCS! "%%f"
echo GekkoNet sources: %G%
clang-cl /nologo /std:c++20 /EHsc /O2 /DGEKKONET_STATIC /DGEKKONET_NO_ASIO /D_MSVC_STL_HARDENING=1 /I"%G%\include" /I"%G%\include\private" /I"%G%\thirdparty" "%~dp0np_live_host_test.cpp" "%N%\np_live.cpp" "%N%\np_exec.cpp" "%N%\np_gekko.cpp" "%N%\np_core.cpp" "%N%\np_rules.cpp" "%N%\np_wire.cpp" !SRCS! /Fo"%OUT%\\" /Fe"%OUT%\np_live_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\np_live_host_test.exe" %*

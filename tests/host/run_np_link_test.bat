@echo off
rem Builds and runs the link host test with clang-cl (no CMake): two real GekkoNet sessions (np_gekko, np_exec,
rem np_core, every third_party\GekkoNet\src\*.cpp) over two real transports (np_transport, np_wire, np_rules) on
rem 127.0.0.1, paced by wall clock. Nothing reaches the internet. Flags as run_np_exec_test.bat plus Winsock.
rem NP_LINK_OUT (optional) names the output folder; the mutation probes use it so that they never share one.
setlocal enabledelayedexpansion
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if not defined NP_LINK_OUT set "NP_LINK_OUT=%TEMP%\doa4_np_link_test"
set "OUT=%NP_LINK_OUT%"
set "G=%~dp0..\..\third_party\GekkoNet"
set "N=%~dp0..\..\src\net"
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\np_link_host_test.exe" del "%OUT%\np_link_host_test.exe"
set "SRCS="
for %%f in ("%G%\src\*.cpp") do set SRCS=!SRCS! "%%f"
echo GekkoNet sources: %G%
clang-cl /nologo /std:c++20 /EHsc /O2 /DGEKKONET_STATIC /DGEKKONET_NO_ASIO /D_MSVC_STL_HARDENING=1 /D_WIN32_WINNT=0x0A00 /I"%G%\include" /I"%G%\include\private" /I"%G%\thirdparty" "%~dp0np_link_host_test.cpp" "%N%\np_transport.cpp" "%N%\np_wire.cpp" "%N%\np_rules.cpp" "%N%\np_gekko.cpp" "%N%\np_exec.cpp" "%N%\np_core.cpp" !SRCS! /Fo"%OUT%\\" /Fe"%OUT%\np_link_host_test.exe" ws2_32.lib iphlpapi.lib
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\np_link_host_test.exe" %*

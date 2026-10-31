@echo off
rem Builds and runs the event-list executor / GekkoNet wrapper host test with clang-cl (no CMake): np_exec.cpp,
rem np_gekko.cpp, np_core.cpp and every third_party\GekkoNet\src\*.cpp, with the pair test's flags (GEKKONET_STATIC
rem GEKKONET_NO_ASIO, C++20, _MSVC_STL_HARDENING). No SDK headers, no socket, no file written outside the output folder.
rem Pass -v to print each check group before it runs.
rem NP_EXEC_OUT (optional) names the output folder; the mutation probes use it so that they never share one.
setlocal enabledelayedexpansion
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if not defined NP_EXEC_OUT set "NP_EXEC_OUT=%TEMP%\doa4_np_exec_test"
set "OUT=%NP_EXEC_OUT%"
set "G=%~dp0..\..\third_party\GekkoNet"
set "N=%~dp0..\..\src\net"
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\np_exec_host_test.exe" del "%OUT%\np_exec_host_test.exe"
set "SRCS="
for %%f in ("%G%\src\*.cpp") do set SRCS=!SRCS! "%%f"
echo GekkoNet sources: %G%
clang-cl /nologo /std:c++20 /EHsc /O2 /DGEKKONET_STATIC /DGEKKONET_NO_ASIO /D_MSVC_STL_HARDENING=1 /I"%G%\include" /I"%G%\include\private" /I"%G%\thirdparty" "%~dp0np_exec_host_test.cpp" "%N%\np_exec.cpp" "%N%\np_gekko.cpp" "%N%\np_core.cpp" !SRCS! /Fo"%OUT%\\" /Fe"%OUT%\np_exec_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\np_exec_host_test.exe" %*

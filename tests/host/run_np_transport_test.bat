@echo off
rem Builds and runs the transport host test with clang-cl (no CMake): np_transport.cpp (one UDP socket on its own I/O
rem thread, np_spsc.h rings, STUN client, simulator), np_wire.cpp and np_rules.cpp, linked with ws2_32 and iphlpapi.
rem Every socket of the test binds 127.0.0.1 and nothing reaches the internet (the STUN servers are fakes on loopback).
rem np_upnp.cpp is compiled once (/c, same flags, the miniupnpc headers) so that it is known to build; nothing calls it
rem here and it is not linked (the router is the user's).
rem _MSVC_STL_HARDENING makes an out-of-range index fast-fail. Pass -v to print each check group before it runs.
rem NP_TRANSPORT_OUT (optional) names the output folder; the mutation probes use it so that they never share one.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if not defined NP_TRANSPORT_OUT set "NP_TRANSPORT_OUT=%TEMP%\doa4_np_transport_test"
set "OUT=%NP_TRANSPORT_OUT%"
set "N=%~dp0..\..\src\net"
set "U=%~dp0..\..\third_party\miniupnpc"
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\np_transport_host_test.exe" del "%OUT%\np_transport_host_test.exe"
if exist "%OUT%\np_upnp.obj" del "%OUT%\np_upnp.obj"
set "FLAGS=/nologo /std:c++20 /EHsc /O2 /D_MSVC_STL_HARDENING=1 /D_WIN32_WINNT=0x0A00 /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
clang-cl %FLAGS% /DMINIUPNP_STATICLIB /I"%U%\include" /c "%N%\np_upnp.cpp" /Fo"%OUT%\np_upnp.obj"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo np_upnp.cpp compiled
clang-cl %FLAGS% "%~dp0np_transport_host_test.cpp" "%N%\np_transport.cpp" "%N%\np_wire.cpp" "%N%\np_rules.cpp" /Fo"%OUT%\\" /Fe"%OUT%\np_transport_host_test.exe" ws2_32.lib iphlpapi.lib
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\np_transport_host_test.exe" %*

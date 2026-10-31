@echo off
rem Builds and runs the lobby host test with clang-cl (no CMake): np_online.cpp over the real np_transport (one UDP
rem socket per lobby on 127.0.0.1), np_wire, np_rules, np_handoff (with doa4_tweaks for the guest-setting names).
rem Every socket binds 127.0.0.1 and nothing reaches the internet (STUN servers are fakes on loopback; UPnP is off and
rem np_upnp.cpp is not linked: the test defines stand-ins that count calls and asserts none happened; only the
rem header's pure renewal schedule is tested).
rem _MSVC_STL_HARDENING makes an out-of-range index fast-fail. Pass -v to print each group, -only <text> for some groups.
rem NP_ONLINE_OUT (optional) names the output folder; the mutation probes use it so that they never share one.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if not defined NP_ONLINE_OUT set "NP_ONLINE_OUT=%TEMP%\doa4_np_online_test"
set "OUT=%NP_ONLINE_OUT%"
set "N=%~dp0..\..\src\net"
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\np_online_host_test.exe" del "%OUT%\np_online_host_test.exe"
clang-cl /nologo /std:c++latest /EHsc /O2 /D_MSVC_STL_HARDENING=1 /D_WIN32_WINNT=0x0A00 /DWIN32_LEAN_AND_MEAN /DNOMINMAX "%~dp0np_online_host_test.cpp" "%N%\np_online.cpp" "%N%\np_transport.cpp" "%N%\np_wire.cpp" "%N%\np_rules.cpp" "%N%\np_handoff.cpp" "%~dp0..\..\src\doa4_tweaks.cpp" /Fo"%OUT%\\" /Fe"%OUT%\np_online_host_test.exe" ws2_32.lib iphlpapi.lib
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\np_online_host_test.exe" %*

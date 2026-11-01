@echo off
rem Builds and runs the UPnP renewal host test with clang-cl (no CMake): np_upnp.cpp built with DOA4_UPNP_NO_MINIUPNPC
rem over the test's fake router (nothing reaches the network).
setlocal
if not defined DOA4_VSDEVCMD set "DOA4_VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
call "%DOA4_VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul 2>&1
if not defined DOA4_LLVM_BIN set "DOA4_LLVM_BIN=C:\Program Files\LLVM\bin"
set "PATH=%DOA4_LLVM_BIN%;%PATH%"
if not defined NP_UPNP_OUT set "NP_UPNP_OUT=%TEMP%\doa4_np_upnp_test"
set "OUT=%NP_UPNP_OUT%"
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\np_upnp_host_test.exe" del "%OUT%\np_upnp_host_test.exe"
clang-cl /nologo /std:c++latest /EHsc /O2 /D_MSVC_STL_HARDENING=1 /D_WIN32_WINNT=0x0A00 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DDOA4_UPNP_NO_MINIUPNPC "%~dp0np_upnp_host_test.cpp" "%~dp0..\..\src\net\np_upnp.cpp" /Fo"%OUT%\\" /Fe"%OUT%\np_upnp_host_test.exe" ws2_32.lib
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\np_upnp_host_test.exe" %*

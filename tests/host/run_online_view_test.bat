@echo off
rem Builds and runs the Online tab view-model host test with clang-cl (no CMake): doa4_online_view.cpp over np_rules.cpp
rem and np_wire.cpp (the view reads the lobby's Status and the release-tag rules; no ImGui, no SDK headers).
rem No files and no sockets. ONLINE_VIEW_OUT (optional) names the output folder; the mutation probes use it so that
rem they never share one.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if not defined ONLINE_VIEW_OUT set "ONLINE_VIEW_OUT=%TEMP%\doa4_online_view_test"
set "OUT=%ONLINE_VIEW_OUT%"
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\online_view_host_test.exe" del "%OUT%\online_view_host_test.exe"
clang-cl /nologo /std:c++latest /EHsc /O2 /D_MSVC_STL_HARDENING=1 /D_WIN32_WINNT=0x0A00 /DWIN32_LEAN_AND_MEAN /DNOMINMAX "%~dp0online_view_host_test.cpp" "%~dp0..\..\src\doa4_online_view.cpp" "%~dp0..\..\src\net\np_rules.cpp" "%~dp0..\..\src\net\np_wire.cpp" /Fo"%OUT%\\" /Fe"%OUT%\online_view_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\online_view_host_test.exe"

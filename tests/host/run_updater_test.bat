@echo off
rem Builds and runs the in-game updater host test with clang-cl (no CMake): doa4_updater.cpp over np_rules.cpp and
rem np_wire.cpp (Choose reads the release list with rules::PickRelease), linked with winhttp and bcrypt.
rem No network: the test never calls CheckAndUpdate or Start; every file it writes is under %%TEMP%%\doa4_updater_<pid>\.
rem DOA4_UPDATER_HOST_TEST gives the release identity CMake passes to the game a fixed stand-in.
rem DOA4_VSDEVCMD and DOA4_LLVM_BIN (optional) name the Visual Studio environment script and the LLVM bin folder;
rem UPDATER_OUT (optional) names the output folder, so that mutation probes never share one.
setlocal
if not defined DOA4_VSDEVCMD set "DOA4_VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
call "%DOA4_VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul 2>&1
if not defined DOA4_LLVM_BIN set "DOA4_LLVM_BIN=C:\Program Files\LLVM\bin"
set "PATH=%DOA4_LLVM_BIN%;%PATH%"
if not defined UPDATER_OUT set "UPDATER_OUT=%TEMP%\doa4_updater_test"
set "OUT=%UPDATER_OUT%"
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\updater_host_test.exe" del "%OUT%\updater_host_test.exe"
clang-cl /nologo /std:c++latest /EHsc /O2 /D_MSVC_STL_HARDENING=1 /D_WIN32_WINNT=0x0A00 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DDOA4_UPDATER_HOST_TEST "%~dp0updater_host_test.cpp" "%~dp0..\..\src\doa4_updater.cpp" "%~dp0..\..\src\net\np_rules.cpp" "%~dp0..\..\src\net\np_wire.cpp" /Fo"%OUT%\\" /Fe"%OUT%\updater_host_test.exe" winhttp.lib bcrypt.lib
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\updater_host_test.exe" %*

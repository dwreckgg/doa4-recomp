@echo off
rem Builds and runs the netplay lobby-rules host test with clang-cl (no CMake): np_rules.cpp, plus np_wire.cpp
rem (np_rules.h includes np_wire.h for wire::Refusal, and the test builds real datagrams with wire::Encode).
rem _MSVC_STL_HARDENING makes an out-of-range span, string_view or vector index fast-fail, so a parser that reads
rem past its text is a red run, not a lucky green one. Pass -v to print each check group before it runs.
rem The test opens no socket and reaches no network: the STUN datagrams are built by the test itself.
setlocal
if not defined DOA4_VSDEVCMD set "DOA4_VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
call "%DOA4_VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul 2>&1
if not defined DOA4_LLVM_BIN set "DOA4_LLVM_BIN=C:\Program Files\LLVM\bin"
set "PATH=%DOA4_LLVM_BIN%;%PATH%"
set "OUT=%TEMP%\doa4_np_rules_test"
if not exist "%OUT%" mkdir "%OUT%"
clang-cl /nologo /std:c++latest /EHsc /O2 /D_MSVC_STL_HARDENING=1 "%~dp0np_rules_host_test.cpp" "%~dp0..\..\src\net\np_rules.cpp" "%~dp0..\..\src\net\np_wire.cpp" /Fo"%OUT%\\" /Fe"%OUT%\np_rules_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\np_rules_host_test.exe" %*

@echo off
rem Builds and runs the netplay hand-off host test with clang-cl (no CMake): np_handoff.cpp, plus np_rules.cpp and
rem np_wire.cpp (np_handoff.h includes np_rules.h for rules::Endpoint, and np_rules.h includes np_wire.h) and
rem doa4_tweaks.cpp (GuestSettingNames lists the Extras tweak cvars; the file has no SDK dependency, and
rem run_tweaks_test.bat builds it the same way).
rem _MSVC_STL_HARDENING makes an out-of-range span, string_view or vector index fast-fail, so a parser that reads
rem past its bytes is a red run, not a lucky green one. Pass -v to print each check group before it runs.
rem The test reads source files only, to check that every guest setting names a cvar that is really defined: the
rem SDK's src folder (REXSDK_DIR, default sdk-np1 next to this worktree's folder) and this tree's src. It opens no
rem socket and writes no file.
setlocal
if "%REXSDK_DIR%"=="" set "REXSDK_DIR=%~dp0..\..\..\sdk-np1"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "OUT=%TEMP%\doa4_np_handoff_test"
if not exist "%OUT%" mkdir "%OUT%"
clang-cl /nologo /std:c++latest /EHsc /O2 /D_MSVC_STL_HARDENING=1 "%~dp0np_handoff_host_test.cpp" "%~dp0..\..\src\net\np_handoff.cpp" "%~dp0..\..\src\net\np_rules.cpp" "%~dp0..\..\src\net\np_wire.cpp" "%~dp0..\..\src\doa4_tweaks.cpp" /Fo"%OUT%\\" /Fe"%OUT%\np_handoff_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\np_handoff_host_test.exe" --sdk-src "%REXSDK_DIR%\src" --port-src "%~dp0..\..\src" %*

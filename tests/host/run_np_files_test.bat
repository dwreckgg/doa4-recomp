@echo off
rem Builds and runs the netplay game-files fingerprint host test with clang-cl (no CMake): np_files.cpp (Windows file
rem API) plus np_rules.cpp and np_wire.cpp (the test judges groups with rules::DifferingGroups). Temp trees live under
rem <DOA4_ROOT>\np_runs\t8_<pid>_<tick>\ (DOA4_ROOT: the environment variable, by default the parent folder of this
rem repository; the test prints the folder) and are removed at the end. Set DOA4_NP_GAME_DIR to the real game
rem folder (read only) to run the real-data section; without it that section prints SKIPPED and the summary says
rem UNMEASURED. Pass -v to print each check group before it runs.
setlocal
if not defined DOA4_VSDEVCMD set "DOA4_VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
call "%DOA4_VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul 2>&1
if not defined DOA4_LLVM_BIN set "DOA4_LLVM_BIN=C:\Program Files\LLVM\bin"
set "PATH=%DOA4_LLVM_BIN%;%PATH%"
set "OUT=%TEMP%\doa4_np_files_test"
if not exist "%OUT%" mkdir "%OUT%"
if "%NP_FILES_SRC%"=="" set "NP_FILES_SRC=%~dp0..\..\src\net\np_files.cpp"
clang-cl /nologo /std:c++latest /EHsc /O2 /D_MSVC_STL_HARDENING=1 "%~dp0np_files_host_test.cpp" "%NP_FILES_SRC%" "%~dp0..\..\src\net\np_rules.cpp" "%~dp0..\..\src\net\np_wire.cpp" /Fo"%OUT%\\" /Fe"%OUT%\np_files_host_test.exe"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%OUT%\np_files_host_test.exe" %*

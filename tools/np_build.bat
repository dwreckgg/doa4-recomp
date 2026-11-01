@echo off
rem Builds the netplay worktree this script lives in (README section 6), with the SDK from DOA4_SDK
rem (default: sdk-np1 beside this worktree, in forward slashes as README section 6 asks).
rem Usage: np_build.bat configure | build [snapshot-tag]
rem With a tag, the exe and its two DLLs are copied to out\build\win-amd64-release\np_bin\<tag>\.
rem No errorlevel variable is read inside a parenthesised block: cmd expands it when the block is read.
if defined DOA4_SDK goto sdk_set
for %%I in ("%~dp0..\..\sdk-np1") do set "DOA4_SDK=%%~fI"
set "DOA4_SDK=%DOA4_SDK:\=/%"
:sdk_set
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d "%~dp0.."
if not "%1"=="configure" goto build
cmake --preset win-amd64-release "-DREXSDK_DIR=%DOA4_SDK%" "-DCMAKE_C_FLAGS=-march=x86-64-v3" "-DCMAKE_CXX_FLAGS=-march=x86-64-v3"
if errorlevel 1 exit /b 1
exit /b 0
:build
cmake --build --preset win-amd64-release
if errorlevel 1 exit /b 1
if "%2"=="" exit /b 0
set "BIN=%CD%\out\build\win-amd64-release"
if exist "%BIN%\np_bin\%2" goto exists
mkdir "%BIN%\np_bin\%2"
copy /y "%BIN%\dead_or_alive_4.exe" "%BIN%\np_bin\%2\" >nul
if errorlevel 1 exit /b 1
copy /y "%BIN%\rexruntime.dll" "%BIN%\np_bin\%2\" >nul
if errorlevel 1 exit /b 1
copy /y "%BIN%\TracyClient.dll" "%BIN%\np_bin\%2\" >nul
if errorlevel 1 exit /b 1
echo snapshot %BIN%\np_bin\%2
exit /b 0
:exists
echo snapshot %2 exists
exit /b 1

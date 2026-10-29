@echo off
rem Build the netplay proof worktree (port wt-np0 + SDK sdk-np0) as README section 6 says.
rem Usage: build.bat configure | build [snapshot-tag]
rem With a tag, the exe and its DLLs are copied to out\build\win-amd64-release\np_bin\<tag>\ so
rem runs can use a fixed binary while the next one builds.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d C:\Users\fsop1\doa4\wt-np0
if "%1"=="configure" (
  cmake --preset win-amd64-release -DREXSDK_DIR=C:/Users/fsop1/doa4/sdk-np0 "-DCMAKE_C_FLAGS=-march=x86-64-v3" "-DCMAKE_CXX_FLAGS=-march=x86-64-v3"
  exit /b %ERRORLEVEL%
)
cmake --build --preset win-amd64-release
if errorlevel 1 exit /b 1
if "%2"=="" exit /b 0
set "BIN=C:\Users\fsop1\doa4\wt-np0\out\build\win-amd64-release"
if exist "%BIN%\np_bin\%2" (
  echo snapshot %2 exists
  exit /b 1
)
mkdir "%BIN%\np_bin\%2"
copy /y "%BIN%\dead_or_alive_4.exe" "%BIN%\np_bin\%2\" >nul
copy /y "%BIN%\rexruntime.dll" "%BIN%\np_bin\%2\" >nul
copy /y "%BIN%\TracyClient.dll" "%BIN%\np_bin\%2\" >nul
echo snapshot %BIN%\np_bin\%2
exit /b 0

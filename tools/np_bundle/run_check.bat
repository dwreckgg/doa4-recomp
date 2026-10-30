@echo off
rem ---------------------------------------------------------------------------------------------------------------
rem DOA4 netplay, phase 0: plays the recordings in recs\ back on this PC and collects what the game wrote.
rem Read README.txt first. The only line to edit is the GAME line below.
rem
rem For each recording this does what tools\np_run.py does for a replay, without Python: a fresh copy of the save
rem (ud_pristine), the DOA4_NP variables, the game exe with --headless=true and its working folder bin\. The helper
rem run_one.ps1 starts the exe, ends it after TIMEOUT seconds and writes run_result.txt the way np_run.py does.
rem ---------------------------------------------------------------------------------------------------------------
setlocal EnableExtensions

rem === Your DOA4 game folder: the folder that has default.xex in it. It is only read, never written. ===
set "GAME=C:\path\to\your\DOA4 game folder"

rem Seconds a replay may take before it is ended (tools\np_run.py: 900).
set "TIMEOUT=900"
set "EXE=%~dp0bin\dead_or_alive_4.exe"
set "DPPS=%~dp0@DPPS@"

cd /d "%~dp0"
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"

echo.
echo Checking the setup ...
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0run_one.ps1" -Check -Game "%GAME%" -Exe "%EXE%"
if errorlevel 1 goto :fail

if exist "%~dp0out" rmdir /s /q "%~dp0out"
if exist "%~dp0results.zip" del /q "%~dp0results.zip"
mkdir "%~dp0out"

echo.
echo Playing the recordings back. This takes a while. Please do not use the keyboard or the mouse until it says DONE.
for %%R in (@RECS@) do call :one %%R

echo.
echo Running the DPPS test ...
"%DPPS%" > "%~dp0dpps.txt" 2>&1
>> "%~dp0dpps.txt" echo exit %ERRORLEVEL%

echo.
echo Collecting system information ...
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0run_one.ps1" -Sysinfo "%~dp0sysinfo.txt"

echo.
echo Making results.zip ...
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0run_one.ps1" -Zip "%~dp0results.zip"
if errorlevel 1 goto :nozip

echo.
echo DONE. Send the file results.zip (it is in this folder).
pause
exit /b 0

:nozip
echo.
echo results.zip could not be made. Zip the folder out and the files dpps.txt, sysinfo.txt and recordings.txt
echo in this folder by hand, and send that.
pause
exit /b 1

:fail
echo.
echo Nothing was run. Fix the problem above, then run this file again.
pause
exit /b 1

:one
rem One replay, with the environment of tools\np_run.py replay: every DOA4_ variable cleared, then exactly these.
set "NAME=%~1"
set "RUN=%~dp0out\%NAME%"
for /f "delims==" %%V in ('set DOA4_ 2^>nul') do set "%%V="
set "DOA4_NP=replay"
set "DOA4_NP_DIR=%RUN%\out"
set "DOA4_NP_REC=%~dp0recs\%NAME%.d4r"
rem SDL opens no game controller: the allow-list matches no device (np_run.py sets it unless the caller did; here always).
set "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000"
echo.
echo Replaying %NAME% ...
mkdir "%RUN%\out"
xcopy "%~dp0ud_pristine" "%RUN%\ud" /e /i /q /y /h >nul
if errorlevel 1 goto :one_nocopy
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0run_one.ps1" -Name "%NAME%" -Exe "%EXE%" -Game "%GAME%" -Run "%RUN%" -Cache "%~dp0cache" -Timeout %TIMEOUT%
goto :one_end
:one_nocopy
echo   could not copy ud_pristine into the run folder
(echo status failed& echo exit 1& echo timeout 0& echo quit could not copy ud_pristine into the run folder)> "%RUN%\out\run_result.txt"
:one_end
if exist "%RUN%\ud" rmdir /s /q "%RUN%\ud"
exit /b 0

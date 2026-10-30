@echo off
rem ---------------------------------------------------------------------------------------------------------------
rem Gets the DOA4 game files out of your own disc image, with the game's own first-run setup screen, and remembers
rem where they are: run_check.bat then finds them by itself. Read README.txt first.
rem
rem The game starts normally (a window, not headless) with its save and cache folder inside this bundle (setup_ud), so
rem nothing is written to your Documents. The setup screen checks the disc image (Dead or Alive 4, USA / Europe,
rem Rev 1), extracts it to the folder you choose and saves that folder in bin\dead_or_alive_4.toml.
rem ---------------------------------------------------------------------------------------------------------------
setlocal EnableExtensions
set "EXE=%~dp0bin\dead_or_alive_4.exe"
cd /d "%~dp0"
if not exist "%~dp0run_one.ps1" goto :nohelper
if not exist "%EXE%" goto :noexe

rem The game must start as a normal game: no DOA4_ variable (they switch the test session on).
for /f "delims==" %%V in ('set DOA4_ 2^>nul') do set "%%V="

echo.
echo The game's setup window opens now.
echo   1. Click "Choose disc image (.iso)..." and pick your Dead or Alive 4 disc image.
echo   2. Click Extract and wait until it is done (about 7.5 GB are copied).
echo   3. The game itself then starts. Close it (press Alt+F4) when its title screen shows.
echo.
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0run_one.ps1" -Setup -Exe "%EXE%" -UserData "%~dp0setup_ud"
if not "%errorlevel%"=="0" goto :nostart

echo.
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0run_one.ps1" -FindGame
if not "%errorlevel%"=="0" goto :nogame
echo.
echo Now double-click run_check.bat.
pause
exit /b 0

:nogame
echo.
echo Run get_game_files.bat again and choose your disc image, or put your game folder into the GAME line of run_check.bat.
pause
exit /b 1

:nostart
echo.
echo The game's setup could not be started (see above).
pause
exit /b 1

:nohelper
echo.
echo run_one.ps1 is missing from this folder. Extract the whole zip first (Extract All in Explorer) and run this from
echo the extracted folder, not from inside the zip. If run_one.ps1 was there before, your antivirus may have removed it.
pause
exit /b 1

:noexe
echo.
echo bin\dead_or_alive_4.exe is missing from this folder. Extract the whole zip again.
pause
exit /b 1

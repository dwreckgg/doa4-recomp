@echo off
rem ---------------------------------------------------------------------------------------------------------------
rem Gets the DOA4 game files out of your own disc image, with the game's own first-run setup screen, and remembers
rem where they are: run_check.bat then finds them by itself. Read README.txt first.
rem
rem The game starts normally (a window, not headless) with its save and cache folder inside this bundle (setup_ud), so
rem nothing is written to your Documents. The setup screen checks the disc image (Dead or Alive 4, USA / Europe,
rem Rev 1), extracts it to the folder you choose and saves that folder in bin\dead_or_alive_4.toml.
rem A second run with game files already set up says so and does not open the setup window again. A saved folder whose
rem name is not plain ASCII (the setup allows it, the test does not) is explained and cleared from the toml, so that the
rem next run opens the setup window again.
rem ---------------------------------------------------------------------------------------------------------------
setlocal EnableExtensions
set "EXE=%~dp0bin\dead_or_alive_4.exe"
cd /d "%~dp0"
if not exist "%~dp0run_one.ps1" goto :nohelper
if not exist "%EXE%" goto :noexe

rem The game must start as a normal game: no DOA4_ variable (they switch the test session on).
for /f "delims==" %%V in ('set DOA4_ 2^>nul') do set "%%V="

rem Game files already set up? Then there is nothing to do (and the setup window must not open again).
echo.
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0run_one.ps1" -FindGame -Quiet -Prefix "Game files already set up"
set "FOUND=%errorlevel%"
if "%FOUND%"=="0" goto :already
if "%FOUND%"=="2" goto :badfolder

echo.
echo The game's setup window opens now.
echo   1. Click "Choose disc image (.iso)..." and pick your Dead or Alive 4 disc image.
echo   2. When it shows "Extract to:", ALWAYS click "Change folder...", choose the C: drive and confirm, so that it says
echo      Extract to: C:\DOA4   (the path must have no accent or special letter anywhere in it).
echo   3. Click Extract and wait until it is done (about 7.5 GB are copied).
echo   4. The game itself then starts. Close it (press Alt+F4) when its title screen shows.
echo.
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0run_one.ps1" -Setup -Exe "%EXE%" -UserData "%~dp0setup_ud"
if not "%errorlevel%"=="0" goto :nostart

echo.
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0run_one.ps1" -FindGame
set "FOUND=%errorlevel%"
if "%FOUND%"=="0" goto :done
if "%FOUND%"=="2" goto :badfolder
goto :nogame

:done
echo.
echo Now double-click run_check.bat.
pause
exit /b 0

:already
echo.
echo There is nothing to do here: your game files are set up (the folder is shown above).
echo To choose other game files, delete the file bin\dead_or_alive_4.toml and run this again.
echo Now double-click run_check.bat.
pause
exit /b 0

:badfolder
echo.
echo The test cannot use those game files: the name of their folder (shown above) has a special letter in it.
echo I have cleared the saved folder, so the game's setup window asks for the files again the next time. Then do ONE of these:
echo   - move (or copy) that folder to C:\DOA4, run get_game_files.bat again, click "Choose extracted folder..." and pick C:\DOA4;
echo   - or run get_game_files.bat again, click "Choose disc image (.iso)...", pick your .iso, click "Change folder...",
echo     choose the C: drive, and click Extract.
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0run_one.ps1" -ForgetGame
pause
exit /b 1

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

@echo off
rem The stand-in for the game's first-run setup in tests/tools/test_np_tools.py (see fake_setup.py): get_game_files.bat starts it
rem as the exe. The test sets FAKE_GAME_PYTHON to the interpreter.
"%FAKE_GAME_PYTHON%" -I "%~dp0fake_setup.py" %*
exit /b %ERRORLEVEL%

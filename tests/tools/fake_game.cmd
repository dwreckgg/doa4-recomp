@echo off
rem The stand-in game for tests/tools/test_np_tools.py: np_run.py starts it as the game's exe
rem (CreateProcess runs .cmd files). The test sets FAKE_GAME_PYTHON to the interpreter.
"%FAKE_GAME_PYTHON%" -I "%~dp0fake_game.py" %*
exit /b %ERRORLEVEL%

@echo off
rem The stand-in game for the np_offline_check.py test in tests/tools/test_np_tools.py: the check starts it as
rem the game's exe (CreateProcess runs .cmd files). The test sets FAKE_GAME_PYTHON to the interpreter.
"%FAKE_GAME_PYTHON%" -I "%~dp0fake_offline_game.py" %*
exit /b %ERRORLEVEL%

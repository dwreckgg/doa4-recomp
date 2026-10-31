@echo off
rem The stand-in for the DPPS test exe in the run_check.bat test (tests/tools/test_np_tools.py): prints a marker and every
rem DOA4_ variable it was started with, so the test can see that the replays' variables did not reach it.
echo fake dpps ran
set DOA4_ 2>nul
exit /b 0

DOA4 netplay test, phase 0
==========================

What this is
  A check that two different PCs play the same recorded fight in exactly the same way. This folder holds @N@ recorded
  fights (@RECS@) that were played on another PC. run_check.bat plays each of them back on your PC, with no input from
  you, and keeps what the game wrote about every frame. It also runs a small arithmetic test (dpps_host_test_canon.exe).
  You then send back one file, results.zip, and it is compared with the other PC's.

What you need
  - Windows 10 or 11, 64-bit.
  - Your own copy of DOA4: the game folder that has default.xex in it. It must be the same dump of the game as the one
    the recordings were made with (game data id @DATA@ in the game's own check). If it is not, the game says so and
    refuses the recording; send results.zip anyway, that is a useful answer too.
  - A CPU with AVX2 (any CPU from about 2013 on) and the Microsoft Visual C++ 2015-2022 x64 runtime (most PCs have it;
    if the game does not start, install vc_redist.x64.exe from Microsoft).
  - About 1 GB of free disk space and about @MINUTES@ minutes.

Steps
  1. Extract the zip into a NEW folder whose whole path uses plain English letters and digits only, for example
     C:\doa4test (in Explorer: right-click the zip, Extract All). A path with accented letters does not work: the game
     reads its folders in a way that breaks them, and run_check.bat will tell you so.
  2. Open run_check.bat with Notepad (right-click, Edit, or Open with, Notepad). Find the line
         set "GAME=C:\path\to\your\DOA4 game folder"
     Replace what is between = and the last quote with your DOA4 game folder, keep the quotes, save the file.
  3. Double-click run_check.bat. If Windows says "Windows protected your PC", choose More info, then Run anyway.
     A black window shows what it is doing and a game window may flash up for every recording. Please leave the
     PC alone until the black window says DONE: no keyboard or mouse in the game window, and no other games or heavy
     programs meanwhile (they change the timing the test is looking at).
  4. Send me results.zip, which is in the same folder. If there is no results.zip, send what the black window says.

What it does not do
  - It does not change your DOA4 folder, your own saves or your settings. The recordings are played with a copy of a
    save that is in this folder (ud_pristine) and with a shader cache of its own (the folder cache). Both stay here.
  - It does not use your game controllers and does not need the internet.
  - It does not limit which CPU cores the game uses.
  - You can delete the whole folder afterwards.

Recordings in this bundle: @RECS@ (game build @BUILD@).

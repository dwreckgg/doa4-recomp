DOA4 test: does your PC play a fight exactly like mine?
=======================================================

This folder replays @N@ recorded fights on your PC with no input from you, and keeps what the game wrote.
I compare that with what my PC wrote. You only start two files and send me one.

Before you start
  - Windows 10 or 11, 64-bit.
  - Extract this zip into a NEW folder with a plain name, for example C:\doa4test (Explorer: right-click the zip,
    Extract All). No accented letters in the folder name, and do not start anything from inside the zip.
  - About 8 GB of free disk space (1 GB if you already have the game extracted).

Step 0 - get your game files (skip this if you already have the game extracted to a folder)
  You need your own disc image of Dead or Alive 4: a file ending in .iso, the USA / Europe version (Rev 1).
  1. Double-click get_game_files.bat.
  2. The game's setup window opens. Click "Choose disc image (.iso)...", pick your .iso, click Extract, and wait
     until it is done. (It makes a DOA4 folder next to your .iso. If it says the folder name is a problem, click
     "Change folder..." and choose a plain one such as C:\DOA4.)
  3. The game then starts by itself. Close it (press Alt+F4). The black window says "Game files found".
  If Windows asks whether the game may use the network, click Cancel: this test does not use the internet.

Step 1 - run the test (about @MINUTES@ minutes)
  Double-click run_check.bat. There is nothing to edit if you did step 0.
  (If you already have the game extracted and skipped step 0: open run_check.bat in Notepad, put your game folder
  between the quotes of the line  set "GAME=...  and save. Pasting it with its own quotes is fine.)
  If Windows says "Windows protected your PC", click More info, then Run anyway.
  A black window shows what is happening and a game window may flash up for each fight. Leave the PC alone until
  the black window says DONE: no keyboard or mouse, no other games or programs (they change the timing I am measuring).

Step 2 - send me the file results.zip (it is in the same folder)
  It holds the test's result files, nothing of the game and no saves. It does contain folder names, including your
  Windows user name (C:\Users\your name\...), and your PC's CPU, graphics card and Windows version.

If something goes wrong, send results.zip anyway. If there is none, send a photo of what the black window says.
Then you can delete this whole folder (and the DOA4 folder from step 0 if you do not want it).

For me: @N@ recordings (@RECS@), game build @BUILD@, game data id @DATA@. The test needs Windows PowerShell 5.1
(part of Windows 10 and 11) and the Microsoft Visual C++ 2015-2022 x64 runtime; if the game does not start, the black
window says so. It does not limit which CPU cores the game uses.

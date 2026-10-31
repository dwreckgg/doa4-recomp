DOA4 test: does your PC play a fight exactly like mine?
=======================================================

This folder replays @N@ recorded fights on your PC with no input from you, and keeps what the game wrote.
I compare that with what my PC wrote. You only start two files and send me one.

Before you start
  - Windows 10 or 11, 64-bit. About 8 GB of free disk space (1 GB if you already have the game extracted).
  - Extract this zip: right-click the zip, choose Extract All, and in the window that opens REPLACE the folder shown
    with  C:\doa4test  and click Extract. The path must not have an accent or a special letter anywhere in it, and the
    folder Windows suggests is usually inside your user folder, which may have one. Do not start anything from inside the zip.
  - When you double-click run_check.bat or get_game_files.bat, and when the game window opens in step 0, Windows may say
    "Windows protected your PC". That is normal for files that came from the internet: click "More info", then "Run anyway".

Step 0 - get your game files (skip this if you already have the game extracted to a folder)
  You need your own disc image of Dead or Alive 4: a file ending in .iso, the USA / Europe version (Rev 1).
  1. Double-click get_game_files.bat.
  2. The game's setup window opens. Click "Choose disc image (.iso)...", pick your .iso.
  3. It then shows "Extract to:". ALWAYS click "Change folder...", choose the C: drive and confirm, so that it says
     "Extract to: C:\DOA4". The path must have no accent or special letter anywhere in it, and the .iso's own folder or
     your Windows user name often has one without you noticing; the test cannot use files in such a folder.
  4. Click Extract and wait until it is done. If you close the game or the window during the extraction, it stops and
     removes the half copy: just double-click get_game_files.bat again.
  5. The game then starts by itself. Close it (press Alt+F4). The black window says "Game files found".
  If Windows asks whether the game may use the network, click Cancel: this test does not use the internet.

Step 1 - run the test (about @MINUTES@ minutes)
  Double-click run_check.bat. There is nothing to edit if you did step 0.
  (If you already have the game extracted and skipped step 0: right-click run_check.bat, choose Edit, put your game folder
  - the folder that has the file default.xex in it - between the quotes of the line  set "GAME=...  and save. Pasting it
  with its own quotes is fine, but not if the folder name has an & sign in it: then paste it without quotes, or copy the
  folder to C:\DOA4 first.)
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

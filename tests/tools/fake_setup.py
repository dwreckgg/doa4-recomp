"""A stand-in for dead_or_alive_4.exe's first-run setup, for the get_game_files.bat test (tests/tools/test_np_tools.py).
It does what the real app does when the user finishes the setup screen, as far as the bundle can see:
  src/dead_or_alive_4_app.h SaveGameFiles / ConfigPath and src/doa4_config.cpp UpsertTomlKey / TomlString save the chosen folder
  as  doa4_game_files = "<folder, forward slashes, \\ and " escaped>"  into dead_or_alive_4.toml in the EXE'S OWN folder,
  replacing that top-level line or inserting it before the first [table] header; the other lines stay as they were.
(The real line shape was read from a real saved file, suits_play/bin/dead_or_alive_4.toml.) Here the "exe's own folder" is the
folder of this script, which the test copies into the bundle's bin folder. Nothing of the real game is touched.

Environment (the stand-in's own):
  FAKE_SETUP_GAME  the folder the user "chose"
  FAKE_SETUP_MODE  save (default) | cancel (the user quit the setup: nothing saved) | missing (saves a folder without default.xex)
  FAKE_SETUP_DELAY seconds to wait before saving (the real setup takes minutes: the launcher must wait for the process to end)
It also writes <--user_data_root>/fake_setup.txt: its working folder, its arguments (a|b|c) and one line per DOA4_ variable.
"""
import os
import sys
import time


def console_window():
    """"none" (no console window), "hidden" or "visible": the checks start everything without a window."""
    import ctypes
    hwnd = ctypes.windll.kernel32.GetConsoleWindow()
    if not hwnd:
        return "none"
    return "visible" if ctypes.windll.user32.IsWindowVisible(hwnd) else "hidden"


def main():
    args = sys.argv[1:]
    ud = next((a[len("--user_data_root="):] for a in args if a.startswith("--user_data_root=")), None)
    if ud:
        os.makedirs(ud, exist_ok=True)
        with open(os.path.join(ud, "fake_setup.txt"), "w") as f:
            f.write(f"cwd {os.getcwd()}\nargs {'|'.join(args)}\nconsole-window {console_window()}\n")
            for k, v in sorted(os.environ.items()):
                if k.startswith("DOA4_"):
                    f.write(f"env {k}={v}\n")
    time.sleep(float(os.environ.get("FAKE_SETUP_DELAY", "0")))  # a setup that takes a while: the launcher must wait for it
    mode = os.environ.get("FAKE_SETUP_MODE", "save")
    if mode == "cancel":
        return
    game = os.environ["FAKE_SETUP_GAME"]
    if mode == "missing":
        game = os.path.join(game, "no_such_folder")
    text = game.replace("\\", "/").replace("\\", "\\\\").replace('"', '\\"')  # TomlString of Utf8Generic(folder)
    toml = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dead_or_alive_4.toml")
    # a file the user (or the Esc menu) has saved other settings into: top-level keys, then a table with a key of the same name
    with open(toml, "w", newline="") as f:
        f.write(f'fullscreen = true\r\ndoa4_game_files = "{text}"\r\nresolution_scale = 4\r\n\r\n[net]\r\ndoa4_game_files = "decoy"\r\n')


if __name__ == "__main__":
    main()

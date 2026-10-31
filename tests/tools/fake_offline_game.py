"""A stand-in for dead_or_alive_4.exe in tools/np_offline_check.py's test (tests/tools/test_np_tools.py).
Takes the game's command line (only --log_file= is read), writes the two environment variables the
check hands the game into that log and exits at once. Nothing of the real game or its data is touched.
"""
import os
import sys

SDL_PADS = "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"


def main():
    log = None
    for a in sys.argv[1:]:
        if a.startswith("--log_file="):
            log = a[len("--log_file="):]
    if log is None:
        sys.exit(2)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import fake_game
    ud = next((a[len("--user_data_root="):] for a in sys.argv[1:] if a.startswith("--user_data_root=")), None)
    if ud and os.environ.get("FAKE_STARTUP_NOTE") == "1":  # how the tool started the process (behind the .cmd: its parent)
        show, flags = fake_game.startup_info(os.getppid())
        with open(os.path.join(os.path.dirname(os.path.normpath(ud)), "startup.txt"), "w") as f:
            f.write(f"show_window={show} flags={flags:#x}\n")
    with open(log, "w") as f:
        f.write(f"stand-in offline game: {SDL_PADS}={os.environ.get(SDL_PADS, '(unset)')}\n")
        f.write(f"stand-in offline game: DOA4_PHOTO_TEST={os.environ.get('DOA4_PHOTO_TEST', '(unset)')}\n")
        f.write(f"stand-in offline game: DOA4_NP={os.environ.get('DOA4_NP', '(unset)')}\n")


if __name__ == "__main__":
    main()

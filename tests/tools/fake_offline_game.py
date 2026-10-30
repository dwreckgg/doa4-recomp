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
    with open(log, "w") as f:
        f.write(f"stand-in offline game: {SDL_PADS}={os.environ.get(SDL_PADS, '(unset)')}\n")
        f.write(f"stand-in offline game: DOA4_PHOTO_TEST={os.environ.get('DOA4_PHOTO_TEST', '(unset)')}\n")
        f.write(f"stand-in offline game: DOA4_NP={os.environ.get('DOA4_NP', '(unset)')}\n")


if __name__ == "__main__":
    main()

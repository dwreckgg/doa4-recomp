"""Offline run of a build (no DOA4_NP): the game's own photo test takes photos at 20/40/60/80 s into a
run folder, then this script ends its own process. Shows that a netplay build plays as before when no
session runs: the game.log has no [NP] line and no "skipped at frame" line, and the photos show the title
and the attract demo. Startup and attract play only; shutdown is not exercised (the run ends with
TerminateProcess, which never runs the application's shutdown hook).

  py -3 -I tools/np_offline_check.py EXE NAME

EXE is a snapshot from tools/np_build.bat (np_bin/<tag>/dead_or_alive_4.exe); NAME is a new folder under
RUNS. The run copies the pristine save, uses the shared shader cache, and only the process started here is
ended (never another game process). The game gets SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 (SDL
opens no real controller) unless this script's own environment already sets that variable.
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # py -3 -I does not put the script's folder on the path
sys.dont_write_bytecode = True
import np_run  # noqa: E402  (hidden_startupinfo, DOA4_ROOT, GAME_DIR)

# Under DOA4_ROOT (np_run.py: the environment variable, by default this repository's parent folder).
RUNS = os.path.join(np_run.DOA4_ROOT, "np_runs")
PRISTINE = os.path.join(np_run.DOA4_ROOT, "np_phase0", "ud_pristine")
CACHE = os.path.join(RUNS, "cache_shared")
GAME = np_run.GAME_DIR
RUN_SECONDS = 95  # how long the game runs before this script ends it (the tools test shortens it)

# The runtime opens the host's game controllers through SDL even without a session, and a game running
# unattended would read the user's pad (a pad in use in another game took the attract demo into the menus). This
# allow-list matches no device, so SDL opens none. Same value and rule as np_run.py's SDL_PADS: set unless the
# caller's own environment already has the variable.
SDL_PADS = ("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x0000/0x0000")


def child_env(environ):
    """The game's environment: the caller's without its DOA4_ variables, the photo test on, no real controller."""
    env = {k: v for k, v in environ.items() if not k.startswith("DOA4_")}
    env["DOA4_PHOTO_TEST"] = "hotkey"
    env.setdefault(*SDL_PADS)
    return env


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    exe, name = argv[1], argv[2]
    run = os.path.join(RUNS, name)
    os.makedirs(run)  # refuses an existing run folder
    ud = os.path.join(run, "ud")
    shutil.copytree(PRISTINE, ud)
    photos = os.path.join(run, "photos")
    env = child_env(os.environ)
    args = [exe, f"--game_data_root={GAME}", f"--user_data_root={ud}",
            f"--cache_path={CACHE}", f"--log_file={os.path.join(run, 'game.log')}",
            f"--doa4_photo_dir={photos}", "--doa4_photo_format=png_full"]
    p = subprocess.Popen(args, env=env, cwd=os.path.dirname(exe), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         startupinfo=np_run.hidden_startupinfo())  # minimized, no activation (as np_run.py)
    print("pid", p.pid, flush=True)
    time.sleep(RUN_SECONDS)
    p.terminate()
    p.wait()
    print("photos:", sorted(os.listdir(photos)) if os.path.isdir(photos) else "none")
    log = open(os.path.join(run, "game.log"), encoding="utf-8", errors="replace").read()
    print("[NP] lines:", log.count("[doa4][NP]"), "| movie skipped lines:", log.count("skipped at frame"))
    print("startup and attract play only; shutdown is not exercised (the run ends with TerminateProcess)")


if __name__ == "__main__":
    main(sys.argv)

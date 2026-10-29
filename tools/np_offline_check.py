"""Offline run of a build (no DOA4_NP): the game's own photo test takes photos at 20/40/60/80 s into a
run folder, then this script ends its own process. Shows that a netplay build plays as before when no
session runs: the game.log has no [NP] line and no "skipped at frame" line, and the photos show the title
and the attract demo. Startup and attract play only; shutdown is not exercised (the run ends with
TerminateProcess, which never runs the application's shutdown hook).

  py -3 -I tools/np_offline_check.py EXE NAME

EXE is a snapshot from tools/np_build.bat (np_bin/<tag>/dead_or_alive_4.exe); NAME is a new folder under
RUNS. The run copies the pristine save, uses the shared shader cache, and only the process started here is
ended (never another game process).
"""
import os
import shutil
import subprocess
import sys
import time

RUNS = r"C:/Users/fsop1/doa4/np_runs"
PRISTINE = r"C:/Users/fsop1/doa4/np_phase0/ud_pristine"
CACHE = r"C:/Users/fsop1/doa4/np_runs/cache_shared"
GAME = r"C:\Users\fsop1\doa4\xex\DOA4"

if len(sys.argv) != 3:
    sys.exit(__doc__)
exe, name = sys.argv[1], sys.argv[2]
run = os.path.join(RUNS, name)
os.makedirs(run)  # refuses an existing run folder
ud = os.path.join(run, "ud")
shutil.copytree(PRISTINE, ud)
photos = os.path.join(run, "photos")
env = {k: v for k, v in os.environ.items() if not k.startswith("DOA4_")}
env["DOA4_PHOTO_TEST"] = "hotkey"
args = [exe, f"--game_data_root={GAME}", f"--user_data_root={ud}",
        f"--cache_path={CACHE}", f"--log_file={os.path.join(run, 'game.log')}",
        f"--doa4_photo_dir={photos}", "--doa4_photo_format=png_full"]
p = subprocess.Popen(args, env=env, cwd=os.path.dirname(exe), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("pid", p.pid, flush=True)
time.sleep(95)
p.terminate()
p.wait()
print("photos:", sorted(os.listdir(photos)) if os.path.isdir(photos) else "none")
log = open(os.path.join(run, "game.log"), encoding="utf-8", errors="replace").read()
print("[NP] lines:", log.count("[doa4][NP]"), "| movie skipped lines:", log.count("skipped at frame"))
print("startup and attract play only; shutdown is not exercised (the run ends with TerminateProcess)")

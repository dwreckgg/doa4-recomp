"""Runs one DOA4 netplay phase-0 session (record or replay) and waits for it to end.

Every run gets a fresh copy of a pristine user-data folder (the save the game reads and may
write), so a recording and its replays start from the same save. The game's own data folder
is only read. The process is started by this script and only that process is ended on a
timeout.

  py -3 tools/np_run.py record NAME --script boot.txt [--quit 4000] [--shots 100,200]
  py -3 tools/np_run.py replay NAME --rec runs/NAME0/out/rec.d4r
Options: --exe, --game, --userdata (the pristine source), --runs (where run folders go),
--cache (shared shader cache), --sweep N|all, --dump f,f, --shot-every N,
--env KEY=VALUE (repeatable: extra environment, e.g. DOA4_NP_FIX=none), --timeout seconds.
Prints the run folder and the session log's last lines; writes out/run_result.txt and exits 0 only for a good run (see judge()).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# The quit reasons that end a good run (np_session.cpp Quit(): "quit at frame N: <reason>").
OK_REASONS = {"record": ("quit frame",), "replay": ("end of the recording",)}


def judge(mode, code, timed_out, log_lines):
    """(status, quit line): ok only if the game exited 0 by itself after the session quit for a
    reason that ends a good run of this mode."""
    quit_line = next((l for l in reversed(log_lines) if l.startswith("quit at frame ")), None)
    reason = quit_line.split(": ", 1)[1] if quit_line and ": " in quit_line else None
    ok = not timed_out and code == 0 and reason in OK_REASONS[mode]
    return ("ok" if ok else "failed"), (quit_line or "none")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["record", "replay"])
    ap.add_argument("name")
    ap.add_argument("--exe", default=os.path.join(REPO, "out", "build", "win-amd64-release", "dead_or_alive_4.exe"))
    ap.add_argument("--game", default=r"C:\Users\fsop1\doa4\xex\DOA4")
    ap.add_argument("--userdata", required=True, help="pristine user data folder (copied, never written)")
    ap.add_argument("--runs", required=True, help="folder for run folders")
    ap.add_argument("--cache", default=None, help="shared shader cache folder")
    ap.add_argument("--script")
    ap.add_argument("--rec")
    ap.add_argument("--seed")
    ap.add_argument("--quit")
    ap.add_argument("--shots")
    ap.add_argument("--shot-every")
    ap.add_argument("--sweep")
    ap.add_argument("--dump")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--log-level", default=None, help="the game's log_level (info by default)")
    ap.add_argument("--arg", action="append", default=[], help="extra game argument, e.g. --arg=--headless=true")
    ap.add_argument("--timeout", type=float, default=900)
    a = ap.parse_args()

    run = os.path.join(a.runs, a.name)
    if os.path.exists(run):
        sys.exit(f"run folder exists: {run}")
    ud = os.path.join(run, "ud")
    out = os.path.join(run, "out")
    shutil.copytree(a.userdata, ud)
    os.makedirs(out)

    env = dict(os.environ)
    for k in list(env):
        if k.startswith("DOA4_"):
            del env[k]
    env["DOA4_NP"] = a.mode
    env["DOA4_NP_DIR"] = out
    if a.script:
        env["DOA4_NP_SCRIPT"] = os.path.abspath(a.script)
    if a.rec:
        env["DOA4_NP_REC"] = os.path.abspath(a.rec)
    for key, val in (("DOA4_NP_SEED", a.seed), ("DOA4_NP_QUIT", a.quit), ("DOA4_NP_SHOTS", a.shots),
                     ("DOA4_NP_SHOT_EVERY", a.shot_every), ("DOA4_NP_SWEEP", a.sweep), ("DOA4_NP_DUMP", a.dump)):
        if val:
            env[key] = val
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    args = [a.exe, f"--game_data_root={a.game}", f"--user_data_root={ud}",
            f"--log_file={os.path.join(run, 'game.log')}"]
    if a.log_level:
        args.append(f"--log_level={a.log_level}")
    args += a.arg
    if a.cache:
        os.makedirs(a.cache, exist_ok=True)
        args.append(f"--cache_path={a.cache}")
    with open(os.path.join(run, "command.txt"), "w") as f:
        f.write(" ".join(args) + "\n")
        for k in sorted(env):
            if k.startswith("DOA4_"):
                f.write(f"{k}={env[k]}\n")

    t0 = time.time()
    p = subprocess.Popen(args, env=env, cwd=os.path.dirname(a.exe),
                         stdout=open(os.path.join(run, "stdout.txt"), "w"), stderr=subprocess.STDOUT)
    with open(os.path.join(run, "pid.txt"), "w") as f:
        f.write(str(p.pid))
    print(f"run {run} pid {p.pid}", flush=True)
    timed_out = False
    try:
        code = p.wait(timeout=a.timeout)
    except subprocess.TimeoutExpired:
        print(f"timeout after {a.timeout} s: ending pid {p.pid}", flush=True)
        timed_out = True
        p.kill()
        code = p.wait()
    print(f"exit {code} after {time.time() - t0:.0f} s", flush=True)
    log = os.path.join(out, "np_log.txt")
    lines = []
    if os.path.exists(log):
        lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
        print("\n".join(lines[-8:]))
    else:
        print("no np_log.txt: the session did not start")
    status, quit_line = judge(a.mode, code, timed_out, lines)
    with open(os.path.join(out, "run_result.txt"), "w", encoding="utf-8") as f:
        f.write(f"status {status}\nexit {code}\ntimeout {int(timed_out)}\nquit {quit_line}\n")
    print(f"status {status}", flush=True)
    sys.exit(0 if status == "ok" else 1)


if __name__ == "__main__":
    main()

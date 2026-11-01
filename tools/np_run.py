"""Runs one DOA4 netplay phase-0 session (record or replay) and waits for it to end.

Every run gets a fresh copy of a pristine user-data folder (the save the game reads and may
write), so a recording and its replays start from the same save. The game's own data folder
is only read. The process is started by this script and only that process is ended on a
timeout.

  py -3 tools/np_run.py record NAME --script boot.txt [--quit 4000] [--shots 100,200]
  py -3 tools/np_run.py replay NAME --rec runs/NAME0/out/rec.d4r
Options: --exe, --game (default GAME_DIR: <DOA4_ROOT>/xex/DOA4), --userdata (the pristine source), --runs (where run folders go),
--cache (shared shader cache), --sweep N|all, --dump f,f, --shot-every N,
--env KEY=VALUE (repeatable: extra environment, e.g. DOA4_NP_FIX=none), --timeout seconds,
--affinity MASK (logical CPU mask, e.g. 0x3: the game is started suspended, limited to those CPUs
before any of its threads runs, then resumed; only the run's own process; command.txt ends with
"affinity MASK"),
--show-window (the game is started with the window state it asks for; by default it is started minimized without
taking the focus: STARTF_USESHOWWINDOW + SW_SHOWMINNOACTIVE, so a run never pops a window in front of the user).
The game's environment gets SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 (SDL opens no real controller)
unless the caller set that variable; command.txt lists it with the DOA4_ variables.
A run whose game.log lacks the dot-product self-test line "[doa4][LIFE] dot products: canonical NaN on" (or reports OFF)
is never a good run either (selftest_problem()), nor one whose game.log holds the SDK's lazy-pool WARN tag "[netplay-lazy-pool]"
(lazy_pool_problem(); the run's own game.log, in its fresh run folder).
Prints the run folder and the session log's last lines; writes out/run_result.txt and exits 0 only for a good run (see judge()).
A replay the session refuses (another build, fix set, game data or settings than the recording's) or cannot read is
never a good run: run_result.txt then has "status failed" and "quit refused: ..." / "quit cannot replay ...", or
"quit refusal not honoured (...): ..." when the session ran on after the refusal. A run whose np_log holds a
"WARN " line (a fix's assumption did not hold, np_det.cpp) is never a good run either: "quit WARN ... (<quit line>)".
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# The folder that holds the worktrees, the extracted game (xex/DOA4) and the runs (np_runs): the environment variable
# DOA4_ROOT, by default the parent folder of this repository (every worktree sits in that folder). The other np_ tools
# take their default paths from here.
DOA4_ROOT = os.path.abspath(os.environ.get("DOA4_ROOT") or os.path.dirname(REPO))
GAME_DIR = os.path.join(DOA4_ROOT, "xex", "DOA4")

# The quit reasons that end a good run (np_session.cpp Quit(): "quit at frame N: <reason>").
OK_REASONS = {"record": ("quit frame",), "replay": ("end of the recording",)}


# The log lines with which np_session.cpp Start() refuses to run: a replay whose recording was made by
# another build, fix set, game data, settings ("refused: ...") or cannot be read ("cannot replay ...").
# Either ends the run at once, with no session: no "session on" line, no hash file, exit 0.
REFUSAL_PREFIXES = ("refused: ", "cannot replay ")

# The start of the line the determinism layer logs when an assumption one of its fixes relies on did not hold and
# the session ran on (np_det.cpp: "WARN keep-alive: ..." - the D3D kick thread called a re-present function from a
# site other than its timeout branch). Such a run is never a good run, however it ended.
WARN_PREFIX = "WARN "

# The runtime opens the host's game controllers through SDL even in a headless session (game.log "SDL
# OnControllerDeviceAdded: Added at index 0"). A session's game never reads them (the input hooks serve the script or
# the recording), but rumble (XamInputSetState) could reach the user's controller, and a run without a session would
# read it. This allow-list matches no device, so SDL opens none. Set unless the caller already set the variable (by
# --env or in this script's own environment); a launcher setting, not part of the recording's meta.
SDL_PADS = ("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x0000/0x0000")

# The line the game logs at start-up (dead_or_alive_4_app.h: REXLOG_INFO("[doa4][LIFE] dot products: canonical NaN {}",
# DotProductSelfTest())): "on" when the dot-product redirect returns the canonical NaN, else "OFF (<lane bit patterns>)".
# A build whose redirect is off computes other results than the recordings' (observation Task 1 review M2).
SELFTEST_PREFIX = "[doa4][LIFE] dot products: canonical NaN "

# Hidden launch (ledger ruling 2026-10-30): a game started by a tool never takes the foreground.
STARTF_USESHOWWINDOW = 0x00000001
SW_SHOWMINNOACTIVE = 7


def hidden_startupinfo():
    """A STARTUPINFO that starts the process's first window minimized and without activation (the game's window is
    created by the process itself, which honours wShowWindow for its first ShowWindow call)."""
    si = subprocess.STARTUPINFO()
    si.dwFlags |= STARTF_USESHOWWINDOW
    si.wShowWindow = SW_SHOWMINNOACTIVE
    return si


# The tag of the SDK's two lazy-pool WARN lines (sdk-np1 7c52ae8: object_table.cpp FindFreeSlot "an ordinary object took slot N
# of the lazy pool", xobject.cpp GetNativeObject "keeps the low handle ... later guest handles are shifted"): either means the
# run's guest handles may depend on host timing (debug 82B70000). game.log prints them as "[warning] [sys] [tN] <tag> ...", which
# the np_log WARN_PREFIX rule never sees, so a run's own game.log is searched for the tag (Task 15 carry a).
LAZY_POOL_TAG = "[netplay-lazy-pool]"


def lazy_pool_problem(lines):
    """None if no line of the game log `lines` carries LAZY_POOL_TAG; else "lazy-pool WARN: <the first such line, from the tag>"."""
    for l in lines:
        at = l.find(LAZY_POOL_TAG)
        if at >= 0:
            return "lazy-pool WARN: " + l[at:].strip()
    return None


# Task 15 close-out: the loader-idle hang (np_det_middleware.cpp: "loader: frame N: not idle after 5 s" in np_log) and FIX 13's
# WARN lines (doa4_app_hooks.cpp: "[doa4][FIX][Loader] ..." at warning level in game.log) mean a run whose loader did not
# behave; the record judge passed such runs before (debug-loader-idle-report). FIX 13's info line is the healthy case.
LOADER_IDLE_PREFIX = "loader: frame "
LOADER_IDLE_TEXT = ": not idle"
LOADER_FIX_TAG = "[doa4][FIX][Loader]"


def loader_problem(np_lines, game_lines):
    """None if np_log `np_lines` hold no loader-idle line and game.log `game_lines` no FIX 13 WARN line; else the first one."""
    for l in np_lines or []:
        if l.startswith(LOADER_IDLE_PREFIX) and LOADER_IDLE_TEXT in l:
            return "loader idle: " + l.strip()
    for l in game_lines or []:
        if LOADER_FIX_TAG in l and "[warning]" in l:
            return "loader WARN: " + l[l.find(LOADER_FIX_TAG):].strip()
    return None


def selftest_problem(lines):
    """None if the game log `lines` hold the dot-product self-test line and every such line says "on"; else why not."""
    found = []
    for l in lines:
        at = l.find(SELFTEST_PREFIX)
        if at >= 0:
            found.append(l[at:].strip())
    if not found:
        return "NO SELFTEST: game.log has no '" + SELFTEST_PREFIX + "on' line"
    bad = next((l for l in found if l[len(SELFTEST_PREFIX):] != "on"), None)
    return None if bad is None else f"SELFTEST not on: {bad}"


def judge(mode, code, timed_out, log_lines, out_dir=None, game_lines=None):
    """(status, quit line): ok only if the game exited 0 by itself after the session quit for a
    reason that ends a good run of this mode, and the log holds no WARN line.

    A run whose log holds a refusal is never ok, and its reason (the quit line of run_result.txt) is
    the refusal itself when the refusal was honoured: the game exited 0 on its own with no session
    started after it ("session on" line) and, when out_dir is given, no digest.d4d in it. Otherwise the
    reason is "refusal not honoured (<what>): <the refusal>": the status is "failed" either way, so
    this is the line that tells a clean refusal from one the session ignored.
    A run whose log holds a line starting with WARN_PREFIX is failed with the reason "<the first WARN line>
    (<the quit line, or 'no quit line'>)".
    game_lines (the game.log lines; None: not checked) must hold the dot-product self-test line saying "on"
    (selftest_problem): a run that would otherwise be good fails with "<the problem> (<the quit line>)"; and must hold no
    line with the SDK's LAZY_POOL_TAG (lazy_pool_problem): such a run fails with "lazy-pool WARN: <line> (<the quit line>)"."""
    at = next((i for i, l in enumerate(log_lines) if l.startswith(REFUSAL_PREFIXES)), None)
    if at is not None:
        flaws = []
        if any(l.startswith("session on") for l in log_lines[at + 1:]):
            flaws.append("a session started after it")
        if out_dir and os.path.exists(os.path.join(out_dir, "digest.d4d")):
            flaws.append("digest.d4d was written")
        if timed_out:
            flaws.append("the run had to be ended after the timeout")
        elif code != 0:
            flaws.append(f"exit {code}")
        if flaws:
            return "failed", f"refusal not honoured ({'; '.join(flaws)}): {log_lines[at]}"
        return "failed", log_lines[at]
    quit_line = next((l for l in reversed(log_lines) if l.startswith("quit at frame ")), None)
    warn = next((l for l in log_lines if l.startswith(WARN_PREFIX)), None)
    if warn is not None:
        return "failed", f"{warn} ({quit_line or 'no quit line'})"
    lazy = lazy_pool_problem(game_lines) if game_lines is not None else None
    if lazy is not None:
        return "failed", f"{lazy} ({quit_line or 'no quit line'})"
    loader = loader_problem(log_lines, game_lines)
    if loader is not None:
        return "failed", f"{loader} ({quit_line or 'no quit line'})"
    reason = quit_line.split(": ", 1)[1] if quit_line and ": " in quit_line else None
    ok = not timed_out and code == 0 and reason in OK_REASONS[mode]
    problem = selftest_problem(game_lines) if ok and game_lines is not None else None
    if problem is not None:
        return "failed", f"{problem} ({quit_line})"
    return ("ok" if ok else "failed"), (quit_line or "none")


def limit_cpus(process, mask):
    """Puts the (suspended) process, a Windows process handle as an int, into a new job object whose affinity
    limit is `mask`, and returns the job handle (closing the last handle of a job with live processes keeps the
    limit). OSError if Windows refuses (a mask of no CPU of the system, a job the process cannot join).
    A job limit is used instead of SetProcessAffinityMask because the game's runtime widens its own mask to the
    system's at startup (rexglue-sdk src/system/runtime.cpp: thread::EnableAffinityConfiguration), which undoes a
    plain process mask but cannot lift a job's."""
    import ctypes
    from ctypes import wintypes

    class BasicLimits(ctypes.Structure):  # JOBOBJECT_BASIC_LIMIT_INFORMATION
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateJobObjectW.restype = wintypes.HANDLE
    k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    job = k32.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    limits = BasicLimits()
    limits.LimitFlags = 0x00000010  # JOB_OBJECT_LIMIT_AFFINITY
    limits.Affinity = mask
    if not k32.SetInformationJobObject(job, 2, ctypes.byref(limits), ctypes.sizeof(limits)):  # JobObjectBasicLimitInformation
        raise ctypes.WinError(ctypes.get_last_error())
    if not k32.AssignProcessToJobObject(job, process):
        raise ctypes.WinError(ctypes.get_last_error())
    return job


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["record", "replay"])
    ap.add_argument("name")
    ap.add_argument("--exe", default=os.path.join(REPO, "out", "build", "win-amd64-release", "dead_or_alive_4.exe"))
    ap.add_argument("--game", default=GAME_DIR)
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
    ap.add_argument("--affinity", help="logical CPU mask, e.g. 0x3 (two logical CPUs)")
    ap.add_argument("--show-window", action="store_true", help="do not start the game minimized without activation")
    a = ap.parse_args()
    affinity = None
    if a.affinity:
        try:
            affinity = int(a.affinity, 0)
        except ValueError:
            sys.exit(f"--affinity {a.affinity!r}: not a number (e.g. 0x3)")

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
    env.setdefault(*SDL_PADS)  # no real controller is opened (see SDL_PADS)
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
            if k.startswith("DOA4_") or k == SDL_PADS[0]:
                f.write(f"{k}={env[k]}\n")

    t0 = time.time()
    flags = 0x00000004 if a.affinity else 0  # CREATE_SUSPENDED: limit the CPUs before any thread runs
    p = subprocess.Popen(args, env=env, cwd=os.path.dirname(a.exe), creationflags=flags,
                         startupinfo=None if a.show_window else hidden_startupinfo(),
                         stdout=open(os.path.join(run, "stdout.txt"), "w"), stderr=subprocess.STDOUT)
    if a.affinity:
        try:
            job = limit_cpus(int(p._handle), affinity)  # the job handle stays open as long as this script runs
        except OSError as e:
            p.kill()
            sys.exit(f"affinity {a.affinity} refused, the game was not run: {e}")
        import ctypes
        ctypes.windll.ntdll.NtResumeProcess(ctypes.c_void_p(int(p._handle)))
        with open(os.path.join(run, "command.txt"), "a") as f:
            f.write(f"affinity {a.affinity}\n")
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
    game_log = os.path.join(run, "game.log")
    game_lines = open(game_log, encoding="utf-8", errors="replace").read().splitlines() if os.path.exists(game_log) else []
    status, quit_line = judge(a.mode, code, timed_out, lines, out, game_lines)
    with open(os.path.join(out, "run_result.txt"), "w", encoding="utf-8") as f:
        f.write(f"status {status}\nexit {code}\ntimeout {int(timed_out)}\nquit {quit_line}\n")
    print(f"status {status}", flush=True)
    sys.exit(0 if status == "ok" else 1)


if __name__ == "__main__":
    main()

"""The two-copy harness of DOA4 netplay phase 1: starts a host and a joiner copy of the game on this PC, connects them
through the Online tab's request files (no keyboard, mouse or focus automation), lets them play a pad script, and judges
what each copy wrote.

  py -3 -I tools/np_pair.py NAME --script <pad script> --userdata <pristine save> --runs <folder> --exe <exe>
      [--joiner-exe EXE] [--cache DIR] [--joiner-affinity MASK] [--sim D,J,L] [--delay N] [--seed N]
      [--env-host K=V ...] [--env-joiner K=V ...] [--arg-host=ARG ...] [--arg-joiner=ARG ...]
      [--kill-joiner-at-frame N] [--third-joiner] [--host-only SECONDS]
      [--expect ok|desync|lost|refused-build|refused-files|refused-busy|refused-session|lobby] [--timeout S]
      [--game DIR] [--end-grace S] [--step-timeout S] [--settle S] [--orphan-watch S]

Run folders <runs>/NAME_host and <runs>/NAME_joiner (and NAME_third), each: ud/ (a copy of the pristine save), ctl/ (the
test switches' folder DOA4_NP_TEST_DIR: np_ctl.on, status.txt, pids.txt, session/), game.log, command.txt, stdout.txt,
run_result.txt. The pair verdict goes to <runs>/NAME_pair.txt; the exit status is 0 only when the verdict is the expected one.

Flow: host (`host`, wait state=hosting) -> joiner (`join 127.0.0.1:<port>`, wait both connected and the host's can_start=1,
or the joiner's refusal) -> `start` to the host -> follow pids.txt to the session processes -> wait for both terminal lines
`session ended at frame <F>: <reason>` + `checksums handed to GekkoNet: <N>` (or a session process that is gone) -> let the offline
relaunch reach its pids.txt line -> end every owned process -> judge. The judge of each --expect is judge_*() below.
Every copy gets the test switches of the run contract (DOA4_NP_TEST_DIR, DOA4_NP_LOCAL_SCRIPT, DOA4_NP_BIND=loopback,
DOA4_NP_STUN=off, DOA4_NP_UPNP=off, ...) and SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 unless the caller set it.

Process safety: this script ends only the processes it started (their Popen handles) and the pids its own runs' pids.txt files
list (and only while the process's creation time matches the line's time: a reused pid is not ours): `taskkill /PID <pid>` (a
close request), `/F` after --end-grace seconds (15). Never by image name. Every process it starts is hidden: the game with
STARTF_USESHOWWINDOW + SW_SHOWMINNOACTIVE, helpers with CREATE_NO_WINDOW.
A refusal is judged from the lobby's status.txt key `refusal=<none|build|files|busy|proto|session|other>` (a missing key fails
with a clear message); the event text is only printed, and for files must name the salted group. A refused copy must also not be
starting / restarting / in_session, have no ctl/session folder, no ud/netplay/handoff.d4h and no non-empty ud/netplay/session or sessions folder (ud/netplay/events.txt is the lobby's own and allowed), watched for --settle
seconds (8: longer than a real game's OnPostInitLogging).
After the last close request end_all keeps watching every copy's pids.txt until it has not grown for --orphan-watch seconds (10),
ending any new owned pid: a game that relaunches itself after a close request leaves nothing behind.
Verdict rules that hold for every --expect: the real save (every file under ud/B13EBABEBABEBABE) is hashed before the launch and
after the offline relaunch has ended, a difference fails the pair; each copy's game.log holds the dot-product self-test line and
no line with the SDK's lazy-pool WARN tag "[netplay-lazy-pool]" (Task 15 carry a).
Frame-0 instrument (Task 15 carry b): both copies get DOA4_NP_DUMP=0 unless --env-host/--env-joiner set it; when both lists hold
frame 0, the two dumps (ctl/session/dumps/data_000000.bin) are compared over the whole .data with the sweep's masks and the SDK
handle stash words zeroed (np_dumpdiff.frame0_difference): a difference, or a missing dump, fails an --expect ok pair with the
first differing address and its 64 KB block (and "handle shift N in K words" when the handles moved); other expects print it.
--expect refused-session (gate S8d): the joiner is started with DOA4_NP_TEST_NO_RESTART=1 by the caller (--env-joiner): it says
yes but stays a lobby. Only the host's session is waited for; judge_session_refused() wants the host ended at frame 0 with
REFUSED_SESSION_REASON and relaunched offline, the joiner with refusal=session in status.txt and no session.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # py -3 -I does not put the script's folder on the path
sys.dont_write_bytecode = True
import np_compare  # noqa: E402
import np_dumpdiff  # noqa: E402
import np_run  # noqa: E402

CREATE_NO_WINDOW = 0x08000000
CREATE_SUSPENDED = 0x00000004
SAVE_DIR = "B13EBABEBABEBABE"
EXPECTS = ("ok", "desync", "lost", "refused-build", "refused-files", "refused-busy", "refused-session", "lobby")
# --expect refused-session (Task 15 carry f, gate S8d): the joiner said yes but stayed a lobby (DOA4_NP_TEST_NO_RESTART=1), so
# the host's session Hello reached a lobby; np_session.cpp ends the host's session with this reason at frame 0.
REFUSED_SESSION_REASON = "internal: the other player's game did not start the session"
DUMP0 = os.path.join("dumps", "data_000000.bin")  # the frame-0 .data dump in a session folder (DOA4_NP_DUMP=0)
TERMINAL = re.compile(r"^session ended at frame (\d+): (.*)$")
CHECKSUMS = re.compile(r"^checksums handed to GekkoNet: (\d+)\s*$")
FRAME_LINE = re.compile(r"^frame (\d+)\b")
TIMING_SLACK_MS = 50
HOLD_TO_BANNER_MS = 1000
HOLD_TO_END_MS = 10000
OK_REASONS = ("left", "peer left")
POLL = 0.2
CLOSE_REPEAT_S = 2.0  # end_all repeats its close request this often (fix round Q3)


# --- small file helpers ------------------------------------------------------------------------------------------

def read_text(path):
    """The text of a file read with FILE_SHARE_DELETE (Python's open() lacks it): a reader that holds a file open without it makes
    the writer's rename-over (the game's and this stand-in's temp-file-then-rename) fail on Windows. Raises OSError."""
    import ctypes
    import msvcrt
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = ctypes.c_void_p
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                wintypes.DWORD, ctypes.c_void_p]
    h = k32.CreateFileW(path, 0x80000000, 7, None, 3, 0x80, None)  # GENERIC_READ, share read|write|delete, OPEN_EXISTING
    if h is None or h == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    with os.fdopen(msvcrt.open_osfhandle(h, os.O_RDONLY), "rb") as f:
        return f.read().decode("utf-8", "replace")


def read_kv(path):
    """key=value lines of a status file; unknown keys are kept, a missing or half-written file reads as {}."""
    out = {}
    try:
        for line in read_text(path).splitlines():
            k, sep, v = line.partition("=")
            if sep:
                out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def read_lines(path):
    try:
        return read_text(path).splitlines()
    except OSError:
        return []


def read_pids(ctl):
    """[(pid, kind, unix ms)] of ctl/pids.txt: lines `pid <n> <lobby|session|offline> <unix ms>`."""
    rows = []
    for line in read_lines(os.path.join(ctl, "pids.txt")):
        w = line.split()
        if len(w) >= 4 and w[0] == "pid" and w[1].isdigit() and w[3].isdigit():
            rows.append((int(w[1]), w[2], int(w[3])))
    return rows


def parse_script_quit(path):
    """The frame of the pad script's first `quit` line (np_core.cpp ParseScript: `<n> <pads>` lines hold n frames, `random <n>
    ...` n frames, connect/shot none); None if there is none."""
    frames = 0
    for line in read_lines(path):
        words = line.split("#", 1)[0].split()
        if not words:
            continue
        head = words[0].upper()
        if head == "QUIT":
            return frames
        if head == "RANDOM" and len(words) > 1 and words[1].isdigit():
            frames += int(words[1])
        elif head.isdigit():
            frames += int(head)
    return None


def hash_saves(ud):
    """{relative path: sha256} of every file under ud/B13EBABEBABEBABE (the copy's real save)."""
    out = {}
    root = os.path.join(ud, SAVE_DIR)
    for dirpath, _, files in os.walk(root):
        for name in files:
            full = os.path.join(dirpath, name)
            with open(full, "rb") as f:
                out[os.path.relpath(full, root).replace("\\", "/")] = hashlib.sha256(f.read()).hexdigest()
    return out


# --- processes ---------------------------------------------------------------------------------------------------

def proc_start(pid):
    """The process's creation time (seconds since the epoch), or None if it cannot be read (gone, no access)."""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION: read only
    if not h:
        return None
    t = [wintypes.FILETIME() for _ in range(4)]
    ok = k32.GetProcessTimes(h, *[ctypes.byref(x) for x in t])
    k32.CloseHandle(h)
    if not ok:
        return None
    return ((t[0].dwHighDateTime << 32) | t[0].dwLowDateTime) / 1e7 - 11644473600


def pid_alive(pid):
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    h = k32.OpenProcess(0x1000, False, pid)
    if not h:
        return False
    code = wintypes.DWORD()
    k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return code.value == 259  # STILL_ACTIVE


def process_affinity(pid):
    """The CPU mask Windows reports for the (live) process, or None."""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.GetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    h = k32.OpenProcess(0x1000, False, pid)
    if not h:
        return None
    mine, system = ctypes.c_size_t(), ctypes.c_size_t()
    ok = k32.GetProcessAffinityMask(h, ctypes.byref(mine), ctypes.byref(system))
    k32.CloseHandle(h)
    return mine.value if ok else None


def taskkill(pid, force):
    cmd = ["taskkill", "/PID", str(pid)] + (["/F"] if force else [])
    subprocess.run(cmd, capture_output=True, creationflags=CREATE_NO_WINDOW)


def owned_alive(pid, ms):
    """The pid is alive and its creation time matches the pids.txt line's time (a reused pid is another process)."""
    if not pid_alive(pid):
        return False
    created = proc_start(pid)
    return created is not None and ms / 1000.0 - 600 <= created <= ms / 1000.0 + 5


class StepError(Exception):
    """A harness step that cannot be done (reported as a problem of the pair, never skipped)."""


class Side:
    """One game copy: its run folder, process handle (and affinity job) and log paths."""

    def __init__(self, name, run):
        self.name = name
        self.run = run
        self.ctl = os.path.join(run, "ctl")
        self.ud = os.path.join(run, "ud")
        self.popen = None
        self.job = None
        self.stdout = None
        self.saves_before = {}
        self.dump0 = False  # the copy's DOA4_NP_DUMP lists frame 0 (game_env)

    def status(self):
        return read_kv(os.path.join(self.ctl, "status.txt"))

    def session_dir(self):
        return os.path.join(self.ctl, "session")

    def np_log(self):
        return read_lines(os.path.join(self.session_dir(), "np_log.txt"))

    def terminal(self):
        """(frame, reason) of the last `session ended` line of np_log.txt, or None."""
        found = None
        for l in self.np_log():
            m = TERMINAL.match(l)
            if m:
                found = (int(m.group(1)), m.group(2))
        return found

    def checksums(self):
        n = None
        for l in self.np_log():
            m = CHECKSUMS.match(l)
            if m:
                n = int(m.group(1))
        return n

    def passed_frame(self):
        """The highest `frame <F>` line of np_log.txt, or -1."""
        best = -1
        for l in self.np_log():
            m = FRAME_LINE.match(l)
            if m:
                best = max(best, int(m.group(1)))
        return best

    def send(self, text, timeout=5.0):
        """Writes the request file np_ctl.on (temp file, then rename) once the game has taken the previous one."""
        path = os.path.join(self.ctl, "np_ctl.on")
        end = time.time() + timeout
        while os.path.exists(path) and time.time() < end:
            time.sleep(0.05)
        if os.path.exists(path):  # replacing it would lose the earlier command
            raise StepError(f"the {self.name} copy did not take the previous np_ctl.on within {timeout} s; {text.strip()!r} not sent")
        tmp = os.path.join(self.ctl, "np_ctl.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text if text.endswith("\n") else text + "\n")
        os.replace(tmp, path)

    def session_pids(self):
        return [p for p in read_pids(self.ctl) if p[1] == "session"]

    def session_alive(self):
        """The last session process is alive AND is the process pids.txt named (creation time matches the line)."""
        rows = self.session_pids()
        return bool(rows) and owned_alive(rows[-1][0], rows[-1][2])


# --- the pair ----------------------------------------------------------------------------------------------------

class Pair:
    def __init__(self, a):
        self.a = a
        self.sides = []
        self.problems = []
        self.notes = []
        self.deadline = None
        self.lobby_status = {}
        self.snap = {}  # the refused / answering copy's status.txt when its answer came (later processes rewrite the file)
        self.ended = False
        self.joiner_event = ""
        self.delay_host = 0

    # ------------------------------------------------------------------ setup
    def game_env(self, side, host):
        a = self.a
        env = {k: v for k, v in os.environ.items() if not k.startswith("DOA4_")}
        env.setdefault(*np_run.SDL_PADS)
        env["DOA4_NP_TEST_DIR"] = side.ctl
        env["DOA4_NP_LOCAL_SCRIPT"] = os.path.abspath(a.script)
        env["DOA4_NP_BIND"] = "loopback"
        env["DOA4_NP_STUN"] = "off"
        env["DOA4_NP_UPNP"] = "off"
        if a.sim:
            env["DOA4_NP_SIM"] = a.sim
        if host and a.seed is not None:
            env["DOA4_NP_SEED"] = str(a.seed)
        if host and a.delay is not None:
            env["DOA4_NP_DELAY"] = str(a.delay)
        env["DOA4_NP_DUMP"] = "0"  # the frame-0 .data instrument (Task 15 carry b); a caller's --env-* replaces it
        for kv in (a.env_host if host else a.env_joiner if side.name == "joiner" else []):
            k, _, v = kv.partition("=")
            env[k] = v
        side.dump0 = "0" in [x.strip() for x in env.get("DOA4_NP_DUMP", "").split(",")]
        return env

    def make_side(self, name):
        a = self.a
        side = Side(name, os.path.join(a.runs, f"{a.name}_{name}"))
        if os.path.exists(side.run):
            raise SystemExit(f"run folder exists: {side.run}")
        os.makedirs(side.ctl)
        shutil.copytree(a.userdata, side.ud)
        side.saves_before = hash_saves(side.ud)
        self.sides.append(side)
        return side

    def launch(self, side, exe, env, extra_args, affinity=None):
        a = self.a
        args = [exe, "--headless=true", f"--game_data_root={a.game}", f"--user_data_root={side.ud}",
                f"--log_file={os.path.join(side.run, 'game.log')}"]
        if a.cache:
            os.makedirs(a.cache, exist_ok=True)
            args.append(f"--cache_path={a.cache}")
        args += extra_args
        with open(os.path.join(side.run, "command.txt"), "w") as f:
            f.write(" ".join(args) + "\n")
            for k in sorted(env):
                if k.startswith("DOA4_") or k == np_run.SDL_PADS[0]:
                    f.write(f"{k}={env[k]}\n")
        side.stdout = open(os.path.join(side.run, "stdout.txt"), "w")
        flags = CREATE_SUSPENDED if affinity else 0  # limit the CPUs before any thread runs
        side.popen = subprocess.Popen(args, env=env, cwd=os.path.dirname(os.path.abspath(exe)), creationflags=flags,
                                      startupinfo=np_run.hidden_startupinfo(), stdout=side.stdout, stderr=subprocess.STDOUT)
        with open(os.path.join(side.run, "pid.txt"), "w") as f:  # the launched process (the batch driver's own-process list)
            f.write(str(side.popen.pid))
        if affinity:
            try:
                side.job = np_run.limit_cpus(int(side.popen._handle), affinity)
            except OSError as e:
                side.popen.kill()
                raise SystemExit(f"affinity {affinity:#x} refused, the {side.name} was not run: {e}")
            import ctypes
            ctypes.windll.ntdll.NtResumeProcess(ctypes.c_void_p(int(side.popen._handle)))
            with open(os.path.join(side.run, "command.txt"), "a") as f:
                f.write(f"affinity {affinity:#x}\n")

    # ------------------------------------------------------------------ waiting
    def wait(self, pred, timeout, what):
        """Polls pred() until true; False (and a problem) when `timeout` seconds or the overall deadline pass."""
        end = time.time() + timeout
        while True:
            if pred():
                return True
            if time.time() >= end:
                self.problems.append(f"timeout waiting for {what}")
                return False
            time.sleep(POLL)

    # ------------------------------------------------------------------ ending processes
    def owned_pids(self):
        """{pid: label} of the live processes this run owns: the Popen handles, and the pids.txt pids whose creation time
        matches the line (a reused pid fails the match and is left alone)."""
        mine = {}
        for side in self.sides:
            if side.popen is not None and side.popen.poll() is None:
                mine[side.popen.pid] = f"{side.name} launch"
            for pid, kind, ms in read_pids(side.ctl):
                if owned_alive(pid, ms):
                    mine[pid] = f"{side.name} {kind}"
        return mine

    def end_all(self):
        """Ends every owned process: a close request, /F after --end-grace seconds. Returns the labels that needed /F."""
        forced = []
        self.ended = True
        quiet_from = time.time()
        seen = -1
        while True:
            mine = self.owned_pids()
            lines = sum(len(read_pids(s.ctl)) for s in self.sides)
            if lines != seen:  # a new pids.txt line (a relaunch) restarts the watch
                seen, quiet_from = lines, time.time()
            if not mine:
                if time.time() - quiet_from >= self.a.orphan_watch:
                    return forced
                time.sleep(0.2)
                continue
            quiet_from = time.time()
            # Task 12 fix round Q3: a close request reaches a process only through its windows, and a game that was just
            # relaunched may not have one yet: the request is repeated every CLOSE_REPEAT_S until the process is gone,
            # /F only after --end-grace.
            for pid in mine:
                taskkill(pid, False)
            end = time.time() + self.a.end_grace
            last = time.time()
            while time.time() < end and any(pid_alive(p) for p in mine):
                if time.time() - last >= CLOSE_REPEAT_S:
                    last = time.time()
                    for pid in mine:
                        if pid_alive(pid):
                            taskkill(pid, False)
                time.sleep(0.1)
            for pid, label in mine.items():
                if pid_alive(pid):
                    taskkill(pid, True)
                    forced.append(label)
            time.sleep(0.2)

    # ------------------------------------------------------------------ the flow
    def run(self):
        a = self.a
        host = self.make_side("host")
        self.launch(host, a.exe, self.game_env(host, True), a.arg_host)
        if not self.wait(lambda: host.status().get("state") is not None, a.step_timeout, "the host's status.txt"):
            return
        host.send("host")
        if not self.wait(lambda: host.status().get("state") == "hosting", a.step_timeout, "the host's state=hosting"):
            return
        port = host.status().get("port", "")
        self.notes.append(f"host hosting on port {port}")
        if a.host_only is not None:
            time.sleep(a.host_only)
            self.lobby_status = host.status()
            return
        joiner = self.make_side("joiner")
        affinity = int(a.joiner_affinity, 0) if a.joiner_affinity else None
        self.launch(joiner, a.joiner_exe or a.exe, self.game_env(joiner, False), a.arg_joiner, affinity)
        self.wait(lambda: joiner.status().get("state") is not None, a.step_timeout, "the joiner's status.txt")
        before = joiner.status().get("event", "")
        joiner.send(f"join 127.0.0.1:{port}")

        def settled():
            st = joiner.status()
            if st.get("state") == "connected" and host.status().get("state") == "connected" and host.status().get("can_start") == "1":
                return True
            refused_now = st.get("refusal", "none") not in ("none", "")
            return st.get("state") == "offline" and (refused_now or st.get("event", "") not in ("", before))  # refused
        if not self.wait(settled, a.step_timeout, "both copies connected (host can_start=1) or the joiner's refusal"):
            return
        self.snap["joiner"] = joiner.status()
        self.joiner_event = joiner.status().get("event", "")
        refused = joiner.status().get("state") == "offline"
        if refused:
            self.notes.append(f"joiner refused: {self.joiner_event}")
            time.sleep(a.settle)  # a refusal that did not hold shows as a session process in this time
            return
        self.delay_host = int(host.status().get("delay", "0") or 0)  # the session's delay is the host's
        if a.third_joiner:
            third = self.make_side("third")
            self.launch(third, a.joiner_exe or a.exe, self.game_env(third, False), a.arg_joiner)
            self.wait(lambda: third.status().get("state") is not None, a.step_timeout, "the third copy's status.txt")
            t_before = third.status().get("event", "")
            third.send(f"join 127.0.0.1:{port}")
            if self.wait(lambda: third.status().get("state") in ("offline", "connected") and
                         (third.status().get("event", "") not in ("", t_before) or
                          third.status().get("refusal", "none") not in ("none", "")), a.step_timeout, "the third copy's answer"):
                self.snap["third"] = third.status()
                self.notes.append(f"third joiner: state {third.status().get('state')}, event {third.status().get('event', '')}")
        if a.expect == "refused-busy":
            time.sleep(a.settle)
            return  # the pair never starts a session
        host.send("start")
        self.play(host, joiner)

    def play(self, host, joiner):
        a = self.a
        self.deadline = time.time() + a.timeout
        killed = False
        read_back = False
        finished = {"host": False, "joiner": False}

        def side_done(side):
            if side.terminal() and side.checksums() is not None:
                return True
            pids = side.session_pids()
            return bool(pids) and not side.session_alive()  # the session process is gone: nothing more will be written

        while time.time() < self.deadline:
            if a.joiner_affinity and not read_back and joiner.session_pids():
                read_back = True
                pid = joiner.session_pids()[-1][0]
                mask = process_affinity(pid)
                text = "unreadable (the session process was already gone)" if mask is None else f"{mask:#x}"
                with open(os.path.join(joiner.run, "command.txt"), "a") as f:
                    f.write(f"affinity read back {text}\n")
                want = int(a.joiner_affinity, 0)
                if mask != want:
                    self.problems.append(f"joiner affinity read back {text}, expected {want:#x}")
            if a.kill_joiner_at_frame is not None and not killed and joiner.passed_frame() >= a.kill_joiner_at_frame \
                    and joiner.session_pids():
                killed = True
                pid = joiner.session_pids()[-1][0]
                if joiner.session_alive():  # ours: the creation time matches the pids.txt line
                    self.notes.append(f"joiner session pid {pid} ended at frame {joiner.passed_frame()}")
                    taskkill(pid, False)
                    end = time.time() + a.end_grace
                    while time.time() < end and joiner.session_alive():
                        time.sleep(0.1)
                    if joiner.session_alive():
                        taskkill(pid, True)
            for s in (host, joiner):
                finished[s.name] = side_done(s)
            if a.expect == "refused-session":  # the joiner stays a lobby: only the host's session is waited for
                finished["joiner"] = True
            if all(finished.values()):
                break
            time.sleep(POLL)
        else:
            self.problems.append(f"timeout after {a.timeout} s waiting for the sessions to end")
        if a.expect == "refused-session":
            self.snap["joiner_end"] = joiner.status()  # read before end_all ends the lobby that answered
        for s in (host, joiner):  # the offline relaunch of every copy whose session ended
            if s.terminal():
                self.wait(lambda s=s: any(p[1] == "offline" for p in read_pids(s.ctl)), a.step_timeout,
                          f"the {s.name}'s offline relaunch (pids.txt line)")

    # ------------------------------------------------------------------ judging
    def judge(self):
        a = self.a
        by_name = {s.name: s for s in self.sides}
        host, joiner = by_name.get("host"), by_name.get("joiner")
        lines = []
        if a.expect == "lobby":
            self.judge_lobby(lines)
        elif a.expect == "refused-session":
            if host and joiner:
                self.judge_session_refused(lines, host, joiner)
            else:
                self.problems.append("refused-session needs a host and a joiner copy")
        elif a.expect.startswith("refused-"):
            self.judge_refusal(lines, by_name)
        elif host and joiner:
            self.judge_session(lines, host, joiner)
        for s in self.sides:  # each copy's own game.log (its fresh run folder; lobby, session and offline processes append)
            game_lines = read_lines(os.path.join(s.run, "game.log"))
            for problem in (np_run.selftest_problem(game_lines), np_run.lazy_pool_problem(game_lines)):
                if problem:
                    self.problems.append(f"{s.name} game.log: {problem}")
        return lines

    def judge_session_refused(self, lines, host, joiner):
        """S8d: the host's session ended at frame 0 with REFUSED_SESSION_REASON and relaunched offline; the joiner answered as a
        lobby (status.txt refusal=session) and never had a session process or session folder."""
        t = host.terminal()
        lines.append(f"host: terminal {t} checksums {host.checksums()}")
        if t is None or t[1] != REFUSED_SESSION_REASON or t[0] != 0:
            self.problems.append(f"the host copy ended {t[1] if t else None!r}{'' if t is None else f' at frame {t[0]}'}, "
                                 f"expected '{REFUSED_SESSION_REASON}' at frame 0")
        kinds = [p[1] for p in read_pids(host.ctl)]
        lines.append(f"host pids.txt: {' '.join(kinds)}")
        if "session" not in kinds or "offline" not in kinds[kinds.index("session"):]:
            self.problems.append(f"the host has no offline relaunch after its session (pids.txt: {' '.join(kinds) or 'empty'})")
        jk = [p[1] for p in read_pids(joiner.ctl)]
        st = self.snap.get("joiner_end") or joiner.status()
        lines.append(f"joiner pids.txt: {' '.join(jk)}; status.txt: state={st.get('state')} refusal={st.get('refusal')}")
        if "session" in jk:
            self.problems.append("the joiner started a session process (pids.txt has a session line)")
        if os.path.isdir(joiner.session_dir()):
            self.problems.append("the joiner has a ctl/session folder")
        if st.get("refusal") != "session":
            self.problems.append(f"the joiner's status.txt says refusal={st.get('refusal')!r}, expected 'session' "
                                 f"(the lobby answered the host's session Hello)")

    def frame0_check(self, lines, host, joiner, fail):
        """Task 15 carry b: the two frame-0 .data dumps compared over the whole .data with the sweep's masks and the stash words
        zeroed (np_dumpdiff.frame0_difference), plus the handle-shift line. With fail, a difference or a missing dump is a
        problem; otherwise only printed."""
        if not (host.dump0 and joiner.dump0):
            lines.append("frame-0 dump check not made (DOA4_NP_DUMP lacks frame 0 on: "
                         + ", ".join(s.name for s in (host, joiner) if not s.dump0) + ")")
            return
        data = {}
        for s in (host, joiner):
            path = os.path.join(s.session_dir(), DUMP0)
            if not os.path.exists(path):
                msg = f"the {s.name} wrote no frame-0 dump ({path})"
                (self.problems if fail else lines).append(msg)
                return
            with open(path, "rb") as f:
                data[s.name] = f.read()
        diff = np_dumpdiff.frame0_difference(data["host"], data["joiner"])
        if diff is None:
            lines.append(f"frame-0 .data dumps equal ({len(data['host'])} bytes; sweep masks and stash words applied)")
            return
        shift = np_dumpdiff.handle_shift(data["host"], data["joiner"])
        msg = f"frame-0 .data dumps differ at {diff[0]:08X} (block {diff[1]:08X})" + \
              (f"; {np_dumpdiff.shift_text(shift)} (joiner - host)" if shift else "")
        (self.problems if fail else lines).append(msg)

    def judge_lobby(self, lines):
        st = getattr(self, "lobby_status", None) or {}
        lines.append("host status.txt: " + " ".join(f"{k}={v}" for k, v in sorted(st.items())))
        if st.get("state") != "hosting":
            self.problems.append(f"the host's state is {st.get('state')!r}, expected hosting")

    def judge_refusal(self, lines, by_name):
        a = self.a
        kind = a.expect[len("refused-"):]
        who = "third" if kind == "busy" and "third" in by_name else "joiner"
        copy = by_name.get(who)
        if copy is None:
            self.problems.append(f"no {who} copy ran")
            return
        st = self.snap.get(who) or copy.status()
        event = st.get("event", "")
        lines.append(f"{who} status.txt: state={st.get('state')} event={event}")
        if st.get("state") != "offline":
            self.problems.append(f"the {who} copy is {st.get('state')!r}, expected offline (refused)")
        if "refusal" not in st:
            self.problems.append(f"the {who} copy's status.txt has no refusal= key (contract: refusal=<none|build|files|busy|proto|session|other>): "
                                 f"the kind of the refusal cannot be judged")
        elif st["refusal"] != kind:
            self.problems.append(f"the {who} copy's refusal is {st['refusal']!r}, not a {kind} refusal (event {event!r})")
        if kind == "files":  # the group name is in the event text
            salts = [kv.partition("=")[2] for kv in a.env_host + a.env_joiner if kv.startswith("DOA4_NP_TEST_FILES_SALT=")]
            for salt in salts:
                if salt and salt.lower() not in event.lower():
                    self.problems.append(f"the files refusal's event {event!r} does not name the salted group {salt!r}")
        for s in self.sides:  # whatever else happened, a session must not have started
            sess = [p for p in read_pids(s.ctl) if p[1] == "session"]
            if sess:
                self.problems.append(f"a session started on the {s.name} copy (pids.txt: {sess[0][0]}) in a refused run")
        for s in ([copy] if copy else []):
            state = s.status().get("state")
            if state in ("starting", "restarting", "in_session"):
                self.problems.append(f"the refused {s.name} copy is {state!r}: its session started in spite of the refusal")
            if os.path.isdir(s.session_dir()):
                self.problems.append(f"the refused {s.name} copy has a ctl/session folder")
            netplay = os.path.join(s.ud, "netplay")
            if os.path.exists(os.path.join(netplay, "handoff.d4h")):
                self.problems.append(f"the refused {s.name} copy wrote ud/netplay/handoff.d4h (a hand-off)")
            for sub in ("session", "sessions"):
                d = os.path.join(netplay, sub)
                if os.path.isdir(d) and any(files or dirs for _, dirs, files in os.walk(d)):
                    self.problems.append(f"the refused {s.name} copy has files under ud/netplay/{sub}")

    def compare(self, host, joiner):
        """(exit code, output lines, first differing digest frame or None) of np_compare.py on the two session folders."""
        p = subprocess.run([sys.executable, "-I", os.path.join(HERE, "np_compare.py"), host.session_dir(), joiner.session_dir(),
                            "--allow-unchecked", "--blocks", "0"], capture_output=True, text=True, creationflags=CREATE_NO_WINDOW)
        out = (p.stdout + p.stderr).splitlines()
        first = None
        for l in out:
            m = re.match(r"digest first difference: frame (\d+)", l)
            if m:
                first = int(m.group(1))
        return p.returncode, out, first

    def judge_session(self, lines, host, joiner):
        a = self.a
        expect = a.expect
        for s in (host, joiner):
            missing = [n for n in ("np_log.txt", "digest.d4d", "sweep.d4s", "counters.d4k")
                       if not os.path.exists(os.path.join(s.session_dir(), n))]
            if not os.path.exists(os.path.join(s.session_dir(), "np_log.txt")):
                self.problems.append(f"the {s.name} copy did not run (no session folder with an np_log.txt: {s.session_dir()})")
                return
            lines.append(f"{s.name}: terminal {s.terminal()} checksums {s.checksums()} missing {missing}")
        self.frame0_check(lines, host, joiner, fail=expect == "ok")
        th, tj = host.terminal(), joiner.terminal()
        if expect == "lost":
            self.judge_lost(lines, host, joiner, th, tj)
            return
        for s, t in ((host, th), (joiner, tj)):
            if t is None:
                self.problems.append(f"the {s.name} copy's np_log.txt has no 'session ended at frame' line")
        if th is None or tj is None:
            return
        if expect == "desync":
            for s, t in ((host, th), (joiner, tj)):
                if not t[1].startswith("desync at frame"):
                    self.problems.append(f"the {s.name} copy ended {t[1]!r}, expected a desync")
            code, out, first = self.compare(host, joiner)
            lines.append(f"np_compare first differing digest frame: {first}")
            if first is None:
                self.problems.append("expected a desync but the two digests never differ (np_compare: digest first difference: none)")
            return
        # expect ok
        for s, t in ((host, th), (joiner, tj)):
            if t[1] not in OK_REASONS:
                self.problems.append(f"the {s.name} copy ended {t[1]!r}, expected a clean leave ({' or '.join(OK_REASONS)})")
        if th[0] != tj[0]:
            self.problems.append(f"the sessions ended at different frames: host {th[0]}, joiner {tj[0]}")
        quit_frame = parse_script_quit(a.script)
        need = quit_frame if quit_frame is not None else 1
        sizes = {}
        for s in (host, joiner):
            for n in ("digest.d4d", "sweep.d4s", "counters.d4k"):
                if not os.path.exists(os.path.join(s.session_dir(), n)):
                    self.problems.append(f"the {s.name} copy wrote no {n}")
            path = os.path.join(s.session_dir(), "digest.d4d")
            sizes[s.name] = len(np_compare.read_digest(path)[1]) if os.path.exists(path) else 0
            if sizes[s.name] < need:
                self.problems.append(f"the {s.name} digest has {sizes[s.name]} frames, fewer than the script's quit frame {need}")
            warn = next((l for l in s.np_log() if l.startswith(np_run.WARN_PREFIX)), None)
            if warn:
                self.problems.append(f"the {s.name} np_log holds a WARN line: {warn}")
            n = s.checksums()
            need_n = (s.terminal()[0] if s.terminal() else 0) - self.delay_host - 1  # both copies run the host's delay
            if n is None:
                self.problems.append(f"the {s.name} np_log has no 'checksums handed to GekkoNet' line (the desync detector was not fed)")
            elif n < need_n:
                self.problems.append(f"the {s.name} handed {n} checksums to GekkoNet, fewer than end frame - delay - 1 = {need_n}")
        if sizes["host"] != sizes["joiner"]:
            self.problems.append(f"the digests have different lengths: host {sizes['host']} frames, joiner {sizes['joiner']}")
        code, out, first = self.compare(host, joiner)
        lines.append(f"np_compare exit {code}, first differing digest frame {first}")
        if first is not None:
            self.problems.append(f"differ at frame {first}")
        elif code != 0:
            self.problems.append("np_compare failed: " + " | ".join(l.strip() for l in out if l.strip())[:600])

    def judge_lost(self, lines, host, joiner, th, tj):
        a = self.a
        if a.kill_joiner_at_frame is None:
            self.problems.append("--expect lost needs --kill-joiner-at-frame")
            return
        if th is None or th[1] != "lost":
            self.problems.append(f"the surviving host copy ended {th[1] if th else None!r}, expected 'lost'")
        events = {}
        for l in read_lines(os.path.join(host.session_dir(), "events.txt")):
            m = re.match(r"^(\d+) (hold begin frame \d+|banner waiting|session ended\b.*)$", l)
            if m:
                events.setdefault(m.group(2).split(" ")[0], int(m.group(1)))
        hold, banner, ended = events.get("hold"), events.get("banner"), events.get("session")
        lines.append(f"events: hold {hold} banner {banner} ended {ended}")
        if hold is None or banner is None or ended is None:
            self.problems.append("events.txt lacks hold begin / banner waiting / session ended lines")
            return
        if abs((banner - hold) - HOLD_TO_BANNER_MS) > TIMING_SLACK_MS:
            self.problems.append(f"the banner came {banner - hold} ms after the hold began, expected {HOLD_TO_BANNER_MS} +-{TIMING_SLACK_MS}")
        if abs((ended - hold) - HOLD_TO_END_MS) > TIMING_SLACK_MS:
            self.problems.append(f"the session ended {ended - hold} ms after the hold began, expected {HOLD_TO_END_MS} +-{TIMING_SLACK_MS}")

    # ------------------------------------------------------------------ result
    def verdict(self):
        a = self.a
        lines = self.judge()
        total = 0
        for s in self.sides:
            after = hash_saves(s.ud)
            total += len(s.saves_before)
            if not s.saves_before:
                self.problems.append(f"the {s.name} copy has no files under ud/{SAVE_DIR}: the unchanged-save check would be vacuous")
            changed = sorted(k for k in set(after) | set(s.saves_before) if after.get(k) != s.saves_before.get(k))
            if changed:
                self.problems.append(f"the {s.name} copy's real save changed: {', '.join(changed)}")
        ok = not self.problems
        out = [f"pair {a.name} expect {a.expect}"] + [f"note: {n}" for n in self.notes] + lines
        if ok:
            out.append(f"{total} save files hashed on {len(self.sides)} copies, real saves unchanged")
        out += [f"problem: {p}" for p in self.problems]
        out.append(f"verdict {a.expect}" if ok else "verdict FAILED: " + "; ".join(self.problems))
        path = os.path.join(a.runs, f"{a.name}_pair.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
        for s in self.sides:
            with open(os.path.join(s.run, "run_result.txt"), "w") as f:
                f.write(f"status {'ok' if ok else 'failed'}\nterminal {s.terminal()}\n")
        print("\n".join(out), flush=True)
        return ok


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--script", required=True)
    ap.add_argument("--userdata", required=True, help="pristine user data folder (copied for every copy, never written)")
    ap.add_argument("--runs", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--joiner-exe")
    ap.add_argument("--cache")
    ap.add_argument("--game", default=r"C:\Users\fsop1\doa4\xex\DOA4")
    ap.add_argument("--joiner-affinity", help="logical CPU mask for the joiner, e.g. 0x3")
    ap.add_argument("--sim", help="DOA4_NP_SIM on both sides: delay_ms,jitter_ms,loss_pct")
    ap.add_argument("--delay", type=int)
    ap.add_argument("--seed", type=int)
    ap.add_argument("--env-host", action="append", default=[])
    ap.add_argument("--env-joiner", action="append", default=[])
    ap.add_argument("--arg-host", action="append", default=[])
    ap.add_argument("--arg-joiner", action="append", default=[])
    ap.add_argument("--kill-joiner-at-frame", type=int)
    ap.add_argument("--third-joiner", action="store_true")
    ap.add_argument("--host-only", type=float, metavar="SECONDS")
    ap.add_argument("--expect", choices=EXPECTS, default="ok")
    ap.add_argument("--timeout", type=float, default=600, help="seconds the sessions may take")
    ap.add_argument("--end-grace", type=float, default=15, help="seconds a close request gets before /F")
    ap.add_argument("--step-timeout", type=float, default=60, help="seconds each lobby step may take")
    ap.add_argument("--settle", type=float, default=8, help="seconds a refused run is watched for a session that starts anyway")
    ap.add_argument("--orphan-watch", type=float, default=10, help="seconds pids.txt must stay unchanged after the last kill")
    a = ap.parse_args(argv)
    if a.host_only is not None and a.expect != "lobby":
        sys.exit("--host-only goes with --expect lobby")
    if a.expect == "lobby" and a.host_only is None:
        sys.exit("--expect lobby needs --host-only SECONDS")
    if a.joiner_affinity:
        try:
            int(a.joiner_affinity, 0)
        except ValueError:
            sys.exit(f"--joiner-affinity {a.joiner_affinity!r}: not a number (e.g. 0x3)")
    os.makedirs(a.runs, exist_ok=True)
    pair = Pair(a)
    ok = False
    try:
        try:
            pair.run()
        except StepError as e:
            pair.problems.append(str(e))
        forced = pair.end_all()
        if forced:
            pair.notes.append(f"needed /F after {a.end_grace} s: {', '.join(sorted(forced))}")
        ok = pair.verdict()
    finally:
        if not pair.ended:
            pair.end_all()
        for s in pair.sides:
            if s.stdout:
                s.stdout.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

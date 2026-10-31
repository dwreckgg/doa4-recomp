"""The online mode of the stand-in game (tests/tools/fake_game.py hands over to online_main() when a start has
DOA4_NP_TEST_DIR and no DOA4_NP). It is one copy of an online pair; np_pair.py starts two of them (a third for the busy
refusal) and the tool tests judge its verdicts. Nothing of the real game, its data or the network is touched: no socket is
opened.

The contract it speaks (plan 1a Task 9, Task 10 "Interfaces"; the game implements the same):
  DOA4_NP_TEST_DIR=<run>/ctl   np_ctl.on  polled (here every 0.1 s), each line run, the file taken (deleted):
                                 host [port] | join <ip:port> | reach <code> | start | disconnect | delay auto|<n> | status
                               status.txt rewritten after each command and every second (temp file, then rename):
                                 state=<offline|hosting|connecting|connected|starting|restarting|in_session>, port, peer_port,
                                 ping, delay, can_start, event
                               pids.txt  "pid <n> <lobby|session|offline> <unix ms>", appended by the process itself
                               session/  np_log.txt, digest.d4d, sweep.d4s, counters.d4k, events.txt
  The processes hand over like the real game's restarts: lobby -> (start) -> session -> offline relaunch. Each is a new
  process that appends its own pids.txt line and starts the next one (a child, in the first process's job) before it exits.
  A copy finds its peer through the run folders beside its own (<runs>/*/ctl/status.txt carries the port).
  DOA4_NP_LOCAL_SCRIPT=<file>  only its quit frame is used (np_core.cpp ParseScript: a line `<n> <pads>` holds n frames,
                               `random <n> ...` n frames, connect/shot none); no quit line: DEFAULT_FRAMES frames.
Additions the stand-in makes to the contract (np_pair.py needs them; Task 12 implements them in the game):
  * np_log.txt holds a line `frame <F>: <text>` as the session passes frame F (np_pair --kill-joiner-at-frame reads it);
  * np_log.txt holds `session on: ...` first; the terminal line is `session ended at frame <F>: <reason>`, the next line
    `checksums handed to GekkoNet: <N>`.
Switches of the stand-in itself (FAKE_ONLINE_*, per copy, in its environment):
  DIVERGE=<f>       this copy's digest rows differ from frame f on
  END=<reason>      the terminal reason (default `left`): left | peer left | lost | desync | frame 0 timeout | internal: <text>
                    `desync` names the first differing frame (DIVERGE, else the last); `lost` makes this copy watch its peer's
                    session process: when it is gone without a terminal line in its np_log the copy holds (events.txt `hold begin frame F`), 1.0 s later
                    `banner waiting`, and 10.0 s after the hold began ends with `lost`
  NO_SESSION=1      the session process starts (pids.txt line) but writes no session folder
  REFUSE=build|files|busy   this copy's join is refused with that reason (the host is not asked)
  LEAKY=1           with REFUSE: the refusal is reported, and LEAK_S (default 0.5) seconds later a session process starts
                    anyway (a refusal that did not hold)
  HANDOFF=1         with REFUSE: the refused copy writes ud/netplay/handoff.d4h at once (a hand-off, no session)
  NO_REFUSAL_KEY=1  status.txt carries no `refusal=` line
  RELAUNCH=1        the offline relaunch is relaunched once more, 1 s after it was ended (a game that relaunches itself after a
                    close request); LINGER bounds the life of the relaunched process
  BANNER_S=<s>      lost: the banner comes this long after the hold began (default 1.0)
  WRITE_SAVE=1      the session appends one byte to a file under <user_data_root>/B13EBABEBABEBABE
  FALLBACK=1 / WARN=1   fb_main_wait is 1 from frame 4 on / np_log holds a WARN line
  NO_CHECKSUMS=1    no `checksums handed to GekkoNet` line     CHECKSUMS=<n>   that count instead of end frame - delay
  QUIT_AT=<n>       the session ends at frame n whatever the script says
  FRAME_MS=<n>      n ms of wall clock per frame (default 0): a session others can watch and end while it runs
  LINGER=<s>        the offline relaunch lives this long (default 60)
  ROLE=decoy        (the tests) only logs its pid into its ctl/pids.txt and sleeps 120 s: a stand-in the harness does not own
  DUMP0_DIFF / HANDLE_SHIFT / STASH   the frame-0 dump this copy writes when DOA4_NP_DUMP lists 0 (write_dump0, Task 15 carry b)
  NO_DUMP=1         no frame-0 dump even when DOA4_NP_DUMP lists 0
  BOOT_HOLD=1       the session starts with the real game's frame-0 hold (hold begin frame 0, banner 1.0 s later, hold end)
  LEFT_AT=<f>       after frame f the session ends `left` (a graceful quit), starts no offline relaunch and lingers until ended
  LOST_ANY=1        with END=lost: a peer whose session process is gone counts as vanished even if it wrote a terminal line
  SKIP_PEER_CHECK=1 the session does not look for a lobby-only peer (plays its frames)   NO_OFFLINE=1  after the lobby-only
                    peer's end, no offline relaunch
The game's own test switch DOA4_NP_TEST_NO_RESTART=1 (Task 15 carry f) is honoured as the game does: the joiner says yes and
stays a lobby (state=restarting, refusal=session); the host's session then ends at frame 0 with `internal: the other player's
game did not start the session` and relaunches offline.
status.txt carries `refusal=<none|build|files|busy|proto|session|other>` (Task 9's contract): the kind of the join's refusal.
The event texts are the REAL ones (np_rules.cpp of wt-np1 66ba37d), so a harness judging by keywords fails here as on the game.
A host refuses a join with `refuse build` (build ids differ: BUILD_ID, +1 with DOA4_NP_TEST_BUILD_SALT=1), `refuse files <group>`
(DOA4_NP_TEST_FILES_SALT differs) or `refuse busy` (it already has a peer).
"""
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # py -3 -I does not put the script's folder on the path
import fake_game  # noqa: E402

CREATE_NO_WINDOW = 0x08000000
DEFAULT_FRAMES = 12
PORT_LOW, PORT_HIGH = 7200, 7215
BUILD_ID = 1000


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
    out = {}
    try:
        for line in read_text(path).splitlines():
            k, sep, v = line.partition("=")
            if sep:
                out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def retry(fn, tries=100):
    """fn() again after a PermissionError (a reader or a virus scanner has the file open for a moment)."""
    for i in range(tries):
        try:
            return fn()
        except PermissionError:
            if i == tries - 1:
                raise
            time.sleep(0.02)


def atomic_write(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    retry(lambda: os.replace(tmp, path))


def take(path):
    """The text of a request file, which is deleted (taken); None if there is none."""
    try:
        text = read_text(path)
    except OSError:
        return None
    retry(lambda: os.remove(path))
    return text


def script_quit_frame(path):
    """The frames before the script's first `quit` line; None: no readable script or no quit line."""
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except (OSError, TypeError):
        return None
    frames = 0
    for line in text.splitlines():
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


def pid_alive(pid):
    import ctypes
    k32 = ctypes.windll.kernel32
    k32.OpenProcess.restype = ctypes.c_void_p
    k32.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong()
    k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return code.value == 259  # STILL_ACTIVE


def note_pid(ctl, kind):
    with open(os.path.join(ctl, "pids.txt"), "a") as f:
        f.write(f"pid {os.getpid()} {kind} {int(time.time() * 1000)}\n")


def last_session_pid(ctl):
    pid = None
    try:
        for line in read_text(os.path.join(ctl, "pids.txt")).splitlines():
            w = line.split()
            if len(w) >= 3 and w[0] == "pid" and w[2] == "session":
                pid = int(w[1])
    except (OSError, ValueError):
        pass
    return pid


def spawn_next(role, extra_env):
    """Starts the next process of the hand-over (same arguments) and returns; it outlives this one."""
    env = dict(os.environ, FAKE_ONLINE_ROLE=role, **extra_env)
    subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "fake_game.py"), *sys.argv[1:]], env=env,
                     creationflags=CREATE_NO_WINDOW, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)


def my_files_salt():
    return os.environ.get("DOA4_NP_TEST_FILES_SALT", "")


def my_build_id():
    return BUILD_ID + (1 if os.environ.get("DOA4_NP_TEST_BUILD_SALT") == "1" else 0)


def refusal_text(kind, group=""):
    """The lobby's real texts. build: MismatchAdvice, np_rules.cpp:551-566 (the "self-built exe" variant; the others: "...they need
    the newest netplay release...", "...the other player runs a self-built exe...", "You have an older release (...)", "The other
    player has an older release (...)", "Both games say the same version (...) but the builds differ: ..."); files: Judge,
    np_rules.cpp:612-617; busy: Judge, np_rules.cpp:591."""
    return {"build": "You run a self-built exe: both of you should use the latest netplay release (press Check for Updates).",
            "files": f"The game files differ ({group or 'disc'}): both players need the same game files.",
            "busy": "The host is already in an online session."}[kind]


class State:
    def __init__(self, ctl):
        self.ctl = ctl
        self.state = "offline"
        self.port = 0
        self.peer_port = 0
        self.delay = int(os.environ.get("DOA4_NP_DELAY", "") or 2)
        self.can_start = 0
        self.event = ""
        self.refusal = "none"
        self.peer_ctl = None
        self.leak_at = None
        self.runs = os.path.dirname(os.path.dirname(os.path.normpath(ctl)))

    def write(self):
        atomic_write(os.path.join(self.ctl, "status.txt"),
                     f"state={self.state}\nport={self.port}\npeer_port={self.peer_port}\nping=1\ndelay={self.delay}\n"
                     f"can_start={self.can_start}\nevent={self.event}\n"
                     + ("" if os.environ.get("FAKE_ONLINE_NO_REFUSAL_KEY") == "1" else f"refusal={self.refusal}\n"))

    def ctls(self):
        found = []
        for d in sorted(os.listdir(self.runs)) if os.path.isdir(self.runs) else []:
            if os.path.isdir(os.path.join(self.runs, d, "ctl")):
                found.append(os.path.join(self.runs, d, "ctl"))
        return found

    def free_port(self):
        used = {int(kv["port"]) for kv in (read_kv(os.path.join(c, "status.txt")) for c in self.ctls())
                if kv.get("port", "").isdigit()}
        return next((p for p in range(PORT_LOW, PORT_HIGH + 1) if p not in used), PORT_HIGH)

    def find_host(self, port):
        for c in self.ctls():
            kv = read_kv(os.path.join(c, "status.txt"))
            if kv.get("port") == str(port) and kv.get("state") in ("hosting", "connected") \
                    and os.path.normpath(c) != os.path.normpath(self.ctl):
                return c
        return None


def command(st, line):
    """Runs one np_ctl.on line; returns "start" when the session is to start."""
    words = line.split()
    if not words:
        return None
    cmd = words[0].lower()
    if cmd == "host" and st.state == "offline":
        st.port = int(words[1]) if len(words) > 1 and words[1].isdigit() else st.free_port()
        st.state, st.event = "hosting", "hosting"
    elif cmd == "join" and len(words) > 1 and st.state == "offline":
        port = int(words[1].rsplit(":", 1)[-1])
        st.port = st.free_port()
        refuse = os.environ.get("FAKE_ONLINE_REFUSE")
        host = None if refuse else st.find_host(port)
        if refuse:
            st.state, st.event, st.refusal = "offline", refusal_text(refuse, my_files_salt()), refuse
            if os.environ.get("FAKE_ONLINE_LEAKY") == "1":
                st.leak_at = time.time() + float(os.environ.get("FAKE_ONLINE_LEAK_S", "0.5"))
            ud = fake_game.arg_value("--user_data_root=")
            os.makedirs(os.path.join(ud, "netplay"), exist_ok=True)
            with open(os.path.join(ud, "netplay", "events.txt"), "a") as f:  # the real lobby always writes its events here
                f.write(f"{int(time.time() * 1000)} refused: {st.event}\n")
            if os.environ.get("FAKE_ONLINE_HANDOFF") == "1":
                with open(os.path.join(ud, "netplay", "handoff.d4h"), "wb") as f:
                    f.write(b"handoff")
        elif host is None:
            st.state, st.event = "offline", f"no host at port {port}"
        else:
            atomic_write(os.path.join(host, f"join_{os.getpid()}.req"),
                         f"build={my_build_id()}\nfiles={my_files_salt()}\nctl={st.ctl}\nport={st.port}\n")
            st.state, st.event, st.peer_ctl, st.peer_port = "connecting", "joining", host, port
    elif cmd == "disconnect":
        st.state, st.can_start, st.peer_port, st.peer_ctl, st.event, st.refusal = "offline", 0, 0, None, "disconnected", "none"
    elif cmd == "delay" and len(words) > 1 and words[1].isdigit():
        st.delay = int(words[1])
    elif cmd == "start" and st.state == "connected" and st.can_start:
        return "start"
    return None  # reach, status, delay auto: only the status rewrite


def host_poll(st):
    """The host's side of a join: each join_*.req is answered (accept, refuse build, refuse files <group>, refuse busy)."""
    for name in sorted(os.listdir(st.ctl)):
        if not (name.startswith("join_") and name.endswith(".req")):
            continue
        req = read_kv(os.path.join(st.ctl, name))
        retry(lambda: os.remove(os.path.join(st.ctl, name)))
        if st.peer_ctl is not None:
            reply = "refuse busy"
        elif req.get("build") != str(my_build_id()):
            reply = "refuse build"
        elif req.get("files", "") != my_files_salt():
            reply = "refuse files " + (req.get("files") or my_files_salt())
        else:
            reply = "accept"
            st.peer_ctl, st.peer_port, st.state, st.can_start, st.event = req["ctl"], int(req["port"]), "connected", 1, "peer joined"
        atomic_write(os.path.join(req["ctl"], "join_reply.txt"), reply + "\n")


def joiner_poll(st):
    reply = take(os.path.join(st.ctl, "join_reply.txt"))
    if reply is None:
        return
    words = reply.split()
    if words and words[0] == "accept":
        st.state, st.event = "connected", "joined"
    else:
        kind = words[1] if len(words) > 1 else "build"
        st.state, st.event, st.peer_ctl, st.peer_port = "offline", refusal_text(kind, " ".join(words[2:])), None, 0
        st.refusal = kind


def lobby(ctl):
    note_pid(ctl, "lobby")
    st = State(ctl)
    st.write()
    last = 0.0
    while True:
        text = take(os.path.join(ctl, "np_ctl.on"))
        go = False
        if text is not None:
            for line in text.splitlines():
                go = command(st, line) == "start" or go
            st.write()
        if st.state in ("hosting", "connected"):
            host_poll(st)
        if st.state == "connecting":
            joiner_poll(st)
        if st.state == "connected":
            note = take(os.path.join(ctl, "start.now"))
            if note is not None:
                go = True
                kv = dict(l.partition("=")[::2] for l in note.splitlines())
                if kv.get("delay", "").isdigit():
                    st.delay = int(kv["delay"])  # both copies run the host's delay
        if st.leak_at is not None and time.time() >= st.leak_at:
            go = True
        if go and os.environ.get("DOA4_NP_TEST_NO_RESTART") == "1" and not st.can_start:
            # the game's lobby-only peer (Task 15 carry f): the joiner said yes but stays a lobby (state restarting); the
            # host's session Hello then reaches a lobby, which answers "not in the session" (refusal=session)
            st.state, st.event, st.refusal = "restarting", "The host started the online session: restarting...", "session"
            st.write()
            go = False
        if go:
            if st.peer_ctl and st.can_start:  # the host tells its peer
                atomic_write(os.path.join(st.peer_ctl, "start.now"), f"go\ndelay={st.delay}\n")
            st.state, st.event = "starting", "starting"
            st.write()
            spawn_next("session", {"FAKE_ONLINE_PEER": st.peer_ctl or "", "FAKE_ONLINE_DELAY": str(st.delay)})
            return 0
        if time.time() - last >= 0.25:
            st.write()
            last = time.time()
        time.sleep(0.1)


def write_digest(path, rows, diverge):
    with open(path, "wb") as f:
        f.write(b"D4NPDIG1" + struct.pack("<II", 1, len(fake_game.REGIONS)))
        for name, addr, length in fake_game.REGIONS:
            f.write(struct.pack("<II", addr, length) + name.encode().ljust(24, bytes(1)))
        for i in range(rows):
            bad = 7 if diverge is not None and i >= diverge else 0
            f.write(struct.pack("<II", i, 0) + struct.pack(f"<{len(fake_game.REGIONS)}Q", 1000 + i + bad, 2000 + i))


DUMP_LO = 0x82B70000
DUMP_BYTES = 0x30000  # three 64 KB sweep blocks (the game's dump is 0x82B70000..0x835B0000; the tools take any length)
SHIFT_AT = 0x82B80000  # FAKE_ONLINE_HANDLE_SHIFT: the guest handles stored here (one per 16 bytes, 12 of them)
STASH_AT = 0x82B90000  # a "REX\0" + host handle stash pair present in both dumps (FAKE_ONLINE_STASH: another handle)


def write_dump0(path):
    """The frame-0 .data dump (dumps/data_000000.bin, guest DUMP_LO..), the same bytes on every copy unless:
    FAKE_ONLINE_DUMP0_DIFF=<hex guest address>  that byte is flipped;
    FAKE_ONLINE_HANDLE_SHIFT=<n>                the 12 handles F80000xx at SHIFT_AT are n higher (the debug-82B70000 shape);
    FAKE_ONLINE_STASH=<hex handle>              the stash word after the signature at STASH_AT holds that handle."""
    data = bytearray((i * 7 + 3) & 0xFF for i in range(DUMP_BYTES))
    shift = int(os.environ.get("FAKE_ONLINE_HANDLE_SHIFT", "0") or 0)
    for k in range(12):
        struct.pack_into(">I", data, SHIFT_AT - DUMP_LO + 16 * k, 0xF8000100 + 4 * k + shift)
    stash = int(os.environ.get("FAKE_ONLINE_STASH", "F8000054"), 16)
    data[STASH_AT - DUMP_LO:STASH_AT - DUMP_LO + 8] = b"REX\x00" + struct.pack(">I", stash)
    flip = os.environ.get("FAKE_ONLINE_DUMP0_DIFF")
    if flip:
        data[int(flip, 16) - DUMP_LO] ^= 0x5A
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def peer_stayed_lobby(peer, limit=3.0):
    """True when the peer copy answered as a lobby (its status.txt refusal=session) and has no session process; False as soon
    as the peer is starting / in a session (or after `limit` seconds)."""
    end = time.time() + limit
    while time.time() < end:
        kv = read_kv(os.path.join(peer, "status.txt"))
        if last_session_pid(peer) is not None or kv.get("state") in ("starting", "in_session"):
            return False
        if kv.get("refusal") == "session":
            return True
        time.sleep(0.05)
    return False


def session(ctl):
    note_pid(ctl, "session")
    delay = int(os.environ.get("FAKE_ONLINE_DELAY", "2"))
    peer = os.environ.get("FAKE_ONLINE_PEER") or None
    atomic_write(os.path.join(ctl, "status.txt"),
                 f"state=in_session\nport=0\npeer_port=0\nping=1\ndelay={delay}\ncan_start=0\nevent=in session\n")
    ud = fake_game.arg_value("--user_data_root=")
    if os.environ.get("FAKE_ONLINE_WRITE_SAVE") == "1" and ud:
        for dirpath, _, files in os.walk(os.path.join(ud, "B13EBABEBABEBABE")):
            if files:
                with open(os.path.join(dirpath, sorted(files)[0]), "ab") as f:
                    f.write(b"X")
                break
    if os.environ.get("FAKE_ONLINE_NO_SESSION") == "1":
        spawn_next("offline", {})
        return 0
    end = os.environ.get("FAKE_ONLINE_END", "left")
    quit_at = os.environ.get("FAKE_ONLINE_QUIT_AT")
    quit_frame = int(quit_at) if quit_at else script_quit_frame(os.environ.get("DOA4_NP_LOCAL_SCRIPT"))
    rows = quit_frame if quit_frame is not None else DEFAULT_FRAMES
    pace = int(os.environ.get("FAKE_ONLINE_FRAME_MS", "0")) / 1000.0
    diverge = os.environ.get("FAKE_ONLINE_DIVERGE")
    diverge = int(diverge) if diverge else None
    sess = os.path.join(ctl, "session")
    os.makedirs(sess, exist_ok=True)
    log = open(os.path.join(sess, "np_log.txt"), "a", buffering=1)
    events = open(os.path.join(sess, "events.txt"), "a", buffering=1)

    def event(text):
        events.write(f"{int(time.time() * 1000)} {text}\n")

    log.write(f"session on: online stand-in, {rows} frames, delay {delay}\n")
    if os.environ.get("FAKE_ONLINE_WARN") == "1":
        log.write(fake_game.KEEPALIVE_WARN + "\n")
    if peer and os.environ.get("FAKE_ONLINE_SKIP_PEER_CHECK") != "1" and peer_stayed_lobby(peer):
        # the real session's session_refused poll (np_session.cpp)
        log.write("the other player's game answered as a lobby: it did not start the session (ending at once)\n")
        reason = "internal: the other player's game did not start the session"
        event(f"session ended at frame 0: {reason}")
        log.write(f"session ended at frame 0: {reason}\nchecksums handed to GekkoNet: 0\n")
        log.close()
        events.close()
        if os.environ.get("FAKE_ONLINE_NO_OFFLINE") != "1":
            spawn_next("offline", {})
        return 0
    if "0" in os.environ.get("DOA4_NP_DUMP", "").split(",") and os.environ.get("FAKE_ONLINE_NO_DUMP") != "1":
        write_dump0(os.path.join(sess, "dumps", "data_000000.bin"))
    if os.environ.get("FAKE_ONLINE_BOOT_HOLD") == "1":  # the real session's frame-0 hold (both copies start), with its banner
        t0 = time.time()
        event("hold begin frame 0")
        time.sleep(max(0.0, t0 + 1.0 - time.time()))
        event("banner waiting")
        time.sleep(0.05)
        event(f"hold end frame 0 after {int((time.time() - t0) * 1000)} ms")
    left_at = os.environ.get("FAKE_ONLINE_LEFT_AT")
    left_at = int(left_at) if left_at else None
    frame, lost, peer_seen = 0, False, False
    while frame < rows:
        if end == "lost" and peer:
            pid = last_session_pid(peer)
            peer_seen = peer_seen or pid is not None
            if peer_seen and pid is not None and not pid_alive(pid):
                # gone without a terminal line: a vanished peer (a peer that ended its session cleanly is not lost)
                ended = any(l.startswith("session ended at frame") for l in
                            read_text(os.path.join(peer, "session", "np_log.txt")).splitlines())                     if os.path.exists(os.path.join(peer, "session", "np_log.txt")) else False
                if not ended or os.environ.get("FAKE_ONLINE_LOST_ANY") == "1":
                    lost = True
                    break
        log.write(f"frame {frame}: ok\n")
        if left_at is not None and frame >= left_at:
            # the real game's graceful quit (a close request in a session): `left`, no Leave sent, no offline relaunch;
            # the process lingers until it is ended
            write_digest(os.path.join(sess, "digest.d4d"), frame + 1, diverge)
            event(f"session ended at frame {frame + 1}: left")
            log.write(f"session ended at frame {frame + 1}: left\nchecksums handed to GekkoNet: {max(0, frame + 1 - delay)}\n")
            log.close()
            events.close()
            time.sleep(120)
            return 0
        frame += 1
        if pace:
            time.sleep(pace)
    reason = end
    if lost:
        t0 = time.time()
        event(f"hold begin frame {frame}")
        time.sleep(max(0.0, t0 + float(os.environ.get("FAKE_ONLINE_BANNER_S", "1.0")) - time.time()))
        event("banner waiting")
        time.sleep(max(0.0, t0 + 10.0 - time.time()))
        reason = "lost"
    elif end == "desync":
        first = diverge if diverge is not None else max(0, rows - 1)
        reason = f"desync at frame {first} (this side 0000a1b2c3d4e5f6, other side 0000f6e5d4c3b2a1)"
    elif end == "lost":
        reason = "left"  # nobody went away: an ordinary end
    write_digest(os.path.join(sess, "digest.d4d"), frame, diverge)
    fake_game.write_sweep(os.path.join(sess, "sweep.d4s"), frame)
    fake_game.write_counters(os.path.join(sess, "counters.d4k"), frame, os.environ.get("FAKE_ONLINE_FALLBACK") == "1", False)
    event(f"session ended at frame {frame}: {reason}")
    log.write(f"session ended at frame {frame}: {reason}\n")
    if os.environ.get("FAKE_ONLINE_NO_CHECKSUMS") != "1":
        count = os.environ.get("FAKE_ONLINE_CHECKSUMS")
        log.write(f"checksums handed to GekkoNet: {int(count) if count else max(0, frame - delay)}\n")
    log.close()
    events.close()
    spawn_next("offline", {})
    return 0


def guard(parent_pid, left):
    """RELAUNCH: waits for the offline process to be ended, then starts another one (a game that relaunches itself)."""
    while pid_alive(parent_pid):
        time.sleep(0.05)
    time.sleep(1.0)
    spawn_next("offline", {"FAKE_ONLINE_RELAUNCH_LEFT": str(left - 1)})


def offline(ctl):
    note_pid(ctl, "offline")
    left = int(os.environ.get("FAKE_ONLINE_RELAUNCH_LEFT", "1" if os.environ.get("FAKE_ONLINE_RELAUNCH") == "1" else "0"))
    if left > 0:
        spawn_next("guard", {"FAKE_ONLINE_PARENT": str(os.getpid()), "FAKE_ONLINE_RELAUNCH_LEFT": str(left)})
    atomic_write(os.path.join(ctl, "status.txt"),
                 "state=offline\nport=0\npeer_port=0\nping=0\ndelay=0\ncan_start=0\nevent=back offline\n")
    deadline = time.time() + float(os.environ.get("FAKE_ONLINE_LINGER", "60"))
    while time.time() < deadline:
        time.sleep(0.2)
    return 0


def online_main():
    ctl = os.environ["DOA4_NP_TEST_DIR"]
    os.makedirs(ctl, exist_ok=True)
    role = os.environ.get("FAKE_ONLINE_ROLE", "lobby")
    if "FAKE_ONLINE_ROLE" not in os.environ or role == "decoy":
        fake_game.write_startup_note()  # the process the tool launched (the later ones are this stand-in's own children)
    fake_game.write_game_log_line()
    if role == "decoy":
        note_pid(ctl, "lobby")
        time.sleep(120)
        return 0
    try:
        if role == "guard":
            return guard(int(os.environ["FAKE_ONLINE_PARENT"]), int(os.environ["FAKE_ONLINE_RELAUNCH_LEFT"]))
        return {"lobby": lobby, "session": session, "offline": offline}[role](ctl)
    except BaseException:  # a stand-in that dies leaves its traceback beside its pids.txt (its output is discarded)
        import traceback
        with open(os.path.join(ctl, f"standin_error_{role}.txt"), "a") as f:
            traceback.print_exc(file=f)
        raise

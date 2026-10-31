"""A stand-in for dead_or_alive_4.exe in the tool tests (tests/tools/test_np_tools.py). It speaks the
run contract of src/net/np_session.cpp, which is what np_run.py, np_compare.py and np_gate.py read,
without the game: nothing of the real game or its data is touched.

Reads DOA4_NP (record|replay), DOA4_NP_DIR (the run's out folder) and, for a replay, DOA4_NP_REC.
Writes digest.d4d, sweep.d4s, counters.d4k and np_log.txt into the out folder, and for a record run
rec.d4r (here just the frame count as text). Endings, as the real session logs them:
  record N frames: N digest rows, "quit at frame N-1: quit frame"
  replay of N frames: N digest rows, "quit at frame N: end of the recording"
Test-only environment (the stand-in's own, not the game's):
  FAKE_GAME_FRAMES  frames a record run plays (default 12)
  FAKE_GAME_FAIL    comma-separated run-name suffixes (the run folder's name ends with one of them):
                    that run crashes halfway - partial digest, no quit line, exit code 3
  FAKE_GAME_SHORT   same form: that replay ends one frame early but cleanly (exit 0, the right
                    quit reason, one digest row short)
  FAKE_GAME_FALLBACK  same form: that run reports a fallback - fb_main_wait is 1 from frame 4 on -
                    and otherwise ends cleanly
  FAKE_GAME_PROGRESS  same form: that run's pr_cri_passes differs from every other run's at frame 6
                    only (+1 there)
  FAKE_GAME_PROGRESS_END  same form: that run's pr_notify differs from every other run's at its last
                    counters row only (+1 there), so the runs END with different progress values
  FAKE_GAME_SWEEP   same form: that run's sweep row of frame 5 differs from every other run's (its
                    first block, 0x82C10000) and otherwise ends cleanly
  FAKE_GAME_REFUSE  same form, replays: that replay refuses its recording as the real session does
                    (np_session.cpp Start): the replay line and a "refused:" line, no other file, exit 0
  FAKE_GAME_REFUSE_LEAKY  same form: the refusal that does not stop the session (the `return` after it
                    deleted): "refused:" line, then the digest, sweep and counters files, a "session on"
                    line and no quit line, exit 0
  FAKE_GAME_CANNOT  same form, replays: that replay cannot read its recording: one "cannot replay" line,
                    no other file, exit 0 (the host was asked to quit)
  FAKE_GAME_WARN    same form: that run logs the determinism layer's WARN line (np_det.cpp: the keepalive fix
                    met the D3D kick thread calling from an unexpected site, KEEPALIVE_WARN below) halfway, and
                    otherwise ends cleanly - equal hashes, no fallback, the good quit line, exit 0
  FAKE_GAME_HANG    same form: that run logs "session on" (no quit line, no digest), then sleeps HANG_SECONDS and
                    exits 0 without writing anything more: a hung game that the caller's timeout has to end
  FAKE_GAME_BADEXIT same form: that run ends with the good quit line and every file, but exits with code 5
  FAKE_GAME_WRONGQUIT  same form, replays: that replay ends cleanly (exit 0) with another quit reason than the end of the
                    recording ("quit at frame N: quit frame", the reason a record run or DOA4_NP_QUIT gives)
  FAKE_GAME_STARTFAIL  a comma-separated list of SUFFIX=CODE: a run whose name ends with SUFFIX exits at once with CODE (a
                    negative number is a Windows error status such as -1073741515 = 0xC0000135, a missing DLL), writing no
                    log and no file: a game that could not start
Self-test line (Task 10): every run with a --log_file writes the line the real game logs at start-up
("[doa4][LIFE] dot products: canonical NaN on") into that file, as np_run.py and np_pair.py require of a good run:
  FAKE_NO_SELFTEST=1  the line is left out; FAKE_SELFTEST_OFF=1  the line says OFF (<lane bits>) instead of on.
  FAKE_LAZY_POOL=1  one more line, the SDK's lazy-pool WARN with its tag "[netplay-lazy-pool]" (LAZY_POOL_LINE; Task 15 carry a).
Hidden launch (Task 10): with FAKE_STARTUP_NOTE=1 the first process of a run writes "show_window=<n> flags=<hex>" (its own
STARTUPINFO as read from the process the tool started: wShowWindow and dwFlags) into startup.txt next to its --user_data_root folder, so the tests see how the
tool started it (np_run.py and np_pair.py: 7 and flag 0x1, SW_SHOWMINNOACTIVE + STARTF_USESHOWWINDOW).
Online mode (a start with DOA4_NP_TEST_DIR and no DOA4_NP, np_pair.py's): fake_online.py.
Every run also logs where it was started and with what: "stand-in game: cwd <folder>", "stand-in game: args a|b|c",
one "stand-in game: env DOA4_X=value" line per DOA4_ variable in its environment and "stand-in game: ud yes a,b" (the
top-level names in its --user_data_root folder, "no" if there is none): the bundle's run_check.bat test reads them.
"stand-in game: console-window none|hidden|visible" says whether it sits in a console with a window a person would see (the
checks start everything without one). A replay reads its recording as the real format (np_core.h: "D4NPREC1", version 2, 32-byte header, meta text,
52-byte frames) or as the text file the stand-in's own record runs write (just the frame count).
Like the real runtime at startup (rexglue-sdk src/system/runtime.cpp: thread::EnableAffinityConfiguration sets
the process mask to the SYSTEM mask, result ignored) every run first tries to widen its own CPU mask, then
logs the mask it ends up with ("stand-in game: affinity 0x.."). That is how the tests see whether
np_run.py --affinity survives the game's own startup: a plain SetProcessAffinityMask on the suspended
process does not, a job object's affinity limit does. The stand-in is started behind fake_game.cmd.
Every run writes the same hashes and the same counters for the same frame, so runs of equal length
compare equal. Counters: the real file layout (np_core.h) with the real names (np_det.cpp, pinned by
test_counter_names_match_source); fb_* are 0, pr_* are a function of the frame.
"""
import os
import struct
import sys
import time

HANG_SECONDS = 8  # FAKE_GAME_HANG: long enough for a test's timeout of a few seconds, short enough that the orphan ends itself
REGIONS =[("game_obj", 0x8320D110, 0x63B0), ("frame_ctr", 0x831013E4, 4)]
SWEEP_PER = 2
COUNTERS = ["fb_worker_token", "fb_main_wait", "fb_unranked", "fb_loader_outside", "fb_loader_idle",
            "fb_audio_late", "fb_spin_cap", "fb_cri_ready", "fb_audio_start", "fb_reader", "pr_cri_passes",
            "pr_spin_steps", "pr_loader_jobs", "pr_cri_jobs", "pr_reseeds", "pr_notify"]
# The line np_det.cpp KeepAliveSkip logs, filled in as the real game would (pinned against the source's format
# string by test_keepalive_warn_text).
KEEPALIVE_WARN = ("WARN keep-alive: sub_8275AEE0 called on the D3D kick thread 8275BD18 from lr 82765D70, not from "
                  "8275BDAC (frame 6): run, not skipped; the fix's assumption does not hold")
# The session's last lines (np_det.cpp Stop) as a good run of the real game ends: the keep-alive and reader counts.
SUMMARY = ["keep-alive (on): 22 calls skipped on the D3D kick thread (sub_8275AD70 11, sub_8275AEE0 11), "
           "0 from another site", "reader (on): 3 file threads waited for, 0 fallbacks"]
SDL_PADS = "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"
FALLBACK_FRAME = 4
PROGRESS_FRAME = 6
SWEEP_FRAME = 5


def write_digest(path, rows):
    with open(path, "wb") as f:
        f.write(b"D4NPDIG1" + struct.pack("<II", 1, len(REGIONS)))
        for name, addr, length in REGIONS:
            f.write(struct.pack("<II", addr, length) + name.encode().ljust(24, bytes(1)))
        for i in range(rows):
            f.write(struct.pack("<II", i, 0) + struct.pack(f"<{len(REGIONS)}Q", 1000 + i, 2000 + i))


def write_sweep(path, rows, differ=False):
    with open(path, "wb") as f:
        f.write(b"D4NPSWP1" + struct.pack("<IIIIII", 1, 0x82B70000, 0x835B0000, 0x10000, SWEEP_PER, 0))
        for i in range(rows):
            first = 3000 + i + (1 if differ and i == SWEEP_FRAME else 0)
            f.write(struct.pack("<II", i, (i * SWEEP_PER) % 164) + struct.pack(f"<{SWEEP_PER}Q", first, 4000 + i))


def counter_values(frame, fallback, progress, bump_notify=False):
    values = []
    for i, name in enumerate(COUNTERS):
        if name.startswith("fb_"):
            value = 1 if fallback and name == "fb_main_wait" and frame >= FALLBACK_FRAME else 0
        else:
            value = (i + 1) * 100 + frame
            if progress and name == "pr_cri_passes" and frame == PROGRESS_FRAME:
                value += 1
            if bump_notify and name == "pr_notify":
                value += 1
        values.append(value)
    return values


def write_counters(path, rows, fallback, progress, progress_end=False):
    with open(path, "wb") as f:
        f.write(b"D4NPCNT1" + struct.pack("<II", 1, len(COUNTERS)))
        for name in COUNTERS:
            f.write(name.encode().ljust(24, bytes(1)))
        for i in range(rows):
            f.write(struct.pack("<II", i, 0) + struct.pack(f"<{len(COUNTERS)}I", *counter_values(i, fallback, progress, progress_end and i == rows - 1)))


def process_affinity():
    """What the real runtime does at startup (thread::EnableAffinityConfiguration): set the process mask to
    the system mask, ignoring the result. Returns the CPU mask the process ends up with."""
    import ctypes
    k32 = ctypes.windll.kernel32
    me = ctypes.c_void_p(-1)  # the current process
    mine, system = ctypes.c_size_t(), ctypes.c_size_t()
    k32.GetProcessAffinityMask(me, ctypes.byref(mine), ctypes.byref(system))
    k32.SetProcessAffinityMask(me, system)
    k32.GetProcessAffinityMask(me, ctypes.byref(mine), ctypes.byref(system))
    return mine.value


def console_window():
    """Whether this process sits in a console with a window: "none" (no console window at all: the checks start everything with
    CREATE_NO_WINDOW), "hidden" or "visible" (a window a person would see)."""
    import ctypes
    hwnd = ctypes.windll.kernel32.GetConsoleWindow()
    if not hwnd:
        return "none"
    return "visible" if ctypes.windll.user32.IsWindowVisible(hwnd) else "hidden"


def listed(name, var):
    return any(s and name.endswith(s) for s in os.environ.get(var, "").split(","))


def recording_frames(path):
    """The frame count of a recording: the real format (np_core.h: "D4NPREC1", u32 version, u32 frame bytes (52), u64
    seed, u32 flags, u32 meta bytes, the meta text, then 52 bytes a frame) or the text file the stand-in's own record
    runs write (just the count)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] == b"D4NPREC1":
        meta = struct.unpack_from("<I", data, 28)[0]
        return (len(data) - 32 - meta) // 52
    return int(data.split()[0])


def startup_info(pid):
    """(wShowWindow, dwFlags) the process `pid` was started with, read from its PEB (RTL_USER_PROCESS_PARAMETERS, x64:
    WindowFlags at 0xA4, ShowWindowFlags at 0xA8): the STARTUPINFO its creator passed to CreateProcess. The stand-in sits
    behind fake_game.cmd, and cmd.exe starts python with a default STARTUPINFO of its own, so the process the tool started
    (the parent, cmd.exe) is the one to read."""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    nt = ctypes.WinDLL("ntdll")
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    h = k32.OpenProcess(0x0410, False, pid)  # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
    if not h:
        raise OSError(f"cannot open process {pid}")
    try:
        info = (ctypes.c_void_p * 6)()
        if nt.NtQueryInformationProcess(h, 0, ctypes.byref(info), ctypes.sizeof(info), None) != 0:
            raise OSError("NtQueryInformationProcess failed")

        def read(address, size):
            buf = ctypes.create_string_buffer(size)
            got = ctypes.c_size_t()
            if not k32.ReadProcessMemory(h, ctypes.c_void_p(address), buf, size, ctypes.byref(got)) or got.value != size:
                raise OSError(f"cannot read process memory at {address:#x}")
            return buf.raw

        params = struct.unpack("<Q", read(info[1] + 0x20, 8))[0]  # PEB.ProcessParameters
        flags, show = struct.unpack("<II", read(params + 0xA4, 8))
        return show, flags
    finally:
        k32.CloseHandle(h)


def arg_value(prefix):
    return next((a[len(prefix):] for a in sys.argv[1:] if a.startswith(prefix)), None)


def write_game_log_line():
    """The start-up line of the game's own log (--log_file): the dot-product self-test np_run.py and np_pair.py require."""
    path = arg_value("--log_file=")
    if not path:
        return
    if os.environ.get("FAKE_NO_SELFTEST") == "1":
        text = "[info] [core] [doa4][LIFE] stand-in game started, no self-test line"
    elif os.environ.get("FAKE_SELFTEST_OFF") == "1":
        text = "[info] [core] [doa4][LIFE] dot products: canonical NaN OFF (7FC00000 00000000 00000000 00000000)"
    else:
        text = "[info] [core] [doa4][LIFE] dot products: canonical NaN on"
    if os.environ.get("FAKE_LAZY_POOL") == "1":
        text += "\n" + LAZY_POOL_LINE
    with open(path, "a", encoding="utf-8") as f:
        f.write(text + "\n")


# The SDK's failed-move WARN as game.log shows it (sdk-np1 ad1b589, xobject.cpp GetNativeObject; REXSYS_WARN prints
# "[warning] [sys]"). Every SDK line of this kind starts with the tag "[netplay-lazy-pool]" (Task 15 carry a).
LAZY_POOL_LINE = ("[2026-10-31 10:40:49.001] [warning] [sys] [t4242] [netplay-lazy-pool] GetNativeObject: the lazy object for "
                  "header 82B747D0 (type 3) keeps the low handle F8000054 (MoveToHighSlot status C0000001): later guest "
                  "handles are shifted in this run")


def write_startup_note():
    ud = arg_value("--user_data_root=")
    if ud and os.environ.get("FAKE_STARTUP_NOTE") == "1":
        show, flags = startup_info(os.getppid())  # behind fake_game.cmd: cmd.exe, the process the tool started
        with open(os.path.join(os.path.dirname(os.path.normpath(ud)), "startup.txt"), "w") as f:
            f.write(f"show_window={show} flags={flags:#x}\n")


def main():
    if "DOA4_NP" not in os.environ and "DOA4_NP_TEST_DIR" in os.environ:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # py -3 -I does not put the script's folder on the path
        import fake_online
        sys.exit(fake_online.online_main())
    mode = os.environ["DOA4_NP"]
    out = os.environ["DOA4_NP_DIR"]
    name = os.path.basename(os.path.dirname(os.path.normpath(out)))
    for item in os.environ.get("FAKE_GAME_STARTFAIL", "").split(","):  # a game that could not start: no log, no file
        suffix, _, code = item.partition("=")
        if suffix and code and name.endswith(suffix):
            sys.exit(int(code))
    write_startup_note()
    write_game_log_line()
    if mode == "record":
        frames = int(os.environ.get("FAKE_GAME_FRAMES", "12"))
    else:
        frames = recording_frames(os.environ["DOA4_NP_REC"])
    rows = frames
    if mode == "replay" and listed(name, "FAKE_GAME_SHORT"):
        rows = frames - 1
    crash = listed(name, "FAKE_GAME_FAIL")
    if crash:
        rows = frames // 2
    os.makedirs(out, exist_ok=True)
    if mode == "replay" and listed(name, "FAKE_GAME_CANNOT"):
        rec = os.environ["DOA4_NP_REC"]
        with open(os.path.join(out, "np_log.txt"), "w") as f:
            f.write(f"cannot replay {rec}: unsupported recording version\n")
        sys.exit(0)
    leaky = mode == "replay" and listed(name, "FAKE_GAME_REFUSE_LEAKY")
    if mode == "replay":
        refusal = [f"replay {os.environ['DOA4_NP_REC']}: {frames} frames, seed 1",
                   "refused: the recording was made with a different sweep 4 vs 8"]
        if listed(name, "FAKE_GAME_REFUSE"):
            with open(os.path.join(out, "np_log.txt"), "w") as f:
                f.write("\n".join(refusal) + "\n")
            sys.exit(0)
    started = [f"stand-in game: {mode} run {name}, {rows} frames", f"stand-in game: affinity {process_affinity():#x}",
               # the SDL controller allow-list the game's SDL reads at start (np_run.py sets it so no real pad is opened)
               f"stand-in game: {SDL_PADS}={os.environ.get(SDL_PADS, '(unset)')}",
               # where and how the launcher started this process (the bundle's run_check.bat test pins them)
               f"stand-in game: cwd {os.getcwd()}", "stand-in game: args " + "|".join(sys.argv[1:]),
               f"stand-in game: console-window {console_window()}"]
    started += [f"stand-in game: env {k}={v}" for k, v in sorted(os.environ.items()) if k.startswith("DOA4_")]
    ud = next((a[len("--user_data_root="):] for a in sys.argv[1:] if a.startswith("--user_data_root=")), None)
    started.append("stand-in game: ud " + ("(no --user_data_root)" if ud is None else
                   "no" if not os.path.isdir(ud) else "yes " + ",".join(sorted(os.listdir(ud)))))
    if mode == "replay" and listed(name, "FAKE_GAME_HANG"):
        with open(os.path.join(out, "np_log.txt"), "w") as f:
            f.write("\n".join(started + [f"session on: replay, sweep {SWEEP_PER} blocks/frame, quit frame none"]) + "\n")
        time.sleep(HANG_SECONDS)
        sys.exit(0)
    write_digest(os.path.join(out, "digest.d4d"), rows)
    write_sweep(os.path.join(out, "sweep.d4s"), rows, listed(name, "FAKE_GAME_SWEEP"))
    write_counters(os.path.join(out, "counters.d4k"), rows, listed(name, "FAKE_GAME_FALLBACK"),
                   listed(name, "FAKE_GAME_PROGRESS"), listed(name, "FAKE_GAME_PROGRESS_END"))
    log = list(started)
    if listed(name, "FAKE_GAME_WARN"):
        log.append(KEEPALIVE_WARN)
    if mode == "record":
        with open(os.path.join(out, "rec.d4r"), "w") as f:
            f.write(f"{frames}\n")
    if leaky:
        log = refusal + [f"session on: replay, sweep {SWEEP_PER} blocks/frame, quit frame none"]
        code = 0
    elif crash:
        log.append("stand-in game: crashing")
        code = 3
    else:
        wrong_quit = mode == "replay" and listed(name, "FAKE_GAME_WRONGQUIT")
        log.append(f"quit at frame {rows - 1}: quit frame" if mode == "record"
                   else f"quit at frame {rows}: " + ("quit frame" if wrong_quit else "end of the recording"))
        log += SUMMARY  # after the quit line, as np_det.cpp Stop logs them
        code = 5 if listed(name, "FAKE_GAME_BADEXIT") else 0
    with open(os.path.join(out, "np_log.txt"), "w") as f:
        f.write("\n".join(log) + "\n")
    sys.exit(code)


if __name__ == "__main__":
    main()

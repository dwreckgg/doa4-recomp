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

REGIONS = [("game_obj", 0x8320D110, 0x63B0), ("frame_ctr", 0x831013E4, 4)]
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


def listed(name, var):
    return any(s and name.endswith(s) for s in os.environ.get(var, "").split(","))


def main():
    mode = os.environ["DOA4_NP"]
    out = os.environ["DOA4_NP_DIR"]
    name = os.path.basename(os.path.dirname(os.path.normpath(out)))
    if mode == "record":
        frames = int(os.environ.get("FAKE_GAME_FRAMES", "12"))
    else:
        with open(os.environ["DOA4_NP_REC"]) as f:
            frames = int(f.read().split()[0])
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
    write_digest(os.path.join(out, "digest.d4d"), rows)
    write_sweep(os.path.join(out, "sweep.d4s"), rows, listed(name, "FAKE_GAME_SWEEP"))
    write_counters(os.path.join(out, "counters.d4k"), rows, listed(name, "FAKE_GAME_FALLBACK"),
                   listed(name, "FAKE_GAME_PROGRESS"), listed(name, "FAKE_GAME_PROGRESS_END"))
    log = [f"stand-in game: {mode} run {name}, {rows} frames", f"stand-in game: affinity {process_affinity():#x}"]
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
        log.append(f"quit at frame {rows - 1}: quit frame" if mode == "record"
                   else f"quit at frame {rows}: end of the recording")
        log += SUMMARY  # after the quit line, as np_det.cpp Stop logs them
        code = 0
    with open(os.path.join(out, "np_log.txt"), "w") as f:
        f.write("\n".join(log) + "\n")
    sys.exit(code)


if __name__ == "__main__":
    main()

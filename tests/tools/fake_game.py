"""A stand-in for dead_or_alive_4.exe in the tool tests (tests/tools/test_np_tools.py). It speaks the
run contract of src/net/np_session.cpp, which is what np_run.py, np_compare.py and np_gate.py read,
without the game: nothing of the real game or its data is touched.

Reads DOA4_NP (record|replay), DOA4_NP_DIR (the run's out folder) and, for a replay, DOA4_NP_REC.
Writes digest.d4d, sweep.d4s and np_log.txt into the out folder, and for a record run rec.d4r (here
just the frame count as text). Endings, as the real session logs them:
  record N frames: N digest rows, "quit at frame N-1: quit frame"
  replay of N frames: N digest rows, "quit at frame N: end of the recording"
Test-only environment (the stand-in's own, not the game's):
  FAKE_GAME_FRAMES  frames a record run plays (default 12)
  FAKE_GAME_FAIL    comma-separated run-name suffixes (the run folder's name ends with one of them):
                    that run crashes halfway - partial digest, no quit line, exit code 3
  FAKE_GAME_SHORT   same form: that replay ends one frame early but cleanly (exit 0, the right
                    quit reason, one digest row short)
Every run writes the same hashes for the same frame, so runs of equal length compare equal.
"""
import os
import struct
import sys

REGIONS = [("game_obj", 0x8320D110, 0x63B0), ("frame_ctr", 0x831013E4, 4)]
SWEEP_PER = 2


def write_digest(path, rows):
    with open(path, "wb") as f:
        f.write(b"D4NPDIG1" + struct.pack("<II", 1, len(REGIONS)))
        for name, addr, length in REGIONS:
            f.write(struct.pack("<II", addr, length) + name.encode().ljust(24, bytes(1)))
        for i in range(rows):
            f.write(struct.pack("<II", i, 0) + struct.pack(f"<{len(REGIONS)}Q", 1000 + i, 2000 + i))


def write_sweep(path, rows):
    with open(path, "wb") as f:
        f.write(b"D4NPSWP1" + struct.pack("<IIIIII", 1, 0x82B70000, 0x835B0000, 0x10000, SWEEP_PER, 0))
        for i in range(rows):
            f.write(struct.pack("<II", i, (i * SWEEP_PER) % 164) + struct.pack(f"<{SWEEP_PER}Q", 3000 + i, 4000 + i))


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
    write_digest(os.path.join(out, "digest.d4d"), rows)
    write_sweep(os.path.join(out, "sweep.d4s"), rows)
    log = [f"stand-in game: {mode} run {name}, {rows} frames"]
    if mode == "record":
        with open(os.path.join(out, "rec.d4r"), "w") as f:
            f.write(f"{frames}\n")
    if crash:
        log.append("stand-in game: crashing")
        code = 3
    else:
        log.append(f"quit at frame {rows - 1}: quit frame" if mode == "record"
                   else f"quit at frame {rows}: end of the recording")
        code = 0
    with open(os.path.join(out, "np_log.txt"), "w") as f:
        f.write("\n".join(log) + "\n")
    sys.exit(code)


if __name__ == "__main__":
    main()

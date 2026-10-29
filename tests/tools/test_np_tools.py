"""Tests for the netplay phase-0 tools on synthetic run folders (no game needed).

  py -3 -I tests/tools/test_np_tools.py
"""
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "tools"))
FAKE_EXE = os.path.join(HERE, "fake_game.cmd")  # the stand-in game (fake_game.py behind a .cmd)
SCRIPT = os.path.join(TOOLS, "np_scripts", "single.txt")  # handed to the runs, ignored by the stand-in
checks = 0
failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL", what)


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


REGIONS = [("game_obj", 0x8320D110, 0x63B0), ("frame_ctr", 0x831013E4, 4)]


def write_digest(path, frames):
    with open(path, "wb") as f:
        f.write(b"D4NPDIG1" + struct.pack("<II", 1, len(REGIONS)))
        for name, addr, length in REGIONS:
            f.write(struct.pack("<II", addr, length) + name.encode().ljust(24, b"\0"))
        for i, hashes in enumerate(frames):
            f.write(struct.pack("<II", i, 0) + struct.pack(f"<{len(REGIONS)}Q", *hashes))


def write_sweep(path, frames, per=2):
    with open(path, "wb") as f:
        f.write(b"D4NPSWP1" + struct.pack("<IIIIII", 1, 0x82B70000, 0x835B0000, 0x10000, per, 0))
        for i, hashes in enumerate(frames):
            f.write(struct.pack("<II", i, (i * per) % 164) + struct.pack(f"<{per}Q", *hashes))


def make_run(root, name, nframes=10, digest_change=None, sweep_change=None, status="ok"):
    out = os.path.join(root, name, "out")
    os.makedirs(out)
    frames = [(1000 + i, 2000 + i) for i in range(nframes)]
    if digest_change is not None:
        frames[digest_change] = (9, 9)
    write_digest(os.path.join(out, "digest.d4d"), frames)
    sweep = [(3000 + i, 4000 + i) for i in range(nframes)]
    if sweep_change is not None:
        sweep[sweep_change] = (7, 7)
    write_sweep(os.path.join(out, "sweep.d4s"), sweep)
    with open(os.path.join(out, "run_result.txt"), "w") as f:
        f.write(f"status {status}\nexit 0\ntimeout 0\nquit quit at frame {nframes - 1}: end of the recording\n")
    return os.path.join(root, name)


def compare(a, b, *extra):
    p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_compare.py"), a, b, *extra],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def advisory_still_fails(root, what, **bad):
    """--sweep-advisory forgives sweep differences and nothing else: a run that is bad in another way
    still fails. B carries a sweep difference too, so the advisory branch really runs (checked)."""
    a, b = make_run(root, "adv_a"), make_run(root, "adv_b", sweep_change=3, **bad)
    code, out = compare(a, b, "--sweep-advisory")
    check("sweep differences reported only" in out, f"{what} with --sweep-advisory: advisory branch not reached\n{out}")
    check(code == 1, f"{what} with --sweep-advisory must still fail: exit {code}\n{out}")


def test_equal(root):
    code, out = compare(make_run(root, "a"), make_run(root, "b"))
    check(code == 0, f"equal runs: exit {code}\n{out}")
    check("digest first difference: none" in out, "equal runs: digest line")


def test_truncated(root):
    code, out = compare(make_run(root, "a"), make_run(root, "b", nframes=6))
    check(code == 1, f"truncated run must fail: exit {code}\n{out}")
    check("run lengths differ (A 10 frames, B 6 frames)" in out, f"truncated run: length line\n{out}")
    advisory_still_fails(root, "truncated run", nframes=6)


def test_failed_status(root):
    code, out = compare(make_run(root, "a"), make_run(root, "b", status="failed"))
    check(code == 1, f"failed run must fail: exit {code}")
    check("run B failed: status failed" in out, f"failed run: line\n{out}")
    advisory_still_fails(root, "failed run", status="failed")


def test_digest_diff(root):
    code, out = compare(make_run(root, "a"), make_run(root, "b", digest_change=5))
    check(code == 1, f"digest difference: exit {code}")
    check("digest first difference: frame 5" in out, f"digest difference: line\n{out}")
    advisory_still_fails(root, "digest difference", digest_change=5)


def test_sweep_diff(root):
    a, b = make_run(root, "a"), make_run(root, "b", sweep_change=3)
    code, out = compare(a, b)
    check(code == 1, f"sweep difference must fail: exit {code}\n{out}")
    check("2 blocks ever differ" in out, f"sweep difference: count line\n{out}")
    code, out = compare(a, b, "--sweep-advisory")
    check(code == 0, f"sweep difference with --sweep-advisory: exit {code}\n{out}")


def test_missing_result(root):
    a, b = make_run(root, "a"), make_run(root, "b")
    os.remove(os.path.join(b, "out", "run_result.txt"))
    code, out = compare(a, b)
    check(code == 1, f"missing run_result must fail: exit {code}")
    check("run B: no run_result.txt" in out, f"missing run_result: line\n{out}")
    code, out = compare(a, b, "--allow-unchecked")
    check(code == 0, f"missing run_result with --allow-unchecked: exit {code}\n{out}")


def test_judge(root):
    judge = load("np_run").judge
    end = ["frame 599: hook avg ...", "quit at frame 9: end of the recording"]
    check(judge("replay", 0, False, end) == ("ok", "quit at frame 9: end of the recording"), "judge: replay ok")
    check(judge("replay", 0, True, end)[0] == "failed", "judge: timeout fails")
    check(judge("replay", 3, False, end)[0] == "failed", "judge: non-zero exit fails")
    check(judge("replay", 0, False, [])[0] == "failed", "judge: no quit line fails")
    check(judge("record", 0, False, ["quit at frame 9: quit frame"])[0] == "ok", "judge: record ok")
    check(judge("record", 0, False, end)[0] == "failed", "judge: record must not end at 'end of the recording'")
    check(judge("replay", 0, False, ["quit at frame 9: shutdown"])[0] == "failed", "judge: shutdown fails")
    # a request-file quit now ends a record run as "quit frame" (np_session.cpp PollRequests); a log that
    # says np_ctl.on is a quit inside the frame, before its input was recorded: not a good run
    check(judge("record", 0, False, ["quit at frame 9: np_ctl.on"])[0] == "failed", "judge: record must not end at np_ctl.on")


def stand_in_env(**extra):
    return dict(os.environ, FAKE_GAME_PYTHON=sys.executable, **extra)


def make_userdata(root):
    ud = os.path.join(root, "ud")
    os.makedirs(ud)
    with open(os.path.join(ud, "save.bin"), "w") as f:
        f.write("x")
    return ud


def gate(root, name, *args, **env):
    """np_gate.py end to end with the stand-in game; temp user data and run folders."""
    runs = os.path.join(root, "runs")
    p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_gate.py"), name, "--userdata", make_userdata(root),
                        "--runs", runs, "--exe", FAKE_EXE, "--timeout", "60", *args],
                       capture_output=True, text=True, env=stand_in_env(**env), timeout=300)
    return p.returncode, p.stdout + p.stderr, runs


def result(runs, name):
    p = os.path.join(runs, name, "out", "run_result.txt")
    return open(p).read() if os.path.exists(p) else None


def digest_rows(runs, name):
    path = os.path.join(runs, name, "out", "digest.d4d")
    if not os.path.exists(path):
        return -1
    d = open(path, "rb").read()
    n = struct.unpack_from("<II", d, 8)[1]
    return (len(d) - 16 - 32 * n) // (8 + 8 * n)


REC_OK = "status ok\nexit 0\ntimeout 0\nquit quit at frame 11: quit frame\n"
REP_OK = "status ok\nexit 0\ntimeout 0\nquit quit at frame 12: end of the recording\n"
CRASHED = "status failed\nexit 3\ntimeout 0\nquit none\n"


def test_gate_good(root):
    code, out, runs = gate(root, "g", "--script", SCRIPT, "--replays", "1", "--concurrent", "1")
    check(code == 0, f"gate, all runs good: exit {code}\n{out}")
    check("gate PASSED: 3 runs (0 failed), 3 pairs, 0 differ" in out, f"gate, all runs good: summary\n{out}")
    check(result(runs, "g_rec") == REC_OK, f"gate, all runs good: record result {result(runs, 'g_rec')!r}")
    check(result(runs, "g_rep1") == REP_OK, f"gate, all runs good: replay result {result(runs, 'g_rep1')!r}")
    check(result(runs, "g_con1") == REP_OK, f"gate, all runs good: concurrent result {result(runs, 'g_con1')!r}")
    rows = [digest_rows(runs, n) for n in ("g_rec", "g_rep1", "g_con1")]
    check(rows == [12, 12, 12], f"gate, all runs good: every run played 12 frames, got {rows}")


def test_gate_record_fails(root):
    code, out, runs = gate(root, "r", "--script", SCRIPT, "--replays", "1", FAKE_GAME_FAIL="_rec")
    check(code == 1, f"gate, record run fails: exit {code}\n{out}")
    check("gate FAILED: the record run failed" in out, f"gate, record run fails: message\n{out}")
    check(not os.path.exists(os.path.join(runs, "r_rep1")), "gate, record run fails: no replay may start")
    check(result(runs, "r_rec") == CRASHED, f"gate, record run fails: result {result(runs, 'r_rec')!r}")


def test_gate_replay_fails(root):
    code, out, runs = gate(root, "p", "--script", SCRIPT, "--replays", "1", "--concurrent", "1", FAKE_GAME_FAIL="_rep1")
    check(code == 1, f"gate, a replay fails: exit {code}\n{out}")
    check("p_rep1: run failed" in out, f"gate, a replay fails: run named\n{out}")
    check("gate FAILED: 3 runs (1 failed), 3 pairs, 2 differ" in out, f"gate, a replay fails: summary\n{out}")
    check(result(runs, "p_rep1") == CRASHED, f"gate, a replay fails: result {result(runs, 'p_rep1')!r}")
    check(result(runs, "p_rec") == REC_OK and result(runs, "p_con1") == REP_OK, "gate, a replay fails: the others ended well")


def test_gate_short_replay(root):
    code, out, runs = gate(root, "s", "--script", SCRIPT, "--replays", "1", "--concurrent", "1", FAKE_GAME_SHORT="_con1")
    check(code == 1, f"gate, a replay one frame short: exit {code}\n{out}")
    check("run lengths differ (A 12 frames, B 11 frames)" in out, f"gate, a replay one frame short: length line\n{out}")
    check("gate FAILED: 3 runs (0 failed), 3 pairs, 2 differ" in out, f"gate, a replay one frame short: summary\n{out}")
    check((result(runs, "s_con1") or "").startswith("status ok\nexit 0\n"),"gate, a replay one frame short: the run itself ended cleanly")


def test_gate_single_replay_fails(root):
    # a replay-only gate with one replay has no pair to compare: the failed run alone must fail it
    rec = os.path.join(root, "rec.txt")
    with open(rec, "w") as f:
        f.write("12\n")
    code, out, runs = gate(root, "o", "--rec", rec, "--replays", "1", FAKE_GAME_FAIL="_rep1")
    check(code == 1, f"gate, one replay, fails: exit {code}\n{out}")
    check("gate FAILED: 1 runs (1 failed), 0 pairs, 0 differ" in out, f"gate, one replay, fails: summary\n{out}")


def test_np_run_exit_status(root):
    ud, runs = make_userdata(root), os.path.join(root, "runs")

    def np_run(name, **env):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_run.py"), "record", name, "--script", SCRIPT,
                            "--userdata", ud, "--runs", runs, "--exe", FAKE_EXE, "--timeout", "60"],
                           capture_output=True, text=True, env=stand_in_env(**env), timeout=120)
        return p.returncode, p.stdout + p.stderr

    code, out = np_run("good")
    check(code == 0, f"np_run, good run: exit {code}\n{out}")
    check(result(runs, "good") == REC_OK, f"np_run, good run: result {result(runs, 'good')!r}")
    code, out = np_run("bad", FAKE_GAME_FAIL="bad")
    check(code == 1, f"np_run, crashed run: exit {code}\n{out}")
    check(result(runs, "bad") == CRASHED, f"np_run, crashed run: result {result(runs, 'bad')!r}")


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_") and callable(f)]
    for name, fn in tests:
        root = tempfile.mkdtemp(prefix="np_tools_")
        try:
            fn(root)
        except Exception:
            global failures
            failures += 1
            print("FAIL", name, "raised:")
            traceback.print_exc()
        finally:
            shutil.rmtree(root, ignore_errors=True)
    print(f"{checks} checks, {failures} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()

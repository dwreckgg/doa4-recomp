"""Tests for the netplay phase-0 tools on synthetic run folders (no game needed).

  py -3 -I tests/tools/test_np_tools.py
"""
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import traceback

sys.dont_write_bytecode = True  # load() must not leave tools/__pycache__ in the tree

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


COUNTERS = ["fb_main_wait", "pr_cri_passes"]


def write_counters(path, frames, names=COUNTERS):
    with open(path, "wb") as f:
        f.write(b"D4NPCNT1" + struct.pack("<II", 1, len(names)))
        for n in names:
            f.write(n.encode().ljust(24, b"\0"))
        for i, values in enumerate(frames):
            f.write(struct.pack("<II", i, 0) + struct.pack(f"<{len(names)}I", *values))


def make_run(root, name, nframes=10, digest_change=None, sweep_change=None, status="ok", fallback_at=None,
             progress_change=None, counter_rows=None, counter_names=COUNTERS, sweep_rows=None, sweep_file=True,
             sweep_per=2):
    """A synthetic run folder. Counters: fb_main_wait is 0 (1 from frame `fallback_at` on),
    pr_cri_passes is 10 + frame (99 at frame `progress_change` only); counter_rows cuts the counters
    file to that many rows (0: header only). Sweep: sweep_rows cuts sweep.d4s the same way,
    sweep_file=False leaves it out, sweep_per sets its blocks per frame."""
    out = os.path.join(root, name, "out")
    os.makedirs(out)
    frames = [(1000 + i, 2000 + i) for i in range(nframes)]
    if digest_change is not None:
        frames[digest_change] = (9, 9)
    write_digest(os.path.join(out, "digest.d4d"), frames)
    sweep = [tuple(3000 + 1000 * k + i for k in range(sweep_per)) for i in range(nframes)]
    if sweep_change is not None:
        sweep[sweep_change] = (7,) * sweep_per
    if sweep_file:
        write_sweep(os.path.join(out, "sweep.d4s"), sweep[:sweep_rows], sweep_per)
    counters = [(0, 10 + i) for i in range(nframes)]
    if fallback_at is not None:
        counters = [(1 if i >= fallback_at else 0, 10 + i) for i in range(nframes)]
    if progress_change is not None:
        counters[progress_change] = (counters[progress_change][0], 99)
    write_counters(os.path.join(out, "counters.d4k"), counters[:counter_rows], counter_names)
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
    tag = what.replace(" ", "_")
    a, b = make_run(root, f"adv_a_{tag}"), make_run(root, f"adv_b_{tag}", sweep_change=3, **bad)
    code, out = compare(a, b, "--sweep-advisory")
    check("sweep differences reported only" in out, f"{what} with --sweep-advisory: advisory branch not reached\n{out}")
    check(code == 1, f"{what} with --sweep-advisory must still fail: exit {code}\n{out}")


def test_equal(root):
    code, out = compare(make_run(root, "a"), make_run(root, "b"))
    check(code == 0, f"equal runs: exit {code}\n{out}")
    check("digest first difference: none" in out, "equal runs: digest line")
    check("counters: 10 common frames; no fallbacks, progress equal" in out, f"equal runs: counters line\n{out}")


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


def test_sweep_missing(root):
    # a run without sweep.d4s says nothing about the sweep: like the digest and the counters, a missing
    # file fails the pair (in either run, and in both), unless --allow-unchecked
    a = make_run(root, "a")
    for label, x, y in (("B", a, make_run(root, "b", sweep_file=False)), ("A", make_run(root, "c", sweep_file=False), a)):
        code, out = compare(x, y)
        check(code == 1, f"sweep.d4s missing in run {label} must fail: exit {code}\n{out}")
        check(f"run {label}: no sweep.d4s" in out, f"sweep.d4s missing in run {label}: line\n{out}")
        code, out = compare(x, y, "--allow-unchecked")
        check(code == 0, f"sweep.d4s missing in run {label} with --allow-unchecked: exit {code}\n{out}")
        check(f"run {label}: no sweep.d4s (allowed)" in out, f"sweep.d4s missing in run {label}, allowed: line\n{out}")
    both = make_run(root, "d", sweep_file=False), make_run(root, "e", sweep_file=False)
    code, out = compare(*both)
    check(code == 1, f"sweep.d4s missing in both runs must fail: exit {code}\n{out}")
    check("run A: no sweep.d4s" in out and "run B: no sweep.d4s" in out, f"sweep.d4s missing in both: lines\n{out}")
    code, out = compare(*both, "--allow-unchecked")
    check(code == 0, f"sweep.d4s missing in both runs with --allow-unchecked: exit {code}\n{out}")
    # --sweep-advisory forgives sweep differences only, not a sweep that was never written
    code, out = compare(a, make_run(root, "f", sweep_file=False), "--sweep-advisory")
    check(code == 1, f"sweep.d4s missing with --sweep-advisory must fail: exit {code}\n{out}")


def test_sweep_length(root):
    # the sweep is judged over every digest frame, not over the frames both sweeps happen to have: a sweep
    # cut short (here 6 rows for 10 digest frames, equal digests) fails, in either run
    a = make_run(root, "a")
    for label, x, y in (("B", a, make_run(root, "b", sweep_rows=6)), ("A", make_run(root, "c", sweep_rows=6), a)):
        code, out = compare(x, y)
        check(code == 1, f"sweep.d4s with 6 rows in run {label} must fail: exit {code}\n{out}")
        check(f"run {label}: sweep.d4s has 6 rows for 10 digest frames" in out, f"6 sweep rows in run {label}: line\n{out}")
        check("sweep: run lengths differ" in out, f"6 sweep rows in run {label}: length line\n{out}")
        code, out = compare(x, y, "--sweep-advisory")
        check(code == 1, f"sweep.d4s with 6 rows in run {label} with --sweep-advisory must fail: exit {code}\n{out}")
    # an empty sweep (header only) has no differing block: it must not read as an equal one
    code, out = compare(a, make_run(root, "d", sweep_rows=0))
    check(code == 1, f"sweep.d4s with 0 rows must fail: exit {code}\n{out}")
    check("run B: sweep.d4s has 0 rows for 10 digest frames" in out, f"0 sweep rows: line\n{out}")
    # both sweeps short by the same amount: equal to each other, still not covering the digest
    code, out = compare(make_run(root, "e", sweep_rows=6), make_run(root, "f", sweep_rows=6))
    check(code == 1, f"both sweeps cut to 6 rows must fail: exit {code}\n{out}")
    check("sweep: run lengths differ" not in out, f"both sweeps cut to 6 rows: lengths are equal\n{out}")


def test_sweep_configuration(root):
    # sweeps hashed with different settings cannot be compared: a failure, not a silent skip
    a, b = make_run(root, "a"), make_run(root, "b", sweep_per=4)
    code, out = compare(a, b)
    check(code == 1, f"different sweep configurations must fail: exit {code}\n{out}")
    check("sweep: different configurations" in out, f"different sweep configurations: line\n{out}")
    code, out = compare(a, b, "--allow-unchecked")
    check(code == 0, f"different sweep configurations with --allow-unchecked: exit {code}\n{out}")
    check("sweep: different configurations, not compared (allowed)" in out,
          f"different sweep configurations, allowed: line\n{out}")


def test_missing_result(root):
    a, b = make_run(root, "a"), make_run(root, "b")
    os.remove(os.path.join(b, "out", "run_result.txt"))
    code, out = compare(a, b)
    check(code == 1, f"missing run_result must fail: exit {code}")
    check("run B: no run_result.txt" in out, f"missing run_result: line\n{out}")
    code, out = compare(a, b, "--allow-unchecked")
    check(code == 0, f"missing run_result with --allow-unchecked: exit {code}\n{out}")


def test_counters_fallback(root):
    code, out = compare(make_run(root, "a"), make_run(root, "b", fallback_at=4))
    check(code == 1, f"a fallback must fail: exit {code}\n{out}")
    check("run B: fb_main_wait = 1 (first at frame 4)" in out, f"fallback: line\n{out}")
    check("run A:" not in out, f"fallback: the clean run A must not be blamed\n{out}")
    advisory_still_fails(root, "a fallback", fallback_at=4)
    # in run A as well (the recording): every run is judged, not only the second one
    code, out = compare(make_run(root, "c", fallback_at=2), make_run(root, "d"))
    check(code == 1, f"a fallback in run A must fail: exit {code}\n{out}")
    check("run A: fb_main_wait = 1 (first at frame 2)" in out, f"fallback in run A: line\n{out}")
    check("run B:" not in out, f"fallback in run A: the clean run B must not be blamed\n{out}")


def test_counters_fallback_total(root):
    # the count is the last row's value and "first at" the first row above zero: a counter that rises
    # twice tells them apart from "the first nonzero value" and "the frame it reached its total"
    b = make_run(root, "b")
    fb = [0, 1, 1, 1, 1, 1, 1, 3, 3, 3]
    write_counters(os.path.join(b, "out", "counters.d4k"), [(fb[i], 10 + i) for i in range(10)])
    code, out = compare(make_run(root, "a"), b)
    check(code == 1, f"a counter that rose twice must fail: exit {code}\n{out}")
    check("run B: fb_main_wait = 3 (first at frame 1)" in out, f"fallback total: line\n{out}")


def test_counters_progress(root):
    code, out = compare(make_run(root, "a"), make_run(root, "b", progress_change=6))
    check(code == 1, f"a progress difference must fail: exit {code}\n{out}")
    check("pr_cri_passes first differs at frame 6" in out, f"progress: line\n{out}")
    check("fb_main_wait" not in out, f"progress: no fallback may be reported\n{out}")
    advisory_still_fails(root, "a progress difference", progress_change=6)
    # either direction: the run that is ahead (99 against 16) may be A as well as B
    code, out = compare(make_run(root, "c", progress_change=6), make_run(root, "d"))
    check(code == 1, f"a progress difference with A ahead must fail: exit {code}\n{out}")
    check("pr_cri_passes first differs at frame 6" in out, f"progress, A ahead: line\n{out}")


def test_counters_from(root):
    # --from starts the progress comparison later; a fallback counts wherever in the run it happened
    code, out = compare(make_run(root, "a"), make_run(root, "b", progress_change=3), "--from", "5")
    check(code == 0, f"a progress difference before --from: exit {code}\n{out}")
    check("counters: 5 common frames; no fallbacks, progress equal" in out, f"--from: counters line\n{out}")
    code, out = compare(make_run(root, "c"), make_run(root, "d", fallback_at=2), "--from", "5")
    check(code == 1, f"a fallback before --from must still fail: exit {code}\n{out}")
    check("run B: fb_main_wait = 1 (first at frame 2)" in out, f"fallback before --from: line\n{out}")


def test_counters_missing(root):
    a, b = make_run(root, "a"), make_run(root, "b")
    os.remove(os.path.join(b, "out", "counters.d4k"))
    code, out = compare(a, b)
    check(code == 1, f"missing counters must fail: exit {code}\n{out}")
    check("run B: no counters.d4k" in out, f"missing counters: line\n{out}")
    advisory = compare(a, b, "--sweep-advisory")
    check(advisory[0] == 1, f"missing counters with --sweep-advisory must fail: exit {advisory[0]}\n{advisory[1]}")
    code, out = compare(a, b, "--allow-unchecked")
    check(code == 0, f"missing counters with --allow-unchecked: exit {code}\n{out}")
    check("run B: no counters.d4k (allowed)" in out, f"missing counters, allowed: line\n{out}")
    # allowed means "not compared", not "forgiven": a run that has counters is still judged on its own
    code, out = compare(make_run(root, "c", fallback_at=4), b, "--allow-unchecked")
    check(code == 1, f"a fallback beside a run without counters, allowed: exit {code}\n{out}")
    check("run A: fb_main_wait = 1 (first at frame 4)" in out, f"fallback beside missing counters: line\n{out}")


def test_counters_shape(root):
    a = make_run(root, "a")
    code, out = compare(a, make_run(root, "b", counter_names=["fb_main_wait", "pr_other"]))
    check(code == 1, f"runs that counted different things must fail: exit {code}\n{out}")
    check("the runs counted different things" in out, f"different counters: line\n{out}")
    # a counters file that does not cover the digest's frames says nothing about the frames it lacks
    # (an empty one has no fallbacks and no differences: it must not read as a clean run)
    for rows in (6, 0):
        code, out = compare(a, make_run(root, f"c{rows}", counter_rows=rows))
        check(code == 1, f"counters with {rows} rows for 10 digest frames must fail: exit {code}\n{out}")
        check(f"run B: counters.d4k has {rows} rows for 10 digest frames" in out, f"{rows} counter rows: line\n{out}")
    advisory_still_fails(root, "cut counters", counter_rows=6)


def test_counter_names_match_source(root):
    # the stand-in game writes the real counters file layout with the real names (T3.1): pin its list, the
    # C++ names table and the enum's order against each other
    net = os.path.join(HERE, "..", "..", "src", "net")
    src = open(os.path.join(net, "np_det.cpp"), encoding="utf-8").read()
    table = re.search(r"kCounterNames\[\]\s*=\s*\{(.*?)\};", src, re.S)
    names = re.findall(r'"([^"]*)"', table.group(1)) if table else []
    # 16 since Task 11c: fb_reader (the reader fix's fallback) - a fallback counter, so np_compare's "no fallbacks"
    # judges it (NOT a pr_ name, which would only be compared between runs)
    check(len(names) == 16, f"np_det.cpp: expected 16 counter names, found {len(names)}")
    check("fb_reader" in names, f"np_det.cpp: the reader fix's fallback counter fb_reader: {names}")
    check(all(re.fullmatch(r"(fb|pr)_[a-z_]+", n) and len(n) <= 23 for n in names),
          f"np_det.cpp: every name is fb_/pr_ and fits the file's 23 characters: {names}")
    enum = re.search(r"enum Counter : int \{(.*?)kCounterCount", open(os.path.join(net, "np_det_internal.h"),
                                                                      encoding="utf-8").read(), re.S)
    idents = re.findall(r"\bk([A-Z]\w*),", enum.group(1)) if enum else []
    snake = ["_".join(re.findall(r"[A-Z][a-z]*", i)).lower() for i in idents]
    check(len(snake) == len(names) and snake == names, f"np_det_internal.h enum order vs the names table:\n{snake}\n{names}")
    spec = importlib.util.spec_from_file_location("fake_game", os.path.join(HERE, "fake_game.py"))
    fake = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fake)
    check(fake.COUNTERS == names, f"fake_game.COUNTERS vs np_det.cpp:\n{fake.COUNTERS}\n{names}")


def fake_game_module():
    spec = importlib.util.spec_from_file_location("fake_game", os.path.join(HERE, "fake_game.py"))
    fake = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fake)
    return fake


def test_keepalive_warn_text(root):
    # The WARN the keepalive fix logs (np_det.cpp KeepAliveSkip) is the line np_run.judge fails a run on: the
    # source's format string must start with judge's prefix, and the stand-in's filled-in line must be what that
    # format string produces (T4.2: the needle comes from the code's own message).
    src = open(os.path.join(HERE, "..", "..", "src", "net", "np_det.cpp"), encoding="utf-8").read()
    fmts = re.findall(r'LogLine\(fmt::format\("(WARN[^"]*)"\s*"([^"]*)"', src)
    check(len(fmts) == 1, f"np_det.cpp: one WARN format string (two literal halves), found {fmts}")
    if len(fmts) != 1:
        return
    fmt_text = fmts[0][0] + fmts[0][1]
    prefix = getattr(load("np_run"), "WARN_PREFIX", None)
    check(isinstance(prefix, str) and prefix.strip() == "WARN" and fmt_text.startswith(prefix),
          f"np_det.cpp's WARN line starts with np_run.WARN_PREFIX {prefix!r}: {fmt_text!r}")
    prefix = prefix if isinstance(prefix, str) else "WARN "
    pattern = re.escape(fmt_text).replace(r"\{:08X\}", "[0-9A-F]{8}").replace(r"\{\}", r"\S+")
    fake = fake_game_module()
    check(re.fullmatch(pattern, fake.KEEPALIVE_WARN) is not None,
          f"the stand-in's WARN line is what np_det.cpp's format produces:\n{fmt_text}\n{fake.KEEPALIVE_WARN}")
    # the session's end-of-run summary lines (np_det.cpp Stop) are no WARN: the stand-in logs them in every good run
    check(not any(l.startswith(prefix) for l in fake.SUMMARY), f"the summary lines are not WARN lines: {fake.SUMMARY}")
    for needle in ('"keep-alive ({}): {} calls skipped on the D3D kick thread', '"reader ({}): {} file threads waited for'):
        check(needle in src, f"np_det.cpp Stop still logs {needle!r} (the stand-in's SUMMARY copies it)")


def test_judge_warn(root):
    # A WARN line (np_det.cpp: an assumption of a fix did not hold; the session ran on) fails a run that otherwise
    # ended well, in either mode, wherever it is in the log; the reason names the WARN and how the run ended.
    np_run = load("np_run")
    judge, warn = np_run.judge, fake_game_module().KEEPALIVE_WARN
    end = "quit at frame 6995: end of the recording"
    for log in (GOOD_REPLAY_LOG[:2] + [warn, end], GOOD_REPLAY_LOG + [warn]):
        check(judge("replay", 0, False, log) == ("failed", f"{warn} ({end})"), f"judge: a WARN fails a replay: {judge('replay', 0, False, log)}")
    rec = [warn, "quit at frame 9: quit frame"]
    check(judge("record", 0, False, rec) == ("failed", f"{warn} (quit at frame 9: quit frame)"),
          f"judge: a WARN fails a record run: {judge('record', 0, False, rec)}")
    check(judge("replay", 0, False, [warn]) == ("failed", f"{warn} (no quit line)"), "judge: a WARN with no quit line")
    # healthy output: a good replay with the session's summary lines, and "WARN" not at a line's start, stay ok
    summary = GOOD_REPLAY_LOG + fake_game_module().SUMMARY
    check(judge("replay", 0, False, summary) == ("ok", end), f"judge: the summary lines are healthy: {judge('replay', 0, False, summary)}")
    inside = GOOD_REPLAY_LOG[:2] + ["shot 12: WARN keep-alive in the file name"] + GOOD_REPLAY_LOG[2:]
    check(judge("replay", 0, False, inside) == ("ok", end), "judge: 'WARN' inside a line is not a WARN line")
    # a refusal is still the reason when the refused session (wrongly) also logged a WARN
    check(judge("replay", 0, False, REFUSAL_LOG + [warn])[1] == REFUSED, "judge: a refusal stays the reason")


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
    # a replay that a request file cut short logs the same reason a record run ends with: not a good replay
    check(judge("replay", 0, False, ["quit at frame 9: quit frame"])[0] == "failed",
          "judge: a replay cut short by a request ('quit frame') fails")


REFUSED = "refused: the recording was made with a different sweep 4 vs 8"
CANNOT = r"cannot replay C:\runs\x\rec.d4r: unsupported recording version"
REFUSAL_LOG = [r"replay C:\runs\x\rec.d4r: 6995 frames, seed 1", REFUSED]
LEAKED_LOG = REFUSAL_LOG + ["sweep masks: 5 ranges, 1068 bytes in 4 blocks hashed as zero",
                            "session on: replay, sweep 8 blocks/frame, quit frame none"]
GOOD_REPLAY_LOG = [r"replay C:\runs\x\rec.d4r: 6995 frames, seed 1", "session on: replay, sweep 4 blocks/frame, quit frame none",
                   "quit at frame 6995: end of the recording"]


def test_judge_refusal(root):
    # A session that refuses its recording (np_session.cpp Start: another build, fix set, settings, or a
    # recording it cannot read) ends the run at once, writes no hash file and logs no "session on" line.
    # The refusal is the failure reason in run_result ("quit refused: ..."); a refusal the session did not
    # honour is a different reason, because the status is "failed" either way.
    judge = load("np_run").judge
    check(judge("replay", 0, False, REFUSAL_LOG) == ("failed", REFUSED), f"judge: a clean refusal: {judge('replay', 0, False, REFUSAL_LOG)}")
    check(judge("replay", 0, False, [CANNOT]) == ("failed", CANNOT), "judge: a recording that cannot be read is the reason")
    for line, log in ((REFUSED, LEAKED_LOG), (CANNOT, [CANNOT, "session on: replay, sweep 4 blocks/frame, quit frame none"])):
        status, why = judge("replay", 0, False, log)
        check(status == "failed" and why.startswith("refusal not honoured (a session started after it): "),
              f"judge: a session on line after '{line}' is not a clean refusal: {why!r}")
        check(line in why, f"judge: the not-honoured reason still names the refusal: {why!r}")
    status, why = judge("replay", 0, True, REFUSAL_LOG)
    check(status == "failed" and why.startswith("refusal not honoured (the run had to be ended after the timeout): "),
          f"judge: a refusal that hangs to the timeout (the host never quit): {why!r}")
    status, why = judge("replay", 5, False, REFUSAL_LOG)
    check(status == "failed" and why.startswith("refusal not honoured (exit 5): "), f"judge: a refusal that exits with an error: {why!r}")
    # the reason is the refusal line itself, not the last line of the log
    check(judge("replay", 0, False, REFUSAL_LOG + ["np_pad.on ignored: not recording"]) == ("failed", REFUSED),
          "judge: a clean refusal followed by an unrelated line still names the refusal")
    # negative controls: a refusal is recognised at the start of a line only, and a good replay stays good
    # (its log has a "session on" line, which is only a flaw AFTER a refusal)
    shot = ["shot 12: refused: no guest output yet"] + GOOD_REPLAY_LOG[1:]
    check(judge("replay", 0, False, shot) == ("ok", "quit at frame 6995: end of the recording"),
          f"judge: 'refused:' inside a line is not a refusal: {judge('replay', 0, False, shot)}")
    check(judge("replay", 0, False, GOOD_REPLAY_LOG) == ("ok", "quit at frame 6995: end of the recording"), "judge: a good replay is still ok")
    check(judge("replay", 0, False, ["session on: replay, sweep 4 blocks/frame, quit frame none"] + REFUSAL_LOG)[1].startswith("refused: "),
          "judge: a session on line BEFORE a refusal is not a flaw of the refusal")


def test_judge_refusal_digest(root):
    # a refusal that kept going has written the digest file; judge looks in the run's out folder for it
    judge = load("np_run").judge
    out = os.path.join(root, "out")
    os.makedirs(out)
    check(judge("replay", 0, False, REFUSAL_LOG, out) == ("failed", REFUSED), "judge: a refusal with an empty out folder is clean")
    open(os.path.join(out, "digest.d4d"), "wb").close()
    status, why = judge("replay", 0, False, REFUSAL_LOG, out)
    check(status == "failed" and why.startswith("refusal not honoured (digest.d4d was written): ") and REFUSED in why,
          f"judge: digest.d4d beside a refusal: {why!r}")
    status, why = judge("replay", 0, False, LEAKED_LOG, out)
    check("a session started after it; digest.d4d was written" in why, f"judge: both flaws are named: {why!r}")
    check(judge("replay", 0, False, GOOD_REPLAY_LOG, out) == ("ok", "quit at frame 6995: end of the recording"),
          "judge: digest.d4d beside a good replay is what a good replay leaves")


def stand_in_env(**extra):
    return dict(os.environ, FAKE_GAME_PYTHON=sys.executable, **extra)


def make_userdata(root):
    ud = os.path.join(root, "ud")
    os.makedirs(ud, exist_ok=True)
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
    check(out.count("counters: 12 common frames; no fallbacks, progress equal") == 3,
          f"gate, all runs good: every pair prints its counters line\n{out}")
    check(out.count("sweep: 12 common frames, 2 blocks/frame of 164; 0 blocks ever differ (0 block-frames)") == 3,
          f"gate, all runs good: every pair prints its sweep line\n{out}")


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


def test_gate_short_replay_advisory(root):
    # --sweep-advisory is the production gate mode (until the strict sweep arrives): it forgives the sweep
    # and nothing else, and the gate must not turn it into "every pair is equal"
    code, out, runs = gate(root, "sa", "--script", SCRIPT, "--replays", "1", "--concurrent", "1", "--sweep-advisory",
                           FAKE_GAME_SHORT="_con1")
    check(code == 1, f"gate --sweep-advisory, a replay one frame short: exit {code}\n{out}")
    check("run lengths differ (A 12 frames, B 11 frames)" in out, f"gate --sweep-advisory, short replay: length line\n{out}")
    check("gate FAILED: 3 runs (0 failed), 3 pairs, 2 differ" in out, f"gate --sweep-advisory, short replay: summary\n{out}")


def test_gate_fallback(root):
    # a fallback fails the gate in the production mode, in a run that itself ended well (the stand-in
    # reports fb_main_wait = 1 from frame 4 in the runs named by FAKE_GAME_FALLBACK)
    code, out, runs = gate(root, "fb", "--script", SCRIPT, "--replays", "1", "--concurrent", "1", "--sweep-advisory",
                           FAKE_GAME_FALLBACK="_con1")
    check(code == 1, f"gate --sweep-advisory, a fallback in a replay: exit {code}\n{out}")
    check("run B: fb_main_wait = 1 (first at frame 4)" in out, f"gate, a fallback in a replay: line\n{out}")
    check("gate FAILED: 3 runs (0 failed), 3 pairs, 2 differ" in out, f"gate, a fallback in a replay: summary\n{out}")
    check(result(runs, "fb_con1") == REP_OK, f"gate, a fallback in a replay: the run ended well {result(runs, 'fb_con1')!r}")
    code, out, runs = gate(root, "fa", "--script", SCRIPT, "--replays", "1", "--sweep-advisory", FAKE_GAME_FALLBACK="_rec")
    check(code == 1, f"gate --sweep-advisory, a fallback in the record run: exit {code}\n{out}")
    check("run A: fb_main_wait = 1 (first at frame 4)" in out, f"gate, a fallback in the record run: line\n{out}")


def test_gate_warn(root):
    # a WARN line fails the gate in the production mode (strict), in a replay whose hashes, counters and quit line
    # are all good: the run itself is failed (np_run judge), so the gate counts it and every pair with it differs
    code, out, runs = gate(root, "kw", "--script", SCRIPT, "--replays", "1", "--concurrent", "1", FAKE_GAME_WARN="_con1")
    check(code == 1, f"gate, a WARN in a replay: exit {code}\n{out}")
    check("kw_con1: run failed (see its out/run_result.txt)" in out, f"gate, a WARN in a replay: the run is failed\n{out}")
    check("gate FAILED: 3 runs (1 failed), 3 pairs, 2 differ" in out, f"gate, a WARN in a replay: summary\n{out}")
    warn = fake_game_module().KEEPALIVE_WARN
    check(result(runs, "kw_con1") == f"status failed\nexit 0\ntimeout 0\nquit {warn} (quit at frame 12: end of the recording)\n",
          f"gate, a WARN in a replay: run_result {result(runs, 'kw_con1')!r}")
    check(result(runs, "kw_rep1") == REP_OK, f"gate, a WARN in another replay: rep1 untouched {result(runs, 'kw_rep1')!r}")
    # and in the record run
    code, out, runs = gate(root, "kr", "--script", SCRIPT, "--replays", "1", FAKE_GAME_WARN="_rec")
    check(code == 1 and "gate FAILED: the record run failed" in out, f"gate, a WARN in the record run: exit {code}\n{out}")


def test_gate_progress(root):
    code, out, runs = gate(root, "pg", "--script", SCRIPT, "--replays", "1", "--concurrent", "1", "--sweep-advisory",
                           FAKE_GAME_PROGRESS="_rep1")
    check(code == 1, f"gate --sweep-advisory, a progress difference: exit {code}\n{out}")
    check("pr_cri_passes first differs at frame 6" in out, f"gate, a progress difference: line\n{out}")
    check("gate FAILED: 3 runs (0 failed), 3 pairs, 2 differ" in out, f"gate, a progress difference: summary\n{out}")


def test_gate_sweep_strict(root):
    # from Task 7 on the production gate runs WITHOUT --sweep-advisory: a sweep difference in a run that
    # otherwise ended well fails it, through the gate's own compare calls; with the flag it is forgiven
    code, out, runs = gate(root, "sw", "--script", SCRIPT, "--replays", "1", "--concurrent", "1", FAKE_GAME_SWEEP="_con1")
    check(code == 1, f"strict gate, a sweep difference in a replay: exit {code}\n{out}")
    check(out.count("sweep: 12 common frames, 2 blocks/frame of 164; 1 blocks ever differ (1 block-frames)") == 2,
          f"strict gate, a sweep difference: each differing pair's sweep line\n{out}")
    check("gate FAILED: 3 runs (0 failed), 3 pairs, 2 differ" in out, f"strict gate, a sweep difference: summary\n{out}")
    check(result(runs, "sw_con1") == REP_OK, f"strict gate, a sweep difference: the run ended well {result(runs, 'sw_con1')!r}")
    code, out, runs = gate(root, "sv", "--script", SCRIPT, "--replays", "1", "--concurrent", "1", "--sweep-advisory",
                           FAKE_GAME_SWEEP="_con1")
    check(code == 0, f"gate --sweep-advisory, a sweep difference only: exit {code}\n{out}")
    check("gate PASSED: 3 runs (0 failed), 3 pairs, 0 differ" in out, f"gate --sweep-advisory, sweep only: summary\n{out}")


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


def test_np_run_refusal(root):
    # np_run end to end with the stand-in: the refusal reaches run_result as the reason, and the out
    # folder reaches judge (the leaky stand-in's reason names digest.d4d, which only the folder shows)
    ud, runs = make_userdata(root), os.path.join(root, "runs")
    rec = os.path.join(root, "rec.txt")
    with open(rec, "w") as f:
        f.write("12\n")

    def np_run(name, **env):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_run.py"), "replay", name, "--rec", rec,
                            "--userdata", ud, "--runs", runs, "--exe", FAKE_EXE, "--timeout", "60"],
                           capture_output=True, text=True, env=stand_in_env(**env), timeout=120)
        return p.returncode, p.stdout + p.stderr

    code, out = np_run("ref", FAKE_GAME_REFUSE="ref")
    check(code == 1, f"np_run, a refused replay: exit {code}\n{out}")
    check(result(runs, "ref") == f"status failed\nexit 0\ntimeout 0\nquit {REFUSED}\n",
          f"np_run, a refused replay: result {result(runs, 'ref')!r}")
    check(digest_rows(runs, "ref") == -1, "np_run, a refused replay writes no digest")
    code, out = np_run("leaky", FAKE_GAME_REFUSE_LEAKY="leaky")
    check(code == 1, f"np_run, a refusal the session ignored: exit {code}\n{out}")
    check(result(runs, "leaky") == "status failed\nexit 0\ntimeout 0\nquit refusal not honoured (a session started after it; "
          f"digest.d4d was written): {REFUSED}\n", f"np_run, a refusal the session ignored: result {result(runs, 'leaky')!r}")
    code, out = np_run("cant", FAKE_GAME_CANNOT="cant")
    check(code == 1, f"np_run, an unreadable recording: exit {code}\n{out}")
    check(result(runs, "cant") == f"status failed\nexit 0\ntimeout 0\nquit cannot replay {os.path.abspath(rec)}: "
          "unsupported recording version\n", f"np_run, an unreadable recording: result {result(runs, 'cant')!r}")


def system_affinity():
    """The CPU mask of the whole system (what the stand-in widens its own mask to, like the real runtime)."""
    import ctypes
    mine, system = ctypes.c_size_t(), ctypes.c_size_t()
    ctypes.windll.kernel32.GetProcessAffinityMask(ctypes.c_void_p(-1), ctypes.byref(mine), ctypes.byref(system))
    return system.value


def logged_affinity(runs, name):
    """The CPU mask the stand-in game logged for a run (None: the run has no such line)."""
    path = os.path.join(runs, name, "out", "np_log.txt")
    found = re.search(r"^stand-in game: affinity (0x[0-9a-f]+)$", open(path).read(), re.M) if os.path.exists(path) else None
    return int(found.group(1), 16) if found else None


def command_tail(runs, name):
    """The last line of the run's command.txt (None: the run has none)."""
    path = os.path.join(runs, name, "command.txt")
    return open(path).read().splitlines()[-1] if os.path.exists(path) else None


def suspended_leftovers(run_dir):
    """How many cmd.exe processes (the stand-in game's launcher) have the run folder on their command line:
    -1 if the listing failed. A refused --affinity run must leave none; the same count is 1 while one runs."""
    script = ("(Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'cmd.exe' -and $_.CommandLine -like '*"
              + run_dir.replace("'", "''") + "*' } | Measure-Object).Count")
    p = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                       capture_output=True, text=True, timeout=120)
    return int(p.stdout.strip()) if p.stdout.strip().isdigit() else -1


def test_np_run_affinity(root):
    # --affinity limits the game process to the given CPUs before any of its threads runs, and the limit holds
    # when the game tries to widen it at startup (the stand-in does what the real runtime does and logs the mask it
    # ends up with). The control is a run without the option: the widened mask is the system's.
    ud, runs = make_userdata(root), os.path.join(root, "runs")
    system = system_affinity()
    one = system & -system  # the lowest CPU of the system
    check(system != one, f"np_run --affinity: the test needs at least two CPUs to tell a limit from none (system {system:#x})")

    def np_run(name, *extra):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_run.py"), "record", name, "--script", SCRIPT,
                            "--userdata", ud, "--runs", runs, "--exe", FAKE_EXE, "--timeout", "60", *extra],
                           capture_output=True, text=True, env=stand_in_env(), timeout=120)
        return p.returncode, p.stdout + p.stderr

    code, out = np_run("free")
    check(code == 0 and result(runs, "free") == REC_OK, f"np_run, no --affinity: exit {code}\n{out}")
    check(logged_affinity(runs, "free") == system, f"np_run, no --affinity: the game ends on the system's CPUs {system:#x}, got {logged_affinity(runs, 'free')}")
    check(not (command_tail(runs, "free") or "").startswith("affinity"), "np_run, no --affinity: command.txt has no affinity line")
    code, out = np_run("cut", "--affinity", hex(one))
    check(code == 0 and result(runs, "cut") == REC_OK, f"np_run --affinity: the limited run was resumed and ended well: exit {code}\n{out}")
    check(logged_affinity(runs, "cut") == one, f"np_run --affinity {one:#x}: the game ran on {logged_affinity(runs, 'cut')}")
    check(command_tail(runs, "cut") == f"affinity {hex(one)}", f"np_run --affinity: command.txt ends with {command_tail(runs, 'cut')!r}")
    # a mask of no CPU is refused by Windows: the run stops there, before the game runs at all
    code, out = np_run("none", "--affinity", "0x0")
    check(code != 0 and "affinity 0x0 refused, the game was not run" in out, f"np_run --affinity 0x0: exit {code}\n{out}")
    check(logged_affinity(runs, "none") is None and result(runs, "none") is None, "np_run --affinity 0x0: the game must not run")
    # ... and the process np_run created suspended is ended, not left behind (a suspended game would hold its memory)
    left = suspended_leftovers(os.path.join(runs, "none"))
    check(left == 0, f"np_run --affinity 0x0: {left} process(es) of the refused run left behind (-1: the listing failed)")
    # a mask that is not a number is refused before a run folder or a process exists
    code, out = np_run("junk", "--affinity", "abc")
    check(code != 0 and "not a number" in out and not os.path.exists(os.path.join(runs, "junk")),
          f"np_run --affinity abc: exit {code}\n{out}")


def test_gate_affinity(root):
    # the gate hands --affinity to every run it starts: the record, the sequential and the concurrent replays
    system = system_affinity()
    one = system & -system
    code, out, runs = gate(root, "af", "--script", SCRIPT, "--replays", "1", "--concurrent", "1", "--affinity", hex(one))
    check(code == 0, f"gate --affinity: exit {code}\n{out}")
    for n in ("af_rec", "af_rep1", "af_con1"):
        check(logged_affinity(runs, n) == one, f"gate --affinity {one:#x}: run {n} ran on {logged_affinity(runs, n)}")
        check(command_tail(runs, n) == f"affinity {hex(one)}", f"gate --affinity: {n} command.txt ends with {command_tail(runs, n)!r}")


def test_compare_final_progress(root):
    # each run's progress counters (pr_*) at the last counters row are printed: the compare judges that the
    # runs agree, never what the value is, so a counter that is 0 in every run passes and only the printed
    # value shows it. Fallback counters (fb_*) are not progress and are not printed here.
    def pair(root, tag, a_rows, b_rows):
        names = ["fb_main_wait", "pr_cri_passes", "pr_notify"]
        runs = []
        for label, rows in (("a", a_rows), ("b", b_rows)):
            run = make_run(root, f"{tag}{label}")
            write_counters(os.path.join(run, "out", "counters.d4k"), rows, names)
            runs.append(run)
        return runs

    rising = [(0, 10 + i, 5 + i) for i in range(10)]
    code, out = compare(*pair(root, "e", rising, rising))
    check(code == 0, f"final progress, equal runs: exit {code}\n{out}")
    check("  run A final progress: pr_cri_passes=19 pr_notify=14\n" in out, f"final progress, run A line\n{out}")
    check("  run B final progress: pr_cri_passes=19 pr_notify=14\n" in out, f"final progress, run B line\n{out}")
    # B differs from A at its last row only: each line carries its own run's LAST row
    ends_low = rising[:-1] + [(0, 19, 0)]
    code, out = compare(*pair(root, "d", rising, ends_low))
    check(code == 1 and "pr_notify first differs at frame 9" in out, f"final progress, differing runs: exit {code}\n{out}")
    check("  run A final progress: pr_cri_passes=19 pr_notify=14\n" in out, f"final progress, differing runs: run A line\n{out}")
    check("  run B final progress: pr_cri_passes=19 pr_notify=0\n" in out, f"final progress, differing runs: run B line\n{out}")
    # a counter that never moves in either run is equal, so the compare passes: the printed 0 is what shows it
    zero = [(0, 10 + i, 0) for i in range(10)]
    code, out = compare(*pair(root, "z", zero, zero))
    check(code == 0, f"final progress, zeroed counter in both runs: exit {code}\n{out}")
    check("  run A final progress: pr_cri_passes=19 pr_notify=0\n" in out, f"final progress, zeroed counter: the printed value\n{out}")
    # a run without a counters file has nothing to print (the compare fails it on its own)
    a, b = make_run(root, "ma"), make_run(root, "mb")
    os.remove(os.path.join(b, "out", "counters.d4k"))
    code, out = compare(a, b)
    check(code == 1 and "run B: no counters.d4k" in out and "final progress" in out and "run B final progress" not in out,
          f"final progress, a run without counters: exit {code}\n{out}")


def test_gate_final_progress(root):
    # the gate prints every run's final progress counters once (not once per pair), so the values a good
    # build ends with are in the gate's own output. The stand-in's counter at COUNTERS index i ends at
    # (i + 1) * 100 + 11 (12 frames); pr_cri_passes is index 10 since fb_reader (index 9) came in (Task 11c).
    final = "pr_cri_passes=1111 pr_spin_steps=1211 pr_loader_jobs=1311 pr_cri_jobs=1411 pr_reseeds=1511 pr_notify=1611"
    code, out, runs = gate(root, "fp", "--script", SCRIPT, "--replays", "1", "--concurrent", "1")
    check(code == 0, f"gate, final progress: exit {code}\n{out}")
    for n in ("fp_rec", "fp_rep1", "fp_con1"):
        check(f"{n} final progress: {final}\n" in out, f"gate, final progress: the line of {n}\n{out}")
    check(out.count("final progress: ") == 3, f"gate, final progress: one line per run, not per pair\n{out}")
    # the gate imports np_compare: that must not leave bytecode behind in the tools folder (a tracked tree)
    check(not os.path.exists(os.path.join(TOOLS, "__pycache__")), "gate: no __pycache__ in tools after a gate run")
    # a run that left no counters file is named, not skipped
    code, out, runs = gate(root, "fq", "--script", SCRIPT, "--replays", "1", FAKE_GAME_CANNOT="_rep1")
    check(code == 1, f"gate, final progress, an unreadable recording: exit {code}\n{out}")
    check("fq_rep1 final progress: none (no counters.d4k)\n" in out, f"gate, final progress: the run without counters\n{out}")
    check(f"fq_rec final progress: {final}\n" in out, f"gate, final progress: the good run still printed\n{out}")
    # each line comes from its own run: one concurrent replay ends with pr_notify 1612 (+1 at its last row only),
    # the others with 1611; the pairs that include it differ and fail the gate
    code, out, runs = gate(root, "fr", "--script", SCRIPT, "--replays", "1", "--concurrent", "2", FAKE_GAME_PROGRESS_END="_con2")
    check(code == 1 and "pr_notify first differs at frame 11" in out, f"gate, final progress, runs that end apart: exit {code}\n{out}")
    for n, notify in (("fr_rec", 1611), ("fr_rep1", 1611), ("fr_con1", 1611), ("fr_con2", 1612)):
        line = final.replace("pr_notify=1611", f"pr_notify={notify}")
        check(f"{n} final progress: {line}\n" in out, f"gate, final progress, runs that end apart: the line of {n}\n{out}")
    # the helper alone: runs of different values, a missing file, a file that is not a counters file
    gate_final_progress = load("np_gate").final_progress

    def final_progress(name):  # a raise (SystemExit included) is a failed check, not the end of the suite
        try:
            return gate_final_progress(root, name)
        except BaseException as e:
            return f"raised {type(e).__name__}: {e}"

    make_run(root, "ha"), make_run(root, "hb", progress_change=9)
    c = make_run(root, "hc")
    os.remove(os.path.join(c, "out", "counters.d4k"))
    check(final_progress("ha") == "pr_cri_passes=19", f"final_progress, run a: {final_progress('ha')!r}")
    check(final_progress("hb") == "pr_cri_passes=99", f"final_progress, run b: {final_progress('hb')!r}")
    check(final_progress("hc") == "none (no counters.d4k)", f"final_progress, no file: {final_progress('hc')!r}")
    with open(os.path.join(c, "out", "counters.d4k"), "wb") as f:
        f.write(b"not a counters file at all")
    check(final_progress("hc").startswith("none (") and "not a counters file" in final_progress("hc"),
          f"final_progress, a file that is not a counters file: {final_progress('hc')!r}")


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

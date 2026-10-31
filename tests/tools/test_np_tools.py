"""Tests for the netplay phase-0 tools on synthetic run folders (no game needed).

  py -3 -I tests/tools/test_np_tools.py
"""
import contextlib
import importlib.util
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import zipfile

sys.dont_write_bytecode = True  # load() must not leave tools/__pycache__ in the tree

if os.name == "nt":
    # Nothing these checks start may open a window on the PC they run on (the stand-in games, the batch files, PowerShell): every
    # process this one starts gets a console without a window, and what it starts in turn inherits that console. (Only the checks:
    # the bundle's own scripts are unchanged and open whatever windows they open on the friend's PC.)
    _Popen = subprocess.Popen

    class _QuietPopen(_Popen):
        def __init__(self, *args, **kwargs):
            kwargs["creationflags"] = kwargs.get("creationflags", 0) | subprocess.CREATE_NO_WINDOW
            super().__init__(*args, **kwargs)

    subprocess.Popen = _QuietPopen

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


def stash_memory(flink, handle, after=0x11223344, signal=0):
    """Guest memory with a KEVENT header at +8 (np_core_host_test.cpp StashMemory, the same bytes): type word,
    signal_state, wait_list_flink, wait_list_blink = handle, then a word `after`."""
    m = bytearray((0xA0 + i) & 0xFF for i in range(48))
    m[8:12] = bytes([0, 0, 4, 0])
    m[12:16] = struct.pack(">I", signal)
    m[16:20] = flink
    m[20:24] = struct.pack(">I", handle)
    m[24:28] = struct.pack(">I", after)
    return bytes(m)


def words_left(a, b, stash_words):
    """Offsets of the 4-aligned words that differ between dumps a and b and are not a stash word in both: what a dump
    check reports once the SDK handle stash is left out (np_runs/t11c/t11c_dump_check.py, t11_dump_check.py)."""
    ignored = stash_words(a) & stash_words(b)
    return {w for w in range(0, min(len(a), len(b)) - 3, 4) if a[w:w + 4] != b[w:w + 4] and w not in ignored}


def test_stash_words(root):
    # Task 11c ruling: the SDK's handle stash (sdk-np0 include/rex/system/xobject.h StashHandle: wait_list_flink =
    # "REX\0", wait_list_blink = handle) is left out of dump checks by structure, as the in-game sweep does
    # (np_core.cpp ZeroStashWords). The same fixtures as np_core_host_test.cpp TestStashMask.
    mod = load("np_dumpdiff")
    stash_words = getattr(mod, "stash_words", None)
    check(getattr(mod, "STASH_SIGNATURE", None) == b"REX\x00", "np_dumpdiff.STASH_SIGNATURE is the bytes in guest memory")
    check(stash_words is not None, "np_dumpdiff.stash_words exists")
    if stash_words is None:
        stash_words = lambda data, before=None: set()  # noqa: E731 - lets every check below report (T1.7)
    sig, other, sig1 = b"REX\x00", b"REY\x00", b"REX\x01"
    # (1) the stash word differs, the signature is in both: ignored; the signature itself is not a stash word
    a, b = stash_memory(sig, 0xF8000054), stash_memory(sig, 0xF800005C)
    check(stash_words(a) == {20}, f"stash_words: the word after the signature only: {stash_words(a)}")
    check(words_left(a, b, stash_words) == set(), f"a differing stash word with the signature in both is ignored: {words_left(a, b, stash_words)}")
    # (2) the same difference without the signature still fails
    a, b = stash_memory(other, 0xF8000054), stash_memory(other, 0xF800005C)
    check(words_left(a, b, stash_words) == {20}, f"without the signature the difference stays: {words_left(a, b, stash_words)}")
    # (3) a difference in the signature word itself still fails (also with equal, zero handles)
    a, b = stash_memory(sig, 0xF8000054), stash_memory(sig1, 0xF8000054)
    check(16 in words_left(a, b, stash_words), f"a difference in the signature word stays: {words_left(a, b, stash_words)}")
    a, b = stash_memory(sig, 0), stash_memory(sig1, 0)
    check(words_left(a, b, stash_words) == {16}, f"only the signature word differs: reported {words_left(a, b, stash_words)}")
    # (4) a non-stash word next to a stash still fails: the word after it, and signal_state before the signature
    a, b = stash_memory(sig, 0xF8000054, 0x11223344), stash_memory(sig, 0xF800005C, 0x11223345)
    check(words_left(a, b, stash_words) == {24}, f"the word after a stash stays: {words_left(a, b, stash_words)}")
    a, b = stash_memory(sig, 0xF8000054, signal=0), stash_memory(sig, 0xF800005C, signal=1)
    check(words_left(a, b, stash_words) == {12}, f"the word before the signature stays: {words_left(a, b, stash_words)}")
    # (5) alignment and byte order: unaligned signature, reversed bytes at an aligned offset
    m = bytearray(b"\x77" * 32)
    m[6:10] = sig
    check(stash_words(bytes(m)) == set(), f"an unaligned signature marks nothing: {stash_words(bytes(m))}")
    m = bytearray(b"\x77" * 32)
    m[8:12] = b"\x00XER"
    check(stash_words(bytes(m)) == set(), f"the reversed bytes mark nothing: {stash_words(bytes(m))}")
    # (6) borders: a signature in the last word marks nothing (its stash would be beyond the data); `before`
    m = bytearray(b"\x77" * 16)
    m[12:16] = sig
    check(stash_words(bytes(m)) == set(), f"a signature in the last word marks nothing: {stash_words(bytes(m))}")
    try:
        got = stash_words(b"\x77" * 16, before=sig)
    except TypeError as e:
        got = f"raised {e}"
    check(got == {0}, f"a signature just before the data marks word 0: {got}")
    # (7) decided on the data as it is: signature, signature, X -> both following words
    m = bytearray(b"\x77" * 16)
    m[0:4], m[4:8] = sig, sig
    check(stash_words(bytes(m)) == {4, 8}, f"signature, signature, X: {stash_words(bytes(m))}")


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
    return dict(os.environ, FAKE_GAME_PYTHON=sys.executable, NP_BUNDLE_NO_WINDOW="1", **extra)  # (the helper then opens no console window for a stand-in game)


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


def test_np_run_sdl_pads(root):
    # Task 11c (coordinator): the runtime opens the host's real game controllers through SDL even in a headless
    # session (game.log "SDL OnControllerDeviceAdded: Added at index 0"), so rumble could reach the user's pad.
    # np_run.py gives the game SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 (an allow-list that matches no
    # device) unless the caller already set the variable, by --env or in its own environment.
    ud, runs = make_userdata(root), os.path.join(root, "runs")
    var = "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"

    def seen(name, *extra, **env):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_run.py"), "record", name, "--script", SCRIPT,
                            "--userdata", ud, "--runs", runs, "--exe", FAKE_EXE, "--timeout", "60", *extra],
                           capture_output=True, text=True, env=stand_in_env(**env), timeout=120)
        path = os.path.join(runs, name, "out", "np_log.txt")
        m = re.search(rf"^stand-in game: {var}=(.*)$", open(path).read(), re.M) if os.path.exists(path) else None
        return (m.group(1) if m else None), p.returncode

    # precondition: the default case is only a default case if the suite's own environment lacks the variable
    check(var not in os.environ, f"the suite runs without {var} in its environment (else the default is untested)")
    got, code = seen("pads_default")
    check(got == "0x0000/0x0000" and code == 0, f"np_run: the game gets {var}=0x0000/0x0000 by default: {got!r} (exit {code})")
    got, code = seen("pads_flag", "--env", f"{var}=0x045E/0x02EA")
    check(got == "0x045E/0x02EA", f"np_run: --env {var}=... is kept: {got!r}")
    got, code = seen("pads_inherited", **{var: "0x1234/0x5678"})
    check(got == "0x1234/0x5678", f"np_run: {var} in np_run's own environment is kept: {got!r}")


def test_np_offline_check_sdl_pads(root):
    # Task 11b (11c review minor 1): np_offline_check.py starts the game like np_run.py does, with
    # SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 (SDL opens no real controller) unless the caller's own
    # environment already has the variable. Checked on the function that builds the game's environment, and through
    # main() with a stand-in game that logs the variable it was started with (the wiring).
    mod = load("np_offline_check")
    var = "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"
    check(var not in os.environ, f"the suite runs without {var} in its environment (else the default is untested)")
    env = mod.child_env({"PATH": "p", "DOA4_NP": "record", "KEEP_ME": "1"})
    check(env.get(var) == "0x0000/0x0000", f"child_env: the default is 0x0000/0x0000: {env.get(var)!r}")
    check(env.get("KEEP_ME") == "1" and env.get("PATH") == "p", "child_env keeps the caller's other variables")
    check("DOA4_NP" not in env and env.get("DOA4_PHOTO_TEST") == "hotkey", "child_env: DOA4_ variables out, photo test on")
    env = mod.child_env({var: "0x1234/0x5678"})
    check(env.get(var) == "0x1234/0x5678", f"child_env: the caller's {var} is kept: {env.get(var)!r}")
    check(mod.child_env({}).get(var) == "0x0000/0x0000", "child_env: an empty environment gets the default")

    ud = make_userdata(root)
    runs = os.path.join(root, "runs")
    os.makedirs(runs)
    fake_exe = os.path.join(HERE, "fake_offline_game.cmd")

    def started_with(name, **environ):
        """The variable's value in the log of the stand-in game that main() started, plus the other two lines."""
        saved = {k: os.environ.get(k) for k in ("FAKE_GAME_PYTHON", var, "DOA4_NP")}
        old = (mod.RUNS, mod.PRISTINE, mod.CACHE, mod.GAME, mod.RUN_SECONDS)
        try:
            os.environ["FAKE_GAME_PYTHON"] = sys.executable
            for k in (var, "DOA4_NP"):
                os.environ.pop(k, None)
            os.environ.update(environ)
            mod.RUNS, mod.PRISTINE, mod.CACHE, mod.GAME, mod.RUN_SECONDS = runs, ud, os.path.join(root, "cache"), root, 1
            with contextlib.redirect_stdout(io.StringIO()):
                mod.main(["np_offline_check.py", fake_exe, name])
        finally:
            (mod.RUNS, mod.PRISTINE, mod.CACHE, mod.GAME, mod.RUN_SECONDS) = old
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
        path = os.path.join(runs, name, "game.log")
        text = open(path).read() if os.path.exists(path) else ""
        got = {m.group(1): m.group(2) for m in re.finditer(r"^stand-in offline game: (\w+)=(.*)$", text, re.M)}
        return got

    got = started_with("offline_default")
    check(got.get(var) == "0x0000/0x0000", f"np_offline_check: the game gets {var}=0x0000/0x0000 by default: {got!r}")
    check(got.get("DOA4_PHOTO_TEST") == "hotkey", f"np_offline_check: the photo test is on: {got!r}")
    got = started_with("offline_caller", **{var: "0x1234/0x5678", "DOA4_NP": "record"})
    check(got.get(var) == "0x1234/0x5678", f"np_offline_check: the caller's {var} is kept: {got!r}")
    check(got.get("DOA4_NP") == "(unset)", f"np_offline_check: no DOA4_NP reaches the offline game: {got!r}")


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


# ======================================================================================================================
# Task 12: the cross-PC bundle (tools/np_bundle.py with its templates in tools/np_bundle/) and the cross-check of the
# friend's results (tools/np_crosscheck.py). Fixture files beside this test, each from a real source:
#   dpps_canon_sample.txt       the canonical DPPS test's output on this PC (np_phase0/friend_test/dpps_canon_local_2026-10-30.txt)
#   dpps_friend_old_sample.txt  the OLD DPPS test's output on the friend's PC (np_phase0/friend_test/dpps_friend_2026-10-29.txt)
#   rec_head_v2.bin             the first 353 bytes of np_runs/t11bsingle_rec/out/rec.d4r: header, meta, two frames
#   xxh3_vectors.txt            the output of xxh3_vectors.cpp (the xxHash library's own XXH3_64bits_withSeed answers)
# ======================================================================================================================
def fixture_text(name):
    with open(os.path.join(HERE, name), "rb") as f:
        return f.read().decode("utf-8")


def attempt(fn, *args, **kw):
    """(result, None), or (None, 'Type: message') when fn raises (SystemExit included): a raise is a failed check, not the
    end of the test (T1.7)."""
    try:
        return fn(*args, **kw), None
    except BaseException as e:  # noqa: BLE001
        return None, f"{type(e).__name__}: {e}"


DPPS_LOCAL_CANONICAL = "9358b1f6111e7b0e"  # this PC, np_phase0/friend_test/dpps_canon_local_2026-10-30.txt (task-11b-report.md)
DPPS_LOCAL_COMBINED = "2d0081405299d44a"  # the raw combined hash on this PC (the old exe printed the same)
DPPS_FRIEND_OLD_COMBINED = "3b6dca40b68d00a6"  # the friend's i5-12400, old exe, 2026-10-29


def test_dpps_hash(root):
    crosscheck = load("np_crosscheck")
    sample = fixture_text("dpps_canon_sample.txt")
    friend_old = fixture_text("dpps_friend_old_sample.txt")
    # fixture precondition: the per-class lines carry other 16-digit hashes, which a parse that takes "a hash" would return
    class_hashes = set(re.findall(r"\b[0-9a-f]{16}\b", "\n".join(l for l in sample.splitlines() if " hash: " not in l)))
    check(len(class_hashes) >= 6 and DPPS_LOCAL_CANONICAL not in class_hashes and DPPS_LOCAL_COMBINED not in class_hashes,
          f"fixture precondition: the sample's per-class lines hold other hashes: {sorted(class_hashes)}")

    def h(text, *kind):
        got, err = attempt(crosscheck.dpps_hash, text, *kind)
        return err if err else got

    check(h(sample, "canonical") == DPPS_LOCAL_CANONICAL, f"dpps_hash canonical: {h(sample, 'canonical')!r}")
    check(h(sample, "combined") == DPPS_LOCAL_COMBINED, f"dpps_hash combined: {h(sample, 'combined')!r}")
    check(h(sample) == DPPS_LOCAL_CANONICAL, f"dpps_hash: the default kind is the judged one, canonical: {h(sample)!r}")
    # NOT the other kind and NOT a class hash (the likeliest wrong parses: any 16-digit hash, the first one, the combined line)
    check(h(sample, "canonical") not in class_hashes | {DPPS_LOCAL_COMBINED}, "dpps_hash canonical returns neither a class hash nor the combined one")
    check(h(sample, "combined") not in class_hashes | {DPPS_LOCAL_CANONICAL}, "dpps_hash combined returns neither a class hash nor the canonical one")
    # the friend's real output of the OLD test: no canonical line (the old exe cannot give the judged value), a combined one
    check(h(friend_old, "canonical") is None, f"dpps_hash canonical over the old exe's output: {h(friend_old, 'canonical')!r}")
    check(h(friend_old, "combined") == DPPS_FRIEND_OLD_COMBINED, f"dpps_hash combined over the old exe's output: {h(friend_old, 'combined')!r}")
    # a text with one kind of line only
    only_canonical = "\n".join(l for l in sample.splitlines() if not l.startswith("DPPS combined hash"))
    only_combined = "\n".join(l for l in sample.splitlines() if not l.startswith("DPPS canonical hash"))
    check(h(only_canonical, "combined") is None and h(only_canonical, "canonical") == DPPS_LOCAL_CANONICAL,
          f"dpps_hash: canonical line only: {h(only_canonical, 'combined')!r} / {h(only_canonical, 'canonical')!r}")
    check(h(only_combined, "canonical") is None and h(only_combined, "combined") == DPPS_LOCAL_COMBINED,
          f"dpps_hash: combined line only: {h(only_combined, 'canonical')!r} / {h(only_combined, 'combined')!r}")
    # no hash, an empty text, a hash that is not 16 lowercase hex digits
    for what, text in (("no hash", "no hash here"), ("empty", ""), ("15 digits", "DPPS canonical hash: 9358b1f6111e7b0 (x)"),
                       ("17 digits", "DPPS canonical hash: 9358b1f6111e7b0e1 (x)"), ("not hex", "DPPS canonical hash: 9358b1f6111e7b0z (x)")):
        check(h(text, "canonical") is None, f"dpps_hash canonical, {what}: {h(text, 'canonical')!r}")
    # two canonical lines that disagree are no answer; two that agree are one
    check(h(sample + "DPPS canonical hash: 0123456789abcdef (x)\n", "canonical") is None, "dpps_hash: two disagreeing canonical lines")
    check(h(sample + "DPPS canonical hash: " + DPPS_LOCAL_CANONICAL + " (x)\n", "canonical") == DPPS_LOCAL_CANONICAL, "dpps_hash: two equal canonical lines")
    got, err = attempt(crosscheck.dpps_hash, sample, "nonsense")
    check(err is not None and err.startswith("ValueError"), f"dpps_hash: an unknown kind raises ValueError: {got!r} {err!r}")


FRIEND_LOG = "\n".join([  # lines of a real np_log.txt (np_runs/t11bsingle_rep1/out), the ones the cross-check prints
    "job workers: one at a time in a fixed order, never beside the main thread",
    "frame 599: hook avg 32.8 us max 1466.2 us, hash avg 20.4 us max 1465.9 us, det wait avg 641.8 us max 15984.5 us, "
    "frame period avg 16.678 ms (59.96 fps) max 77.60 ms",
    "job workers: main thread waited for them avg 994.8 us max 1676.4 us a frame in the update half, join avg 4.7 us max 20.1 us (600 frames)",
    "quit at frame 9: end of the recording"]) + "\n"


def make_results(root, names=("ra", "rb"), dpps=None, listed=True, sysinfo="OS: Windows 11\nCPU: 12th Gen Intel(R) Core(TM) i5-12400, 6 cores, 12 logical\n",
                 friend=None):
    """A results folder as run_check.bat leaves it (out/<name>/out/*, dpps.txt, sysinfo.txt, recordings.txt) and the local
    record run folders it is compared with (root/local/<name>/out/*). friend maps a name to make_run keywords for the
    FRIEND's run of it. Returns (results, local)."""
    res, local = os.path.join(root, "res"), os.path.join(root, "local")
    os.makedirs(os.path.join(res, "out"))
    for n in names:
        make_run(local, n)
        run = make_run(os.path.join(res, "out"), n, **((friend or {}).get(n, {})))
        with open(os.path.join(run, "out", "np_log.txt"), "w") as f:
            f.write(FRIEND_LOG)
    for fname, text in (("dpps.txt", dpps), ("sysinfo.txt", sysinfo), ("recordings.txt", "\r\n".join(names) + "\r\n" if listed else None)):
        if text is not None:
            with open(os.path.join(res, fname), "w", newline="") as f:
                f.write(text)
    return res, local


def crosscheck_run(res, local, *extra):
    p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_crosscheck.py"), res, "--local", local, *extra],
                       capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout + p.stderr


def test_crosscheck(root):
    good = fixture_text("dpps_canon_sample.txt")
    friend_old = fixture_text("dpps_friend_old_sample.txt")
    sub = lambda n: os.path.join(root, n)  # noqa: E731
    # positive control: two recordings equal, the canonical hash equal -> exit 0; the friend's lines printed
    res, local = make_results(sub("t1"), dpps=good)
    code, out = crosscheck_run(res, local)
    check(code == 0, f"crosscheck, all equal: exit {code}\n{out}")
    check("== ra: EQUAL" in out and "== rb: EQUAL" in out, f"crosscheck, all equal: per-recording lines\n{out}")
    check("friend result: status ok; exit 0; timeout 0; quit quit at frame 9: end of the recording" in out, f"crosscheck, all equal: the friend's run_result line\n{out}")
    check("crosscheck PASSED: 2 of 2 recordings equal, DPPS canonical equal" in out, f"crosscheck, all equal: summary line\n{out}")
    check(f"DPPS canonical: friend {DPPS_LOCAL_CANONICAL}, this PC {DPPS_LOCAL_CANONICAL}: EQUAL" in out, f"crosscheck, all equal: DPPS line\n{out}")
    check(f"DPPS combined (raw, not judged): friend {DPPS_LOCAL_COMBINED}, this PC {DPPS_LOCAL_COMBINED}" in out, f"crosscheck, all equal: raw hash printed for information\n{out}")
    check("friend: job workers: one at a time" in out and "frame period avg 16.678 ms" in out,
          f"crosscheck: the friend's mode-3 and frame-period lines are printed\n{out}")
    check("friend's PC: OS: Windows 11 | CPU: 12th Gen Intel(R) Core(TM) i5-12400, 6 cores, 12 logical" in out, f"crosscheck: sysinfo line\n{out}")
    # one recording differs (its digest): that one DIFFERS, the other stays EQUAL, exit 1
    res, local = make_results(sub("t2"), dpps=good, friend={"rb": {"digest_change": 5}})
    code, out = crosscheck_run(res, local)
    check(code == 1 and "== rb: DIFFERS" in out and "== ra: EQUAL" in out, f"crosscheck, a digest differs: exit {code}\n{out}")
    # equal recordings do not excuse the hash: the friend's canonical hash differs
    res, local = make_results(sub("t3"), dpps=good.replace("DPPS canonical hash: " + DPPS_LOCAL_CANONICAL, "DPPS canonical hash: 0123456789abcdef"))
    code, out = crosscheck_run(res, local)
    check(code == 1 and "== ra: EQUAL" in out and "DPPS canonical: friend 0123456789abcdef" in out and ": DIFFERS" in out, f"crosscheck, canonical hash differs: exit {code}\n{out}")
    # the raw combined hash is information only: it differs, the canonical one is equal -> exit 0 (the wrong code judges it)
    res, local = make_results(sub("t4"), dpps=good.replace("DPPS combined hash: " + DPPS_LOCAL_COMBINED, "DPPS combined hash: 0123456789abcdef"))
    code, out = crosscheck_run(res, local)
    check(code == 0 and "DPPS combined (raw, not judged): friend 0123456789abcdef" in out, f"crosscheck, only the raw hash differs: exit {code}\n{out}")
    # the OLD test's output (the friend's real one) has no canonical line: missing, exit 1
    res, local = make_results(sub("t5"), dpps=friend_old)
    code, out = crosscheck_run(res, local)
    check(code == 1 and "DPPS canonical: friend missing" in out, f"crosscheck, the old exe's output: exit {code}\n{out}")
    res, local = make_results(sub("t6"), dpps=None)
    code, out = crosscheck_run(res, local)
    check(code == 1 and "DPPS canonical: friend missing" in out, f"crosscheck, no dpps.txt: exit {code}\n{out}")
    # a recording that never ran: recordings.txt lists it, out/ does not hold it
    res, local = make_results(sub("t7"), dpps=good)
    shutil.rmtree(os.path.join(res, "out", "rb"))
    code, out = crosscheck_run(res, local)
    check(code == 1 and "rb" in out and "did not run" in out, f"crosscheck, a recording missing from the results: exit {code}\n{out}")
    # nothing ran at all: an empty out/ is not "every recording equal"
    res, local = make_results(sub("t8"), dpps=good)
    for n in ("ra", "rb"):
        shutil.rmtree(os.path.join(res, "out", n))
    code, out = crosscheck_run(res, local)
    check(code == 1, f"crosscheck, empty out/: exit {code}\n{out}")
    res, local = make_results(sub("t9"), names=(), dpps=good, listed=True)
    code, out = crosscheck_run(res, local)
    check(code == 1, f"crosscheck, no recordings at all: exit {code}\n{out}")
    # no recordings.txt: the list of what should have run is unknown; --expect gives it
    res, local = make_results(sub("t10"), dpps=good, listed=False)
    code, out = crosscheck_run(res, local)
    check(code == 1 and "recordings.txt" in out, f"crosscheck, no recordings.txt and no --expect: exit {code}\n{out}")
    code, out = crosscheck_run(res, local, "--expect", "ra,rb")
    check(code == 0, f"crosscheck, --expect names the recordings: exit {code}\n{out}")
    code, out = crosscheck_run(res, local, "--expect", "ra,rb,rc")
    check(code == 1 and "rc" in out, f"crosscheck, --expect names one that did not run: exit {code}\n{out}")
    # the friend's run did not end well (timeout): its status fails the pair
    res, local = make_results(sub("t11"), dpps=good, friend={"ra": {"status": "timeout"}})
    code, out = crosscheck_run(res, local)
    check(code == 1 and "== ra: DIFFERS" in out and "status timeout" in out, f"crosscheck, the friend's run timed out: exit {code}\n{out}")
    # no local run folder for a recording
    res, local = make_results(sub("t12"), dpps=good)
    shutil.rmtree(os.path.join(local, "rb"))
    code, out = crosscheck_run(res, local)
    check(code == 1 and "no local run folder" in out, f"crosscheck, no local run: exit {code}\n{out}")
    # an extra run in out/ that recordings.txt does not list is reported, not judged
    res, local = make_results(sub("t13"), names=("ra", "rb", "rx"), dpps=good, friend={"rx": {"digest_change": 3}})
    with open(os.path.join(res, "recordings.txt"), "w") as f:
        f.write("ra\nrb\n")
    code, out = crosscheck_run(res, local)
    check(code == 0 and "rx" in out and "not in recordings.txt" in out, f"crosscheck, an unlisted extra run: exit {code}\n{out}")


def pattern(n, mul, add):
    """The bytes xxh3_vectors.cpp's Pattern(n, mul, add) makes."""
    return bytes((i * mul + add) & 0xFF for i in range(n))


def xxh3_reference():
    """(vectors, build) from xxh3_vectors.txt: [(length, seed, hex)] and (exe length, dll length, hex)."""
    vectors, build = [], None
    for line in fixture_text("xxh3_vectors.txt").splitlines():
        w = line.split()
        if w[:1] == ["vec"]:
            vectors.append((int(w[1]), int(w[2]), w[3]))
        elif w[:1] == ["build"]:
            build = (int(w[1]), int(w[2]), w[3])
    return vectors, build


def test_xxh3(root):
    bundle = load("np_bundle")
    vectors, build = xxh3_reference()
    lengths = sorted({n for n, _, _ in vectors})
    # fixture precondition: every code path of XXH3_64bits_withSeed (xxhash.h XXH3_64bits_internal) has a length, with both
    # sides of each of its boundaries, a seed of 0 and several other seeds (the seed changes the secret above 240 bytes),
    # and inputs of more than one 1024-byte block (the scramble step)
    needed = {"0": 0, "1-3": 3, "4-8": 5, "9-16": 9, "16/17": 17, "17-32": 32, "33-64": 33, "65-96": 65, "97-128": 97,
              "128/129": 129, "129-240": 200, "240/241": 241, "one block": 1024, "two blocks": 1025, "many blocks": 4101}
    for what, n in needed.items():
        check(n in lengths, f"xxh3 fixture precondition: a vector of {n} bytes ({what})")
    seeds = {s for _, s, _ in vectors}
    check(0 in seeds and len(seeds) >= 3 and max(seeds) > 2**63, f"xxh3 fixture precondition: seeds {sorted(seeds)}")
    check(len(vectors) == len(lengths) * len(seeds) and build is not None, f"xxh3 fixture precondition: {len(vectors)} vectors, build {build}")
    for n in lengths:
        data = pattern(n, 131, 7)
        bad = []
        for m, seed, want in vectors:
            if m == n:
                got, err = attempt(bundle.xxh3_64, data, seed)
                if err or got != int(want, 16):
                    bad.append(f"seed {seed}: got {err or format(got, '016x')}, library {want}")
        check(not bad, f"xxh3_64 over {n} bytes: {'; '.join(bad)}")


def write_bin_folder(path, exe=None, dll=None, tracy=b"tracy"):
    """A build folder as np_build.bat snapshots it (np_bin/<tag>): the three files, small stand-ins of the real ones."""
    os.makedirs(path)
    for name, data in (("dead_or_alive_4.exe", exe), ("rexruntime.dll", dll), ("TracyClient.dll", tracy)):
        if data is not None:
            with open(os.path.join(path, name), "wb") as f:
                f.write(data)


def test_exe_build_id(root):
    # src/net/np_session.cpp BuildId() (line 186): h = XXH3_64bits_withSeed(exe, 0), then h = XXH3_64bits_withSeed(rexruntime.dll, h),
    # 16 lowercase hex digits; TracyClient.dll is not part of it. The expected id is the library's (xxh3_vectors.cpp "build").
    bundle = load("np_bundle")
    _, (exe_len, dll_len, want) = xxh3_reference()
    exe, dll = pattern(exe_len, 17, 1), pattern(dll_len, 29, 5)
    got, err = attempt(bundle.exe_build_id, (lambda p: (write_bin_folder(p, exe, dll), p)[1])(os.path.join(root, "b1")))
    check(err is None and got == want, f"exe_build_id of the fixture files: {err or got}, library {want}")
    # the id depends on the exe, on the dll and on the order (the dll's hash is seeded with the exe's)
    flipped = lambda d: bytes([d[0] ^ 1]) + d[1:]  # noqa: E731
    cases = {"b2": (flipped(exe), dll), "b3": (exe, flipped(dll)), "b4": (dll, exe)}  # b4: the files swapped
    for tag, (e, d) in cases.items():
        write_bin_folder(os.path.join(root, tag), e, d)
        got, err = attempt(bundle.exe_build_id, os.path.join(root, tag))
        check(err is None and got != want, f"exe_build_id changes when the files change ({tag}): {err or got}")
    write_bin_folder(os.path.join(root, "b5"), exe, dll, tracy=b"another tracy")
    got, err = attempt(bundle.exe_build_id, os.path.join(root, "b5"))
    check(err is None and got == want, f"exe_build_id ignores TracyClient.dll: {err or got}")
    # the exe and the dll must both be readable and non-empty (BuildId() reads "unreadable" otherwise, which agrees with nothing)
    for tag, (e, d) in {"b6": (exe, None), "b7": (None, dll), "b8": (b"", dll), "b9": (exe, b"")}.items():
        write_bin_folder(os.path.join(root, tag), e, d)
        got, err = attempt(bundle.exe_build_id, os.path.join(root, tag))
        check(err is not None and "BundleError" in err, f"exe_build_id refuses a missing or empty file ({tag}): {err or got}")


def rec_bytes(meta, frames=3, version=2, seed=1, flags=0):
    """A recording as src/net/np_core.cpp EncodeRecHeader/EncodeRecFrame write it (lines 300-318): "D4NPREC1", u32 version, u32 52,
    u64 seed, u32 flags, u32 meta bytes (0 in version 1), the meta text (version 2), then 52 bytes a frame."""
    meta = meta.encode() if isinstance(meta, str) else meta
    head = b"D4NPREC1" + struct.pack("<II", version, 52) + struct.pack("<Q", seed) + struct.pack("<II", flags, len(meta) if version == 2 else 0)
    return head + (meta if version == 2 else b"") + bytes(52 * frames)


def test_recording_meta(root):
    bundle = load("np_bundle")
    real = os.path.join(HERE, "rec_head_v2.bin")
    meta, err = attempt(bundle.recording_meta, real)
    check(err is None and isinstance(meta, dict), f"recording_meta of a real recording header: {err or meta}")
    meta = meta or {}
    check(meta.get("build") == "b3d118544499d1e8" and meta.get("data") == "8ce3302b3fcea98d", f"recording_meta: build and data of the real header: {meta}")
    check(meta.get("sweep") == "4" and meta.get("serial") == "default" and meta.get("fixes", "").startswith("seed,clock,sdkclock,xam,"),
          f"recording_meta: the other keys of the real header: {meta}")
    check(meta.get("tweaks") == "doa4_tag_all_stages=0,doa4_mp_hidden_characters=0", f"recording_meta: tweaks (a value with '=' signs and commas): {meta.get('tweaks')!r}")
    got, err = attempt(bundle.recording_build_id, real)
    check(err is None and got == "b3d118544499d1e8", f"recording_build_id of the real header: {err or got}")

    def write(name, data):
        p = os.path.join(root, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    # synthetic recordings built from the documented layout: the same answer for a build line anywhere in the meta
    p = write("syn.d4r", rec_bytes("fixes=seed\nbuild=0123456789abcdef\ndata=x\n"))
    got, err = attempt(bundle.recording_build_id, p)
    check(err is None and got == "0123456789abcdef", f"recording_build_id: build line not first: {err or got}")
    # refusals, each with its reason: version 1 (no meta), no build line, build=unreadable, a cut-short header, not a recording, empty
    # (each with the words of its own reason: a refusal for the wrong reason is a defect the tests must see)
    bad = {"version 1": (rec_bytes("", version=1), "has no meta"), "no build line": (rec_bytes("data=x\nfixes=seed\n"), "no build id"),
           "unreadable": (rec_bytes("build=unreadable\ndata=x\n"), "build=unreadable"), "empty build": (rec_bytes("build=\ndata=x\n"), "no build id"),
           "header cut short": (rec_bytes("build=0123456789abcdef\n")[:40], "cut short"),
           "not a recording": (b"hello world, this is no recording at all....", "not a recording"), "empty file": (b"", "not a recording"),
           "version 3": (b"D4NPREC1" + struct.pack("<II", 3, 52) + bytes(24), "unsupported recording version 3"),
           "frames of 53 bytes": (b"D4NPREC1" + struct.pack("<II", 2, 53) + bytes(24), "unsupported recording version 2 (frames of 53 bytes)")}
    for what, (data, reason) in bad.items():
        got, err = attempt(bundle.recording_build_id, write(f"bad_{what.replace(' ', '_')}.d4r", data))
        check(err is not None and "BundleError" in err and reason in err, f"recording_build_id refuses {what} with {reason!r}: {err or got}")
    got, err = attempt(bundle.recording_build_id, os.path.join(root, "missing.d4r"))
    check(err is not None and "BundleError" in err and "cannot be read" in err, f"recording_build_id refuses a missing file: {err or got}")


BUNDLE_META = "build={build}\ndata=8ce3302b3fcea98d\nfixes=seed,clock\nserial=default\nsweep=4\ntweaks=a=0,b=0\n"


def make_bundle_inputs(root, names=("ra", "rb"), builds=None, frames=12):
    """Inputs of np_bundle.py: a build folder (np_bin/<tag>: the three files), record run folders runs/<name>/out/{rec.d4r,
    run_result.txt} whose recordings (the real format) carry the build id of that folder - or builds[name] - , a user data
    folder (a nested file, an empty folder) and a stand-in for the DPPS exe."""
    _, (exe_len, dll_len, build) = xxh3_reference()  # the library's id of the fixture files, not the tool's own
    bin_dir = os.path.join(root, "np_bin", "t12x")
    write_bin_folder(bin_dir, pattern(exe_len, 17, 1), pattern(dll_len, 29, 5))
    runs = os.path.join(root, "runs")
    for n in names:
        out = os.path.join(runs, n, "out")
        os.makedirs(out)
        with open(os.path.join(out, "rec.d4r"), "wb") as f:
            f.write(rec_bytes(BUNDLE_META.format(build=(builds or {}).get(n, build)), frames=frames))
        with open(os.path.join(out, "run_result.txt"), "w") as f:
            f.write(REC_OK)
    ud = os.path.join(root, "ud_source")
    os.makedirs(os.path.join(ud, "sub", "deep"))
    os.makedirs(os.path.join(ud, "empty"))
    for rel, data in (("save.bin", b"save"), ("sub/deep/x.dat", b"\x00\x01\x02")):
        with open(os.path.join(ud, *rel.split("/")), "wb") as f:
            f.write(data)
    dpps = os.path.join(root, "dpps_host_test_canon.exe")
    with open(dpps, "wb") as f:
        f.write(b"MZ stand-in for the DPPS exe")
    return {"bin": bin_dir, "runs": runs, "recs": [os.path.join(runs, n) for n in names], "ud": ud, "dpps": dpps, "build": build}


def run_bundle(inp, out, recs=None, **over):
    """np_bundle.py as the user runs it. over replaces an option by its name without dashes (userdata=None drops it)."""
    args = {"bin": inp["bin"], "recs": ",".join(inp["recs"] if recs is None else recs), "userdata": inp["ud"], "dpps": inp["dpps"], "out": out}
    args.update(over)
    cmd = [sys.executable, "-I", os.path.join(TOOLS, "np_bundle.py")]
    for k, v in args.items():
        if v is not None:
            cmd += [f"--{k}", v]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    return p.returncode, p.stdout + p.stderr


def tree_files(folder):
    """{relative path with '/': absolute path} of every file under folder."""
    found = {}
    for dirpath, _, filenames in os.walk(folder):
        for f in filenames:
            p = os.path.join(dirpath, f)
            found[os.path.relpath(p, folder).replace(os.sep, "/")] = p
    return found


def file_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def test_bundle_layout(root):
    inp = make_bundle_inputs(root)
    out = os.path.join(root, "bundle_x")
    code, text = run_bundle(inp, out)
    check(code == 0, f"np_bundle, good inputs: exit {code}\n{text}")
    files = tree_files(out)
    expected = sorted(["bin/TracyClient.dll", "bin/dead_or_alive_4.exe", "bin/rexruntime.dll", "dpps_host_test_canon.exe", "README.txt",
                       "recordings.txt", "run_check.bat", "get_game_files.bat", "run_one.ps1", "recs/ra.d4r", "recs/rb.d4r",
                       "ud_pristine/save.bin", "ud_pristine/sub/deep/x.dat"])
    check(sorted(files) == expected, f"np_bundle: the folder holds exactly the expected files:\n{sorted(files)}\n{expected}")
    # byte-for-byte copies of the sources
    copies = {f"bin/{n}": os.path.join(inp["bin"], n) for n in ("dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll")}
    copies.update({"dpps_host_test_canon.exe": inp["dpps"], "recs/ra.d4r": os.path.join(inp["runs"], "ra", "out", "rec.d4r"),
                   "recs/rb.d4r": os.path.join(inp["runs"], "rb", "out", "rec.d4r"), "ud_pristine/save.bin": os.path.join(inp["ud"], "save.bin"),
                   "ud_pristine/sub/deep/x.dat": os.path.join(inp["ud"], "sub", "deep", "x.dat")})
    for rel, src in copies.items():
        check(rel in files and file_bytes(files[rel]) == file_bytes(src), f"np_bundle: {rel} is a byte-for-byte copy of its source")
    check(os.path.isdir(os.path.join(out, "ud_pristine", "empty")), "np_bundle: the user data's empty folder is copied too")
    # the batch file and the PowerShell helper are written for Windows: ASCII, CRLF throughout, no BOM, no placeholder left
    for rel in ("run_check.bat", "get_game_files.bat", "run_one.ps1", "README.txt", "recordings.txt"):
        data = file_bytes(files[rel]) if rel in files else b""
        cr, lf, crlf = data.count(bytes([13])), data.count(bytes([10])), data.count(bytes([13, 10]))
        check(len(data) > 0 and all(b < 128 for b in data), f"np_bundle: {rel} is non-empty ASCII")
        check(cr == lf == crlf and lf > 0 and not data.startswith(bytes([0xEF, 0xBB, 0xBF])),
              f"np_bundle: {rel} has CRLF line ends only and no BOM (CR {cr}, LF {lf}, CRLF {crlf})")
        check(not any(p in data for p in (b"@RECS@", b"@DPPS@", b"@N@", b"@MINUTES@", b"@BUILD@", b"@DATA@")),
              f"np_bundle: {rel} holds no template placeholder")
    bat = file_bytes(files["run_check.bat"]).decode("ascii") if "run_check.bat" in files else ""
    check(len(re.findall(r'^set "GAME=', bat, re.M)) == 1, "run_check.bat: exactly one GAME line, the line the friend edits")
    check("for %%R in (ra rb) do" in bat, "run_check.bat: the recordings in the for line, in order")
    check("dpps_host_test_canon.exe" in bat and "\\" in bat, "run_check.bat: runs the DPPS exe the bundle holds, with backslash paths")
    check(file_bytes(files["recordings.txt"]).decode("ascii").split() == ["ra", "rb"], "np_bundle: recordings.txt lists the names, one a line")
    readme = file_bytes(files["README.txt"]).decode("ascii") if "README.txt" in files else ""
    check(inp["build"] in readme and "8ce3302b3fcea98d" in readme, "README.txt names the build id and the game data id of the recordings")
    check("about 6 minutes" in readme, "README.txt: the time is 3 minutes a recording (default --minutes-each 3) times the 2 recordings")
    # what the friend must be told (fix round 1): step 0 gets the game files with get_game_files.bat, the network prompt, the privacy note
    for what, token in (("step 0 and its file", "get_game_files.bat"), ("the test's file", "run_check.bat"), ("the disc image", ".iso"),
                        ("the firewall prompt: Cancel is fine", "click Cancel"), ("the privacy note", "Windows user name (C:\\Users\\your name"),
                        ("the first action of step 0", "1. Double-click get_game_files.bat."), ("a pasted folder with quotes", "its own quotes")):
        check(token in readme, f"README.txt says {what} ({token!r})")
    # fix round 2 (N1, N2, N5, N7): where to extract the zip, always change the extraction folder to C:\DOA4 and why, the Windows
    # warning before step 0, the right-click Edit route for GAME, an & in a folder name, and a closed game during the extraction
    for what, token in (("replace the folder in the Extract All window", "REPLACE the folder shown"), ("the zip goes to C:\\doa4test", "C:\\doa4test"),
                        ("no accents anywhere in the path", "must not have an accent or a special letter anywhere"),
                        ("always change the extraction folder", 'ALWAYS click "Change folder..."'), ("to C:\\DOA4", 'Extract to: C:\\DOA4'),
                        ("why: no accent or special letter in the path", "must have no accent or special letter anywhere"),
                        ("the Edit route for the GAME line", "right-click run_check.bat, choose Edit"), ("what GAME means", "the folder that has the file default.xex in it"),
                        ("an & sign in the folder name", "has an & sign in it"), ("closing the game during the extraction", "removes the half copy"),
                        ("run get_game_files.bat again after that", "just double-click get_game_files.bat again")):
        check(token in readme, f"README.txt says {what} ({token!r})")
    check(0 <= readme.find("Run anyway") < readme.find("Step 0"), "README.txt: the 'Windows protected your PC' hint (Run anyway) comes before step 0")
    check("get_game_files.bat, and when the game window opens in step 0" in readme, "README.txt: the 'Windows protected your PC' hint names get_game_files.bat and the game window of step 0")
    # no CPU affinity anywhere in what runs on the friend's PC (a mask from this 16-thread PC must never reach a 12-thread CPU)
    ps1 = file_bytes(files["run_one.ps1"]).decode("ascii") if "run_one.ps1" in files else ""
    gbat = file_bytes(files["get_game_files.bat"]).decode("ascii") if "get_game_files.bat" in files else ""
    for word in ("affinity", "ProcessorAffinity", "SetProcessAffinityMask"):
        check(word.lower() not in (bat + ps1 + gbat).lower(), f"run_check.bat, get_game_files.bat and run_one.ps1 never mention {word}")
    # get_game_files.bat starts the game normally (not headless) with its save and cache folder inside the bundle (nothing in Documents)
    check('-Setup -Exe "%EXE%" -UserData "%~dp0setup_ud"' in gbat and "--headless" not in gbat.lower(),
          "get_game_files.bat: starts the exe (run_one.ps1 -Setup) with its user data folder inside the bundle, not headless")
    # the zip beside the folder: the same files at its root (no top folder), '/' separators, same bytes
    with zipfile.ZipFile(out + ".zip") as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        check(sorted(names) == expected, f"np_bundle: the zip holds the same files:\n{sorted(names)}")
        # (orig_filename: ZipFile.namelist() turns a '\' of a Windows-written name into '/', which hides the defect)
        check(all("\\" not in i.orig_filename for i in z.infolist()), "np_bundle: zip entry names use '/' as written in the file")
        check(all(z.read(n) == file_bytes(files[n]) for n in names if n in files), "np_bundle: the zip's files equal the folder's")
    check("recordings ra, rb" in text, f"np_bundle prints what it built\n{text}")


def test_bundle_refusals(root):
    # control first: inputs that are fine give a bundle (a refusal below is then about the one thing changed)
    good = make_bundle_inputs(os.path.join(root, "good"))
    code, text = run_bundle(good, os.path.join(root, "good", "out_bundle"))
    check(code == 0, f"np_bundle refusals, control: good inputs exit {code}\n{text}")
    other = "0123456789abcdef"

    def refused(case, *why, mutate=None, names=("ra", "rb"), recs=None, **over):
        sub = os.path.join(root, case)
        inp = make_bundle_inputs(sub, names=names, **({"builds": over.pop("builds")} if "builds" in over else {}))
        if mutate:
            mutate(inp)
        out = os.path.join(sub, "bundle_out")
        code, text = run_bundle(inp, out, recs=recs(inp) if recs else None, **over)
        left = [p for p in (out, out + ".zip", out + ".partial") if os.path.exists(p)]
        check(code != 0, f"np_bundle {case}: must be refused, exit {code}\n{text}")
        check("Traceback" not in text, f"np_bundle {case}: a refusal is a message, not a crash (a traceback quotes the code and satisfies any token)\n{text}")
        check(all(w in text for w in why), f"np_bundle {case}: the message names {why}\n{text}")
        check(not left, f"np_bundle {case}: nothing is left behind: {left}")

    def edit_file(inp, rel, fn):
        p = os.path.join(inp["bin"], rel) if rel.endswith((".exe", ".dll")) else rel
        data = fn(file_bytes(p))  # read and transform BEFORE the file is opened for writing (open("wb") empties it)
        with open(p, "wb") as f:
            f.write(data)

    def put_rec(name, data):
        def mutate(inp):
            with open(os.path.join(inp["runs"], name, "out", "rec.d4r"), "wb") as f:
                f.write(data)
        return mutate

    flip = lambda d: bytes([d[0] ^ 1]) + d[1:]  # noqa: E731
    # A1: every recording's build id must be the id of --bin's exe (the id the tool computes from the exe and the dll)
    refused("one_other_build", "recording rb was made by build " + other, builds={"rb": other})
    # (the next one: equal to each other, not to the exe; a check of the recordings against each other only would pass it)
    refused("all_other_build", "recording ra was made by build " + other, "recording rb was made by build " + other, builds={"ra": other, "rb": other})
    refused("two_other_builds", "recording ra was made by build " + other, "recording rb was made by build fedcba9876543210",
            builds={"ra": other, "rb": "fedcba9876543210"})
    refused("exe_changed", "made by build " + good["build"], mutate=lambda i: edit_file(i, "dead_or_alive_4.exe", flip))
    refused("dll_changed", "made by build " + good["build"], mutate=lambda i: edit_file(i, "rexruntime.dll", flip))
    refused("version_1", "recording ra", "has no meta", mutate=put_rec("ra", rec_bytes("", version=1)))
    refused("no_build_line", "recording ra", "no build id", mutate=put_rec("ra", rec_bytes("data=x\nfixes=seed\n")))
    refused("unreadable_build", "recording ra", "build=unreadable", builds={"ra": "unreadable"})
    refused("cut_short", "recording ra", "cut short", mutate=put_rec("ra", rec_bytes(BUNDLE_META.format(build=other))[:40]))
    refused("not_a_recording", "recording ra", "not a recording", mutate=put_rec("ra", b"hello world, this is no recording at all...."))

    def write_text(inp, name, rel, text):
        with open(os.path.join(inp["runs"], name, "out", rel), "w") as f:
            f.write(text)

    # the record run must have ended well, and be a run folder with a recording
    refused("record_failed", "recording ra", "status failed", mutate=lambda i: write_text(i, "ra", "run_result.txt", "status failed\nexit 3\ntimeout 0\nquit none\n"))
    refused("no_run_result", "recording ra", "run_result.txt", mutate=lambda i: os.remove(os.path.join(i["runs"], "ra", "out", "run_result.txt")))
    refused("no_recording", "recording ra", "rec.d4r", mutate=lambda i: os.remove(os.path.join(i["runs"], "ra", "out", "rec.d4r")))
    refused("no_recs", "--recs", recs=lambda i: [])
    # names that cannot be put in a batch `for` list and in paths
    for tag, bad in {"space": "r a", "amp": "r&a", "percent": "r%a", "caret": "r^a", "paren": "r(a)", "semicolon": "r;a", "quote": "r'a"}.items():
        refused(f"name_{tag}", bad, "name", names=(bad,))
    refused("duplicate_names", "recording ra", "twice", recs=lambda i: [i["recs"][0], i["recs"][0]])  # (the same folder named twice)
    # the other inputs
    for rel in ("dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll"):
        refused(f"no_{rel.split('.')[0]}", rel, mutate=lambda i, rel=rel: os.remove(os.path.join(i["bin"], rel)))
    refused("empty_exe", "dead_or_alive_4.exe", mutate=lambda i: edit_file(i, "dead_or_alive_4.exe", lambda d: b""))
    refused("no_userdata", "--userdata", "is not a folder", userdata=os.path.join(root, "nope"))
    refused("no_dpps", "--dpps", "is not a file", dpps=os.path.join(root, "nope.exe"))
    # an existing --out (the folder or the zip) is never overwritten or merged into
    sub = os.path.join(root, "exists")
    inp = make_bundle_inputs(sub)
    out = os.path.join(sub, "bundle_out")
    os.makedirs(out)
    with open(os.path.join(out, "keep.txt"), "w") as f:
        f.write("mine")
    code, text = run_bundle(inp, out)
    check(code != 0 and "exists" in text and os.listdir(out) == ["keep.txt"], f"np_bundle: an existing --out is refused and untouched: exit {code}\n{text}")
    shutil.rmtree(out)
    with open(out + ".zip", "w") as f:
        f.write("mine")
    code, text = run_bundle(inp, out)
    check(code != 0 and "exists" in text and not os.path.exists(out) and file_bytes(out + ".zip") == b"mine",
          f"np_bundle: an existing --out.zip is refused and untouched: exit {code}\n{text}")


def test_bundle_rollback(root):
    # a failure while the bundle is being put together leaves no folder, no .partial and no zip (an interrupted writer must not
    # leave a half bundle that looks finished: the folder is built aside and renamed when complete)
    bundle = load("np_bundle")
    inp = make_bundle_inputs(root)
    out = os.path.join(root, "bundle_rb")
    real_copy, calls = bundle.copy_file, []

    def failing(src, dst):
        calls.append(dst)
        if len(calls) == 6:
            raise OSError("disk full (injected)")
        return real_copy(src, dst)

    bundle.copy_file = failing
    try:
        _, err = attempt(bundle.build_bundle, inp["bin"], inp["recs"], inp["ud"], inp["dpps"], out)
    finally:
        bundle.copy_file = real_copy
    check(err is not None and "disk full" in err, f"bundle rollback: the injected failure comes through: {err}")
    # (shutil.copytree collects a failed copy and goes on, so the calls after the sixth still happen: at least six were made)
    check(len(calls) >= 6, f"bundle rollback fixture: the failure came after five copies, not before any: {len(calls)} calls")
    left = [p for p in (out, out + ".zip", out + ".partial") if os.path.exists(p)]
    check(not left, f"bundle rollback: nothing is left behind: {left}")
    # and the tool still builds into the same place afterwards
    got, err = attempt(bundle.build_bundle, inp["bin"], inp["recs"], inp["ud"], inp["dpps"], out)
    check(err is None and os.path.isdir(out) and os.path.exists(out + ".zip"), f"bundle rollback: a later build into the same --out works: {err}")


def snapshot(folder):
    """(relative path, size, mtime in ns) of every file and folder under `folder`: what a "was never written" check compares."""
    rows = []
    for dirpath, dirnames, filenames in os.walk(folder):
        for n in dirnames + filenames:
            p = os.path.join(dirpath, n)
            st = os.stat(p)
            rows.append((os.path.relpath(p, folder), st.st_size if os.path.isfile(p) else -1, st.st_mtime_ns))
    return sorted(rows)


def patch_bat(bundle_dir, _file="run_check.bat", **values):
    """Sets `set "KEY=value"` lines of the bundle's run_check.bat (or another batch file, _file) (the way the friend sets the
    GAME line); returns the previous values and the problems (a key that is not exactly one line)."""
    path = os.path.join(bundle_dir, _file)
    text = file_bytes(path).decode("ascii")
    old, problems = {}, []
    for key, value in values.items():
        found = re.findall(rf'^set "{key}=(.*)"\r$', text, re.M)
        if len(found) != 1:
            problems.append(f"{key}: {len(found)} lines")
            continue
        old[key] = found[0]
        # only the line that starts with the set (a plain replace would also hit a longer line that ends the same way)
        text = re.sub(rf'^set "{key}=.*"\r$', lambda mo: f'set "{key}={value}"\r', text, flags=re.M)
    with open(path, "wb") as f:
        f.write(text.encode("ascii"))
    return old, problems


def runnable_bundle(root, names, timeout=None, game_name="game dir (x86)"):
    """Record runs made by the stand-in game (np_run.py record) with real-format recordings written over theirs, a bundle
    built from them by np_bundle.py, and that bundle set up as the friend sets it up (GAME) plus the two lines only a test
    changes: EXE (the stand-in game in the bundle's bin folder, like the real exe) and TIMEOUT. The DPPS exe is a harmless
    real one (hostname.exe). Returns a dict: dir, game, local, shipped_game (the GAME line as shipped)."""
    inp = make_bundle_inputs(root, names=())
    ud, local = make_userdata(root), os.path.join(root, "local")
    for n in names:
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_run.py"), "record", n, "--script", SCRIPT, "--userdata", ud,
                            "--runs", local, "--exe", FAKE_EXE, "--timeout", "60"], capture_output=True, text=True, env=stand_in_env(), timeout=120)
        check(p.returncode == 0, f"runnable_bundle: the stand-in's record run {n}: exit {p.returncode}\n{p.stdout}{p.stderr}")
        with open(os.path.join(local, n, "out", "rec.d4r"), "wb") as f:  # the real format, 12 frames, the bundle's build id
            f.write(rec_bytes(BUNDLE_META.format(build=inp["build"]), frames=12))
    out = os.path.join(root, "bundle")
    code, text = run_bundle(inp, out, recs=[os.path.join(local, n) for n in names])
    check(code == 0, f"runnable_bundle: np_bundle exit {code}\n{text}")
    game = os.path.join(root, game_name)
    os.makedirs(os.path.join(game, "media"))
    for rel in ("default.xex", os.path.join("media", "a.dat")):
        with open(os.path.join(game, rel), "wb") as f:
            f.write(b"stand-in game data")
    for f in ("fake_game.cmd", "fake_game.py"):
        shutil.copy(os.path.join(HERE, f), os.path.join(out, "bin", f))
    shutil.copy(os.path.join(os.environ.get("SystemRoot", "C:\\Windows"), "System32", "hostname.exe"), os.path.join(out, "dpps_host_test_canon.exe"))
    values = {"GAME": game, "EXE": "%~dp0bin\\fake_game.cmd"}
    if timeout:
        values["TIMEOUT"] = str(timeout)
    old, problems = patch_bat(out, **values)
    check(not problems, f"runnable_bundle: run_check.bat lines to set: {problems}")
    return {"dir": out, "game": game, "local": local, "shipped_game": old.get("GAME"), "shipped_exe": old.get("EXE")}


def run_bat(bundle_dir, _script="run_check.bat", **env):
    """run_check.bat (or get_game_files.bat, _script) as the friend runs it, its `pause` answered; (exit code, its console output)."""
    p = subprocess.run(["cmd", "/c", os.path.join(bundle_dir, _script)], input=b"\r\n", capture_output=True,
                       env=stand_in_env(**env), timeout=600)
    return p.returncode, (p.stdout + p.stderr).decode("oem", errors="replace")


def stand_in_lines(log_text, key):
    prefix = f"stand-in game: {key} "
    return [l[len(prefix):] for l in log_text.splitlines() if l.startswith(prefix)]


def same_path(a, b):
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


def test_run_check_bat(root):
    # run_check.bat end to end with the stand-in game: what it does for a replay, as np_run.py does it (the environment, the
    # command line, the working folder), what it leaves in results.zip, what it never touches, and that its result feeds np_crosscheck
    names = ("ra_ok", "rb_ok")
    b = runnable_bundle(root, names)
    bundle, game = b["dir"], b["game"]
    check(b["shipped_exe"] == "%~dp0bin\\dead_or_alive_4.exe", f"run_check.bat as shipped: the EXE line: {b['shipped_exe']!r}")
    game_before, ud_before = snapshot(game), snapshot(os.path.join(bundle, "ud_pristine"))
    # leftovers of an earlier run in the bundle folder: run_check.bat starts from nothing (a stale out/ run or results.zip would be sent back)
    os.makedirs(os.path.join(bundle, "out", "stale_run"))
    with open(os.path.join(bundle, "out", "stale_run", "x.txt"), "w") as f:
        f.write("stale")
    with open(os.path.join(bundle, "results.zip"), "w") as f:
        f.write("stale")
    # the caller's environment holds DOA4_ variables left over from other runs and a real controller allow-list
    caller = {"DOA4_NP_SEED": "999", "DOA4_NP_FIX": "none", "DOA4_NP_DUMP": "0,12", "DOA4_NP_QUIT": "5", "DOA4_PHOTO_TEST": "hotkey",
              "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT": "0x045E/0x028E"}
    code, text = run_bat(bundle, **caller)
    check(code == 0 and "DONE" in text, f"run_check.bat, good run: exit {code}\n{text}")
    check(snapshot(game) == game_before, "run_check.bat never writes the game folder (names, sizes and times equal before and after)")
    check(snapshot(os.path.join(bundle, "ud_pristine")) == ud_before, "run_check.bat never writes the bundle's pristine save (each run copies it)")
    check(os.path.isdir(os.path.join(bundle, "cache")), "run_check.bat: the shader cache folder is inside the bundle")
    zpath = os.path.join(bundle, "results.zip")
    check(os.path.exists(zpath), "run_check.bat: results.zip is made")
    unz = os.path.join(root, "unz")
    members = []
    if os.path.exists(zpath) and zipfile.is_zipfile(zpath):
        with zipfile.ZipFile(zpath) as z:
            members = z.namelist()
            raw_names = [i.orig_filename for i in z.infolist()]
            z.extractall(unz)
    else:
        raw_names = []
    check(len(members) > 0 and not any("stale" in m for m in members), f"run_check.bat: results.zip is the new one and holds nothing of the stale run: {members[:3]}")
    for n in names:
        for rel in ("out/np_log.txt", "out/run_result.txt", "out/digest.d4d", "out/sweep.d4s", "out/counters.d4k", "command.txt", "pid.txt", "stdout.txt"):
            check(f"out/{n}/{rel}" in members, f"results.zip holds out/{n}/{rel}")
        check(not any(m.startswith(f"out/{n}/ud") for m in members), f"results.zip holds no copy of the save (out/{n}/ud)")
        run = os.path.join(unz, "out", n)
        log = file_bytes(os.path.join(run, "out", "np_log.txt")).decode("utf-8", "replace") if os.path.exists(os.path.join(run, "out", "np_log.txt")) else ""
        want_args = [f"--game_data_root={game}", f"--user_data_root={os.path.join(bundle, 'out', n, 'ud')}",
                     f"--log_file={os.path.join(bundle, 'out', n, 'game.log')}", "--headless=true", f"--cache_path={os.path.join(bundle, 'cache')}"]
        got_args = (stand_in_lines(log, "args") or [""])[0].split("|")
        check(len(got_args) == 5 and all(same_path(a, w) for a, w in zip(got_args, want_args)), f"{n}: the game's command line (np_run's order, quoted paths):\n{got_args}\n{want_args}")
        cwd = (stand_in_lines(log, "cwd") or [""])[0]
        check(cwd != "" and same_path(cwd, os.path.join(bundle, "bin")), f"{n}: the game starts with the bundle's bin folder as its working folder: {cwd!r}")
        check(stand_in_lines(log, "ud") == ["yes empty,save.bin,sub"], f"{n}: the game's --user_data_root held a fresh copy of the save when it started: {stand_in_lines(log, 'ud')}")
        check(stand_in_lines(log, "console-window") in (["none"], ["hidden"]), f"{n}: the check started the whole chain without a visible window: {stand_in_lines(log, 'console-window')}")
        envs = stand_in_lines(log, "env")
        want_env = ["DOA4_NP=replay", f"DOA4_NP_DIR={os.path.join(bundle, 'out', n, 'out')}", f"DOA4_NP_REC={os.path.join(bundle, 'recs', n + '.d4r')}"]
        check(len(envs) == 3 and envs[0] == want_env[0] and all(same_path(e.split("=", 1)[1], w.split("=", 1)[1]) and e.split("=", 1)[0] == w.split("=", 1)[0]
                                                               for e, w in zip(envs[1:], want_env[1:])),
              f"{n}: exactly DOA4_NP, DOA4_NP_DIR, DOA4_NP_REC in the game's environment (the caller's DOA4_ variables are cleared):\n{envs}")
        check("stand-in game: SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000\n" in log.replace("\r\n", "\n"),
              f"{n}: SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT is 0x0000/0x0000 whatever the caller's was (0x045E/0x028E)")
        rr = file_bytes(os.path.join(run, "out", "run_result.txt")).decode("utf-8") if os.path.exists(os.path.join(run, "out", "run_result.txt")) else ""
        check(rr.replace("\r\n", "\n") == REP_OK, f"{n}: run_result.txt as np_run.py writes it:\n{rr!r}")
        cmd = file_bytes(os.path.join(run, "command.txt")).decode("utf-8").splitlines() if os.path.exists(os.path.join(run, "command.txt")) else []
        check(len(cmd) == 5 and cmd[0].startswith(os.path.join(bundle, "bin", "fake_game.cmd") + " ") and cmd[1].startswith("DOA4_NP=replay")
              and cmd[4] == "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000", f"{n}: command.txt lists the command line and the DOA4_ and SDL variables, as np_run.py's does:\n{cmd}")
    for rel in ("dpps.txt", "sysinfo.txt", "recordings.txt"):
        check(rel in members, f"results.zip holds {rel}")
    dpps_text = file_bytes(os.path.join(unz, "dpps.txt")).decode("oem", "replace").splitlines() if "dpps.txt" in members else []
    check(len(dpps_text) >= 2 and dpps_text[-1].strip() == "exit 0", f"dpps.txt: the exe's output and its exit code as the last line: {dpps_text}")
    sysinfo = file_bytes(os.path.join(unz, "sysinfo.txt")).decode("utf-8", "replace") if "sysinfo.txt" in members else ""
    check(re.search(r"^OS: .+", sysinfo, re.M) and re.search(r"^CPU: .+logical", sysinfo, re.M), f"sysinfo.txt names the OS and the CPU with its logical count:\n{sysinfo}")
    check(len(raw_names) > 0 and not any("\\" in m for m in raw_names), "results.zip: entry names use '/' as written in the file (orig_filename, not namelist())")
    # the whole chain: results -> np_crosscheck. The stand-in DPPS output has no hash: runs EQUAL, the DPPS line DIFFERS, exit 1;
    # with the real canonical output in dpps.txt (the stand-in for what the friend's exe prints) every line is EQUAL, exit 0
    if os.path.exists(unz):
        code, out = crosscheck_run(unz, b["local"])
        check(code == 1 and "== ra_ok: EQUAL" in out and "== rb_ok: EQUAL" in out and "DPPS canonical: friend missing" in out, f"chain: runs equal, no DPPS hash: exit {code}\n{out}")
        with open(os.path.join(unz, "dpps.txt"), "w", newline="") as f:
            f.write(fixture_text("dpps_canon_sample.txt"))
        code, out = crosscheck_run(unz, b["local"])
        check(code == 0 and "DPPS canonical: friend 9358b1f6111e7b0e, this PC 9358b1f6111e7b0e: EQUAL" in out, f"chain: everything equal: exit {code}\n{out}")


def test_run_check_bat_modes(root):
    # every way a replay can end, through run_check.bat: its run_result.txt is what np_run.py writes (np_run.judge is the
    # oracle for status and quit text), a hung replay is ended after TIMEOUT with "status timeout" and the NEXT recording still runs
    names = ("m_ok", "m_warn", "m_crash", "m_refuse", "m_leaky", "m_cannot", "m_badexit", "m_wrongquit", "m_hang", "m_ok2")
    b = runnable_bundle(root, names, timeout=3)
    bundle = b["dir"]
    modes = {"FAKE_GAME_WARN": "_warn", "FAKE_GAME_FAIL": "_crash", "FAKE_GAME_REFUSE": "_refuse", "FAKE_GAME_REFUSE_LEAKY": "_leaky",
             "FAKE_GAME_CANNOT": "_cannot", "FAKE_GAME_HANG": "_hang", "FAKE_GAME_BADEXIT": "_badexit", "FAKE_GAME_WRONGQUIT": "_wrongquit"}
    started = time.time()
    code, text = run_bat(bundle, **modes)
    elapsed = time.time() - started
    check(code == 0 and "DONE" in text, f"run_check.bat, every ending: exit {code}\n{text}")
    check(elapsed < 150, f"run_check.bat, every ending: finished in {elapsed:.0f} s (the hung run was ended after its 3 s timeout)")
    check("timeout after 3 s: ending pid" in text and "m_hang: status timeout" in text, f"run_check.bat: the console says the hung run was ended\n{text}")
    np_run = load("np_run")
    want = {"m_ok": ("ok", "0", "0"), "m_warn": ("failed", "0", "0"), "m_crash": ("failed", "3", "0"), "m_refuse": ("failed", "0", "0"),
            "m_leaky": ("failed", "0", "0"), "m_cannot": ("failed", "0", "0"), "m_badexit": ("failed", "5", "0"), "m_wrongquit": ("failed", "0", "0"),
            "m_hang": ("timeout", None, "1"), "m_ok2": ("ok", "0", "0")}
    fake = fake_game_module()
    for n in names:
        run_out = os.path.join(bundle, "out", n, "out")
        rr_path, log_path = os.path.join(run_out, "run_result.txt"), os.path.join(run_out, "np_log.txt")
        rr = file_bytes(rr_path).decode("utf-8") if os.path.exists(rr_path) else ""
        fields = dict(l.split(" ", 1) for l in rr.replace("\r\n", "\n").splitlines() if " " in l)
        status, exit_code, timeout = want[n]
        check(fields.get("status") == status and fields.get("timeout") == timeout and (fields.get("exit") != "0" if exit_code is None else fields.get("exit") == exit_code),
              f"{n}: run_result.txt status/exit/timeout {status}/{exit_code or 'not 0 (the process was ended)'}/{timeout}:\n{rr!r}")
        check(rr.count("\n") == 4 and rr.endswith("\n") and "\r" not in rr and not rr.startswith("\ufeff"), f"{n}: run_result.txt is four LF lines, no BOM:\n{rr!r}")
        lines = file_bytes(log_path).decode("utf-8", "replace").splitlines() if os.path.exists(log_path) else []
        oracle_status, oracle_quit = np_run.judge("replay", int(fields.get("exit", "-1")), fields.get("timeout") == "1", lines, run_out)
        check(fields.get("quit") == oracle_quit and (fields.get("status") == oracle_status or fields.get("status") == "timeout"),
              f"{n}: status and quit text equal np_run.judge's on the same log: {fields} vs {(oracle_status, oracle_quit)}")
    quit_of = lambda n: dict(l.split(" ", 1) for l in file_bytes(os.path.join(bundle, "out", n, "out", "run_result.txt")).decode().splitlines() if " " in l).get("quit", "")  # noqa: E731
    check(quit_of("m_warn").startswith(fake.KEEPALIVE_WARN) and quit_of("m_warn").endswith("(quit at frame 12: end of the recording)"), f"m_warn: {quit_of('m_warn')!r}")
    check(quit_of("m_refuse") == "refused: the recording was made with a different sweep 4 vs 8", f"m_refuse: {quit_of('m_refuse')!r}")
    check(quit_of("m_leaky").startswith("refusal not honoured (a session started after it; digest.d4d was written): refused: "), f"m_leaky: {quit_of('m_leaky')!r}")
    check(quit_of("m_cannot").startswith("cannot replay ") and "m_cannot.d4r" in quit_of("m_cannot"), f"m_cannot: {quit_of('m_cannot')!r}")
    check(quit_of("m_crash") == "none" and quit_of("m_hang") == "none", f"m_crash/m_hang: no quit line: {quit_of('m_crash')!r} {quit_of('m_hang')!r}")
    # a good quit line does not make a run good: the exit code counts, and so does the reason (only the end of the recording is good)
    check(quit_of("m_badexit") == "quit at frame 12: end of the recording", f"m_badexit: {quit_of('m_badexit')!r}")
    check(quit_of("m_wrongquit") == "quit at frame 12: quit frame", f"m_wrongquit: {quit_of('m_wrongquit')!r}")
    check(os.path.exists(os.path.join(bundle, "results.zip")), "run_check.bat: results.zip is made although runs failed")
    # the hung run's process (the stand-in's cmd.exe, started by the helper) is gone; the helper ended only that one
    check(suspended_leftovers(os.path.join(bundle, "out", "m_hang")) == 0, "run_check.bat: the hung run's process was ended")
    time.sleep(fake.HANG_SECONDS + 2)  # the stand-in's python child outlives its killed cmd.exe: let it end before the folder is removed


def test_run_check_bat_preflight(root):
    b = runnable_bundle(root, ("pa",))
    bundle, game = b["dir"], b["game"]
    # as shipped, GAME is not set: nothing is run, nothing is made, the exit code says so
    patch_bat(bundle, GAME=b["shipped_game"])
    code, text = run_bat(bundle)
    check(code == 1 and "GAME" in text, f"run_check.bat, GAME as shipped: exit {code}\n{text}")
    check(not os.path.exists(os.path.join(bundle, "out")) and not os.path.exists(os.path.join(bundle, "results.zip")), "run_check.bat, GAME as shipped: no run was made")
    # a folder without default.xex
    empty = os.path.join(root, "empty_game")
    os.makedirs(empty)
    patch_bat(bundle, GAME=empty)
    code, text = run_bat(bundle)
    check(code == 1 and "default.xex" in text and not os.path.exists(os.path.join(bundle, "out")), f"run_check.bat, GAME without default.xex: exit {code}\n{text}")
    # GAME typed with a trailing backslash (a common way to write a folder): the game gets the folder without it
    patch_bat(bundle, GAME=game + "\\")
    code, text = run_bat(bundle)
    log_path = os.path.join(bundle, "out", "pa", "out", "np_log.txt")
    log = file_bytes(log_path).decode("utf-8", "replace") if os.path.exists(log_path) else ""
    args = (stand_in_lines(log, "args") or [""])[0].split("|")
    check(code == 0 and args[0] == f"--game_data_root={game}" and "pa" in os.listdir(os.path.join(bundle, "out")),
          f"run_check.bat, GAME with a trailing backslash: exit {code}, args {args[:1]}\n{text}")
    # the helper's check, directly: a path with a letter outside plain ASCII is refused (the game decodes its command line
    # in the ANSI code page, observation #0296), and so is a bundle folder with one
    ps1 = os.path.join(bundle, "run_one.ps1")
    exe = os.path.join(bundle, "bin", "fake_game.cmd")

    def ps_check(script, game_dir, exe_path):
        p = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script, "-Check", "-Game", game_dir,
                            "-Exe", exe_path], capture_output=True, timeout=120)
        return p.returncode, (p.stdout + p.stderr).decode("oem", errors="replace")

    code, text = ps_check(ps1, game, exe)
    check(code == 0, f"run_one.ps1 -Check, good paths: exit {code}\n{text}")
    accent = os.path.join(root, "caf\u00e9 game")
    os.makedirs(accent)
    shutil.copy(os.path.join(game, "default.xex"), os.path.join(accent, "default.xex"))
    code, text = ps_check(ps1, accent, exe)
    check(code == 1 and "plain" in text and "caf" not in text, f"run_one.ps1 -Check, a non-ASCII game folder: exit {code} (the folder is not echoed: typed into a .bat it comes out garbled)\n{text}")
    accent_bundle = os.path.join(root, "caf\u00e9 bundle")
    os.makedirs(accent_bundle)
    shutil.copy(ps1, os.path.join(accent_bundle, "run_one.ps1"))
    code, text = ps_check(os.path.join(accent_bundle, "run_one.ps1"), game, exe)
    check(code == 1 and "plain" in text, f"run_one.ps1 -Check, a non-ASCII bundle folder: exit {code}\n{text}")
    # the setup start refuses a bundle folder with such a letter too (the game decodes --user_data_root in the ANSI code page)
    p = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", os.path.join(accent_bundle, "run_one.ps1"), "-Setup",
                        "-Exe", exe, "-UserData", os.path.join(accent_bundle, "setup_ud")], capture_output=True, timeout=120)
    setup_text = (p.stdout + p.stderr).decode("oem", errors="replace")
    check(p.returncode == 1 and "plain" in setup_text and not os.path.exists(os.path.join(accent_bundle, "setup_ud")),
          f"run_one.ps1 -Setup, a non-ASCII bundle folder: exit {p.returncode}\n{setup_text}")
    code, text = ps_check(ps1, game, os.path.join(bundle, "bin", "nope.exe"))
    check(code == 1 and "nope.exe" in text, f"run_one.ps1 -Check, no exe: exit {code}\n{text}")


def write_stub_helper(bundle, **codes):
    """Replaces the bundle's run_one.ps1 by a stub that only exits: with codes['check'] for -Check, 'find' for -FindGame, 'zip' for
    -Zip, 'sysinfo' for -Sysinfo and 'name' for a replay (0 each unless given; 1 for -FindGame). -196608 is what Windows PowerShell
    exits with when it cannot run the script it was given (the file is missing, or it does not parse)."""
    body = ("param([switch]$Check, [switch]$FindGame, [string]$Sysinfo, [string]$Zip, [string]$Name, [string]$Game, [string]$Exe,\n"
            "      [string]$Run, [string]$Cache, [int]$Timeout)\n"
            f"if ($Check) {{ exit {codes.get('check', 0)} }}\n"
            f"if ($FindGame) {{ exit {codes.get('find', 1)} }}\n"
            f"if ($Zip) {{ exit {codes.get('zip', 0)} }}\n"
            f"if ($Sysinfo) {{ exit {codes.get('sysinfo', 0)} }}\n"
            f"exit {codes.get('name', 0)}\n")
    with open(os.path.join(bundle, "run_one.ps1"), "w", newline="") as f:
        f.write(body.replace("\n", "\r\n"))


def test_run_check_bat_helper_failures(root):
    # Fix round 1, I1: `if errorlevel 1` is false for a NEGATIVE exit code. A missing run_one.ps1 (the bat started from a zip viewer, or the
    # helper removed by an antivirus) made PowerShell exit -196608 and the bat print "DONE. Send the file results.zip" with no zip.
    b = runnable_bundle(root, ("hf",))
    bundle = b["dir"]
    helper = file_bytes(os.path.join(bundle, "run_one.ps1"))
    os.remove(os.path.join(bundle, "run_one.ps1"))
    for script in ("run_check.bat", "get_game_files.bat"):
        code, text = run_bat(bundle, script)
        check(code == 1 and "run_one.ps1 is missing" in text and "DONE. Send" not in text, f"{script}, no run_one.ps1: says what is wrong, no DONE: exit {code}\n{text}")
    check(not os.path.exists(os.path.join(bundle, "out")) and not os.path.exists(os.path.join(bundle, "results.zip")), "no run_one.ps1: nothing was run or made")
    # the preflight helper fails with a negative code: nothing runs
    write_stub_helper(bundle, check=-196608)
    code, text = run_bat(bundle)
    check(code == 1 and "Nothing was run" in text and "DONE. Send" not in text and not os.path.exists(os.path.join(bundle, "out")),
          f"the preflight exits -196608: nothing is run, no DONE: exit {code}\n{text}")
    # the replay helper and the zip helper fail with a negative code: every recording gets a failed result from the bat itself, and the
    # bat does not claim DONE
    write_stub_helper(bundle, name=-196608, zip=-196608)
    code, text = run_bat(bundle)
    rr_path = os.path.join(bundle, "out", "hf", "out", "run_result.txt")
    rr = file_bytes(rr_path).decode("utf-8") if os.path.exists(rr_path) else ""
    check("did not finish (exit code -196608)" in text and rr.startswith("status failed") and "quit the helper run_one.ps1 did not write a result" in rr,
          f"the replay helper exits -196608: the bat writes a failed run_result itself:\n{text}\n{rr!r}")
    check(code == 1 and "results.zip could not be made" in text and "DONE. Send" not in text, f"the zip helper exits -196608: no DONE: exit {code}\n{text}")
    # (the helper restored: the bundle works again)
    with open(os.path.join(bundle, "run_one.ps1"), "wb") as f:
        f.write(helper)
    code, text = run_bat(bundle)
    check(code == 0 and "DONE" in text, f"the real helper restored: exit {code}\n{text}")


def test_run_check_bat_quoted_game(root):
    # Fix round 1, I2: Explorer's "Copy as path" puts quotes around the folder, so the GAME line reads set "GAME="D:\x y"" and
    # the unquoted `if "%GAME:~-1%"==...` after it ended the batch with "was unexpected at this time", before any pause.
    b = runnable_bundle(root, ("qg",))
    bundle, game = b["dir"], b["game"]
    for what, value in (("quoted", f'"{game}"'), ("quoted with a trailing backslash", f'"{game}\\"'), ("with a trailing backslash", game + "\\")):
        patch_bat(bundle, GAME=value)
        code, text = run_bat(bundle)
        log_path = os.path.join(bundle, "out", "qg", "out", "np_log.txt")
        log = file_bytes(log_path).decode("utf-8", "replace") if os.path.exists(log_path) else ""
        args = (stand_in_lines(log, "args") or [""])[0].split("|")
        check(code == 0 and "DONE" in text and "unexpected" not in text and args[0] == f"--game_data_root={game}",
              f"GAME {what}: the game gets the folder without quotes or trailing backslash: exit {code}, {args[:1]}\n{text}")
        shutil.rmtree(os.path.join(bundle, "out"), ignore_errors=True)


def test_run_check_bat_start_failures(root):
    # Fix round 1, 3d: a game that cannot start gets a hint for 0xC0000135 (a missing Visual C++ runtime) and for 0xC0000139 (an old one:
    # rexruntime.dll imports msvcp140_atomic_wait.dll); another exit code gets neither
    names = ("h_135", "h_139", "h_5")
    b = runnable_bundle(root, names)
    code, text = run_bat(b["dir"], FAKE_GAME_STARTFAIL="_135=-1073741515,_139=-1073741511,_5=5")
    parts = {p.split(" ...", 1)[0]: p for p in text.replace("\r\n", "\n").split("Replaying ")[1:]}
    check(sorted(parts) == sorted(names), f"start failures: the console has a section per recording: {sorted(parts)}")
    missing, old = "a Windows component is missing", "runtime is too old"
    check(missing in parts.get("h_135", "") and old not in parts.get("h_135", ""), f"exit 0xC0000135: the hint for a missing runtime:\n{parts.get('h_135')}")
    check(old in parts.get("h_139", "") and missing not in parts.get("h_139", ""), f"exit 0xC0000139: the hint for an old runtime:\n{parts.get('h_139')}")
    check(missing not in parts.get("h_5", "") and old not in parts.get("h_5", "") and "status failed (exit 5" in parts.get("h_5", ""), f"exit 5: no hint:\n{parts.get('h_5')}")
    check("status failed (exit -1073741515" in parts.get("h_135", "") and "status failed (exit -1073741511" in parts.get("h_139", ""), "the exit codes are reported as they are")
    check(code == 0 and "DONE" in text, f"start failures: the bat goes on to collect the results: exit {code}")


def test_run_check_bat_later_steps_env(root):
    # Fix round 1, item 8: the replays' DOA4_ variables (the last one's DOA4_NP, DOA4_NP_DIR, DOA4_NP_REC) must not reach the DPPS test
    # (nor the system information and the zip step). The stand-in DPPS prints every DOA4_ variable it was started with.
    b = runnable_bundle(root, ("le",))
    bundle = b["dir"]
    shutil.copy(os.path.join(HERE, "fake_dpps.cmd"), os.path.join(bundle, "bin", "fake_dpps.cmd"))
    old, problems = patch_bat(bundle, DPPS="%~dp0bin\\fake_dpps.cmd")
    check(not problems, f"later steps: the DPPS line: {problems}")
    code, text = run_bat(bundle, DOA4_NP_SEED="9", DOA4_PHOTO_TEST="hotkey")
    zpath = os.path.join(bundle, "results.zip")
    dpps = []
    if os.path.exists(zpath) and zipfile.is_zipfile(zpath):
        with zipfile.ZipFile(zpath) as z:
            dpps = z.read("dpps.txt").decode("oem", "replace").splitlines() if "dpps.txt" in z.namelist() else []
    check(code == 0 and "fake dpps ran" in dpps and dpps[-1].strip() == "exit 0", f"later steps: the stand-in DPPS ran and exited 0: {dpps}")
    check(not any(l.startswith("DOA4_") for l in dpps), f"later steps: no DOA4_ variable reached the DPPS test: {dpps}")


def test_get_game_files_bat(root):
    # Fix round 1, the friend has a disc image and no game folder: get_game_files.bat starts the exe normally (not headless) with its
    # user data folder inside the bundle, the game's setup saves the folder it extracted to as doa4_game_files = "..." in
    # bin\dead_or_alive_4.toml (src/dead_or_alive_4_app.h SaveGameFiles/ConfigPath: the exe's own folder, NOT the user data folder), and
    # run_check.bat uses that folder while its GAME line is unset. The setup is stood in for by fake_setup.py, which writes that file.
    b = runnable_bundle(root, ("gg",))
    bundle, game = b["dir"], b["game"]
    for f in ("fake_setup.cmd", "fake_setup.py"):
        shutil.copy(os.path.join(HERE, f), os.path.join(bundle, "bin", f))
    old, problems = patch_bat(bundle, _file="get_game_files.bat", EXE="%~dp0bin\\fake_setup.cmd")
    check(not problems and old.get("EXE") == "%~dp0bin\\dead_or_alive_4.exe", f"get_game_files.bat as shipped: the EXE line {old}, {problems}")
    toml, result = os.path.join(bundle, "bin", "dead_or_alive_4.toml"), os.path.join(bundle, "game_path.txt")
    left_over = {"DOA4_NP": "replay", "DOA4_SETUP_TEST": "folder=C:/nowhere", "DOA4_NP_SEED": "7"}
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game, FAKE_SETUP_DELAY="1.5", **left_over)
    check(code == 0 and "Game files found: " in text and "run_check.bat" in text, f"get_game_files.bat, the setup saved a folder: exit {code}\n{text}")
    log_path = os.path.join(bundle, "setup_ud", "fake_setup.txt")
    log = file_bytes(log_path).decode("utf-8", "replace").splitlines() if os.path.exists(log_path) else []
    cwd = next((l[4:] for l in log if l.startswith("cwd ")), "")
    args = next((l[5:] for l in log if l.startswith("args ")), "|").split("|")
    check(cwd != "" and same_path(cwd, os.path.join(bundle, "bin")), f"get_game_files.bat: the exe starts in bin\\: {cwd!r}")
    window = next((l[len("console-window "):] for l in log if l.startswith("console-window ")), "?")
    check(window in ("none", "hidden"), f"get_game_files.bat: the stand-in setup opened no console window of its own: {window!r}")
    want =[f"--user_data_root={os.path.join(bundle, 'setup_ud')}", f"--log_file={os.path.join(bundle, 'setup_ud', 'setup.log')}"]
    check(len(args) == 2 and all(same_path(a, w) for a, w in zip(args, want)) and not any("headless" in a for a in args),
          f"get_game_files.bat: the exe's arguments (the save and cache folder inside the bundle, not headless): {args}")
    check(not any(l.startswith("env ") for l in log), f"get_game_files.bat: no DOA4_ variable reaches the game: {[l for l in log if l.startswith('env ')]}")
    check(os.path.exists(toml), "get_game_files.bat: the setup's file bin\\dead_or_alive_4.toml exists")
    found = file_bytes(result).decode("utf-8").strip() if os.path.exists(result) else ""
    check(found != "" and same_path(found, game), f"get_game_files.bat: game_path.txt holds the saved folder: {found!r}")
    # run_check.bat with the GAME line as shipped (unset): uses that folder
    patch_bat(bundle, GAME=b["shipped_game"])
    code, text = run_bat(bundle)
    log_path = os.path.join(bundle, "out", "gg", "out", "np_log.txt")
    log = file_bytes(log_path).decode("utf-8", "replace") if os.path.exists(log_path) else ""
    args = (stand_in_lines(log, "args") or [""])[0].split("|")
    check(code == 0 and "DONE" in text and "get_game_files.bat found" in text and same_path(args[0].partition("=")[2], game),
          f"run_check.bat, GAME unset: the game files get_game_files.bat found are used: exit {code}, {args[:1]}\n{text}")
    # a GAME line that is set wins over the saved folder
    other = os.path.join(root, "another game")
    os.makedirs(other)
    with open(os.path.join(other, "default.xex"), "wb") as f:
        f.write(b"x")
    patch_bat(bundle, GAME=other)
    code, text = run_bat(bundle)
    log = file_bytes(log_path).decode("utf-8", "replace") if os.path.exists(log_path) else ""
    args = (stand_in_lines(log, "args") or [""])[0].split("|")
    check(code == 0 and same_path(args[0].partition("=")[2], other), f"run_check.bat, GAME set: it wins over the saved folder: exit {code}, {args[:1]}")
    # an empty GAME is the same as unset: the value deleted (the variable is then undefined), or only its quotes left
    for what, value in (("deleted", ""), ("only quotes", '""')):
        patch_bat(bundle, GAME=value)
        code, text = run_bat(bundle)
        log = file_bytes(log_path).decode("utf-8", "replace") if os.path.exists(log_path) else ""
        args = (stand_in_lines(log, "args") or [""])[0].split("|")
        check(code == 0 and same_path(args[0].partition("=")[2], game), f"run_check.bat, GAME empty ({what}): the saved folder is used: exit {code}, {args[:1]}\n{text}")
    # the user quit the setup (nothing saved): both batch files say so and run_check.bat does not start
    os.remove(toml)
    patch_bat(bundle, GAME=b["shipped_game"])
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game, FAKE_SETUP_MODE="cancel")
    check(code == 1 and "No game files found" in text and "Now double-click" not in text and not os.path.exists(result), f"get_game_files.bat, nothing saved: exit {code}\n{text}")
    shutil.rmtree(os.path.join(bundle, "out"), ignore_errors=True)
    os.remove(os.path.join(bundle, "results.zip"))  # (the earlier good runs left one)
    code, text = run_bat(bundle)
    check(code == 1 and "get_game_files.bat" in text and "default.xex" in text and not os.path.exists(os.path.join(bundle, "out")) and not os.path.exists(os.path.join(bundle, "results.zip")),
          f"run_check.bat, GAME unset and nothing saved: says so, runs nothing: exit {code}\n{text}")
    # a saved folder that has no default.xex (moved, deleted)
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game, FAKE_SETUP_MODE="missing")
    check(code == 1 and "has no default.xex" in text, f"get_game_files.bat, the saved folder has no default.xex: exit {code}\n{text}")


def test_find_game(root):
    # run_one.ps1 -FindGame reads the folder the game's setup saved: the top-level line doa4_game_files = "..." of dead_or_alive_4.toml
    # (the line shape of src/doa4_config.cpp UpsertTomlKey/TomlString: forward slashes, \ and " escaped; the real file
    # suits_play/bin/dead_or_alive_4.toml has doa4_game_files = "C:/Users/.../DOA4" among other top-level keys)
    script = os.path.join(TOOLS, "np_bundle", "run_one.ps1")
    games = {}
    for tag, name in (("one", "g one"), ("two", "g (2)"), ("bare", "g bare")):
        games[tag] = os.path.join(root, name)
        os.makedirs(games[tag])
        if tag != "bare":
            with open(os.path.join(games[tag], "default.xex"), "wb") as f:
                f.write(b"x")
    fwd = lambda p: p.replace("\\", "/")  # noqa: E731

    def find(name, toml, stale=None, *extra):
        path, result = os.path.join(root, name + ".toml"), os.path.join(root, name + ".result.txt")
        with open(path, "wb") as f:
            f.write(toml if isinstance(toml, bytes) else toml.encode("utf-8"))
        if stale is not None:
            with open(result, "w") as f:
                f.write(stale)
        p = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script, "-FindGame", "-Toml", path,
                            "-Result", result, *extra], capture_output=True, timeout=120)
        return p.returncode, (p.stdout + p.stderr).decode("oem", errors="replace"), (file_bytes(result).decode("utf-8") if os.path.exists(result) else None)

    want = os.path.normpath(games["one"]) + "\r\n"
    code, text, res = find("plain", f'fullscreen = true\ndoa4_game_files = "{fwd(games["one"])}"\nresolution_scale = 4\n')
    check(code == 0 and res == want, f"find_game, the saved line: exit {code}, result {res!r} (wanted {want!r})\n{text}")
    code, text, res = find("crlf_bom", "\ufeffa = 1\r\ndoa4_game_files = \"" + fwd(games["one"]) + "\" # chosen\r\nb = 2\r\n")
    check(code == 0 and res == want, f"find_game, BOM, CRLF and a comment after the value: exit {code}, result {res!r}")
    code, text, res = find("escaped", 'doa4_game_files = "' + games["one"].replace("\\", "\\\\") + '"\n')
    check(code == 0 and res == want, f"find_game, backslashes escaped as TOML writes them: exit {code}, result {res!r}")
    code, text, res = find("spaces", f'doa4_game_files   =   "{fwd(games["two"])}"   \n')
    check(code == 0 and res == os.path.normpath(games["two"]) + "\r\n", f"find_game, spaces and parentheses in the folder name: exit {code}, result {res!r}")
    code, text, res = find("decoy_first", f'[net]\ndoa4_game_files = "{fwd(games["one"])}"\n')
    check(code == 1 and res is None and "No game files found" in text, f"find_game, the key only inside a [table] is another key: exit {code}, result {res!r}\n{text}")
    code, text, res = find("decoy_after", f'x = 1\ndoa4_game_files = "{fwd(games["two"])}"\n[net]\ndoa4_game_files = "{fwd(games["one"])}"\n')
    check(code == 0 and res == os.path.normpath(games["two"]) + "\r\n", f"find_game, the top-level line wins over a table's: exit {code}, result {res!r}")
    code, text, res = find("no_key", "fullscreen = true\n")
    check(code == 1 and res is None and "No game files found" in text, f"find_game, no such line: exit {code}\n{text}")
    code, text, res = find("empty_value", 'doa4_game_files = ""\n')
    check(code == 1 and res is None, f"find_game, an empty value: exit {code}, result {res!r}")
    code, text, res = find("upper_case", f'DOA4_GAME_FILES = "{fwd(games["one"])}"\n')
    check(code == 1 and res is None, f"find_game, TOML keys are case-sensitive: exit {code}, result {res!r}")
    code, text, res = find("literal_string", f"doa4_game_files = '{fwd(games['one'])}'\n")
    check(code == 1 and res is None, f"find_game, a literal string is not what the setup writes: exit {code}, result {res!r}")
    code, text, res = find("other_escape", 'doa4_game_files = "' + fwd(games["one"]) + '\\nmore"\n')
    check(code == 1 and res is None and "escape" in text, f"find_game, an escape the setup does not write: exit {code}\n{text}")
    code, text, res = find("no_xex", f'doa4_game_files = "{fwd(games["bare"])}"\n')
    check(code == 1 and res is None and "has no default.xex" in text, f"find_game, the saved folder has no default.xex: exit {code}\n{text}")
    code, text, res = find("stale", f"x = 1\n", stale="C:\\stale\\folder\r\n")
    check(code == 1 and res is None, f"find_game, a failed search removes an older result: exit {code}, result {res!r}")
    # Fix round 2, N1: a saved folder with a letter that is not plain ASCII (the setup accepts any ANSI-code-page path; "e with
    # diaeresis" is one): exit 2, the folder named correctly in the message, and NOTHING written to the result file that
    # run_check.bat reads with `set /p` (which would read it in the wrong code page). A folder without default.xex is exit 1.
    accent = os.path.join(root, "g \u00eb folder")
    os.makedirs(accent)
    with open(os.path.join(accent, "default.xex"), "wb") as f:
        f.write(b"x")
    bare_accent = os.path.join(root, "g \u00eb bare")
    os.makedirs(bare_accent)
    code, text, res = find("accent", f'doa4_game_files = "{fwd(accent)}"\n', "C:\\stale\\folder\r\n")
    check(code == 2 and res is None and accent in text and "not plain English" in text, f"find_game, a folder with a special letter: exit {code}, result {res!r}\n{text}")
    check(not any(m in text for m in ("\u00c3\u00ab", "\u251c\u00ab", "\ufffd")), f"find_game, a folder with a special letter: the name is printed without garbling\n{text}")
    code, text, res = find("accent_bare", f'doa4_game_files = "{fwd(bare_accent)}"\n')
    check(code == 1 and res is None and "has no default.xex" in text, f"find_game, a special-letter folder without default.xex is exit 1: exit {code}\n{text}")
    # -Quiet: nothing is said when nothing is saved (the first run of get_game_files.bat); -Prefix words the first line of a success
    code, text, res = find("quiet", "x = 1\n", None, "-Quiet")
    check(code == 1 and text.strip() == "", f"find_game -Quiet, nothing saved: exit {code}, output {text!r}")
    code, text, res = find("quiet_found", f'doa4_game_files = "{fwd(games["one"])}"\n', None, "-Quiet", "-Prefix", "Game files already set up")
    check(code == 0 and text.strip() == f"Game files already set up: {os.path.normpath(games['one'])}", f"find_game -Quiet -Prefix, a saved folder: exit {code}, output {text!r}")
    code, text, res = find("quiet_accent", f'doa4_game_files = "{fwd(accent)}"\n', None, "-Quiet")
    check(code == 2 and accent in text, f"find_game -Quiet still explains a special-letter folder: exit {code}\n{text}")
    p = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script, "-FindGame", "-Toml", os.path.join(root, "nope.toml"),
                        "-Result", os.path.join(root, "nope.result.txt")], capture_output=True, timeout=120)
    check(p.returncode == 1 and not os.path.exists(os.path.join(root, "nope.result.txt")), f"find_game, no toml file: exit {p.returncode}")


def write_saved_toml(bundle, folder):
    """bin\\dead_or_alive_4.toml as the game's setup leaves it (fake_setup.py): other keys, the folder with forward slashes in UTF-8
    (Utf8Generic), then a table with a decoy key of the same name."""
    text = folder.replace("\\", "/")
    with open(os.path.join(bundle, "bin", "dead_or_alive_4.toml"), "w", newline="", encoding="utf-8") as f:
        f.write(f'fullscreen = true\r\ndoa4_game_files = "{text}"\r\nresolution_scale = 4\r\n\r\n[net]\r\ndoa4_game_files = "decoy"\r\n')


def setup_bundle(root, names=("gg",), game_name="game dir (x86)"):
    """runnable_bundle + the setup stand-in in bin\\ and get_game_files.bat pointed at it. Returns the runnable_bundle dict."""
    b = runnable_bundle(root, names, game_name=game_name)
    for f in ("fake_setup.cmd", "fake_setup.py"):
        shutil.copy(os.path.join(HERE, f), os.path.join(b["dir"], "bin", f))
    old, problems = patch_bat(b["dir"], _file="get_game_files.bat", EXE="%~dp0bin\\fake_setup.cmd")
    check(not problems, f"setup_bundle: get_game_files.bat EXE line: {problems}")
    return b


MOJIBAKE = ("\u00c3\u00ab", "\u251c\u00ab", "\ufffd")  # an e with diaeresis read as UTF-8 in Latin-1, in the OEM code page 437, a replacement char


def test_get_game_files_second_run(root):
    # Fix round 2, N6: a second run with game files already set up says so (the folder named) and does not open the setup window again
    b = setup_bundle(root)
    bundle, game = b["dir"], b["game"]
    log_path = os.path.join(bundle, "setup_ud", "fake_setup.txt")
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game)
    check(code == 0 and "Game files found: " in text and "No game files found" not in text, f"get_game_files.bat, first run: exit {code}\n{text}")
    check("The game's setup window opens now" in text and 'ALWAYS click "Change folder..."' in text and "Extract to: C:\\DOA4" in text,
          f"get_game_files.bat, first run: the instructions (always change the folder to C:\\DOA4)\n{text}")
    check(os.path.exists(log_path), "get_game_files.bat, first run: the setup was started")
    os.remove(log_path)
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game)
    check(code == 0 and f"Game files already set up: {os.path.normpath(game)}" in text and "Now double-click run_check.bat" in text,
          f"get_game_files.bat, second run: says the files are set up, naming the folder: exit {code}\n{text}")
    check("setup window opens" not in text and not os.path.exists(log_path), f"get_game_files.bat, second run: the setup window is not opened again\n{text}")


def test_get_game_files_special_letters(root):
    # Fix round 2, N1: the setup accepts any folder the ANSI code page can write ("e with diaeresis" is one), this test does not.
    # The saved folder is named correctly (no garbled letters), nothing reads it in the wrong code page (game_path.txt is never
    # written), the setup is made to appear again (the line is removed from the toml), and run_check.bat says the same.
    b = setup_bundle(root)
    bundle, game = b["dir"], b["game"]
    bad = os.path.join(root, "g\u00eb ames")
    os.makedirs(bad)
    with open(os.path.join(bad, "default.xex"), "wb") as f:
        f.write(b"x")
    toml, result = os.path.join(bundle, "bin", "dead_or_alive_4.toml"), os.path.join(bundle, "game_path.txt")
    log_path = os.path.join(bundle, "setup_ud", "fake_setup.txt")
    # (1) the setup saves a folder with the letter: get_game_files.bat explains, clears the saved folder, and ends
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=bad)
    check(code == 1 and bad in text and "special letter" in text and "Now double-click" not in text, f"get_game_files.bat, a saved folder with a special letter: exit {code}\n{text}")
    check('click "Choose extracted folder..." and pick C:\\DOA4' in text and 'pick your .iso, click "Change folder..."' in text,
          f"get_game_files.bat, a special-letter folder: the two ways out (the first-run instructions also say Change folder: the exact words of each route are checked)\n{text}")
    check(not any(m in text for m in MOJIBAKE), f"get_game_files.bat, a special-letter folder: no garbled letters in any message\n{text}")
    saved = file_bytes(toml).decode("utf-8") if os.path.exists(toml) else ""
    check('fullscreen = true\r\n' in saved and 'resolution_scale = 4\r\n' in saved and '[net]\r\ndoa4_game_files = "decoy"\r\n' in saved and 'doa4_game_files = "' + bad.replace("\\", "/") not in saved,
          f"get_game_files.bat, a special-letter folder: the saved line is gone, the rest of the file is as it was:\n{saved!r}")
    check(not os.path.exists(result), "get_game_files.bat, a special-letter folder: game_path.txt is never written")
    # (2) run_check.bat with its GAME line unset and the special-letter folder saved: says why, runs nothing
    write_saved_toml(bundle, bad)
    patch_bat(bundle, GAME=b["shipped_game"])
    code, text = run_bat(bundle)
    check(code == 1 and bad in text and "not plain English" in text and "Run get_game_files.bat" in text and "default.xex was not found" not in text and not os.path.exists(os.path.join(bundle, "out")),
          f"run_check.bat, a saved folder with a special letter: names it, runs nothing: exit {code}\n{text}")
    check(not any(m in text for m in MOJIBAKE) and not os.path.exists(result), f"run_check.bat, a saved folder with a special letter: no garbled letters, no game_path.txt\n{text}")
    # (3) get_game_files.bat again with that folder still saved: explains and clears it WITHOUT starting the setup
    os.path.exists(log_path) and os.remove(log_path)
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game)
    check(code == 1 and bad in text and not os.path.exists(log_path), f"get_game_files.bat, a special-letter folder still saved: explains, does not start the setup: exit {code}\n{text}")
    # (4) the next run opens the setup (nothing saved any more) and, with a plain folder chosen, succeeds
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game)
    check(code == 0 and "Game files found: " in text and os.path.exists(log_path), f"get_game_files.bat, after the saved folder was cleared: the setup opens again: exit {code}\n{text}")
    # (5) a special-letter extraction whose folder was deleted afterwards is "no game files", not a special-letter explanation
    shutil.rmtree(bad)
    write_saved_toml(bundle, bad)
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game, FAKE_SETUP_MODE="cancel")
    check(code == 1 and "No game files found" in text and "has no default.xex" in text, f"get_game_files.bat, a saved special-letter folder that is gone: exit {code}\n{text}")


def test_get_game_files_setup_exit(root):
    # Fix round 2, N4: the setup is where a missing Visual C++ runtime shows first; it gets the same hints as a replay, and stops.
    # Any other exit code (the game closed with Alt+F4) is not an error: the next step looks for the saved folder.
    b = setup_bundle(root)
    bundle, game = b["dir"], b["game"]
    missing, old = "a Windows component is missing", "runtime is too old"
    for what, exit_code, want, other in (("0xC0000135", "-1073741515", missing, old), ("0xC0000139", "-1073741511", old, missing)):
        code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game, FAKE_SETUP_EXIT=exit_code)
        check(code == 1 and want in text and other not in text and "The game's setup could not be started" in text and "Now double-click" not in text,
              f"get_game_files.bat, the setup exits {what}: the hint, then stops: exit {code}\n{text}")
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game, FAKE_SETUP_EXIT="3")
    check(code == 1 and missing not in text and old not in text and "could not be started" not in text and "No game files found" in text,
          f"get_game_files.bat, the setup exits 3 (closed): no hint, the next step reports the missing game files: exit {code}\n{text}")


def test_forget_game(root):
    # run_one.ps1 -ForgetGame removes the top-level doa4_game_files line and nothing else (BOM, line ends, other keys, tables, other letters)
    script = os.path.join(TOOLS, "np_bundle", "run_one.ps1")
    e = "\u00eb"

    def forget(name, data):
        path = os.path.join(root, name + ".toml")
        if data is not None:
            with open(path, "wb") as f:
                f.write(data)
        p = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script, "-ForgetGame", "-Toml", path],
                           capture_output=True, timeout=120)
        return p.returncode, (p.stdout + p.stderr).decode("oem", errors="replace"), (file_bytes(path) if os.path.exists(path) else None)

    bom = bytes([0xEF, 0xBB, 0xBF])
    a = f"a = 1\r\n# {e} comment\r\n".encode("utf-8")
    line = b'doa4_game_files = "C:/x y/DOA4"\r\n'
    c = b'resolution_scale = 4\r\n\r\n[net]\r\ndoa4_game_files = "decoy"\r\n'
    for what, data, want in (("with a BOM, CRLF, other keys and a table", bom + a + line + c, bom + a + c),
                             ("LF line ends", b"a = 1\n" + line.replace(b"\r\n", b"\n") + b"b = 2\n", b"a = 1\nb = 2\n"),
                             ("as the last line without a line end", b"a = 1\r\n" + line.rstrip(b"\r\n"), b"a = 1\r\n"),
                             ("two top-level lines", line + a + line, a),
                             ("only a table's line", b"[net]\r\n" + line, b"[net]\r\n" + line),
                             ("no such line", a + c, a + c),
                             ("another case of the key", b'DOA4_GAME_FILES = "x"\r\n', b'DOA4_GAME_FILES = "x"\r\n')):
        code, text, got = forget("g", data)
        check(code == 0 and got == want, f"forget_game, {what}: exit {code}\n got {got!r}\nwant {want!r}")
        check(not os.path.exists(os.path.join(root, "g.toml.tmp")), f"forget_game, {what}: no temporary file is left")
        if os.path.exists(os.path.join(root, "g.toml")):
            os.remove(os.path.join(root, "g.toml"))
    code, text, got = forget("missing", None)
    check(code == 0 and got is None, f"forget_game, no toml file: nothing is created: exit {code}, {got!r}")
    code, text, got = forget("said", line + a)
    check("will ask again" in text, f"forget_game says the setup will ask again when it removed the line: {text!r}")
    code, text, got = forget("quiet_when_nothing", a)
    check("will ask again" not in text, f"forget_game says nothing when there was no line: {text!r}")


def test_run_check_bat_ampersand(root):
    # Fix round 2, N2/N3: a folder name with an & sign. Typed between the quotes of the GAME line it works. Pasted WITH its own quotes the
    # line itself breaks before the batch can do anything (cmd ends the command at the & outside the quotes): the README says so; the
    # batch then stops at its preflight with a message, never at DONE. The saved-folder route echoes the folder in quotes (N3).
    b = setup_bundle(root, ("am",), game_name="A & B (x)")
    bundle, game = b["dir"], b["game"]
    patch_bat(bundle, GAME=game)
    code, text = run_bat(bundle)
    log_path = os.path.join(bundle, "out", "am", "out", "np_log.txt")
    log = file_bytes(log_path).decode("utf-8", "replace") if os.path.exists(log_path) else ""
    args = (stand_in_lines(log, "args") or [""])[0].split("|")
    check(code == 0 and "DONE. Send" in text and args[0] == f"--game_data_root={game}", f"GAME with an & sign, typed between the quotes: exit {code}, {args[:1]}\n{text}")
    shutil.rmtree(os.path.join(bundle, "out"), ignore_errors=True)
    patch_bat(bundle, GAME=f'"{game}"')
    code, text = run_bat(bundle)
    check(code == 1 and "DONE. Send" not in text and not os.path.exists(os.path.join(bundle, "out")), f"GAME with an & sign, pasted with its own quotes: stops before running: exit {code}\n{text}")
    patch_bat(bundle, GAME=b["shipped_game"])
    code, text = run_bat(bundle, "get_game_files.bat", FAKE_SETUP_GAME=game)
    check(code == 0 and "Game files found: " in text, f"get_game_files.bat, a folder with an & sign: exit {code}\n{text}")
    code, text = run_bat(bundle)
    args = (stand_in_lines(file_bytes(log_path).decode("utf-8", "replace") if os.path.exists(log_path) else "", "args") or [""])[0].split("|")
    check(code == 0 and f'found: "{game}"' in text and "is not recognized" not in text and args[0] == f"--game_data_root={game}",
          f"run_check.bat, the saved folder has an & sign: the echo is quoted and nothing breaks: exit {code}, {args[:1]}\n{text}")


def test_crosscheck_unfinished_runs(root):
    # Fix round 1, item 5: a friend run that never got as far as a digest (a refused recording: no digest.d4d, sweep.d4s or counters.d4k)
    # is one clean line, not a FileNotFoundError traceback from np_compare; and a file np_compare cannot read is one line too
    good = fixture_text("dpps_canon_sample.txt")
    sub = lambda n: os.path.join(root, n)  # noqa: E731
    refusal = "refused: the recording was made with a different build 0123456789abcdef vs b3d118544499d1e8"
    res, local = make_results(sub("t1"), dpps=good)
    friend_out = os.path.join(res, "out", "ra", "out")
    for f in ("digest.d4d", "sweep.d4s", "counters.d4k"):
        os.remove(os.path.join(friend_out, f))
    with open(os.path.join(friend_out, "run_result.txt"), "w") as f:
        f.write(f"status failed\nexit 0\ntimeout 0\nquit {refusal}\n")
    with open(os.path.join(friend_out, "np_log.txt"), "w") as f:
        f.write(f"replay C:\\x\\recs\\ra.d4r: 12 frames, seed 1\n{refusal}\n")
    code, out = crosscheck_run(res, local)
    check(code == 1 and "== ra: DIFFERS" in out and "the friend run has no digest.d4d" in out and f"quit {refusal}" in out, f"crosscheck, a refused friend run: exit {code}\n{out}")
    check("Traceback" not in out and "FileNotFoundError" not in out, f"crosscheck, a refused friend run: no traceback\n{out}")
    check("== rb: EQUAL" in out and "crosscheck FAILED: 1 of 2 recordings equal" in out, f"crosscheck, a refused friend run: the other recording is still judged\n{out}")
    # the same for this PC's run (a record run that left no digest)
    res, local = make_results(sub("t2"), dpps=good)
    os.remove(os.path.join(local, "rb", "out", "digest.d4d"))
    code, out = crosscheck_run(res, local)
    check(code == 1 and "== rb: DIFFERS" in out and "the local run has no digest.d4d" in out and "Traceback" not in out, f"crosscheck, no local digest: exit {code}\n{out}")
    # a friend sweep file cut inside its header: np_compare raises struct.error; the cross-check says so in one line
    res, local = make_results(sub("t3"), dpps=good)
    with open(os.path.join(res, "out", "ra", "out", "sweep.d4s"), "wb") as f:
        f.write(b"D4NPSWP1xy")
    code, out = crosscheck_run(res, local)
    check(code == 1 and "== ra: DIFFERS" in out and "np_compare could not read the runs" in out and "Traceback" not in out, f"crosscheck, an unreadable sweep: exit {code}\n{out}")
    check("== rb: EQUAL" in out, f"crosscheck, an unreadable sweep: the other recording is still judged\n{out}")


def dumpdiff_labels(root, name, a, b, *extra):
    """The range lines np_dumpdiff.py prints for two dumps (the program itself, run as a user runs it)."""
    pa, pb = os.path.join(root, name + "_a.bin"), os.path.join(root, name + "_b.bin")
    for p, data in ((pa, a), (pb, b)):
        with open(p, "wb") as f:
            f.write(data)
    p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_dumpdiff.py"), pa, pb, *extra], capture_output=True, text=True, timeout=60)
    return p.returncode, [l for l in p.stdout.splitlines() if re.match(r"\s+[0-9A-F]{8} \+", l)]


def test_dumpdiff_label(root):
    # 11c review minor 2: the "SDK handle stash" label of np_dumpdiff.py itself (its only_stash test), through the program on
    # dumps, not a re-implementation of the filter. Same fixtures as test_stash_words.
    sig, other, sig1 = b"REX\x00", b"REY\x00", b"REX\x01"
    stash = "SDK handle stash"
    code, lines = dumpdiff_labels(root, "s1", stash_memory(sig, 0xF8000054), stash_memory(sig, 0xF800005C))
    check(code == 0 and len(lines) == 1 and stash in lines[0], f"dumpdiff: a differing stash word (signature in both) is labelled: {lines}")
    code, lines = dumpdiff_labels(root, "s2", stash_memory(other, 0xF8000054), stash_memory(other, 0xF800005C))
    check(code == 0 and len(lines) == 1 and stash not in lines[0], f"dumpdiff: the same difference without the signature is not labelled: {lines}")
    code, lines = dumpdiff_labels(root, "s3", stash_memory(sig, 0xF8000054), stash_memory(sig1, 0xF8000054))
    check(len(lines) == 1 and stash not in lines[0], f"dumpdiff: a difference in the signature word is not labelled: {lines}")
    # a stash word and the word after it differ and the two differences are one range (gap 16): not labelled, a non-stash word differs
    code, lines = dumpdiff_labels(root, "s4", stash_memory(sig, 0xF8000054, 0x11223344), stash_memory(sig, 0xF800005C, 0x11223345))
    check(len(lines) == 1 and stash not in lines[0], f"dumpdiff: a range with a non-stash word in it is not labelled: {lines}")
    # the same dumps with --gap 0: two ranges, the stash word's is labelled and the other is not
    code, lines = dumpdiff_labels(root, "s5", stash_memory(sig, 0xF8000054, 0x11223344), stash_memory(sig, 0xF800005C, 0x11223345), "--gap", "0")
    check(len(lines) == 2 and stash in lines[0] and stash not in lines[1], f"dumpdiff --gap 0: only the stash word's range is labelled: {lines}")
    # a word is a stash word for the label only if it is one in BOTH dumps: "signature, signature, handle" in one dump and
    # "signature, other signature, handle" in the other (the second signature word differs, the handle differs): the handle
    # word is a stash word in the first dump only, so the range is not labelled (a union of the two sets would label it)
    chain = lambda second, handle: b"\x77" * 12 + sig + second + struct.pack(">I", handle) + b"\x77" * 12  # noqa: E731
    code, lines = dumpdiff_labels(root, "s7", chain(sig, 0xF8000054), chain(sig1, 0xF800005C))
    check(len(lines) == 1 and stash not in lines[0], f"dumpdiff: a stash word in one dump only is not labelled: {lines}")
    # (control for that fixture: with the signature in both, the same handle difference is labelled)
    code, lines = dumpdiff_labels(root, "s7c", chain(sig, 0xF8000054), chain(sig, 0xF800005C))
    check(len(lines) == 1 and stash in lines[0], f"dumpdiff: fixture control, the signature in both dumps: {lines}")
    # equal dumps: no ranges, nothing labelled
    code, lines = dumpdiff_labels(root, "s6", stash_memory(sig, 0xF8000054), stash_memory(sig, 0xF8000054))
    check(code == 0 and lines == [], f"dumpdiff: equal dumps print no range: {lines}")


# ---------------------------------------------------------------------------------------------------------------------
# Task 10: the hidden launch, the dot-product self-test rule, and the two-copy harness np_pair.py on the online stand-in
# (fake_online.py). Every pair runs end to end: real processes, the request files, the pids.txt hand-over.
# ---------------------------------------------------------------------------------------------------------------------
def startup_note(runs, name):
    """(wShowWindow, dwFlags) the stand-in read from its own GetStartupInfoW, or None."""
    path = os.path.join(runs, name, "startup.txt")
    m = re.match(r"show_window=(\d+) flags=(0x[0-9a-f]+)", open(path).read()) if os.path.exists(path) else None
    return (int(m.group(1)), int(m.group(2), 16)) if m else None


STARTF_USESHOWWINDOW = 0x1
SW_SHOWMINNOACTIVE = 7


def test_hidden_launch(root):
    # np_run.py starts the game minimized without activation (STARTF_USESHOWWINDOW + SW_SHOWMINNOACTIVE); --show-window turns
    # that off. The stand-in reads its own GetStartupInfoW, so this is what the started process itself sees.
    ud, runs = make_userdata(root), os.path.join(root, "runs")

    def np_run(name, *extra):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_run.py"), "record", name, "--script", SCRIPT,
                            "--userdata", ud, "--runs", runs, "--exe", FAKE_EXE, "--timeout", "60", *extra],
                           capture_output=True, text=True, env=stand_in_env(FAKE_STARTUP_NOTE="1"), timeout=120)
        return p.returncode, p.stdout + p.stderr
    code, out = np_run("hid")
    got = startup_note(runs, "hid")
    check(code == 0 and got is not None, f"np_run hidden launch: run ended well and the stand-in wrote its note: exit {code} {got}\n{out}")
    check(got is not None and got[0] == SW_SHOWMINNOACTIVE and got[1] & STARTF_USESHOWWINDOW,
          f"np_run starts the game with wShowWindow 7 and STARTF_USESHOWWINDOW: {got}")
    code, out = np_run("shown", "--show-window")
    shown = startup_note(runs, "shown")
    check(code == 0 and shown is not None and not shown[1] & STARTF_USESHOWWINDOW and shown[0] != SW_SHOWMINNOACTIVE,
          f"np_run --show-window: no show-window flag, not 7: exit {code} {shown}")
    check(shown != got, "np_run: the startup note differs between a hidden and a shown launch (it is a measurement)")


def test_selftest_rule(root):
    # M2 of Task 1's review: a run whose game.log lacks the dot-product self-test line, or reports OFF, is not a good run.
    mod = load("np_run")
    on = ["x [doa4][LIFE] dot products: canonical NaN on"]
    check(mod.selftest_problem(on) is None, "selftest_problem: the on line is fine")
    check(mod.selftest_problem([]) is not None and mod.selftest_problem(["nothing here"]) is not None,
          "selftest_problem: no line is a problem")
    off = mod.selftest_problem(["a [doa4][LIFE] dot products: canonical NaN OFF (7FC00000 0 0 0)"])
    check(off is not None and "OFF" in off, f"selftest_problem: OFF is a problem: {off!r}")
    check(mod.selftest_problem(on + ["b [doa4][LIFE] dot products: canonical NaN OFF (1 2 3 4)"]) is not None,
          "selftest_problem: one OFF line among on lines is a problem")
    check(mod.selftest_problem(["[doa4][LIFE] dot products: canonical NaN once"]) is not None,
          "selftest_problem: only the word 'on' counts")
    # judge: the otherwise good run fails with the problem and keeps its quit line in the reason
    log = ["quit at frame 11: quit frame"]
    check(mod.judge("record", 0, False, log, None, on) == ("ok", "quit at frame 11: quit frame"), "judge: good run with the self-test line")
    status, why = mod.judge("record", 0, False, log, None, [])
    check(status == "failed" and "NO SELFTEST" in why and "quit at frame 11" in why, f"judge: no self-test line: {status} {why!r}")
    status, why = mod.judge("record", 0, False, log, None, ["[doa4][LIFE] dot products: canonical NaN OFF (1 2 3 4)"])
    check(status == "failed" and "OFF" in why, f"judge: OFF: {status} {why!r}")
    # through np_run and np_gate end to end, with the stand-in leaving the line out / saying OFF
    ud, runs = make_userdata(root), os.path.join(root, "runs")

    def np_run(name, **env):
        p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_run.py"), "record", name, "--script", SCRIPT,
                            "--userdata", ud, "--runs", runs, "--exe", FAKE_EXE, "--timeout", "60"],
                           capture_output=True, text=True, env=stand_in_env(**env), timeout=120)
        return p.returncode, p.stdout + p.stderr
    code, out = np_run("st_ok")
    check(code == 0 and result(runs, "st_ok") == REC_OK, f"np_run, self-test line present: exit {code} {result(runs, 'st_ok')!r}")
    code, out = np_run("st_none", FAKE_NO_SELFTEST="1")
    check(code == 1 and "NO SELFTEST" in (result(runs, "st_none") or ""), f"np_run, no self-test line: exit {code} {result(runs, 'st_none')!r}")
    code, out = np_run("st_off", FAKE_SELFTEST_OFF="1")
    check(code == 1 and "SELFTEST not on" in (result(runs, "st_off") or ""), f"np_run, self-test OFF: exit {code} {result(runs, 'st_off')!r}")
    code, out, runs = gate(root, "sg", "--script", SCRIPT, "--replays", "1", FAKE_NO_SELFTEST="1")
    check(code == 1 and "gate FAILED" in out, f"np_gate, no self-test line: exit {code}\n{out}")


PAIR_SAVE = os.path.join("B13EBABEBABEBABE", "544307D1")


def make_pair_userdata(root):
    ud = os.path.join(root, "pair_ud")
    os.makedirs(os.path.join(ud, PAIR_SAVE), exist_ok=True)
    for name, data in (("save0.bin", b"first save"), ("save1.bin", b"second save")):
        with open(os.path.join(ud, PAIR_SAVE, name), "wb") as f:
            f.write(data)
    return ud


def pair_script(root, quit_at=10):
    path = os.path.join(root, "pair_script.txt")
    with open(path, "w") as f:
        f.write(f"connect 0 1\n{quit_at} -\nquit\n")
    return path


def np_pair(root, name, *args, quit_at=10, tools=None, settle="1.5", **env):
    """np_pair.py end to end on the online stand-in; (exit code, output, runs folder)."""
    runs = os.path.join(root, "pruns")
    cmd = [sys.executable, "-I", os.path.join(tools or TOOLS, "np_pair.py"), name, "--script", pair_script(root, quit_at),
           "--userdata", make_pair_userdata(root), "--runs", runs, "--exe", FAKE_EXE, "--end-grace", "1",
           "--step-timeout", "30", "--timeout", "60", "--orphan-watch", "0.5", *(["--settle", settle] if settle else []), *args]
    p = subprocess.run(cmd, capture_output=True, text=True, env=stand_in_env(**env), timeout=240)
    return p.returncode, p.stdout + p.stderr, runs


def pair_pids(runs, name, side):
    path = os.path.join(runs, f"{name}_{side}", "ctl", "pids.txt")
    return [l.split() for l in open(path).read().splitlines()] if os.path.exists(path) else []


def pair_verdict(runs, name):
    path = os.path.join(runs, f"{name}_pair.txt")
    return open(path).read() if os.path.exists(path) else ""


def leftover_pids(runs, name, sides=("host", "joiner")):
    """The pids of the pair's own pids.txt files that are still alive (there must be none once np_pair has returned)."""
    mod = load("np_pair")
    return [int(row[1]) for side in sides for row in pair_pids(runs, name, side) if mod.pid_alive(int(row[1]))]


def test_pair_ok(root):
    code, out, runs = np_pair(root, "ok")
    check(code == 0 and "verdict ok" in out, f"np_pair, an ok pair: exit {code}\n{out}")
    check("real saves unchanged" in out and "4 save files hashed" in out, f"np_pair, ok: the save check ran on 2 files x 2 copies\n{out}")
    check(pair_verdict(runs, "ok").rstrip().endswith("verdict ok"), f"np_pair, ok: verdict file\n{pair_verdict(runs, 'ok')}")
    for side in ("host", "joiner"):
        kinds = [r[2] for r in pair_pids(runs, "ok", side)]
        check(kinds == ["lobby", "session", "offline"], f"np_pair, ok: {side} pids.txt hand-over {kinds}")
        folder = os.path.join(runs, f"ok_{side}")
        check(all(os.path.exists(os.path.join(folder, n)) for n in ("ud", "ctl", "game.log", "command.txt", "run_result.txt")),
              f"np_pair, ok: {side} run folder has ud, ctl, game.log, command.txt, run_result.txt")
        text = open(os.path.join(folder, "command.txt")).read()
        for needle in ("DOA4_NP_BIND=loopback", "DOA4_NP_STUN=off", "DOA4_NP_UPNP=off", "DOA4_NP_TEST_DIR=",
                       "DOA4_NP_LOCAL_SCRIPT=", "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000", "--headless=true",
                       "--user_data_root="):
            check(needle in text, f"np_pair, ok: {side} command.txt has {needle}")
    st = {side: open(os.path.join(runs, f"ok_{side}", "ctl", "session", "digest.d4d"), "rb").read() for side in ("host", "joiner")}
    check(st["host"] == st["joiner"] and len(st["host"]) > 100, "np_pair, ok: both digests exist and are equal (non-empty)")
    check(leftover_pids(runs, "ok") == [], f"np_pair, ok: no process of the pair is left: {leftover_pids(runs, 'ok')}")


def test_pair_options(root):
    # --delay and --seed reach the host only, --sim both, --env-*/--arg-* their own copy, --cache both
    code, out, runs = np_pair(root, "opt", "--delay", "3", "--seed", "77", "--sim", "40,5,2", "--env-host", "FAKE_X=h",
                              "--env-joiner", "FAKE_X=j", "--arg-host=--netplay_show_stats=true", "--cache", os.path.join(root, "cache"))
    check(code == 0, f"np_pair options: exit {code}\n{out}")
    host = open(os.path.join(runs, "opt_host", "command.txt")).read()
    joiner = open(os.path.join(runs, "opt_joiner", "command.txt")).read()
    check("DOA4_NP_DELAY=3" in host and "DOA4_NP_SEED=77" in host, f"np_pair options: the host has delay and seed\n{host}")
    check("DOA4_NP_DELAY" not in joiner and "DOA4_NP_SEED" not in joiner, f"np_pair options: the joiner has neither\n{joiner}")
    check("DOA4_NP_SIM=40,5,2" in host and "DOA4_NP_SIM=40,5,2" in joiner, "np_pair options: --sim on both sides")
    check("--netplay_show_stats=true" in host and "--netplay_show_stats=true" not in joiner, "np_pair options: --arg-host only on the host")
    check("--cache_path=" in host and "--cache_path=" in joiner, "np_pair options: --cache on both")


def test_pair_diverge(root):
    # digests that differ from frame 7: the verdict names the frame
    code, out, runs = np_pair(root, "dv", "--env-joiner", "FAKE_ONLINE_DIVERGE=7")
    check(code == 1 and "differ at frame 7" in out, f"np_pair, digests differ from frame 7: exit {code}\n{out}")
    check("verdict FAILED" in pair_verdict(runs, "dv"), "np_pair, differ: the verdict file says FAILED")
    check(leftover_pids(runs, "dv") == [], "np_pair, differ: no process of the pair is left")


def test_pair_desync(root):
    # --expect desync: both sides name a desync AND the digests really differ. The control: sides that name a desync while their
    # digests never differ is an expected event that did not happen (#0331).
    both = ("--env-host", "FAKE_ONLINE_END=desync", "--env-joiner", "FAKE_ONLINE_END=desync")
    code, out, runs = np_pair(root, "ds", "--expect", "desync", *both, "--env-joiner", "FAKE_ONLINE_DIVERGE=7")
    check(code == 0 and "verdict desync" in out and "first differing digest frame: 7" in out, f"np_pair, desync expected and seen: exit {code}\n{out}")
    code, out, runs = np_pair(root, "dn", "--expect", "desync", *both)
    check(code == 1 and "never differ" in out, f"np_pair, desync expected but the digests never differ: exit {code}\n{out}")
    code, out, runs = np_pair(root, "dm", "--expect", "desync", "--env-joiner", "FAKE_ONLINE_DIVERGE=7")
    check(code == 1 and "expected a desync" in out, f"np_pair, desync expected, the sides ended with a leave: exit {code}\n{out}")


def test_pair_did_not_run(root):
    code, out, runs = np_pair(root, "nr", "--env-joiner", "FAKE_ONLINE_NO_SESSION=1")
    check(code == 1 and "joiner copy did not run" in out, f"np_pair, one side without a session folder: exit {code}\n{out}")
    check("host copy did not run" not in out, "np_pair, one side without a session folder: only the joiner is named")


def test_pair_end_frames(root):
    # both sides play the same prefix but one ends earlier: equality over a common prefix is not equality (#0311)
    code, out, runs = np_pair(root, "ef", "--env-joiner", "FAKE_ONLINE_QUIT_AT=8")
    check(code == 1 and "ended at different frames: host 10, joiner 8" in out, f"np_pair, different end frames: exit {code}\n{out}")
    check("digests have different lengths: host 10 frames, joiner 8" in out, f"np_pair, different lengths: both lengths named\n{out}")
    # the digest must reach the script's quit frame (10): both sides end at 6 together
    code, out, runs = np_pair(root, "sh", "--env-host", "FAKE_ONLINE_QUIT_AT=6", "--env-joiner", "FAKE_ONLINE_QUIT_AT=6")
    check(code == 1 and "fewer than the script's quit frame 10" in out, f"np_pair, both short of the script's quit frame: exit {code}\n{out}")


def test_pair_judge_rules(root):
    code, out, runs = np_pair(root, "wn", "--env-host", "FAKE_ONLINE_WARN=1")
    check(code == 1 and "np_log holds a WARN line" in out, f"np_pair, a WARN line in one np_log: exit {code}\n{out}")
    code, out, runs = np_pair(root, "fb", "--env-joiner", "FAKE_ONLINE_FALLBACK=1")
    check(code == 1 and "verdict FAILED" in out, f"np_pair, a fallback counter above 0: exit {code}\n{out}")
    check("fb_main_wait" in out or "np_compare failed" in out, f"np_pair, fallback: the counter or np_compare is named\n{out}")
    code, out, runs = np_pair(root, "nc", "--env-host", "FAKE_ONLINE_NO_CHECKSUMS=1")
    check(code == 1 and "no 'checksums handed to GekkoNet' line" in out, f"np_pair, no checksums line: exit {code}\n{out}")
    code, out, runs = np_pair(root, "lc", "--env-joiner", "FAKE_ONLINE_CHECKSUMS=2")
    check(code == 1 and "handed 2 checksums" in out and "fewer than end frame - delay - 1 = 7" in out,
          f"np_pair, too few checksums (10 - 2 - 1 = 7): exit {code}\n{out}")
    # the boundary: end frame - delay - 1 exactly is enough
    code, out, runs = np_pair(root, "lb", "--env-joiner", "FAKE_ONLINE_CHECKSUMS=7")
    check(code == 0, f"np_pair, exactly end frame - delay - 1 checksums: exit {code}\n{out}")
    code, out, runs = np_pair(root, "lr", "--env-host", "FAKE_ONLINE_END=lost")
    check(code == 0, f"np_pair, control: a copy that watches its peer but nobody vanishes ends well: exit {code}\n{out}")
    code, out, runs = np_pair(root, "ie", "--env-host", "FAKE_ONLINE_END=internal: boom")
    check(code == 1 and "ended 'internal: boom'" in out, f"np_pair, an internal end: exit {code}\n{out}")


def test_pair_refusals(root):
    for kind, extra in (("build", ()), ("files", ("--env-joiner", "DOA4_NP_TEST_FILES_SALT=data"))):
        code, out, runs = np_pair(root, f"r_{kind}", "--expect", f"refused-{kind}", "--env-joiner", f"FAKE_ONLINE_REFUSE={kind}", *extra)
        check(code == 0 and f"verdict refused-{kind}" in out, f"np_pair, {kind} refusal expected and seen: exit {code}\n{out}")
        check(all(r[2] != "session" for s in ("host", "joiner") for r in pair_pids(runs, f"r_{kind}", s)),
              f"np_pair, {kind} refusal: no session line in either pids.txt")
    check("data" in pair_verdict(runs, "r_files"), f"np_pair, files refusal: the salted group's name is in the event\n{pair_verdict(runs, 'r_files')}")
    # a real host-side refusal: the build id salted on the joiner only, the host decides
    code, out, runs = np_pair(root, "r_hb", "--expect", "refused-build", "--env-joiner", "DOA4_NP_TEST_BUILD_SALT=1")
    check(code == 0 and "self-built exe" in out, f"np_pair, host refuses a salted build: exit {code}\n{out}")
    code, out, runs = np_pair(root, "r_hf", "--expect", "refused-files", "--env-joiner", "DOA4_NP_TEST_FILES_SALT=audio")
    check(code == 0 and "audio" in out, f"np_pair, host refuses a salted file group (named): exit {code}\n{out}")
    # busy: a third copy joins a host that has its peer
    code, out, runs = np_pair(root, "r_busy", "--expect", "refused-busy", "--third-joiner")
    check(code == 0 and "verdict refused-busy" in out and "busy" in out, f"np_pair, busy refusal of a third joiner: exit {code}\n{out}")
    check(not os.path.exists(os.path.join(runs, "r_busy_host", "ctl", "session")), "np_pair, busy: the pair never started a session")
    # the kind is judged (#0342/#0344): a files refusal where a build refusal is expected, and the reverse
    code, out, runs = np_pair(root, "r_k1", "--expect", "refused-build", "--env-joiner", "FAKE_ONLINE_REFUSE=files")
    check(code == 1 and "not a build refusal" in out, f"np_pair, expect refused-build but files refused: exit {code}\n{out}")
    code, out, runs = np_pair(root, "r_k2", "--expect", "refused-files", "--env-joiner", "FAKE_ONLINE_REFUSE=build")
    check(code == 1 and "not a files refusal" in out, f"np_pair, expect refused-files but build refused: exit {code}\n{out}")
    code, out, runs = np_pair(root, "r_k3", "--expect", "refused-busy", "--env-joiner", "FAKE_ONLINE_REFUSE=build")
    check(code == 1 and "not a busy refusal" in out, f"np_pair, expect refused-busy but build refused: exit {code}\n{out}")
    # a refusal that did not hold: the refused copy reports it and a session starts anyway
    code, out, runs = np_pair(root, "r_leak", "--expect", "refused-build", "--env-joiner", "FAKE_ONLINE_REFUSE=build",
                              "--env-joiner", "FAKE_ONLINE_LEAKY=1", "--env-joiner", "FAKE_ONLINE_FRAME_MS=3000")
    check(any(r[2] == "session" for r in pair_pids(runs, "r_leak", "joiner")), "np_pair, leaky refusal: the joiner's pids.txt has the session line (fixture precondition)")
    check("'in_session'" in out and "has a ctl/session folder" in out, f"np_pair, refused but the copy's state and folder show a session\n{out}")
    check(code == 1 and "a session started on the joiner copy" in out, f"np_pair, refused but a session line exists: exit {code}\n{out}")
    # the control: no refusal where one was expected
    code, out, runs = np_pair(root, "r_none", "--expect", "refused-build")
    check(code == 1 and "expected offline (refused)" in out, f"np_pair, a refusal expected, the pair connected: exit {code}\n{out}")
    check(all(r[2] != "session" for s in ("host", "joiner") for r in pair_pids(runs, "r_none", s)), "np_pair, no refusal: the harness did not start the session")


def test_pair_saves(root):
    code, out, runs = np_pair(root, "sv", "--env-joiner", "FAKE_ONLINE_WRITE_SAVE=1")
    check(code == 1 and "joiner copy's real save changed" in out and "save0.bin" in out, f"np_pair, a session wrote the real save: exit {code}\n{out}")
    check("real saves unchanged" not in out, "np_pair, changed save: the unchanged line must not be printed")
    check("host copy's real save changed" not in out, "np_pair, changed save: only the joiner's copy is named")
    # the pristine itself is only read
    check(open(os.path.join(root, "pair_ud", PAIR_SAVE, "save0.bin"), "rb").read() == b"first save", "np_pair: the pristine save is untouched")
    # a pristine without save files makes the check vacuous: refused
    empty = os.path.join(root, "empty_ud")
    os.makedirs(os.path.join(empty, "cache"))
    runs = os.path.join(root, "eruns")
    p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "np_pair.py"), "ev", "--script", pair_script(root), "--userdata", empty,
                        "--runs", runs, "--exe", FAKE_EXE, "--end-grace", "1", "--step-timeout", "30", "--timeout", "60"],
                       capture_output=True, text=True, env=stand_in_env(), timeout=240)
    check(p.returncode == 1 and "vacuous" in p.stdout, f"np_pair, no save files to hash: exit {p.returncode}\n{p.stdout}{p.stderr}")


def test_pair_own_processes(root):
    # the harness ends only what it started and what its pids.txt files list: another stand-in (a decoy, started here, outside
    # the harness, whose pids.txt sits in the same runs folder) is still alive afterwards.
    runs = os.path.join(root, "pruns")
    decoy_ctl = os.path.join(runs, "decoy", "ctl")
    os.makedirs(decoy_ctl)
    decoy = subprocess.Popen([FAKE_EXE], env=stand_in_env(DOA4_NP_TEST_DIR=decoy_ctl, FAKE_ONLINE_ROLE="decoy"),
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mod = load("np_pair")
    pids_file = os.path.join(decoy_ctl, "pids.txt")
    end = time.time() + 30
    while time.time() < end and not os.path.exists(pids_file):
        time.sleep(0.1)
    rows = [l.split() for l in open(pids_file).read().splitlines()] if os.path.exists(pids_file) else []
    decoy_pid = int(rows[0][1]) if rows else None
    try:
        check(decoy_pid is not None and mod.pid_alive(decoy_pid), "np_pair own processes: the decoy stand-in runs (fixture precondition)")
        code, out, runs = np_pair(root, "own")
        check(code == 0, f"np_pair own processes: the pair itself ended well: exit {code}\n{out}")
        check(decoy_pid is not None and mod.pid_alive(decoy_pid), "np_pair own processes: the decoy stand-in is still alive afterwards")
        check(leftover_pids(runs, "own") == [], "np_pair own processes: the pair's own processes are all gone")
    finally:
        if decoy_pid:
            subprocess.run(["taskkill", "/F", "/PID", str(decoy_pid)], capture_output=True)
        decoy.kill()


def test_pair_pid_reuse(root):
    # a pids.txt line whose pid belongs to another process now (reused) is not ours: its creation time does not match the line
    mod = load("np_pair")
    other = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(60)"])
    try:
        ctl = os.path.join(root, "reuse_host", "ctl")
        os.makedirs(ctl)
        with open(os.path.join(ctl, "pids.txt"), "w") as f:
            f.write(f"pid {other.pid} session {int((time.time() - 86400) * 1000)}\n")
        pair = mod.Pair(type("A", (), {"end_grace": 0.5, "orphan_watch": 0.2})())
        pair.sides.append(mod.Side("host", os.path.join(root, "reuse_host")))
        check(other.pid not in pair.owned_pids(), "np_pair: a pid whose creation time does not match its pids.txt line is not owned")
        with open(os.path.join(ctl, "pids.txt"), "w") as f:
            f.write(f"pid {other.pid} session {int(time.time() * 1000)}\n")
        check(other.pid in pair.owned_pids(), "np_pair: the same pid with a matching line is owned (control)")
        pair.end_all()
        time.sleep(0.5)
        check(other.poll() is not None, "np_pair: end_all ended the owned pid (control)")
    finally:
        other.kill()


def test_pair_affinity(root):
    # the joiner's job limit is read back from its live session process (the stand-in widens its own mask the way the runtime
    # does; a job limit holds), and logged in command.txt
    system = system_affinity()
    one = system & -system
    check(system != one, f"np_pair affinity: the test needs at least two CPUs (system {system:#x})")
    slow = ("--env-joiner", "FAKE_ONLINE_FRAME_MS=50", "--env-host", "FAKE_ONLINE_FRAME_MS=50")
    code, out, runs = np_pair(root, "af", "--joiner-affinity", hex(one), *slow, quit_at=30)
    text = open(os.path.join(runs, "af_joiner", "command.txt")).read()
    check(code == 0, f"np_pair affinity: exit {code}\n{out}")
    check(f"affinity read back {one:#x}" in text, f"np_pair affinity: command.txt logs the read-back mask {one:#x}\n{text}")
    check("affinity read back" not in open(os.path.join(runs, "af_host", "command.txt")).read(), "np_pair affinity: the host has no read-back line")
    code, out, runs = np_pair(root, "af0", *slow, quit_at=30)
    check("affinity read back" not in open(os.path.join(runs, "af0_joiner", "command.txt")).read(),
          "np_pair affinity: without --joiner-affinity nothing is read back (control)")


def test_pair_hidden_launch(root):
    code, out, runs = np_pair(root, "hl", FAKE_STARTUP_NOTE="1")
    check(code == 0, f"np_pair hidden launch: exit {code}\n{out}")
    for side in ("host", "joiner"):
        got = startup_note(runs, f"hl_{side}")
        check(got is not None and got[0] == SW_SHOWMINNOACTIVE and got[1] & STARTF_USESHOWWINDOW,
              f"np_pair starts the {side} with wShowWindow 7 and STARTF_USESHOWWINDOW: {got}")
    code, out, runs = np_pair(root, "hl3", "--third-joiner", "--expect", "refused-busy", FAKE_STARTUP_NOTE="1")
    got = startup_note(runs, "hl3_third")
    check(got is not None and got[0] == SW_SHOWMINNOACTIVE and got[1] & STARTF_USESHOWWINDOW,
          f"np_pair starts the third joiner hidden too: {got}")


def test_pair_selftest(root):
    code, out, runs = np_pair(root, "stn", "--env-joiner", "FAKE_NO_SELFTEST=1")
    check(code == 1 and "joiner game.log: NO SELFTEST" in out, f"np_pair, no self-test line in the joiner's game.log: exit {code}\n{out}")
    check("host game.log" not in out, "np_pair, no self-test line: only the joiner is named")
    code, out, runs = np_pair(root, "sto", "--env-host", "FAKE_SELFTEST_OFF=1")
    check(code == 1 and "host game.log: SELFTEST not on" in out, f"np_pair, self-test OFF in the host's game.log: exit {code}\n{out}")


def test_pair_host_only(root):
    code, out, runs = np_pair(root, "ho", "--host-only", "1", "--expect", "lobby")
    check(code == 0 and "verdict lobby" in out and "state=hosting" in out, f"np_pair --host-only: exit {code}\n{out}")
    check(not os.path.exists(os.path.join(runs, "ho_joiner")), "np_pair --host-only: no joiner run folder")
    check(leftover_pids(runs, "ho", ("host",)) == [], "np_pair --host-only: the host is ended")


def test_pair_lost(root):
    # the survivor (host) sees its peer vanish: --kill-joiner-at-frame ends the joiner's own session pid once its np_log passed
    # the frame; the host holds, shows the banner 1.0 s after, ends `lost` 10.0 s after (each within 50 ms)
    slow = ("--env-host", "FAKE_ONLINE_FRAME_MS=100", "--env-joiner", "FAKE_ONLINE_FRAME_MS=100")
    code, out, runs = np_pair(root, "lo", "--expect", "lost", "--kill-joiner-at-frame", "5", "--env-host", "FAKE_ONLINE_END=lost", *slow, quit_at=200)
    check(code == 0 and "verdict lost" in out, f"np_pair, lost: exit {code}\n{out}")
    events = open(os.path.join(runs, "lo_host", "ctl", "session", "events.txt")).read()
    stamps = {m.group(2).split()[0]: int(m.group(1)) for m in re.finditer(r"^(\d+) (hold begin frame \d+|banner waiting|session ended .*)$", events, re.M)}
    check(set(stamps) == {"hold", "banner", "session"}, f"np_pair, lost: the stand-in's events.txt has the three events: {stamps}")
    check(len(stamps) == 3 and abs(stamps["banner"] - stamps["hold"] - 1000) <= 50 and abs(stamps["session"] - stamps["hold"] - 10000) <= 50,
          f"np_pair, lost: the fixture itself keeps 1.0 s / 10.0 s: {stamps}")
    joiner_frames = [l for l in open(os.path.join(runs, "lo_joiner", "ctl", "session", "np_log.txt")).read().splitlines() if l.startswith("frame ")]
    check(0 < len(joiner_frames) < 100, f"np_pair, lost: the joiner was ended mid-session, after {len(joiner_frames)} frame lines")
    check(leftover_pids(runs, "lo") == [], "np_pair, lost: no process of the pair is left")
    # the survivor that does not notice: its reason is not `lost`
    code, out, runs = np_pair(root, "ln", "--expect", "lost", "--kill-joiner-at-frame", "5", *slow, quit_at=40)
    check(code == 1 and "expected 'lost'" in out, f"np_pair, lost expected but the host ended on its own: exit {code}\n{out}")
    # the timing is judged: the stand-in can be told to show its banner late
    code, out, runs = np_pair(root, "lt", "--expect", "lost", "--kill-joiner-at-frame", "5", "--env-host", "FAKE_ONLINE_END=lost",
                              "--env-host", "FAKE_ONLINE_BANNER_S=1.5", *slow, quit_at=200)
    check(code == 1 and "the banner came" in out, f"np_pair, lost with a late banner: exit {code}\n{out}")
    code, out, runs = np_pair(root, "lw", "--expect", "lost", "--env-host", "FAKE_ONLINE_END=lost", *slow)
    check(code == 1 and "needs --kill-joiner-at-frame" in out, f"np_pair, lost without --kill-joiner-at-frame: exit {code}\n{out}")


# ---------------------------------------------------------------------------------------------------------------------
# Task 10 fix round 1 (review I1-I4, M1-M5)
# ---------------------------------------------------------------------------------------------------------------------
def test_pair_refusal_key(root):
    # I1: the kind of a refusal is read from status.txt `refusal=`; the event text is the lobby's REAL one (np_rules.cpp), which
    # holds no keyword the earlier matcher looked for ("busy" is "already in an online session", the build text is "self-built").
    code, out, runs = np_pair(root, "k_busy", "--expect", "refused-busy", "--third-joiner")
    check(code == 0 and "verdict refused-busy" in out, f"np_pair, the real busy text carries no keyword and the key decides: exit {code}\n{out}")
    third = open(os.path.join(runs, "k_busy_third", "ctl", "status.txt")).read()
    check("refusal=busy" in third and "event=The host is already in an online session." in third, f"np_pair, busy: the third copy's status.txt\n{third}")
    code, out, runs = np_pair(root, "k_build", "--expect", "refused-build", "--env-joiner", "FAKE_ONLINE_REFUSE=build")
    check(code == 0 and "self-built exe" in out, f"np_pair, the real build text ('self-built exe'): exit {code}\n{out}")
    code, out, runs = np_pair(root, "k_files", "--expect", "refused-files", "--env-joiner", "FAKE_ONLINE_REFUSE=files",
                              "--env-joiner", "DOA4_NP_TEST_FILES_SALT=audio")
    check(code == 0 and "The game files differ (audio)" in out, f"np_pair, the real files text names the group: exit {code}\n{out}")
    # the salted group must be in the files event
    code, out, runs = np_pair(root, "k_files2", "--expect", "refused-files", "--env-joiner", "FAKE_ONLINE_REFUSE=files",
                              "--env-host", "DOA4_NP_TEST_FILES_SALT=movies")
    check(code == 1 and "does not name the salted group 'movies'" in out, f"np_pair, the files event lacks the salted group: exit {code}\n{out}")
    # a status.txt without the key cannot be judged, and the message says so
    code, out, runs = np_pair(root, "k_nokey", "--expect", "refused-build", "--env-joiner", "FAKE_ONLINE_REFUSE=build",
                              "--env-joiner", "FAKE_ONLINE_NO_REFUSAL_KEY=1")
    check(code == 1 and "has no refusal= key" in out, f"np_pair, status.txt without refusal=: exit {code}\n{out}")


def test_pair_refusal_leak(root):
    # I2: a session that starts 2 s after the refusal is seen with the default settle (8 s), and the refused copy fails on its
    # own state, session folder and hand-off too
    code, out, runs = np_pair(root, "lk", "--expect", "refused-build", "--env-joiner", "FAKE_ONLINE_REFUSE=build",
                              "--env-joiner", "FAKE_ONLINE_LEAKY=1", "--env-joiner", "FAKE_ONLINE_LEAK_S=2", "--env-joiner", "FAKE_ONLINE_FRAME_MS=3000", settle=None)
    check(any(r[2] == "session" for r in pair_pids(runs, "lk", "joiner")), "np_pair, leak after 2 s: the joiner's pids.txt has the session line (fixture precondition)")
    check(code == 1 and "a session started on the joiner copy" in out, f"np_pair, a session 2 s after the refusal: exit {code}\n{out}")
    check("'in_session'" in out and "has a ctl/session folder" in out, f"np_pair, the refused copy's own state and session folder are named\n{out}")
    check(re.search(r'"--settle", type=float, default=8,', open(os.path.join(TOOLS, "np_pair.py")).read()), "np_pair: --settle defaults to 8 s")
    # a hand-off written by the refused copy
    code, out, runs = np_pair(root, "ho2", "--expect", "refused-build", "--env-joiner", "FAKE_ONLINE_REFUSE=build",
                              "--env-joiner", "FAKE_ONLINE_HANDOFF=1")
    check(code == 1 and "wrote ud/netplay/handoff.d4h" in out, f"np_pair, a hand-off file in the refused copy: exit {code}\n{out}")
    check(not any(r[2] == "session" for r in pair_pids(runs, "ho2", "joiner")), "np_pair, hand-off: no session line (the hand-off check is the only one that sees it)")
    # the lobby's own ud/netplay/events.txt is allowed: a plain refusal (the stand-in writes it as the real lobby does) is fine
    code, out, runs = np_pair(root, "ev2", "--expect", "refused-build", "--env-joiner", "FAKE_ONLINE_REFUSE=build")
    events = os.path.join(runs, "ev2_joiner", "ud", "netplay", "events.txt")
    check(os.path.exists(events) and code == 0, f"np_pair, ud/netplay/events.txt present and allowed: exit {code}, events {os.path.exists(events)}\n{out}")


def test_pair_orphans(root):
    # I3: a game that relaunches itself after a close request (the stand-in starts another offline process 1 s after the first was
    # ended) leaves nothing behind: end_all watches pids.txt until it stops growing
    env = dict(FAKE_ONLINE_RELAUNCH="1", FAKE_ONLINE_LINGER="25")
    code, out, runs = np_pair(root, "orph", "--orphan-watch", "4", **env)
    try:
        check(code == 0, f"np_pair orphans: exit {code}\n{out}")
        rows = [r for s in ("host", "joiner") for r in pair_pids(runs, "orph", s) if r[2] == "offline"]
        check(len(rows) == 4, f"np_pair orphans: each copy has its offline process and the relaunched one (fixture precondition): {len(rows)} offline lines")
        left = leftover_pids(runs, "orph")
        check(left == [], f"np_pair orphans: nothing the stand-in started is alive afterwards: {left}")
    finally:
        for s in ("host", "joiner"):
            for r in pair_pids(runs, "orph", s):
                subprocess.run(["taskkill", "/F", "/PID", r[1]], capture_output=True)


def test_pair_delay(root):
    # M2: the checksum floor uses the HOST's delay (the session's), not the larger of both copies' or --delay
    code, out, runs = np_pair(root, "dl", "--env-joiner", "DOA4_NP_DELAY=6", "--env-joiner", "FAKE_ONLINE_CHECKSUMS=4")
    check(code == 1 and "handed 4 checksums" in out and "fewer than end frame - delay - 1 = 7" in out,
          f"np_pair, joiner status delay 6 but the host's 2 rules (10 - 2 - 1 = 7): exit {code}\n{out}")
    check(open(os.path.join(runs, "dl_joiner", "ctl", "session", "np_log.txt")).read().count("delay 2") == 1,
          "np_pair delay: the joiner's session ran the host's delay 2 (fixture precondition)")
    code, out, runs = np_pair(root, "dl2", "--delay", "3", "--env-joiner", "FAKE_ONLINE_CHECKSUMS=6")
    check(code == 0, f"np_pair, --delay 3 on the host (10 - 3 - 1 = 6 is enough): exit {code}\n{out}")


def test_pair_pid_checks(root):
    # M1: a session pid counts as alive only while its creation time matches the pids.txt line; M3: send() never replaces an
    # unread np_ctl.on
    mod = load("np_pair")
    other = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(60)"])
    try:
        side = mod.Side("joiner", os.path.join(root, "m1_joiner"))
        os.makedirs(side.ctl)
        with open(os.path.join(side.ctl, "pids.txt"), "w") as f:
            f.write(f"pid {other.pid} session {int((time.time() - 86400) * 1000)}\n")
        check(mod.pid_alive(other.pid) and not side.session_alive(), "np_pair: a live pid with a stale pids.txt time is not the session")
        with open(os.path.join(side.ctl, "pids.txt"), "w") as f:
            f.write(f"pid {other.pid} session {int(time.time() * 1000)}\n")
        check(side.session_alive(), "np_pair: the same pid with a matching time is the session (control)")
    finally:
        other.kill()
    side = mod.Side("host", os.path.join(root, "m3_host"))
    os.makedirs(side.ctl)
    path = os.path.join(side.ctl, "np_ctl.on")
    with open(path, "w") as f:
        f.write("old\n")
    try:
        side.send("new", timeout=0.3)
        raised = False
    except mod.StepError as e:
        raised = "did not take the previous np_ctl.on" in str(e)
    check(raised, "np_pair: send() raises a StepError when the previous np_ctl.on is unread")
    check(open(path).read() == "old\n" and not os.path.exists(os.path.join(side.ctl, "np_ctl.tmp")), "np_pair: the unread np_ctl.on is not replaced")


def test_helper_consoles_hidden(root):
    # M5: the helper consoles np_gate.py and np_crosscheck.py start get CREATE_NO_WINDOW; np_gate --show-window reaches np_run
    class Shim:
        def __init__(self, calls):
            self.calls = calls

        def __getattr__(self, name):
            return getattr(subprocess, name)

        def Popen(self, cmd, **kw):  # noqa: N802
            self.calls.append((cmd, kw))
            return subprocess.Popen([sys.executable, "-I", "-c", "pass"], creationflags=kw.get("creationflags", 0))

        def run(self, cmd, **kw):
            self.calls.append((cmd, kw))
            return subprocess.run(cmd, **kw)

    NO_WINDOW = 0x08000000
    mod = load("np_gate")
    calls = []
    mod.subprocess = Shim(calls)
    ns = type("A", (), dict(userdata="u", runs="r", timeout=5, exe=None, cache=None, env=[], arg=[], affinity=None, show_window=False))()
    mod.run("record", "n", ns, [])
    ns.show_window = True
    mod.run("record", "n2", ns, [])
    mod.compare("a", "b", root, False)
    popens = [c for c in calls if "np_run.py" in " ".join(c[0])]
    check(len(popens) == 2 and all(c[1].get("creationflags", 0) & NO_WINDOW for c in popens), f"np_gate: the np_run helpers get CREATE_NO_WINDOW: {[c[1].get('creationflags') for c in popens]}")
    check("--show-window" not in popens[0][0] and "--show-window" in popens[1][0], "np_gate: --show-window is passed to np_run only when asked")
    comp = [c for c in calls if "np_compare.py" in " ".join(c[0])]
    check(len(comp) == 1 and comp[0][1].get("creationflags", 0) & NO_WINDOW, "np_gate: the np_compare helper gets CREATE_NO_WINDOW")
    # end to end: the gate's runs start hidden by default and shown with --show-window
    code, out, runs = gate(root, "gh", "--script", SCRIPT, "--replays", "1", FAKE_STARTUP_NOTE="1")
    got = startup_note(runs, "gh_rec")
    check(code == 0 and got is not None and got[0] == SW_SHOWMINNOACTIVE and got[1] & STARTF_USESHOWWINDOW, f"np_gate: a gate run starts hidden: exit {code} {got}")
    code, out, runs = gate(root, "gs", "--script", SCRIPT, "--replays", "1", "--show-window", FAKE_STARTUP_NOTE="1")
    got = startup_note(runs, "gs_rec")
    check(code == 0 and got is not None and not got[1] & STARTF_USESHOWWINDOW, f"np_gate --show-window: the run is not hidden: exit {code} {got}")
    # np_crosscheck: its np_compare helper
    good = fixture_text("dpps_canon_sample.txt")
    res, local = make_results(os.path.join(root, "cc"), dpps=good)
    cc = load("np_crosscheck")
    calls = []
    cc.subprocess = Shim(calls)
    with contextlib.redirect_stdout(io.StringIO()):
        try:
            cc.main([res, "--local", local])
        except SystemExit:
            pass
    comp = [c for c in calls if "np_compare.py" in " ".join(c[0])]
    check(len(comp) >= 1 and all(c[1].get("creationflags", 0) & NO_WINDOW for c in comp), f"np_crosscheck: np_compare helpers get CREATE_NO_WINDOW ({len(comp)} calls)")


def test_np_offline_check_hidden(root):
    # I4: np_offline_check.py starts the game minimized without activation, like np_run.py
    mod = load("np_offline_check")
    ud = make_userdata(root)
    runs = os.path.join(root, "oruns")
    os.makedirs(runs)
    fake_exe = os.path.join(HERE, "fake_offline_game.cmd")
    saved = {k: os.environ.get(k) for k in ("FAKE_GAME_PYTHON", "FAKE_STARTUP_NOTE")}
    old = (mod.RUNS, mod.PRISTINE, mod.CACHE, mod.GAME, mod.RUN_SECONDS)
    try:
        os.environ["FAKE_GAME_PYTHON"] = sys.executable
        os.environ["FAKE_STARTUP_NOTE"] = "1"
        mod.RUNS, mod.PRISTINE, mod.CACHE, mod.GAME, mod.RUN_SECONDS = runs, ud, os.path.join(root, "cache"), root, 2
        with contextlib.redirect_stdout(io.StringIO()):
            mod.main(["np_offline_check.py", fake_exe, "oh"])
    finally:
        (mod.RUNS, mod.PRISTINE, mod.CACHE, mod.GAME, mod.RUN_SECONDS) = old
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    got = startup_note(runs, "oh")
    check(got is not None and got[0] == SW_SHOWMINNOACTIVE and got[1] & STARTF_USESHOWWINDOW, f"np_offline_check starts the game hidden: {got}")


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_") and callable(f)]
    only = os.environ.get("NP_TOOLS_ONLY")  # a probe's fast route: run the tests whose name contains this text (the full run is the default)
    if only:
        tests = [(n, f) for n, f in tests if only in n]
        print(f"NP_TOOLS_ONLY={only}: {len(tests)} tests")
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

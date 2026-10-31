"""Cross-PC check of the phase-0 bundle's results (the friend's PC) against this PC's runs.

  py -3 -I tools/np_crosscheck.py RESULTS --local C:/Users/fsop1/doa4/np_runs [--expect NAME,NAME]

RESULTS is the unzipped results.zip of the bundle's run_check.bat (tools/np_bundle.py, README.txt in the bundle):
  out/<recording>/   a run folder as np_run.py makes one (out/ with np_log.txt, digest.d4d, sweep.d4s, counters.d4k,
                     run_result.txt; game.log, command.txt, stdout.txt ...)
  dpps.txt           the DPPS test's output on the friend's PC
  sysinfo.txt        the friend's OS and CPU (printed, not judged)
  recordings.txt     the recordings the bundle holds, one name a line
Every recording named in recordings.txt (or by --expect, which wins) is compared by np_compare.py with this PC's
record run <local>/<recording>. A recording that has no folder in out/, or no local run folder, is not equal. The DPPS
line judged is the CANONICAL hash ("DPPS canonical hash:", every NaN lane made 0xFFC00000): it must equal this PC's. The
raw "DPPS combined hash:" is printed for information only (the two CPUs return different NaN payloads for it).
Exit 0 only if every recording is EQUAL, at least one was compared, and the canonical hash is equal.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# AMD Ryzen 7 9800X3D, tests/host/dpps_host_test.cpp as built for the friend (np_phase0/friend_test/dpps_host_test_canon.exe,
# SHA-1 369bf81b...): np_phase0/friend_test/dpps_canon_local_2026-10-30.txt, task-11b-report.md
LOCAL_DPPS_CANONICAL = "9358b1f6111e7b0e"
# The same PC, the raw results (what the old exe printed too): information only, never judged
LOCAL_DPPS_COMBINED = "2d0081405299d44a"

# The DPPS test prints the hash of the whole run on one line of its own; the per-class lines before it carry other
# 16-digit hashes, so the line is named, never "a hash".
_DPPS_LINES = {"canonical": "DPPS canonical hash: ", "combined": "DPPS combined hash: "}


def dpps_hash(text, kind="canonical"):
    """The 16-digit hash on the `kind` line ("canonical" or "combined") of the DPPS test's output, or None when there is
    no such line, its hash is not 16 lowercase hex digits, or two such lines disagree."""
    if kind not in _DPPS_LINES:
        raise ValueError(f"unknown DPPS hash kind {kind!r} (canonical or combined)")
    found = set(re.findall(r"^" + re.escape(_DPPS_LINES[kind]) + r"([0-9a-f]{16})(?![0-9a-f])", text, re.M))
    return found.pop() if len(found) == 1 else None


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def out_dir(run):
    return os.path.join(run, "out") if os.path.isdir(os.path.join(run, "out")) else run


def friend_lines(run):
    """The friend's session log lines worth reading beside the comparison: the job-worker mode line (mode 3: one at a
    time) and the frame-period lines (np_session.cpp / np_det.cpp log them every 600 frames)."""
    path = os.path.join(out_dir(run), "np_log.txt")
    if not os.path.exists(path):
        return ["no np_log.txt"]
    return [l.strip() for l in read_text(path).splitlines() if "frame period" in l or l.startswith("job workers: one at a time")]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--local", required=True)
    ap.add_argument("--expect", help="recordings that should have run, comma separated (default: recordings.txt of the results)")
    a = ap.parse_args(argv)
    ok = True
    out = os.path.join(a.results, "out")
    listing = os.path.join(a.results, "recordings.txt")
    if a.expect:
        expected = [n for n in a.expect.split(",") if n]
    elif os.path.exists(listing):
        expected = read_text(listing).split()
    else:
        print("no recordings.txt in the results and no --expect: cannot tell which recordings should have run")
        expected = sorted(os.listdir(out)) if os.path.isdir(out) else []
        ok = False
    if not expected:
        print("no recordings to compare")
        ok = False
    equal = 0
    for name in expected:
        friend = os.path.join(out, name)
        local = os.path.join(a.local, name)
        if not os.path.isdir(friend):
            print(f"== {name}: did not run (no folder out/{name} in the results)")
            ok = False
            continue
        if not os.path.isdir(local):
            print(f"== {name}: no local run folder {local}")
            ok = False
            continue
        result = os.path.join(out_dir(friend), "run_result.txt")
        friend_result = ("; ".join(l.strip() for l in read_text(result).splitlines() if l.strip())
                         if os.path.exists(result) else "no run_result.txt")
        # A run that never got as far as a digest (a refused recording, a game that did not start) has nothing to compare:
        # say so in a line, instead of letting np_compare fail on the missing file.
        no_digest = [who for who, run in (("friend", friend), ("local", local))
                     if not os.path.exists(os.path.join(out_dir(run), "digest.d4d"))]
        if no_digest:
            print(f"== {name}: DIFFERS")
            print(f"  the {' and the '.join(no_digest)} run has no digest.d4d, so there is nothing to compare; friend result: {friend_result}")
            for line in friend_lines(friend):
                print("  friend:", line)
            ok = False
            continue
        p = subprocess.run([sys.executable, "-I", os.path.join(HERE, "np_compare.py"), local, friend], capture_output=True, text=True,
                           creationflags=0x08000000)  # CREATE_NO_WINDOW
        if "Traceback" in p.stderr:  # a file np_compare cannot read: its last line says which; the traceback is not the answer
            print(f"== {name}: DIFFERS")
            print("  np_compare could not read the runs: " + [l for l in p.stderr.splitlines() if l.strip()][-1])
            ok = False
            continue
        print(f"== {name}: {'EQUAL' if p.returncode == 0 else 'DIFFERS'}")
        print(p.stdout + p.stderr)
        equal += p.returncode == 0
        ok &= p.returncode == 0
        print("  friend result: " + friend_result)
        for line in friend_lines(friend):
            print("  friend:", line)
    if os.path.isdir(out):
        for name in sorted(os.listdir(out)):
            if name not in expected and os.path.isdir(os.path.join(out, name)):
                print(f"== {name}: not in recordings.txt (not compared)")
    si = os.path.join(a.results, "sysinfo.txt")
    if os.path.exists(si):
        print("friend's PC:", " | ".join(l.strip() for l in read_text(si).splitlines() if l.strip()))
    dp = os.path.join(a.results, "dpps.txt")
    text = read_text(dp) if os.path.exists(dp) else ""
    canonical, combined = dpps_hash(text, "canonical"), dpps_hash(text, "combined")
    same = canonical == LOCAL_DPPS_CANONICAL
    print(f"DPPS canonical: friend {canonical or 'missing'}, this PC {LOCAL_DPPS_CANONICAL}: {'EQUAL' if same else 'DIFFERS'}")
    print(f"DPPS combined (raw, not judged): friend {combined or 'missing'}, this PC {LOCAL_DPPS_COMBINED}")
    ok &= same
    ok &= equal > 0
    print(f"crosscheck {'PASSED' if ok else 'FAILED'}: {equal} of {len(expected)} recordings equal, DPPS canonical {'equal' if same else 'not equal'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

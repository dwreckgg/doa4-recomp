"""The phase-0 replay gate: record a pad script once, replay the recording N times (in sequence,
then optionally several at the same time), and compare every run with the recording and with
each other. Exit 0 only if every run is good (np_run.py) and every pair is equal (np_compare.py:
lengths, digest, sweep unless --sweep-advisory).

  py -3 tools/np_gate.py NAME --script tools/np_scripts/single.txt --userdata <pristine save folder>
      --runs <folder for run folders> [--replays 2] [--concurrent 3] [--exe ...] [--cache ...]
      [--env KEY=VALUE ...] [--arg=--x=y ...] [--sweep-advisory] [--affinity MASK]
  py -3 tools/np_gate.py NAME --rec <existing rec.d4r> ...   (replays only)

--affinity MASK limits every run it starts to those logical CPUs (np_run.py).
After the pairs the gate prints each run's final progress counters (pr_* at the last counters row):
the comparison only judges that the runs agree, so the values a good build ends with are read here.
A run that differs is reported with its first differing frame and regions (np_compare.py). The
runs use np_run.py, so the pristine save folder is copied for every run and the game's data folder
is only read. --headless=true is always passed (a session's XAM dialogs answer with the default).
"""
import argparse
import itertools
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # py -3 -I does not put the script's folder on the path
sys.dont_write_bytecode = True  # importing np_compare must not leave a __pycache__ in the tools folder
import np_compare  # noqa: E402


def run(mode, name, a, extra):
    cmd = [sys.executable, os.path.join(HERE, "np_run.py"), mode, name, "--userdata", a.userdata,
           "--runs", a.runs, "--timeout", str(a.timeout), "--arg=--headless=true"] + extra
    if a.exe:
        cmd += ["--exe", a.exe]
    if a.cache:
        cmd += ["--cache", a.cache]
    for kv in a.env:
        cmd += ["--env", kv]
    for arg in a.arg:
        cmd.append(f"--arg={arg}")
    if a.affinity:
        cmd += ["--affinity", a.affinity]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def compare(x, y, runs, advisory):
    cmd = [sys.executable, os.path.join(HERE, "np_compare.py"), os.path.join(runs, x),
           os.path.join(runs, y), "--blocks", "0"]
    if advisory:
        cmd.append("--sweep-advisory")
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout.splitlines()


def final_progress(runs, name):
    """The run's progress counters at its last counters row, as np_compare prints them."""
    path = os.path.join(np_compare.out_dir(os.path.join(runs, name)), "counters.d4k")
    if not os.path.exists(path):
        return "none (no counters.d4k)"
    try:
        names, frames = np_compare.read_counters(path)
    except SystemExit as e:
        return f"none ({e})"
    return np_compare.final_progress(names, frames)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--script")
    ap.add_argument("--rec")
    ap.add_argument("--userdata", required=True)
    ap.add_argument("--runs", required=True)
    ap.add_argument("--replays", type=int, default=2)
    ap.add_argument("--concurrent", type=int, default=0, help="extra replays run at the same time")
    ap.add_argument("--exe")
    ap.add_argument("--cache")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--arg", action="append", default=[])
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--sweep-advisory", action="store_true")
    ap.add_argument("--affinity", help="logical CPU mask for every run, e.g. 0x3")
    a = ap.parse_args()
    if not a.script and not a.rec:
        sys.exit("--script or --rec")

    names = []
    failed = []
    if a.script:
        rec_name = a.name + "_rec"
        if run("record", rec_name, a, ["--script", a.script]).wait() != 0:
            sys.exit(f"gate FAILED: the record run failed ({os.path.join(a.runs, rec_name, 'out', 'run_result.txt')})")
        rec = os.path.join(a.runs, rec_name, "out", "rec.d4r")
        names.append(rec_name)
    else:
        rec = a.rec
    for i in range(a.replays):
        n = f"{a.name}_rep{i + 1}"
        if run("replay", n, a, ["--rec", rec]).wait() != 0:
            failed.append(n)
        names.append(n)
    procs = []
    for i in range(a.concurrent):
        n = f"{a.name}_con{i + 1}"
        procs.append((n, run("replay", n, a, ["--rec", rec])))
        names.append(n)
    for n, p in procs:
        if p.wait() != 0:
            failed.append(n)
    for n in failed:
        print(f"{n}: run failed (see its out/run_result.txt)")

    bad = 0
    for x, y in itertools.combinations(names, 2):
        code, lines = compare(x, y, a.runs, a.sweep_advisory)
        head = next((l for l in lines if l.startswith("digest first")), "no digest")
        print(f"{x} vs {y}: {head}" + ("" if code == 0 else " - DIFFERS"))
        if code != 0:
            bad += 1
            for l in lines:
                if not l.startswith("digest first"):
                    print("   ", l.strip())
        else:  # an equal pair still shows what the sweep and the counters compared (frames, blocks, no fallback)
            for l in lines:
                if l.startswith("sweep:") or l.startswith("counters:"):
                    print("   ", l.strip())
    for n in names:
        print(f"{n} final progress: {final_progress(a.runs, n)}")
    ok = bad == 0 and not failed
    print(f"gate {'PASSED' if ok else 'FAILED'}: {len(names)} runs ({len(failed)} failed), "
          f"{len(names) * (len(names) - 1) // 2} pairs, {bad} differ")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

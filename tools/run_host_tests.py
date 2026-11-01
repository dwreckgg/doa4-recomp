"""Runs the host tests (tests/host/run_*_test.bat) one after another and judges each by its own summary line.

  py -3 -I tools/run_host_tests.py [--only NAME ...] [--dir FOLDER] [--timeout SECONDS]

NAME is a runner's middle part (run_np_files_test.bat -> np_files). --dir (tests: another folder of runners) defaults
to this repository's tests/host.
--timeout is the limit for each runner (default 600 s): a runner still running then is stopped with its child
processes and reported "did not run (timed out after N s)", which fails the run.
Each runner runs as `cmd /d /c <runner>` in its own folder with no input; never through `start /wait`, which hides the
exit code (observation 0354). A runner passes only if its summary line says it passed, with at least one check, AND
it exits 0. A runner that prints no summary line never passes ("did not run"). Its summary line is the last line of
its output in its test's own shape (SUMMARY_SHAPES below, checked against the test sources by
tests/tools/test_release_tools.py); a runner not named there prints "PASSED: N checks, 0 failures" / "FAILED: ...".
Prints one line per runner, "<name>: passed N" (N = its check count; a summary without a count prints
"<name>: passed"), "<name>: FAILED (<summary line>; exit N)" or "<name>: did not run (no summary line; exit N)",
then, for a runner that did not pass, the last 40 lines of its output; at the end a count line. Exit 0 only if every
runner passed. The environment passes through to the runners (REXSDK_DIR, DOA4_VSDEVCMD, DOA4_LLVM_BIN, DOA4_CI,
DOA4_NP_GAME_DIR ...).
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TAIL = 40
DEFAULT_TIMEOUT = 600  # seconds per runner

DEFAULT = (re.compile(r"PASSED: (\d+) checks, 0 failures"), re.compile(r"FAILED: \d+ checks, \d+ failures"))
# Runners whose tests print "<name>: all checks passed" / "N check(s) failed" (no check count).
ALL_CHECKS_PASSED = ("backup", "extras", "gamefiles", "graphics", "photo", "photo_service", "photo_watch", "photo_win",
                     "setup_flow", "setup_win", "toml_upsert", "tweaks", "unlocks", "xdvdfs")
SPECIAL = {
    # speakers_host_test.cpp
    "speakers": (re.compile(r"all speakers checks passed"), re.compile(r"\d+ FAILED")),
    # dpps_host_test.cpp prints hashes and this verdict line (its exit code also needs the control lane)
    "dpps": (re.compile(r"canonical NaN results all 0xFFC00000 and the redirect agrees with the SDK function: yes"),
             re.compile(r"canonical NaN results all 0xFFC00000 and the redirect agrees with the SDK function: NO")),
}


def shape_kind(name):
    """'speakers' / 'dpps' / 'all_checks' / 'default': which summary line runner `name` prints."""
    if name in SPECIAL:
        return name
    if name in ALL_CHECKS_PASSED:
        return "all_checks"
    return "default"


def summary_shapes(name):
    """(pass regex, fail regex) for runner `name`; a pass regex's group 1, if any, is the check count."""
    kind = shape_kind(name)
    if kind in SPECIAL:
        return SPECIAL[kind]
    if kind == "all_checks":
        return re.compile(re.escape(name) + r": all checks passed"), re.compile(r"\d+ check\(s\) failed")
    return DEFAULT


def judge(name, output, code):
    """('passed', count or None, line) / ('FAILED', None, why) / ('did not run', None, why)."""
    pass_re, fail_re = summary_shapes(name)
    last = None
    for line in output.splitlines():
        line = line.strip()
        if pass_re.fullmatch(line) or fail_re.fullmatch(line):
            last = line
    if last is None:
        return "did not run", None, f"no summary line; exit {code}"
    m = pass_re.fullmatch(last)
    if not m:
        return "FAILED", None, f"{last}; exit {code}"
    count = int(m.group(1)) if m.groups() else None
    if count == 0:
        return "FAILED", None, f"{last} (no check ran); exit {code}"
    if code != 0:
        return "FAILED", None, f"{last}; exit {code}"
    return "passed", count, last


def runners(folder):
    """{name: path} of every run_<name>_test.bat in `folder`."""
    out = {}
    for f in sorted(os.listdir(folder)):
        m = re.fullmatch(r"run_(\w+)_test\.bat", f, re.IGNORECASE)
        if m:
            out[m.group(1)] = os.path.join(folder, f)
    return out


def run_one(path, timeout=DEFAULT_TIMEOUT):
    """(output, exit code); exit code None when the runner was still running after `timeout` seconds (it and its child
    processes are then killed, and the output so far is returned)."""
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0  # a hidden console: no window on the user's desktop
    p = subprocess.Popen(["cmd.exe", "/d", "/c", path], cwd=os.path.dirname(path), stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=flags)
    try:
        out, _ = p.communicate(timeout=timeout)
        return out.decode("utf-8", errors="replace"), p.returncode
    except subprocess.TimeoutExpired:
        # kill the whole tree: the runner's compiler and test exe are children of cmd, and keep the pipe open
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       creationflags=flags)
        try:
            out, _ = p.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            out = b""
        return out.decode("utf-8", errors="replace"), None


def main(argv=None):
    ap = argparse.ArgumentParser(description="Runs the host tests and judges each by its own summary line.")
    ap.add_argument("--only", nargs="+", metavar="NAME", help="run only these runners (run_<NAME>_test.bat)")
    ap.add_argument("--dir", default=os.path.join(REPO, "tests", "host"), help="the runners' folder (tests: another one)")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="seconds each runner may take (default 600)")
    a = ap.parse_args(argv)
    found = runners(a.dir) if os.path.isdir(a.dir) else {}
    if not found:
        print(f"run_host_tests: no runner (run_*_test.bat) in {a.dir}")
        return 1
    names = sorted(found)
    if a.only:
        unknown = [n for n in a.only if n not in found]
        if unknown:
            print(f"run_host_tests: no runner named {', '.join(unknown)} in {a.dir}")
            return 1
        names = [n for n in names if n in a.only]
    tally = {"passed": 0, "FAILED": 0, "did not run": 0}
    for name in names:
        output, code = run_one(found[name], a.timeout)
        if code is None:  # a hung runner never passes, whatever it printed before it hung
            verdict, count, why = "did not run", None, f"timed out after {a.timeout:g} s"
        else:
            verdict, count, why = judge(name, output, code)
        tally[verdict] += 1
        if verdict == "passed":
            print(f"{name}: passed" + (f" {count}" if count is not None else ""), flush=True)
        else:
            print(f"{name}: {verdict} ({why})", flush=True)
            for line in output.splitlines()[-TAIL:]:
                print(f"    | {line.rstrip()}")
            sys.stdout.flush()
    print(f"run_host_tests: {len(names)} runners: {tally['passed']} passed, {tally['FAILED']} failed, "
          f"{tally['did not run']} did not run")
    return 0 if tally["passed"] == len(names) else 1


if __name__ == "__main__":
    sys.exit(main())

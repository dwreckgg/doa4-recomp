"""Compares two DOA4 netplay phase-0 runs: digest (per-frame region hashes), sweep (rolling
64 KB .data block hashes) and counters (the determinism layer's fallback and progress counters).
Formats: src/net/np_core.h.

  py -3 tools/np_compare.py RUN_A RUN_B [--from F] [--blocks N] [--allow-unchecked] [--sweep-advisory]
RUN_A/RUN_B are run output folders (holding digest.d4d, sweep.d4s and counters.d4k) or run
folders (with out/). Prints the frames compared, the first differing frame of every region (and
the first frame of all), the sweep blocks that differ with the first frame each was seen to
differ, and the counters: every fallback counter (fb_*) above zero in either run (its final value
and the first frame it was above zero) and every progress counter (pr_*) that differs between the
runs, with the first frame it differs at.
--allow-unchecked (runs without run_result.txt or counters.d4k, e.g. the proof's)
--sweep-advisory (report sweep differences without failing)
Exit status: 0 = both runs good, equal lengths, every compared frame equal, no fallback, equal
progress; 1 = otherwise; 2 = nothing to compare.
"""
import argparse
import os
import struct
import sys


def out_dir(p):
    return os.path.join(p, "out") if os.path.isdir(os.path.join(p, "out")) else p


def read_digest(path):
    d = open(path, "rb").read()
    if d[:8] != b"D4NPDIG1":
        raise SystemExit(f"{path}: not a digest file")
    _, n = struct.unpack_from("<II", d, 8)
    regions, off = [], 16
    for _ in range(n):
        addr, length = struct.unpack_from("<II", d, off)
        name = d[off + 8:off + 32].split(b"\0")[0].decode()
        regions.append((name, addr, length))
        off += 32
    rec = 8 + 8 * n
    frames = {}
    while off + rec <= len(d):
        f, _ = struct.unpack_from("<II", d, off)
        frames[f] = struct.unpack_from(f"<{n}Q", d, off + 8)
        off += rec
    return regions, frames


def read_sweep(path):
    d = open(path, "rb").read()
    if d[:8] != b"D4NPSWP1":
        raise SystemExit(f"{path}: not a sweep file")
    _, lo, hi, block, per = struct.unpack_from("<IIIII", d, 8)
    off, rec = 32, 8 + 8 * per
    frames = {}
    while off + rec <= len(d):
        f, first = struct.unpack_from("<II", d, off)
        frames[f] = (first, struct.unpack_from(f"<{per}Q", d, off + 8))
        off += rec
    return (lo, hi, block, per), frames


def read_counters(path):
    d = open(path, "rb").read()
    if d[:8] != b"D4NPCNT1":
        raise SystemExit(f"{path}: not a counters file")
    _, n = struct.unpack_from("<II", d, 8)
    names = [d[16 + 24 * i:16 + 24 * (i + 1)].split(b"\0")[0].decode() for i in range(n)]
    off, rec = 16 + 24 * n, 8 + 4 * n
    frames = {}
    while off + rec <= len(d):
        f, _ = struct.unpack_from("<II", d, off)
        frames[f] = struct.unpack_from(f"<{n}I", d, off + 8)
        off += rec
    return names, frames


def fallbacks(label, names, frames):
    lines = []
    for i, name in enumerate(names):
        if name.startswith("fb_") and frames:
            first = next((f for f in sorted(frames) if frames[f][i]), None)
            if first is not None:
                lines.append(f"  run {label}: {name} = {frames[max(frames)][i]} (first at frame {first})")
    return lines


def judge_counters(a, b, digest_a, digest_b, start, allow_unchecked):
    """Judges the runs' counters.d4k (np_core.h): a fallback counter (fb_*) above zero anywhere in a
    run, a progress counter (pr_*) that differs between the runs at a frame both have (from `start`),
    runs that counted different things, a counters file that does not have a row for every digest
    frame (an empty one reads as a clean run otherwise), and a missing file (unless allow_unchecked:
    then that run is not compared, but a run that has the file is still judged on its own).
    Prints the findings; returns True if the pair fails."""
    problems, found = [], {}
    for label, d, digest in (("A", a, digest_a), ("B", b, digest_b)):
        path = os.path.join(d, "counters.d4k")
        if not os.path.exists(path):
            if allow_unchecked:
                print(f"run {label}: no counters.d4k (allowed)")
            else:
                problems.append(f"  run {label}: no counters.d4k")
            continue
        names, frames = read_counters(path)
        if set(frames) != set(digest):
            problems.append(f"  run {label}: counters.d4k has {len(frames)} rows for {len(digest)} digest frames")
        found[label] = (names, frames)
        problems += fallbacks(label, names, frames)
    common = []
    if len(found) == 2:
        (names_a, frames_a), (names_b, frames_b) = found["A"], found["B"]
        if names_a != names_b:
            problems.append("  the runs counted different things")
        else:
            common = sorted(f for f in frames_a if f in frames_b and f >= start)
            first = {}
            for f in common:
                for i, name in enumerate(names_a):
                    if name.startswith("pr_") and frames_a[f][i] != frames_b[f][i]:
                        first.setdefault(name, f)
            problems += [f"  {name} first differs at frame {f}" for name, f in sorted(first.items(), key=lambda kv: kv[1])]
        print(f"counters: {len(common)} common frames; " +
              ("no fallbacks, progress equal" if not problems else f"{len(problems)} problems"))
    else:
        print("counters: not compared; " + ("no fallbacks in the run that has them" if not problems else f"{len(problems)} problems"))
    for line in problems:
        print(line)
    return bool(problems)


def run_status(d):
    p = os.path.join(d, "run_result.txt")
    if not os.path.exists(p):
        return None
    for line in open(p, encoding="utf-8"):
        if line.startswith("status "):
            return line.split(None, 1)[1].strip()
    return "unknown"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--from", dest="start", type=int, default=0, help="first frame to compare")
    ap.add_argument("--blocks", type=int, default=40, help="sweep blocks to list")
    ap.add_argument("--allow-unchecked", action="store_true")
    ap.add_argument("--sweep-advisory", action="store_true")
    args = ap.parse_args()
    a, b = out_dir(args.a), out_dir(args.b)
    result = 0
    for label, d in (("A", a), ("B", b)):
        st = run_status(d)
        if st is None:
            print(f"run {label}: no run_result.txt" + (" (allowed)" if args.allow_unchecked else ""))
            if not args.allow_unchecked:
                result = 1
        elif st != "ok":
            print(f"run {label} failed: status {st}")
            result = 1
    ra, fa = read_digest(os.path.join(a, "digest.d4d"))
    rb, fb = read_digest(os.path.join(b, "digest.d4d"))
    if ra != rb:
        raise SystemExit("the runs hashed different regions")
    common = sorted(f for f in fa if f in fb and f >= args.start)
    if not common:
        print("no common frames")
        sys.exit(2)
    print(f"digest: {len(common)} common frames ({common[0]}..{common[-1]}); run A {len(fa)}, run B {len(fb)}")
    if len(fa) != len(fb):
        print(f"digest: run lengths differ (A {len(fa)} frames, B {len(fb)} frames)")
        result = 1
    first_any = None
    differ = {}
    for f in common:
        for i, (ha, hb) in enumerate(zip(fa[f], fb[f])):
            if ha != hb:
                differ.setdefault(i, [f, 0])
                differ[i][1] += 1
                if first_any is None:
                    first_any = f
    for i, (name, addr, length) in enumerate(ra):
        if i in differ:
            print(f"  {name:14s} {addr:08X}+{length:<6X} first differs at frame {differ[i][0]} "
                  f"({differ[i][1]} frames differ)")
        else:
            print(f"  {name:14s} {addr:08X}+{length:<6X} equal on all {len(common)} frames")
    print(f"digest first difference: {'none' if first_any is None else 'frame %d' % first_any}")

    if first_any is not None:
        result = 1
    sa, sb = os.path.join(a, "sweep.d4s"), os.path.join(b, "sweep.d4s")
    if os.path.exists(sa) and os.path.exists(sb):
        ga, wa = read_sweep(sa)
        gb, wb = read_sweep(sb)
        if ga != gb:
            print("sweep: different configurations, not compared")
        else:
            lo, hi, block, per = ga
            nblocks = (hi - lo) // block
            scommon = sorted(f for f in wa if f in wb and f >= args.start)
            first_block = {}
            ndiff = 0
            for f in scommon:
                (fa0, ha), (fb0, hb) = wa[f], wb[f]
                for i in range(per):
                    if ha[i] != hb[i]:
                        k = (fa0 + i) % nblocks
                        ndiff += 1
                        first_block.setdefault(k, f)
            print(f"sweep: {len(scommon)} common frames, {per} blocks/frame of {nblocks}; "
                  f"{len(first_block)} blocks ever differ ({ndiff} block-frames)")
            for k, f in sorted(first_block.items(), key=lambda kv: kv[1])[:args.blocks]:
                print(f"  block {lo + k * block:08X} first differs at frame {f}")
            if first_block:
                if args.sweep_advisory:
                    print("  (sweep differences reported only: --sweep-advisory)")
                else:
                    result = 1
    if judge_counters(a, b, fa, fb, args.start, args.allow_unchecked):
        result = 1
    sys.exit(result)


if __name__ == "__main__":
    main()

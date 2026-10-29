"""Compares two DOA4 netplay phase-0 runs: digest (per-frame region hashes) and sweep (rolling
64 KB .data block hashes). Formats: src/net/np_core.h.

  py -3 tools/np_compare.py RUN_A RUN_B [--from F] [--blocks N]
RUN_A/RUN_B are run output folders (holding digest.d4d and sweep.d4s) or run folders (with
out/). Prints the frames compared, the first differing frame of every region (and the first
frame of all), and the sweep blocks that differ with the first frame each was seen to differ.
Exit status: 0 = every compared frame equal, 1 = a difference, 2 = nothing to compare.
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--from", dest="start", type=int, default=0, help="first frame to compare")
    ap.add_argument("--blocks", type=int, default=40, help="sweep blocks to list")
    args = ap.parse_args()
    a, b = out_dir(args.a), out_dir(args.b)
    ra, fa = read_digest(os.path.join(a, "digest.d4d"))
    rb, fb = read_digest(os.path.join(b, "digest.d4d"))
    if ra != rb:
        raise SystemExit("the runs hashed different regions")
    common = sorted(f for f in fa if f in fb and f >= args.start)
    if not common:
        print("no common frames")
        sys.exit(2)
    print(f"digest: {len(common)} common frames ({common[0]}..{common[-1]}); run A {len(fa)}, run B {len(fb)}")
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

    result = 0 if first_any is None else 1
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
                result = 1
    sys.exit(result)


if __name__ == "__main__":
    main()

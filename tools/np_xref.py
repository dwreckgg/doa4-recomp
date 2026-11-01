"""Functions whose absolute-address operands fall in [LO, HI) (hex): who reads or writes a guest
data range. Reads the cross-reference table (function, instruction, operands, address) built from
the generated code.

  py -3 -I tools/np_xref.py LO HI [--max 60] [--table <DOA4_ROOT>/np_phase0/xref/xref2.tsv]

DOA4_ROOT: np_run.py (the environment variable, by default this repository's parent folder).
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # py -3 -I does not put the script's folder on the path
sys.dont_write_bytecode = True
import np_run  # noqa: E402  (DOA4_ROOT)

ap = argparse.ArgumentParser()
ap.add_argument("lo")
ap.add_argument("hi")
ap.add_argument("--max", type=int, default=60)
ap.add_argument("--table", default=os.path.join(np_run.DOA4_ROOT, "np_phase0", "xref", "xref2.tsv"))
a = ap.parse_args()
lo, hi = int(a.lo, 16), int(a.hi, 16)
hits = collections.defaultdict(list)
for line in open(a.table, encoding="utf-8"):
    p = line.rstrip("\n").split("\t")
    if len(p) < 4:
        continue
    try:
        addr = int(p[3], 16)
    except ValueError:
        continue
    if lo <= addr < hi:
        hits[p[0]].append(f"{p[1]} {p[2]} {addr:08X}")
for fn, v in list(hits.items())[:a.max]:
    print(fn, "|", "; ".join(v[:6]), "..." if len(v) > 6 else "")
print(len(hits), "functions")

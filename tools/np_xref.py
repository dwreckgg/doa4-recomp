"""Functions whose absolute-address operands fall in [LO, HI) (hex): who reads or writes a guest
data range. Reads the cross-reference table (function, instruction, operands, address) built from
the generated code.

  py -3 -I tools/np_xref.py LO HI [--max 60] [--table C:/Users/fsop1/doa4/np_phase0/xref/xref2.tsv]
"""
import argparse
import collections

ap = argparse.ArgumentParser()
ap.add_argument("lo")
ap.add_argument("hi")
ap.add_argument("--max", type=int, default=60)
ap.add_argument("--table", default=r"C:/Users/fsop1/doa4/np_phase0/xref/xref2.tsv")
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

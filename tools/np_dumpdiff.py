"""Diffs two .data dumps (DOA4_NP_DUMP / np_ctl.on "dump": dumps/data_<frame>.bin, guest
0x82B70000..0x835B0000) from two runs at the same frame, and lists the differing byte ranges.

  py -3 tools/np_dumpdiff.py A.bin B.bin [--gap 16] [--max 200] [--words]
Ranges closer than --gap bytes are merged. Each range is printed with its guest address,
length, the known region it falls in (or "SDK handle stash" when every differing word in it is the word after a
"REX\\0" header signature in both dumps: stash_words), and (with --words) the big-endian u32 values on both
sides for its first words.
"""
import argparse
import struct

LO = 0x82B70000
KNOWN = [  # (start, length, name): phase0_facts.md 10, rollback_doa4_fit.md
    (0x8320D110, 0x63B0, "game object (+5 fighters at +3836)"),
    (0x830C1CB0, 0x38C0, "netmatch"),
    (0x831011E0, 5 * 44, "pad entries"),
    (0x8357C2D8, 0xD60, "pad manager"),
    (0x83100F28, 24 * 28, "task slots"),
    (0x833F7600, 2512, "MT gameplay"),
    (0x8357B5C0, 2512, "MT 8357B5C0"),
    (0x83420020, 2512, "MT 83420020"),
    (0x8321B198, 2512, "MT 8321B198"),
    (0x831008E0, 12, "frame timing floats"),
    (0x830FE3FC, 4, "frame timing float"),
    (0x82E76210, 8, "frame start time"),
    (0x831013E0, 4, "GetTickCount at present"),
    (0x831013E4, 4, "frame counter"),
    (0x831013E8, 4, "draw CPU time"),
    (0x83100F24, 4, "device+10396 copy"),
    (0x831011D8, 4, "scheduler time"),
    (0x83579C28, 0x4000, "graphics object (GPU)"),
    (0x8341FAA0, 0x100, "job pool"),
]


# The SDK's handle stash in guest object headers (np_core.h kStashSignature; sdk-np0 include/rex/system/xobject.h
# StashHandle): wait_list_flink = "REX\0" (0x52455800 stored big-endian), wait_list_blink = a host handle number, which
# only the SDK reads. The in-game sweep hashes every such word as zero (np_core.cpp ZeroStashWords); dump checks leave
# out a word that is a stash word in both dumps.
STASH_SIGNATURE = b"REX\x00"


def stash_words(data, before=None):
    """Offsets of the SDK handle stash words in `data` (guest memory from a 4-aligned guest address, e.g. a .data dump):
    every 4-aligned word whose preceding aligned word is STASH_SIGNATURE, decided on the data as it is (a signature that
    is itself a stash word still marks the word after it). `before` = the 4 bytes just before data[0], or None. A
    signature in the last word marks nothing here (its stash word is beyond the data)."""
    out = set()
    if before == STASH_SIGNATURE and len(data) >= 4:
        out.add(0)
    at = data.find(STASH_SIGNATURE)
    while at != -1:
        if at % 4 == 0 and at + 8 <= len(data):
            out.add(at + 4)
        at = data.find(STASH_SIGNATURE, at + 1)
    return out


def region(addr):
    for start, length, name in KNOWN:
        if start <= addr < start + length:
            return f"{name} +{addr - start:#x}"
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--gap", type=int, default=16)
    ap.add_argument("--max", type=int, default=200)
    ap.add_argument("--words", action="store_true")
    args = ap.parse_args()
    a, b = open(args.a, "rb").read(), open(args.b, "rb").read()
    n = min(len(a), len(b))
    ranges = []
    i = 0
    while i < n:
        if a[i] != b[i]:
            j = i
            last = i
            while j < n and j - last <= args.gap:
                if a[j] != b[j]:
                    last = j
                j += 1
            ranges.append((i, last + 1))
            i = last + 1
        else:
            i += 1
    total = sum(e - s for s, e in ranges)
    print(f"{len(ranges)} differing ranges, {total} bytes in ranges (dump sizes {len(a)}, {len(b)})")
    stash = stash_words(a) & stash_words(b)
    for s, e in ranges[:args.max]:
        addr = LO + s
        only_stash = all(w in stash for w in range(s & ~3, e, 4) if a[w:w + 4] != b[w:w + 4])
        label = region(addr) or ("SDK handle stash (the sweep hashes it as zero)" if only_stash else "")
        line = f"  {addr:08X} +{e - s:<6d} {label}"
        if args.words:
            w0 = s & ~3
            wa = [struct.unpack_from(">I", a, k)[0] for k in range(w0, min(e, w0 + 16), 4)]
            wb = [struct.unpack_from(">I", b, k)[0] for k in range(w0, min(e, w0 + 16), 4)]
            line += "  A " + " ".join(f"{x:08X}" for x in wa) + " | B " + " ".join(f"{x:08X}" for x in wb)
        print(line)


if __name__ == "__main__":
    main()

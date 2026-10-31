"""Diffs two .data dumps (DOA4_NP_DUMP / np_ctl.on "dump": dumps/data_<frame>.bin, guest
0x82B70000..0x835B0000) from two runs at the same frame, and lists the differing byte ranges.

  py -3 tools/np_dumpdiff.py A.bin B.bin [--gap 16] [--max 200] [--words]
Ranges closer than --gap bytes are merged. Each range is printed with its guest address,
length, the known region it falls in (or "SDK handle stash" when every differing word in it is the word after a
"REX\\0" header signature in both dumps: stash_words), and (with --words) the big-endian u32 values on both
sides for its first words.
When many differing words are SDK handles (F80xxxxx) on both sides and differ by one constant, one more line names it:
"handle shift +N in K words" (B - A; handle_shift(): a lazily created host object took a low slot in one run and every
later guest handle moved, debug 82B70000).
Library use (np_pair.py, Task 15 carry b): frame0_difference(a, b) compares two frame-0 dumps over the whole .data with the
in-game sweep's masks (SWEEP_MASKS = np_session.cpp kSweepMasks) and every SDK handle stash word zeroed.
"""
import argparse
import collections
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
    (0x82B747D0, 0x1C, "CRT thread-notify critical section (wait-list words swept as zero)"),
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


# The in-game sweep's masks (src/net/np_session.cpp kSweepMasks: guest address, length), pinned against the source by the
# tool test test_sweep_masks_match_source.
SWEEP_MASKS = [
    (0x82B747D8, 8),     # wait-list words of the CRT thread-notify critical section 0x82B747D0
    (0x82F6C9D0, 8),     # CRI server threads B and A: loop-pass counters
    (0x830ADBCC, 0x1C),  # critical section of the 16-slot request table 0x830ADB08
    (0x831A7AC0, 4),     # vblank count stored by the D3D vblank callback
    (0x8357A560, 0x400),  # GPU timestamp ring 2 of the graphics object
    (0x8357D520, 4),     # D3D resource id counter
]
BLOCK = 0x10000  # the sweep's block size (np_core.h: 64 KB blocks from LO)


def masked(data):
    """A copy of a .data dump (guest LO..) as the sweep hashes it: every SDK handle stash word zeroed (decided on the dump's
    own bytes as they are, stash_words) and every SWEEP_MASKS range zeroed (the parts of a range inside the dump)."""
    d = bytearray(data)
    for w in stash_words(data):
        d[w:w + 4] = bytes(4)
    for addr, length in SWEEP_MASKS:
        s, e = max(0, addr - LO), min(len(d), addr - LO + length)
        if s < e:
            d[s:e] = bytes(e - s)
    return d


def first_difference(a, b):
    """The offset of the first byte where a and b differ (the shorter one's length when one is a prefix of the other), or None."""
    n = min(len(a), len(b))
    step = 1 << 16
    for s in range(0, n, step):
        e = min(n, s + step)
        if a[s:e] != b[s:e]:
            return next(i for i in range(s, e) if a[i] != b[i])
    return None if len(a) == len(b) else n


def frame0_difference(a, b):
    """(first differing guest address, its 64 KB sweep block's address) of two frame-0 dumps after masked(); None if equal."""
    at = first_difference(masked(a), masked(b))
    if at is None:
        return None
    return LO + at, LO + (at // BLOCK) * BLOCK


HANDLE_SHIFT_MIN = 4  # at least this many shifted handle words ...
HANDLE_SHIFT_SHARE = 0.75  # ... and at least this share of the differing handle words


def handle_shift(a, b):
    """(N, K) when K >= HANDLE_SHIFT_MIN 4-aligned words differ between the dumps, are SDK handles (F80xxxxx) on both sides and
    all differ by N = b - a (signed), and K is at least HANDLE_SHIFT_SHARE of the differing handle words; else None."""
    deltas = collections.Counter()
    n = min(len(a), len(b)) & ~3
    step = 1 << 16
    for s in range(0, n, step):
        e = min(n, s + step)
        if a[s:e] == b[s:e]:
            continue
        for w in range(s, e, 4):
            wa, wb = struct.unpack_from(">I", a, w)[0], struct.unpack_from(">I", b, w)[0]
            if wa != wb and wa >> 20 == 0xF80 and wb >> 20 == 0xF80:
                deltas[wb - wa] += 1
    if not deltas:
        return None
    delta, count = deltas.most_common(1)[0]
    if count < HANDLE_SHIFT_MIN or count < HANDLE_SHIFT_SHARE * sum(deltas.values()):
        return None
    return delta, count


def shift_text(shift):
    return f"handle shift {shift[0]:+d} in {shift[1]} words"


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
    shift = handle_shift(a, b)
    if shift:
        print(shift_text(shift) + " (B - A)")
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

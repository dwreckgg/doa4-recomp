"""Builds the phase-0 cross-PC bundle: everything the friend's PC needs to replay this PC's recordings and run the DPPS
test, without Python (the friend has a DOA4 disc image or game folder of their own). The friend runs run_check.bat and sends back
results.zip; tools/np_crosscheck.py compares it with this PC's runs.

  py -3 -I tools/np_bundle.py --bin NP_BIN_TAG_FOLDER --recs RUN[,RUN...] --userdata PRISTINE --dpps DPPS_EXE --out FOLDER

--bin   a build snapshot (tools/np_build.bat: np_bin/<tag>) holding dead_or_alive_4.exe, rexruntime.dll, TracyClient.dll
--recs  record run folders (np_run.py record: <run>/out/rec.d4r and run_result.txt); each folder's name is the recording's name
--out   a new folder; FOLDER.zip is made beside it. Built as FOLDER.partial and renamed when complete: a failure leaves nothing.
The bundle holds bin/ (the three files), recs/<name>.d4r, ud_pristine/ (the save every replay starts from: a copy of
--userdata), the DPPS exe, run_check.bat, get_game_files.bat (the game's own first-run setup: the friend gets the game files out
of their own disc image), run_one.ps1, README.txt and recordings.txt (templates: tools/np_bundle/).

It refuses unless every recording was made by the build in --bin: the recording's meta (np_core.h: the "build=" line of
the text after the 32-byte header) and the id of the exe must be the same id. The session refuses a replay of another build
anyway (np_session.cpp MetaMismatch), after 15 minutes of the friend's time; here it costs a few seconds.
The build id is what np_session.cpp BuildId() computes: XXH3-64 of the exe, then XXH3-64 of rexruntime.dll seeded with
the first result, as 16 lowercase hex digits. xxh3_64 below is that hash in pure Python (1.4 s for the real exe and dll,
65 MB); tests/tools/test_np_tools.py pins it against the xxHash library's own answers (tests/tools/xxh3_vectors.cpp), and
the real t11b exe and dll give b3d118544499d1e8, the build id of both recordings the bundle was first made from.
"""
import argparse
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATES = os.path.join(HERE, "np_bundle")
BIN_FILES = ("dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll")
NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*\Z")  # goes into a batch `for` list and into paths: nothing a shell or batch reads


class BundleError(Exception):
    """An input the bundle cannot be built from; the message says which and why."""


copy_file = shutil.copy2  # every file copy goes through here (a test makes it fail halfway)

# ---- XXH3-64 with a seed (xxHash 0.8, sdk-np0 thirdparty/xxHash/xxhash.h: XXH3_64bits_internal and what it calls) ----
M64 = (1 << 64) - 1
M32 = (1 << 32) - 1
PRIME32_1, PRIME32_2, PRIME32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
PRIME64_1, PRIME64_2, PRIME64_3 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9
PRIME64_4, PRIME64_5 = 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5
PRIME_MX1, PRIME_MX2 = 0x165667919E3779F9, 0x9FB21C651E98DF25
SECRET = bytes([  # XXH3_kSecret (xxhash.h:4365), 192 bytes
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e])
STRIPE = 64  # XXH_STRIPE_LEN
BLOCK_STRIPES = (len(SECRET) - STRIPE) // 8  # nbStripesPerBlock: 16 for the 192-byte secret


def _r64(b, o):
    return int.from_bytes(b[o:o + 8], "little")


def _r32(b, o):
    return int.from_bytes(b[o:o + 4], "little")


def _swap64(v):
    return int.from_bytes(v.to_bytes(8, "little"), "big")


def _swap32(v):
    return int.from_bytes(v.to_bytes(4, "little"), "big")


def _rotl64(v, r):
    return ((v << r) | (v >> (64 - r))) & M64


def _fold(a, b):  # XXH3_mul128_fold64
    p = a * b
    return (p & M64) ^ (p >> 64)


def _avalanche3(h):  # XXH3_avalanche
    h ^= h >> 37
    h = (h * PRIME_MX1) & M64
    return h ^ (h >> 32)


def _avalanche64(h):  # XXH64_avalanche
    h ^= h >> 33
    h = (h * PRIME64_2) & M64
    h ^= h >> 29
    h = (h * PRIME64_3) & M64
    return h ^ (h >> 32)


def _rrmxmx(h, n):  # XXH3_rrmxmx
    h ^= _rotl64(h, 49) ^ _rotl64(h, 24)
    h = (h * PRIME_MX2) & M64
    h ^= (h >> 35) + n
    h = (h * PRIME_MX2) & M64
    return h ^ (h >> 28)


def _mix16(data, off, secret, soff, seed):  # XXH3_mix16B
    return _fold(_r64(data, off) ^ ((_r64(secret, soff) + seed) & M64), _r64(data, off + 8) ^ ((_r64(secret, soff + 8) - seed) & M64))


def _init_secret(seed):  # XXH3_initCustomSecret_scalar
    out = bytearray()
    for i in range(len(SECRET) // 16):
        out += ((_r64(SECRET, 16 * i) + seed) & M64).to_bytes(8, "little") + ((_r64(SECRET, 16 * i + 8) - seed) & M64).to_bytes(8, "little")
    return bytes(out)


def _accumulate(acc, data, off, stripes, secret):
    """XXH3_accumulate_scalar: `stripes` stripes of 64 bytes from data[off:], lane i of stripe n keyed by secret[8n + 8i:]."""
    unpack = struct.Struct("<8Q").unpack_from
    keys = struct.unpack_from(f"<{len(secret) // 8}Q", secret)
    a0, a1, a2, a3, a4, a5, a6, a7 = acc
    for n in range(stripes):
        d0, d1, d2, d3, d4, d5, d6, d7 = unpack(data, off + STRIPE * n)
        x = d0 ^ keys[n]
        a1 += d0
        a0 += (x & M32) * (x >> 32)
        x = d1 ^ keys[n + 1]
        a0 += d1
        a1 += (x & M32) * (x >> 32)
        x = d2 ^ keys[n + 2]
        a3 += d2
        a2 += (x & M32) * (x >> 32)
        x = d3 ^ keys[n + 3]
        a2 += d3
        a3 += (x & M32) * (x >> 32)
        x = d4 ^ keys[n + 4]
        a5 += d4
        a4 += (x & M32) * (x >> 32)
        x = d5 ^ keys[n + 5]
        a4 += d5
        a5 += (x & M32) * (x >> 32)
        x = d6 ^ keys[n + 6]
        a7 += d6
        a6 += (x & M32) * (x >> 32)
        x = d7 ^ keys[n + 7]
        a6 += d7
        a7 += (x & M32) * (x >> 32)
    return [a0 & M64, a1 & M64, a2 & M64, a3 & M64, a4 & M64, a5 & M64, a6 & M64, a7 & M64]


def _accumulate_stripe(acc, data, off, secret, soff):
    """One stripe against the secret at an arbitrary byte offset (the last stripe's secret offset is not a multiple of 8)."""
    out = list(acc)
    for i in range(8):
        d = _r64(data, off + 8 * i)
        x = d ^ _r64(secret, soff + 8 * i)
        out[i ^ 1] += d
        out[i] += (x & M32) * (x >> 32)
    return [v & M64 for v in out]


def _scramble(acc, secret):  # XXH3_scrambleAcc_scalar
    base = len(secret) - STRIPE
    out = []
    for i in range(8):
        a = acc[i]
        a ^= a >> 47
        a ^= _r64(secret, base + 8 * i)
        out.append((a * PRIME32_1) & M64)
    return out


def _hash_long(data, secret):  # XXH3_hashLong_64b_internal
    n = len(data)
    acc = [PRIME32_3, PRIME64_1, PRIME64_2, PRIME64_3, PRIME64_4, PRIME32_2, PRIME64_5, PRIME32_1]
    block = STRIPE * BLOCK_STRIPES
    blocks = (n - 1) // block
    for b in range(blocks):
        acc = _scramble(_accumulate(acc, data, b * block, BLOCK_STRIPES, secret), secret)
    acc = _accumulate(acc, data, blocks * block, ((n - 1) - block * blocks) // STRIPE, secret)
    acc = _accumulate_stripe(acc, data, n - STRIPE, secret, len(secret) - STRIPE - 7)  # XXH_SECRET_LASTACC_START
    result = (n * PRIME64_1) & M64  # XXH3_mergeAccs from secret + XXH_SECRET_MERGEACCS_START (11)
    for i in range(4):
        result += _fold(acc[2 * i] ^ _r64(secret, 11 + 16 * i), acc[2 * i + 1] ^ _r64(secret, 11 + 16 * i + 8))
    return _avalanche3(result & M64)


def xxh3_64(data, seed=0):
    """XXH3_64bits_withSeed(data, len, seed) of the xxHash library, as an int."""
    n = len(data)
    seed &= M64
    s = SECRET
    if n == 0:
        return _avalanche64(seed ^ (_r64(s, 56) ^ _r64(s, 64)))
    if n <= 3:
        combined = (data[0] << 16) | (data[n >> 1] << 24) | data[n - 1] | (n << 8)
        return _avalanche64(combined ^ (((_r32(s, 0) ^ _r32(s, 4)) + seed) & M64))
    if n <= 8:
        seed ^= _swap32(seed & M32) << 32
        bitflip = ((_r64(s, 8) ^ _r64(s, 16)) - seed) & M64
        return _rrmxmx((_r32(data, n - 4) + (_r32(data, 0) << 32)) ^ bitflip, n)
    if n <= 16:
        lo = _r64(data, 0) ^ (((_r64(s, 24) ^ _r64(s, 32)) + seed) & M64)
        hi = _r64(data, n - 8) ^ (((_r64(s, 40) ^ _r64(s, 48)) - seed) & M64)
        return _avalanche3((n + _swap64(lo) + hi + _fold(lo, hi)) & M64)
    if n <= 128:
        acc = (n * PRIME64_1) & M64
        if n > 32:
            if n > 64:
                if n > 96:
                    acc += _mix16(data, 48, s, 96, seed) + _mix16(data, n - 64, s, 112, seed)
                acc += _mix16(data, 32, s, 64, seed) + _mix16(data, n - 48, s, 80, seed)
            acc += _mix16(data, 16, s, 32, seed) + _mix16(data, n - 32, s, 48, seed)
        acc += _mix16(data, 0, s, 0, seed) + _mix16(data, n - 16, s, 16, seed)
        return _avalanche3(acc & M64)
    if n <= 240:
        acc = (n * PRIME64_1) & M64
        for i in range(8):
            acc += _mix16(data, 16 * i, s, 16 * i, seed)
        end = _mix16(data, n - 16, s, 136 - 17, seed)  # XXH3_SECRET_SIZE_MIN - XXH3_MIDSIZE_LASTOFFSET
        acc = _avalanche3(acc & M64)
        for i in range(8, n // 16):
            end += _mix16(data, 16 * i, s, 16 * (i - 8) + 3, seed)  # XXH3_MIDSIZE_STARTOFFSET
        return _avalanche3((acc + end) & M64)
    return _hash_long(data, s if seed == 0 else _init_secret(seed))


# ---- build ids ----
def read_nonempty(path, what):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise BundleError(f"{what} {path} cannot be read ({e})")
    if not data:
        raise BundleError(f"{what} {path} is empty")
    return data


def exe_build_id(bin_dir):
    """The build id of the exe and the dll beside it, as the game computes it (np_session.cpp BuildId(), line 186):
    h = XXH3_64bits_withSeed(exe, 0); h = XXH3_64bits_withSeed(rexruntime.dll, h); 16 lowercase hex digits."""
    h = 0
    for name in ("dead_or_alive_4.exe", "rexruntime.dll"):
        h = xxh3_64(read_nonempty(os.path.join(bin_dir, name), name), h)
    return f"{h:016x}"


def recording_meta(path):
    """The meta text of a recording as a dict {key: value} (np_core.h: "D4NPREC1", u32 version, u32 frame bytes (52), u64 seed,
    u32 flags, u32 meta bytes, then the meta: "key=value" lines; a version 1 recording has no meta, {} here)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise BundleError(f"{path}: cannot be read ({e})")
    if len(data) < 32 or data[:8] != b"D4NPREC1":
        raise BundleError(f"{path}: not a recording (no D4NPREC1 header)")
    version, frame_bytes = struct.unpack_from("<II", data, 8)
    if version not in (1, 2) or frame_bytes != 52:
        raise BundleError(f"{path}: unsupported recording version {version} (frames of {frame_bytes} bytes)")
    if version == 1:
        return {}
    size = struct.unpack_from("<I", data, 28)[0]
    if len(data) < 32 + size:
        raise BundleError(f"{path}: recording header cut short (meta of {size} bytes, {len(data) - 32} present)")
    meta = {}
    for line in data[32:32 + size].decode("utf-8", "replace").split("\n"):
        key, eq, value = line.partition("=")
        if eq and key:
            meta[key] = value
    return meta


def recording_build_id(path):
    """The build id in a recording's meta; BundleError if there is none or it is "unreadable" (it agrees with nothing)."""
    meta = recording_meta(path)
    build = meta.get("build")
    if build is None and not meta:
        raise BundleError(f"{path}: a version 1 recording has no meta, so no build id")
    if not build:
        raise BundleError(f"{path}: the meta has no build id")
    if build == "unreadable":
        raise BundleError(f"{path}: its meta says build=unreadable (the exe could not be read when it was recorded)")
    return build


# ---- the bundle ----
def template(name, **values):
    """A template file of tools/np_bundle/ with its @KEY@ markers replaced, as ASCII bytes with CRLF line ends."""
    path = os.path.join(TEMPLATES, name)
    with open(path, "rb") as f:
        raw = f.read()
    text = raw.decode("ascii", "strict").replace("\r\n", "\n")
    for key, value in values.items():
        text = text.replace(f"@{key}@", value)
    left = re.findall(r"@[A-Z]+@", text)
    if left:
        raise BundleError(f"template {name}: markers left over: {sorted(set(left))}")
    return text.replace("\n", "\r\n").encode("ascii")


def check_inputs(bin_dir, recs, userdata, dpps, out):
    """Everything that can be refused is refused here, before anything is written. Returns (names, run folders, meta of the
    first recording, build id of the exe)."""
    for p in (out, out + ".zip", out + ".partial"):
        if os.path.exists(p):
            raise BundleError(f"exists: {p}")
    for name in BIN_FILES:
        read_nonempty(os.path.join(bin_dir, name), name)
    if not os.path.isdir(userdata):
        raise BundleError(f"--userdata {userdata} is not a folder")
    if not os.path.isfile(dpps):
        raise BundleError(f"--dpps {dpps} is not a file")
    if not recs:
        raise BundleError("--recs names no record run folder")
    names, seen = [], set()
    for run in recs:
        name = os.path.basename(os.path.normpath(run))
        if not NAME_RE.match(name):
            raise BundleError(f"recording name {name!r} (the run folder's name) may hold only letters, digits, '_', '.' and '-': it goes into a batch file and into paths")
        if name in seen:
            raise BundleError(f"recording {name} is given twice")
        seen.add(name)
        names.append(name)
    exe_id = exe_build_id(bin_dir)
    metas, wrong = [], []
    for name, run in zip(names, recs):
        rec = os.path.join(run, "out", "rec.d4r")
        result = os.path.join(run, "out", "run_result.txt")
        if not os.path.isfile(rec):
            raise BundleError(f"recording {name}: {rec} not found (rec.d4r)")
        if not os.path.isfile(result):
            raise BundleError(f"recording {name}: {result} not found (run_result.txt): was its record run finished?")
        with open(result, encoding="utf-8", errors="replace") as f:
            status = next((l.split(None, 1)[1].strip() for l in f if l.startswith("status ")), "none")
        if status != "ok":
            raise BundleError(f"recording {name}: its run_result.txt says status {status}, not ok")
        try:
            build = recording_build_id(rec)
        except BundleError as e:
            raise BundleError(f"recording {name}: {e}")
        if build != exe_id:
            wrong.append(f"recording {name} was made by build {build}")
        metas.append(recording_meta(rec))
    if wrong:
        raise BundleError("; ".join(wrong) + f", not by the exe in {bin_dir} (build {exe_id}): the friend's replay would be refused")
    return names, metas, exe_id


def build_bundle(bin_dir, recs, userdata, dpps, out, minutes_each=3):
    """Builds the bundle folder `out` and `out`.zip; nothing is left behind if anything fails. Returns the recording names."""
    names, metas, exe_id = check_inputs(bin_dir, recs, userdata, dpps, out)
    data_ids = sorted({m.get("data", "(none)") for m in metas})
    tmp = out + ".partial"
    made_out = False
    try:
        os.makedirs(os.path.join(tmp, "bin"))
        os.makedirs(os.path.join(tmp, "recs"))
        for f in BIN_FILES:
            copy_file(os.path.join(bin_dir, f), os.path.join(tmp, "bin", f))
        for name, run in zip(names, recs):
            copy_file(os.path.join(run, "out", "rec.d4r"), os.path.join(tmp, "recs", name + ".d4r"))
        shutil.copytree(userdata, os.path.join(tmp, "ud_pristine"), copy_function=copy_file)
        dpps_name = os.path.basename(dpps)
        copy_file(dpps, os.path.join(tmp, dpps_name))
        files = {"run_check.bat": template("run_check.bat", RECS=" ".join(names), DPPS=dpps_name),
                 "get_game_files.bat": template("get_game_files.bat"),
                 "run_one.ps1": template("run_one.ps1"),
                 "README.txt": template("README.txt", N=str(len(names)), MINUTES=str(minutes_each * len(names)), BUILD=exe_id,
                                        DATA=", ".join(data_ids), RECS=", ".join(names)),
                 "recordings.txt": ("\r\n".join(names) + "\r\n").encode("ascii")}
        for fname, data in files.items():
            with open(os.path.join(tmp, fname), "wb") as f:
                f.write(data)
        os.rename(tmp, out)
        made_out = True
        shutil.make_archive(out, "zip", out)
    except BaseException:
        for p in ((tmp, out) if made_out else (tmp,)):
            shutil.rmtree(p, ignore_errors=True)
        if os.path.exists(out + ".zip"):
            os.remove(out + ".zip")
        raise
    return names


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--recs", required=True, help="record run folders, comma-separated")
    ap.add_argument("--userdata", required=True)
    ap.add_argument("--dpps", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--minutes-each", type=int, default=3, help="what README.txt says one recording takes")
    a = ap.parse_args(argv)
    try:
        names = build_bundle(a.bin, [r for r in a.recs.split(",") if r], a.userdata, a.dpps, a.out, a.minutes_each)
    except BundleError as e:
        sys.exit(f"np_bundle: {e}")
    print(f"bundle {a.out} and {a.out}.zip: recordings {', '.join(names)}")


if __name__ == "__main__":
    main()

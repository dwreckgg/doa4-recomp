"""Stages a netplay release's assets (plan 1b Task 2): the three game files, their checksums, one zip for a manual
download and, when given, the launcher zip. The release workflow publishes every file of the staging folder.

  py -3 -I tools/release_stage.py --build <out/build/win-amd64-release> --out <staging> --tag <netplay-vX.Y.Z>
      --port-commit <sha> --sdk-commit <sha> [--launcher-zip <path>] [--extra <name>]

<staging> then holds exactly:
  dead_or_alive_4.exe, rexruntime.dll, TracyClient.dll   byte copies from --build (picked by name: nothing else there
                                                          is ever staged)
  SHA256SUMS.txt                                          "<sha256 lower hex>  <name>" per game file, LF, in that order
  THIRD_PARTY_NOTICES.md                                  a byte copy of the repository's file (the BSD notice duty)
  DOA4-<tag>-win64.zip                                    the three, SHA256SUMS.txt, THIRD_PARTY_NOTICES.md and README-netplay.txt
  DOA4-launcher.zip                                       a copy of --launcher-zip (only when given)
Never game data. Refused (exit 1, the message names the file; nothing is staged): a missing game file; a name under
a folder or matching *.xex, *.afs, *.sfd, *.dat (game data); a name that is not one of the three; a name given
twice; a staging folder that is not empty; a tag that is not <channel>-v<major>.<minor>.<run>; a commit that is not
hex. --extra NAME (tests only) adds one more name, relative to --build, so the refusal rules can be exercised.
"""
import argparse
import hashlib
import os
import re
import shutil
import sys
import zipfile

GAME_FILES = ("dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll")
SUMS = "SHA256SUMS.txt"
README = "README-netplay.txt"
NOTICES = "THIRD_PARTY_NOTICES.md"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = "DOA4-launcher.zip"
GAME_DATA = re.compile(r".*\.(xex|afs|sfd|dat)", re.IGNORECASE)
TAG = re.compile(r"[a-z0-9]+-v\d+\.\d+\.\d+")
COMMIT = re.compile(r"[0-9a-f]{7,40}")


class Refused(Exception):
    pass


def name_problem(name):
    """Why `name` may never be staged by pattern, or None. Checked before the list rule, so its own reason shows."""
    if "/" in name or "\\" in name:
        return f"{name}: a name under a folder is never staged"
    if GAME_DATA.fullmatch(name):
        return f"{name}: game data (*.xex, *.afs, *.sfd, *.dat) is never staged"
    return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def readme_text(tag, port_commit, sdk_commit):
    lines = [
        f"DOA4 netplay {tag}",
        f"Built from the port commit {port_commit} with the SDK commit {sdk_commit} (LLVM 23.1.4).",
        "",
        "Both players need this same release: the online handshake compares the compiled code.",
        "Put dead_or_alive_4.exe, rexruntime.dll and TracyClient.dll together in one folder (replacing older",
        "copies) and start dead_or_alive_4.exe. There is no game data in this release: on the first start the game asks",
        "for your own Dead or Alive 4 (USA, Europe) (Rev 1) disc image or extracted folder.",
        "SHA256SUMS.txt holds the SHA-256 of the three files.",
        "",
        "Needs Windows 10/11 64-bit, a CPU with AVX2 and at least two cores, and the Microsoft Visual C++ 2015-2022 x64",
        "runtime. Online play: Esc menu, Online tab (Host, or Join with the other player's code).",
        "",
        "Firewall: the first time you host or join, Windows asks whether to allow the game through the firewall. This",
        "happens on both players' PCs. Tick both Private and Public networks (Public is unticked by default, and a",
        "Public network then blocks the connection), then Allow. The prompt can hide behind the fullscreen game: press",
        "Alt+Tab. If the other player still cannot connect, turn on UPnP in the host's router, or forward UDP 7200 to",
        "the host PC.",
    ]
    return "\r\n".join(lines) + "\r\n"


def check_args(a):
    if not TAG.fullmatch(a.tag or ""):
        raise Refused(f"--tag {a.tag!r}: not <channel>-v<major>.<minor>.<run> (for example netplay-v1.0.7)")
    for opt, value in (("--port-commit", a.port_commit), ("--sdk-commit", a.sdk_commit)):
        if not COMMIT.fullmatch(value or ""):
            raise Refused(f"{opt} {value!r}: not a commit id (7 to 40 lower-case hex digits)")
    names = list(GAME_FILES) + ([a.extra] if a.extra is not None else [])
    for name in names:
        problem = name_problem(name)
        if problem:
            raise Refused(problem)
    for name in names:
        if name not in GAME_FILES:
            raise Refused(f"{name}: not one of the release files ({', '.join(GAME_FILES)})")
    seen = set()
    for name in names:
        if name.lower() in seen:
            raise Refused(f"{name}: named twice")
        seen.add(name.lower())
    for name in GAME_FILES:
        if not os.path.isfile(os.path.join(a.build, name)):
            raise Refused(f"{name}: missing in {a.build}")
    if not os.path.isfile(os.path.join(REPO, NOTICES)):
        raise Refused(f"{NOTICES}: missing in {REPO}")
    if a.launcher_zip is not None and not os.path.isfile(a.launcher_zip):
        raise Refused(f"{a.launcher_zip}: the launcher zip is missing")
    if os.path.exists(a.out) and (not os.path.isdir(a.out) or os.listdir(a.out)):
        raise Refused(f"{a.out}: the staging folder is not empty")


def stage(a):
    os.makedirs(a.out, exist_ok=True)
    for name in GAME_FILES:
        shutil.copyfile(os.path.join(a.build, name), os.path.join(a.out, name))
    shutil.copyfile(os.path.join(REPO, NOTICES), os.path.join(a.out, NOTICES))
    # the checksums are of the staged copies, the bytes that are published
    sums = "".join(f"{sha256(os.path.join(a.out, name))}  {name}\n" for name in GAME_FILES)
    with open(os.path.join(a.out, SUMS), "w", encoding="ascii", newline="\n") as f:
        f.write(sums)
    zip_name = f"DOA4-{a.tag}-win64.zip"
    with zipfile.ZipFile(os.path.join(a.out, zip_name), "w", zipfile.ZIP_DEFLATED) as z:
        for name in GAME_FILES + (SUMS,):
            z.write(os.path.join(a.out, name), arcname=name)
        z.write(os.path.join(a.out, NOTICES), arcname=NOTICES)
        z.writestr(README, readme_text(a.tag, a.port_commit, a.sdk_commit))
    expected = set(GAME_FILES) | {SUMS, NOTICES, zip_name}
    if a.launcher_zip is not None:
        shutil.copyfile(a.launcher_zip, os.path.join(a.out, LAUNCHER))
        expected.add(LAUNCHER)
    got = set(os.listdir(a.out))
    if got != expected:  # the folder is published as a whole: it must hold exactly these
        raise Refused(f"{a.out}: holds {sorted(got)}, expected {sorted(expected)}")
    for name in sorted(got):
        print(f"staged {name}  {os.path.getsize(os.path.join(a.out, name))} bytes")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Stages a netplay release's assets.")
    ap.add_argument("--build", required=True, help="the build folder (out/build/win-amd64-release)")
    ap.add_argument("--out", required=True, help="the staging folder (new or empty)")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--port-commit", required=True)
    ap.add_argument("--sdk-commit", required=True)
    ap.add_argument("--launcher-zip")
    ap.add_argument("--extra", help="tests only: one more name, relative to --build, to exercise the refusal rules")
    a = ap.parse_args(argv)
    try:
        check_args(a)
        stage(a)
    except Refused as e:
        print(f"release_stage: refused: {e}")
        return 1
    print(f"release_stage: {a.tag} staged in {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

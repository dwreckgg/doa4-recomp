// The game files two players compare (R18): one fingerprint group per KNOWN top-level entry of the disc, so a refused
// join can name what differs. Windows file API; no SDK. Ported from DOA2U's netplay_files.c (MIT, Parker Allan),
// with per-group results and without its XBE special case.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace doa4::np::files {

struct Group {
  std::string name;
  uint64_t hash = 0;
  uint32_t files = 0;
  bool unreadable = false;  // present but could not be read (hash 0)
  bool missing = false;     // a known entry that is not in the folder (hash 0)
};

// The groups are the disc's own top-level entries and no others: anm.afs avt.afs chrone.afs chrthree.afs chrtwo.afs
// cmn.afs default.xex dic mot.afs snd.afs sound spr.afs stg.afs (matched without regard to case, names lower case,
// result sorted by name). "movie" is left out, and any OTHER top-level entry (a readme, a backup folder, extra
// files a player keeps beside the game) is ignored: the game never reads it, it is not hashed and not in Text, so
// it can neither block a match nor grow the text Hello::files carries (at most 13 lines, under 400 bytes); it is
// only COUNTED, in *ignored when given, for a log line. A known entry that is absent is its own group with
// `missing` set. default.xex is hashed in full, every other file by DOA2U's samples (first and last 64 KB, 64 KB at
// every 16 MB), FNV-1a 64 over (relative path lower case with '/', size, samples), files taken in lower-case name
// order; desktop.ini and thumbs.db inside a group are skipped. A file that cannot be opened or read makes its group
// unreadable (hash 0). An unreadable or missing group agrees with nothing (rules::DifferingGroups). A `dir` that
// cannot be listed yields no groups. Cost scales with the files in the known entries (0.03 s for the real game);
// call it off the game and UI threads.
std::vector<Group> Fingerprint(const std::filesystem::path& dir, uint32_t* ignored = nullptr);

// "name=hash16\n" per group, sorted by name; an unreadable group reads "name=unreadable", a missing one "name=missing".
std::string Text(const std::vector<Group>& groups);

}  // namespace doa4::np::files

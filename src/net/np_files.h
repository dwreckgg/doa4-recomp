// The game files two players compare (R18): one fingerprint group per top-level file and per top-level folder of the
// game directory, so a refused join can name what differs. Windows file API; no SDK. Ported from DOA2U's
// netplay_files.c (MIT, Parker Allan), with per-group results and without its XBE special case.
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
  bool unreadable = false;
};

// One group per top-level file and per top-level folder under `dir`, names lower case, sorted by name; "movie" is
// left out (and desktop.ini / thumbs.db, which Windows writes). default.xex is hashed in full, every other file by
// DOA2U's samples (first and last 64 KB, 64 KB at every 16 MB), FNV-1a 64 over (relative path lower case with '/',
// size, samples), files taken in lower-case name order. A file that cannot be opened or read makes its group
// unreadable (hash 0; it then agrees with nothing). A `dir` that cannot be listed yields no groups.
std::vector<Group> Fingerprint(const std::filesystem::path& dir);

// "name=hash16\n" per group, sorted by name; an unreadable group reads "name=unreadable".
std::string Text(const std::vector<Group>& groups);

}  // namespace doa4::np::files

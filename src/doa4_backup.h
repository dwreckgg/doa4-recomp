// One-time backup of the game's save folder before the Extras tab first
// writes unlocks into the profile.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace doa4 {

struct BackupResult {
  bool ok = false;               // false: do not write anything
  std::filesystem::path backup;  // the backup folder, or empty when there was no save
  std::string message;           // why not ok, or a note ("no save existed yet")
};

// Makes sure a backup of `save_dir` (the save package folder, e.g.
// .../00000001/ups.dat) exists beside it. A backup is any sibling folder named
// "<save folder name>.backup-<anything>"; the first one is kept forever and
// never overwritten. Otherwise copies the folder to
// "<save_dir>.backup-<stamp>". When `save_dir` does not exist there is nothing
// to lose: ok, with no backup and a note.
BackupResult EnsureSaveBackup(const std::filesystem::path& save_dir, std::string_view stamp);

}  // namespace doa4

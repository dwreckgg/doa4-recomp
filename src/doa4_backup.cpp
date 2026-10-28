// One-time save backup. See doa4_backup.h.

#include "doa4_backup.h"

#include <system_error>

namespace doa4 {

BackupResult EnsureSaveBackup(const std::filesystem::path& save_dir, std::string_view stamp) {
  namespace fs = std::filesystem;
  BackupResult result;
  std::error_code ec;
  if (!fs::is_directory(save_dir, ec)) {
    result.ok = true;
    result.message = "No save existed yet, so there was nothing to back up.";
    return result;
  }
  const std::string prefix = save_dir.filename().string() + ".backup-";
  for (const auto& entry : fs::directory_iterator(save_dir.parent_path(), ec)) {
    if (entry.is_directory(ec) && entry.path().filename().string().rfind(prefix, 0) == 0) {
      result.ok = true;
      result.backup = entry.path();
      return result;
    }
  }
  if (ec) {
    result.message = "cannot list " + save_dir.parent_path().string() + ": " + ec.message();
    return result;
  }
  fs::path backup = save_dir;
  backup += ".backup-" + std::string(stamp);
  if (fs::exists(backup, ec)) {
    result.message = "cannot back up the save: " + backup.string() + " is in the way";
    return result;
  }
  fs::copy(save_dir, backup, fs::copy_options::recursive, ec);
  if (ec) {
    result.message = "cannot back up the save to " + backup.string() + ": " + ec.message();
    std::error_code ignored;
    fs::remove_all(backup, ignored);
    return result;
  }
  result.ok = true;
  result.backup = backup;
  return result;
}

}  // namespace doa4

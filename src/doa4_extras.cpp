// The Extras tab's actions. See doa4_extras.h.

#include "doa4_extras.h"

#include <chrono>
#include <ctime>
#include <format>
#include <system_error>
#include <vector>

#include "doa4_backup.h"

namespace doa4 {

namespace {

std::optional<std::filesystem::file_time_type> SaveFileTime(const std::filesystem::path& save_dir) {
  std::error_code ec;
  auto time = std::filesystem::last_write_time(save_dir / "ups.dat", ec);
  if (ec) return std::nullopt;
  return time;
}

}  // namespace

std::filesystem::path SaveDir(const std::filesystem::path& user_data_root, uint64_t xuid) {
  if (xuid == 0 || user_data_root.empty()) return {};
  return user_data_root / std::format("{:016X}", xuid) / "544307D1" / "00000001" / "ups.dat";
}

std::string BackupStamp() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  return std::format("{:04}-{:02}-{:02}-{:02}{:02}{:02}", local.tm_year + 1900, local.tm_mon + 1,
                     local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

std::string DescribeUnlock(const unlocks::UnlockResult& result) {
  std::vector<std::string> parts;
  auto add = [&parts](int n, const char* one, const char* many) {
    if (n > 0) parts.push_back(std::format("{} {}", n, n == 1 ? one : many));
  };
  add(result.characters, "character", "characters");
  add(result.costumes, "costume", "costumes");
  add(result.voices, "system voice", "system voices");
  add(result.stages, "stage", "stages");
  if (parts.empty()) return "Nothing to unlock: it was all unlocked already.";
  std::string text = "Unlocked ";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) text += i + 1 == parts.size() ? " and " : ", ";
    text += parts[i];
  }
  return text + ".";
}

bool UnlockWithBackup(const GuestBytes& guest, uint32_t profile, unlocks::Unlock what,
                      const std::filesystem::path& save_dir, std::string_view stamp,
                      ExtrasState* state, std::string* error) {
  if (save_dir.empty()) {
    *error = "Nothing unlocked: the save folder is unknown, so it could not be backed up.";
    return false;
  }
  BackupResult backup = EnsureSaveBackup(save_dir, stamp);
  if (!backup.ok) {
    *error = "Nothing unlocked: " + backup.message;
    return false;
  }
  state->backup = backup.backup;
  state->backup_note = backup.message;
  const std::optional<std::filesystem::file_time_type> before = SaveFileTime(save_dir);
  const unlocks::UnlockResult result = unlocks::ApplyUnlock(guest, profile, what);
  state->last_change = DescribeUnlock(result);
  if (result.characters || result.costumes || result.voices || result.stages) {
    state->waiting_for_save = true;
    state->saved = false;
    state->save_time_at_click = before;
  }
  return true;
}

void PollSaved(const std::filesystem::path& save_dir, ExtrasState* state) {
  if (!state->waiting_for_save) return;
  const std::optional<std::filesystem::file_time_type> now = SaveFileTime(save_dir);
  if (now && now != state->save_time_at_click) {
    state->waiting_for_save = false;
    state->saved = true;
  }
}

}  // namespace doa4

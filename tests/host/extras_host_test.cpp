// Host test for src/doa4_extras.cpp (with doa4_unlocks.cpp and
// doa4_backup.cpp). Built and run by run_extras_test.bat.

#include "../../src/doa4_extras.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "doa4_unlock_fixtures.h"

static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

namespace fs = std::filesystem;
using namespace doa4::unlocks;
namespace fx = doa4_fixtures;

constexpr uint32_t kP = kRecordBase + kRecordToProfile;  // slot 0

// A loaded fresh profile in a map, as in unlocks_host_test.cpp.
struct FakeGuest {
  std::map<uint32_t, uint8_t> bytes;
  int writes = 0;
  doa4::GuestBytes guest{[this](uint32_t a) { return bytes.count(a) ? bytes[a] : uint8_t(0); },
                         [this](uint32_t a, uint8_t v) {
                           bytes[a] = v;
                           ++writes;
                         }};
  FakeGuest() {
    for (size_t i = 0; i < fx::kProfileIndexToChar.size(); ++i)
      bytes[kProfileIndexToChar + uint32_t(i)] = fx::kProfileIndexToChar[i];
    for (size_t i = 0; i < fx::kMaxCostumes.size(); ++i)
      bytes[kMaxCostumes + uint32_t(i)] = fx::kMaxCostumes[i];
    for (size_t k = 0; k < fx::kStoryRosterChars.size(); ++k)
      bytes[kStoryRoster + kStoryRosterStride * uint32_t(k)] = fx::kStoryRosterChars[k];
    for (size_t i = 0; i < fx::kFreshProfile.size(); ++i)
      bytes[kP + 0x3FC0 + uint32_t(i)] = fx::kFreshProfile[i];
  }
};

static void WriteFile(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

static void TestSaveDir() {
  CHECK(doa4::SaveDir("C:/u", 0xB13EBABEBABEBABEull) ==
        fs::path("C:/u") / "B13EBABEBABEBABE" / "544307D1" / "00000001" / "ups.dat");
  CHECK(doa4::SaveDir("C:/u", 0x00000000000000ABull) ==
        fs::path("C:/u") / "00000000000000AB" / "544307D1" / "00000001" / "ups.dat");
  CHECK(doa4::SaveDir("C:/u", 0).empty());
  CHECK(doa4::SaveDir("", 1).empty());
}

static void TestBackupStamp() {
  const std::string stamp = doa4::BackupStamp();  // YYYY-MM-DD-HHMMSS
  CHECK(stamp.size() == 17);
  CHECK(stamp[4] == '-' && stamp[7] == '-' && stamp[10] == '-');
}

static void TestDescribe() {
  CHECK(doa4::DescribeUnlock({}) == "Nothing to unlock: it was all unlocked already.");
  CHECK(doa4::DescribeUnlock({6, 0, 0, 1}) == "Unlocked 6 characters and 1 stage.");
  CHECK(doa4::DescribeUnlock({0, 0, 23, 0}) == "Unlocked 23 system voices.");
  CHECK(doa4::DescribeUnlock({6, 61, 23, 3}) ==
        "Unlocked 6 characters, 61 costumes, 23 system voices and 3 stages.");
  CHECK(doa4::DescribeUnlock({1, 1, 1, 0}) == "Unlocked 1 character, 1 costume and 1 system voice.");
}

static void TestUnlockWithBackup() {
  const fs::path root = fs::temp_directory_path() / "doa4_extras_test_data";
  fs::remove_all(root);
  const fs::path save = root / "00000001" / "ups.dat";
  fs::create_directories(save);
  WriteFile(save / "ups.dat", "fresh");

  // Backed up, then unlocked; waiting for the game's save.
  FakeGuest g;
  doa4::ExtrasState state;
  std::string error;
  CHECK(doa4::UnlockWithBackup(g.guest, kP, Unlock::kCharacters, save, "s1", &state, &error));
  CHECK(error.empty());
  CHECK(state.backup == root / "00000001" / "ups.dat.backup-s1");
  CHECK(fs::exists(state.backup / "ups.dat"));
  CHECK(state.last_change == "Unlocked 6 characters and 1 stage.");
  CHECK(state.waiting_for_save && !state.saved);
  CHECK(g.writes == 6);

  // Not saved while ups.dat is unchanged; saved once the game rewrites it.
  doa4::PollSaved(save, &state);
  CHECK(state.waiting_for_save && !state.saved);
  fs::last_write_time(save / "ups.dat",
                      fs::last_write_time(save / "ups.dat") + std::chrono::seconds(2));
  doa4::PollSaved(save, &state);
  CHECK(!state.waiting_for_save && state.saved);

  // A click that changes nothing leaves "saved" alone.
  CHECK(doa4::UnlockWithBackup(g.guest, kP, Unlock::kCharacters, save, "s2", &state, &error));
  CHECK(state.last_change == "Nothing to unlock: it was all unlocked already.");
  CHECK(state.saved && !state.waiting_for_save);
  CHECK(state.backup == root / "00000001" / "ups.dat.backup-s1");  // still the first

  // Backup impossible (its name is taken by a file): nothing written.
  const fs::path save3 = root / "00000003" / "ups.dat";
  fs::create_directories(save3);
  WriteFile(save3 / "ups.dat", "x");
  WriteFile(root / "00000003" / "ups.dat.backup-s3", "a file");
  FakeGuest g3;
  doa4::ExtrasState state3;
  CHECK(!doa4::UnlockWithBackup(g3.guest, kP, Unlock::kEverything, save3, "s3", &state3, &error));
  CHECK(error.rfind("Nothing unlocked: ", 0) == 0);
  CHECK(g3.writes == 0);
  CHECK(state3.last_change.empty() && !state3.waiting_for_save);

  // Unknown save folder: nothing written.
  FakeGuest g4;
  doa4::ExtrasState state4;
  error.clear();
  CHECK(!doa4::UnlockWithBackup(g4.guest, kP, Unlock::kVoices, fs::path(), "s4", &state4, &error));
  CHECK(!error.empty());
  CHECK(g4.writes == 0);

  // No save yet: unlocked without a backup, with a note; saved once ups.dat appears.
  const fs::path save5 = root / "00000005" / "ups.dat";
  FakeGuest g5;
  doa4::ExtrasState state5;
  CHECK(doa4::UnlockWithBackup(g5.guest, kP, Unlock::kVoices, save5, "s5", &state5, &error));
  CHECK(state5.backup.empty() && !state5.backup_note.empty());
  CHECK(g5.writes == 23);
  doa4::PollSaved(save5, &state5);
  CHECK(state5.waiting_for_save);
  fs::create_directories(save5);
  WriteFile(save5 / "ups.dat", "first save");
  doa4::PollSaved(save5, &state5);
  CHECK(state5.saved);

  fs::remove_all(root);
}

int main() {
  TestSaveDir();
  TestBackupStamp();
  TestDescribe();
  try {
    TestUnlockWithBackup();
  } catch (const std::exception& e) {
    std::printf("FAIL TestUnlockWithBackup threw: %s\n", e.what());
    ++g_failures;
  }
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("extras: all checks passed\n");
  return 0;
}

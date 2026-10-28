// Host test for src/doa4_backup.cpp. Built and run by run_backup_test.bat.

#include "../../src/doa4_backup.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

namespace fs = std::filesystem;

static void WriteFile(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

static std::string ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int CountBackups(const fs::path& dir) {
  int n = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.path().filename().string().rfind("ups.dat.backup-", 0) == 0) ++n;
  }
  return n;
}

static void TestBackup() {
  const fs::path root = fs::temp_directory_path() / "doa4_backup_test_data";
  fs::remove_all(root);
  fs::create_directories(root / "00000001" / "ups.dat");
  const fs::path save = root / "00000001" / "ups.dat";
  WriteFile(save / "ups.dat", "profile-v1");
  WriteFile(save / "__thumbnail.png", "png");

  // First call copies the whole folder.
  doa4::BackupResult first = doa4::EnsureSaveBackup(save, "2026-10-28-120000");
  CHECK(first.ok);
  CHECK(first.backup == root / "00000001" / "ups.dat.backup-2026-10-28-120000");
  CHECK(ReadAll(first.backup / "ups.dat") == "profile-v1");
  CHECK(ReadAll(first.backup / "__thumbnail.png") == "png");
  CHECK(ReadAll(save / "ups.dat") == "profile-v1");  // the save itself untouched

  // Later calls keep the first backup and make no new one, even after the
  // save has changed.
  WriteFile(save / "ups.dat", "profile-v2-unlocked");
  doa4::BackupResult second = doa4::EnsureSaveBackup(save, "2026-10-29-090000");
  CHECK(second.ok);
  CHECK(second.backup == first.backup);
  CHECK(ReadAll(first.backup / "ups.dat") == "profile-v1");
  CHECK(CountBackups(root / "00000001") == 1);

  // No save folder yet: nothing to lose, nothing copied.
  const fs::path missing = root / "00000002" / "ups.dat";
  doa4::BackupResult none = doa4::EnsureSaveBackup(missing, "2026-10-28-120000");
  CHECK(none.ok);
  CHECK(none.backup.empty());
  CHECK(!none.message.empty());
  CHECK(!fs::exists(root / "00000002"));

  // A copy that cannot be made is an error and leaves no partial backup: the
  // backup name is taken by a file.
  fs::create_directories(root / "00000003" / "ups.dat");
  WriteFile(root / "00000003" / "ups.dat" / "ups.dat", "x");
  WriteFile(root / "00000003" / "ups.dat.backup-blocked", "a file, not a folder");
  doa4::BackupResult blocked = doa4::EnsureSaveBackup(root / "00000003" / "ups.dat", "blocked");
  CHECK(!blocked.ok);
  CHECK(!blocked.message.empty());
  CHECK(fs::is_regular_file(root / "00000003" / "ups.dat.backup-blocked"));

  // A file named like a backup is not a backup.
  fs::create_directories(root / "00000004" / "ups.dat");
  WriteFile(root / "00000004" / "ups.dat" / "ups.dat", "y");
  WriteFile(root / "00000004" / "ups.dat.backup-notes.txt", "notes");
  doa4::BackupResult real = doa4::EnsureSaveBackup(root / "00000004" / "ups.dat", "s1");
  CHECK(real.ok);
  CHECK(real.backup == root / "00000004" / "ups.dat.backup-s1");
  CHECK(ReadAll(real.backup / "ups.dat") == "y");

  fs::remove_all(root);
}

int main() {
  try {
    TestBackup();
  } catch (const std::exception& e) {
    std::printf("FAIL TestBackup threw: %s\n", e.what());
    ++g_failures;
  }
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("backup: all checks passed\n");
  return 0;
}

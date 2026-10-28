// Host test for src/doa4_gamefiles.cpp (choosing, checking and extracting the
// game files). Built and run by run_gamefiles_test.bat.

#include "../../src/doa4_gamefiles.h"
#include "xdvdfs_fixtures.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using namespace doa4::gamefiles;
using doa4::xdvdfs::fixtures::BuildImage;
using doa4::xdvdfs::fixtures::FakeFile;
namespace fs = std::filesystem;

static std::string Bytes(size_t n, char seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = char(seed + i * 13);
  return s;
}

static const std::string kXex = "XEX2" + Bytes(9000, 5);
static const std::vector<FakeFile> kGame = {
    {"default.xex", kXex},
    {"anm.afs", Bytes(70000, 1)},
    {"movie/ayane.sfd", Bytes(3000, 2)},
    {"SOUND/BGM/title.xma", Bytes(2048, 3)},
    {"dic/empty.dic", ""},
};

static std::string ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

static void WriteAll(const fs::path& p, const std::string& data) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(data.data(), std::streamsize(data.size()));
}

static fs::path WriteIso(const fs::path& dir, const std::string& name,
                         const std::vector<FakeFile>& files) {
  const auto image = BuildImage(files);
  const fs::path p = dir / name;
  WriteAll(p, std::string(image.bytes.begin(), image.bytes.end()));
  return p;
}

// Every file and folder under `dir`, relative, '/'-separated.
static std::set<std::string> Tree(const fs::path& dir) {
  std::set<std::string> out;
  if (!fs::exists(dir)) return out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    out.insert(fs::relative(e.path(), dir).generic_string() + (e.is_directory() ? "/" : ""));
  }
  return out;
}

static void TestSha1() {
  // FIPS 180 vectors.
  CHECK(Sha1Hex("", 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(Sha1Hex("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  const std::string million(1000000, 'a');
  CHECK(Sha1Hex(million.data(), million.size()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  // Fed in pieces, the same digest.
  Sha1 h;
  h.Update("a", 1);
  h.Update("bc", 2);
  CHECK(h.HexDigest() == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(kSupportedXexSha1 == "af8011945037b37c98fb8a78d5a4e844dd56c33f");
}

// Command line first, then the saved path, then the setup screen.
static void TestChooseStartPath() {
  const fs::path good = "C:/games/DOA4", other = "D:/DOA4", gone = "E:/unplugged/DOA4";
  const std::set<fs::path> valid = {good, other};
  auto has = [&](const fs::path& p) { return valid.count(p) != 0; };

  StartChoice c = ChooseStartPath(good, other, has);
  CHECK(c.path == good && c.from_command_line && c.reason.empty());
  c = ChooseStartPath(good, "", has);
  CHECK(c.path == good && c.from_command_line);
  // A command-line path that is not there opens the setup screen, naming it;
  // it does not fall back to the saved path (the user asked for that folder).
  c = ChooseStartPath(gone, other, has);
  CHECK(c.path.empty() && !c.from_command_line);
  CHECK(c.reason.find("E:/unplugged/DOA4") != std::string::npos);
  CHECK(c.reason.find("--game_data_root") != std::string::npos);
  c = ChooseStartPath("", other, has);
  CHECK(c.path == other && !c.from_command_line && c.reason.empty());
  c = ChooseStartPath("", gone, has);
  CHECK(c.path.empty() && c.reason.find("E:/unplugged/DOA4") != std::string::npos);
  CHECK(c.reason.find("--game_data_root") == std::string::npos);
  c = ChooseStartPath("", "", has);
  CHECK(c.path.empty() && c.reason.empty());
}

static void TestHasXex(const fs::path& dir) {
  CHECK(!HasXex(dir / "nothing"));
  WriteAll(dir / "hx" / "default.xex", "x");
  CHECK(HasXex(dir / "hx"));
  fs::create_directories(dir / "hx2" / "default.xex");  // a folder, not a file
  CHECK(!HasXex(dir / "hx2"));
}

static void TestCheckImage(const fs::path& dir) {
  const std::string expected = Sha1Hex(kXex.data(), kXex.size());
  const fs::path iso = WriteIso(dir, "game.iso", kGame);
  ImageCheck c = CheckImage(iso, expected);
  CHECK(c.verdict == Verdict::kOk);
  CHECK(c.volume.file_count == 5);
  CHECK(c.volume.file_bytes == kXex.size() + 70000 + 3000 + 2048);
  // The supported hash by default: the fixture xex is not the real one.
  CHECK(CheckImage(iso).verdict == Verdict::kWrongVersion);
  // Another xex.
  auto other = kGame;
  other[0].data[100] ^= 1;
  CHECK(CheckImage(WriteIso(dir, "other.iso", other), expected).verdict == Verdict::kWrongVersion);
  // No xex at all.
  CHECK(CheckImage(WriteIso(dir, "noxex.iso", {{"anm.afs", "x"}}), expected).verdict ==
        Verdict::kNoXex);
  // Not an Xbox image; damaged; missing.
  WriteAll(dir / "notxbox.iso", std::string(200000, 'z'));
  CHECK(CheckImage(dir / "notxbox.iso", expected).verdict == Verdict::kNotXbox);
  // Cut inside the root directory table (the fixture writes it last; each
  // table is padded to a whole sector, so cutting only the last byte would
  // remove padding, not data).
  auto bytes = ReadAll(iso);
  uint32_t root_sector = 0;
  std::memcpy(&root_sector, &bytes[32 * 2048 + 20], 4);
  WriteAll(dir / "cut.iso", bytes.substr(0, size_t(root_sector) * 2048 + 10));
  CHECK(CheckImage(dir / "cut.iso", expected).verdict == Verdict::kDamaged);
  CHECK(CheckImage(dir / "missing.iso", expected).verdict == Verdict::kUnreadable);
}

static void TestCheckFolder(const fs::path& dir) {
  const std::string expected = Sha1Hex(kXex.data(), kXex.size());
  WriteAll(dir / "f_ok" / "default.xex", kXex);
  CHECK(CheckFolder(dir / "f_ok", expected) == Verdict::kOk);
  CHECK(CheckFolder(dir / "f_ok") == Verdict::kWrongVersion);
  WriteAll(dir / "f_bad" / "default.xex", kXex + "!");
  CHECK(CheckFolder(dir / "f_bad", expected) == Verdict::kWrongVersion);
  fs::create_directories(dir / "f_empty");
  CHECK(CheckFolder(dir / "f_empty", expected) == Verdict::kNoXex);
  CHECK(CheckFolder(dir / "f_missing", expected) == Verdict::kNoXex);
}

static void TestInspectTarget(const fs::path& dir) {
  CHECK(InspectTarget(dir / "t_new") == TargetState::kNew);
  fs::create_directories(dir / "t_empty");
  CHECK(InspectTarget(dir / "t_empty") == TargetState::kEmpty);
  WriteAll(dir / "t_full" / "keep.txt", "mine");
  CHECK(InspectTarget(dir / "t_full") == TargetState::kNotEmpty);
  WriteAll(dir / "t_file", "x");
  CHECK(InspectTarget(dir / "t_file") == TargetState::kNotAFolder);
}

struct Run {
  ExtractResult result;
  std::vector<Progress> progress;
};

static Run DoExtract(const fs::path& iso, const fs::path& target, const std::string& expected,
                     std::function<bool(const Progress&)> cancel_when = nullptr) {
  doa4::xdvdfs::FileSource src;
  Run run;
  if (!src.Open(iso)) {
    run.result.outcome = ExtractOutcome::kFailed;
    return run;
  }
  ImageCheck check = CheckImage(iso, expected);
  std::atomic<bool> cancel{false};
  run.result = Extract(src, check.volume, target, expected, cancel, [&](const Progress& p) {
    run.progress.push_back(p);
    if (cancel_when && cancel_when(p)) cancel = true;
  });
  return run;
}

static void TestExtract(const fs::path& dir) {
  const std::string expected = Sha1Hex(kXex.data(), kXex.size());
  const fs::path iso = WriteIso(dir, "x.iso", kGame);
  const fs::path target = dir / "out_new" / "DOA4";  // neither folder exists yet
  Run run = DoExtract(iso, target, expected);
  CHECK(run.result.outcome == ExtractOutcome::kDone && run.result.error.empty());
  for (const auto& f : kGame) CHECK(ReadAll(target / fs::path(f.path)) == f.data);
  CHECK(Tree(target) == std::set<std::string>({"default.xex", "anm.afs", "movie/",
                                               "movie/ayane.sfd", "SOUND/", "SOUND/BGM/",
                                               "SOUND/BGM/title.xma", "dic/",
                                               "dic/empty.dic"}));
  // Progress reaches the total and never goes backwards.
  CHECK(!run.progress.empty());
  if (!run.progress.empty()) {
    CHECK(run.progress.back().done == run.progress.back().total);
    CHECK(run.progress.back().total == kXex.size() + 70000 + 3000 + 2048);
    for (size_t i = 1; i < run.progress.size(); ++i)
      CHECK(run.progress[i].done >= run.progress[i - 1].done);
  }
  // The image itself is untouched.
  const auto before = BuildImage(kGame).bytes;
  CHECK(ReadAll(iso) == std::string(before.begin(), before.end()));
}

// Cancel part-way: everything this run created goes, nothing else does.
static void TestCancelCleansUpOnlyWhatItCreated(const fs::path& dir) {
  const std::string expected = Sha1Hex(kXex.data(), kXex.size());
  const fs::path iso = WriteIso(dir, "c.iso", kGame);
  // (a) A folder the user picked that already existed (empty): it stays.
  fs::create_directories(dir / "picked");
  Run run = DoExtract(iso, dir / "picked", expected,
                      [](const Progress& p) { return p.done > 20000; });
  CHECK(run.result.outcome == ExtractOutcome::kCancelled);
  CHECK(fs::is_directory(dir / "picked") && Tree(dir / "picked").empty());
  // (b) New folders, two levels: both go; their existing parent stays.
  fs::create_directories(dir / "parent");
  WriteAll(dir / "parent" / "mine.txt", "user file");
  run = DoExtract(iso, dir / "parent" / "a" / "DOA4", expected,
                  [](const Progress& p) { return p.done > 20000; });
  CHECK(run.result.outcome == ExtractOutcome::kCancelled);
  CHECK(Tree(dir / "parent") == std::set<std::string>({"mine.txt"}));
  // Control: the cancel came after some files were already written.
  CHECK(!run.progress.empty() && run.progress.back().done > 20000);
}

// Cancel inside one big file (the real disc has files of up to ~2 GB): the
// copy stops within that file, it does not run on to the end of it.
static void TestCancelInsideABigFile(const fs::path& dir) {
  const std::string expected = Sha1Hex(kXex.data(), kXex.size());
  const size_t big = 20u << 20;  // more than two 8 MB chunks
  const fs::path iso = WriteIso(dir, "big.iso", {{"default.xex", kXex}, {"big.afs", Bytes(big, 9)}});
  Run run = DoExtract(iso, dir / "big_out", expected,
                      [&](const Progress& p) { return p.file == "big.afs"; });
  CHECK(run.result.outcome == ExtractOutcome::kCancelled);
  CHECK(!run.progress.empty() && run.progress.back().done < kXex.size() + big);
  CHECK(!fs::exists(dir / "big_out"));
}

// A copy whose default.xex does not verify fails and is removed.
static void TestVerifyAfterCopy(const fs::path& dir) {
  const std::string expected = Sha1Hex(kXex.data(), kXex.size());
  const fs::path iso = WriteIso(dir, "v.iso", kGame);
  Run run = DoExtract(iso, dir / "v_out", "0000000000000000000000000000000000000000");
  CHECK(run.result.outcome == ExtractOutcome::kFailed && !run.result.error.empty());
  CHECK(!fs::exists(dir / "v_out"));
  // Control: the same image with the right hash succeeds.
  CHECK(DoExtract(iso, dir / "v_out2", expected).result.outcome == ExtractOutcome::kDone);
}

// A read error part-way (the disc image vanishes or a sector is unreadable).
class FailingSource : public doa4::xdvdfs::Source {
 public:
  FailingSource(doa4::xdvdfs::Source& inner, uint64_t fail_at) : inner_(inner), fail_at_(fail_at) {}
  uint64_t size() const override { return inner_.size(); }
  bool Read(uint64_t offset, void* dst, size_t n) override {
    if (offset + n > fail_at_) return false;
    return inner_.Read(offset, dst, n);
  }
  doa4::xdvdfs::Source& inner_;
  uint64_t fail_at_;
};

static void TestReadErrorCleansUp(const fs::path& dir) {
  const std::string expected = Sha1Hex(kXex.data(), kXex.size());
  const fs::path iso = WriteIso(dir, "r.iso", kGame);
  doa4::xdvdfs::FileSource file;
  CHECK(file.Open(iso));
  ImageCheck check = CheckImage(iso, expected);
  const auto* anm = doa4::xdvdfs::FindFile(check.volume, "anm.afs");
  CHECK(anm != nullptr);
  if (!anm) return;
  FailingSource src(file, anm->offset + 100);
  std::atomic<bool> cancel{false};
  ExtractResult r = Extract(src, check.volume, dir / "r_out", expected, cancel, nullptr);
  CHECK(r.outcome == ExtractOutcome::kFailed && !r.error.empty());
  CHECK(!fs::exists(dir / "r_out"));
}

// A target that is not empty (or is a file) is refused before anything is
// written: extraction never mixes with or overwrites other files.
static void TestRefusesNonEmptyTarget(const fs::path& dir) {
  const std::string expected = Sha1Hex(kXex.data(), kXex.size());
  const fs::path iso = WriteIso(dir, "n.iso", kGame);
  WriteAll(dir / "full" / "default.xex", "the user's own file");
  Run run = DoExtract(iso, dir / "full", expected);
  CHECK(run.result.outcome == ExtractOutcome::kFailed);
  CHECK(Tree(dir / "full") == std::set<std::string>({"default.xex"}));
  CHECK(ReadAll(dir / "full" / "default.xex") == "the user's own file");
  CHECK(run.progress.empty());
  WriteAll(dir / "afile", "x");
  CHECK(DoExtract(iso, dir / "afile", expected).result.outcome == ExtractOutcome::kFailed);
  CHECK(ReadAll(dir / "afile") == "x");
  // Control: the same folder, emptied, is accepted (the refusal was the
  // folder's contents, not something else).
  fs::remove(dir / "full" / "default.xex");
  CHECK(DoExtract(iso, dir / "full", expected).result.outcome == ExtractOutcome::kDone);
}

// Folder names outside the Windows ANSI code page (the photo lesson: such a
// path must never reach path::string()).
static void TestNonAnsiTarget(const fs::path& dir) {
  const std::string expected = Sha1Hex(kXex.data(), kXex.size());
  const fs::path iso = WriteIso(dir, "u.iso", kGame);
  const fs::path target = dir / fs::path(u8"\u30b2\u30fc\u30e0") / "DOA4";
  Run run;
  try {
    run = DoExtract(iso, target, expected);
  } catch (const std::exception& e) {
    std::printf("FAIL threw: %s\n", e.what());
    ++g_failures;
  }
  CHECK(run.result.outcome == ExtractOutcome::kDone);
  CHECK(CheckFolder(target, expected) == Verdict::kOk);
  // Reasons naming such a path are UTF-8 and do not throw.
  StartChoice c = ChooseStartPath("", target / "gone", [](const fs::path&) { return false; });
  CHECK(c.reason.find("\xE3\x82\xB2") != std::string::npos);
}

int main() {
  const fs::path dir = fs::temp_directory_path() / "doa4_gamefiles_test_data";
  fs::remove_all(dir);
  fs::create_directories(dir);
  try {
    TestSha1();
    TestChooseStartPath();
    TestHasXex(dir);
    TestCheckImage(dir);
    TestCheckFolder(dir);
    TestInspectTarget(dir);
    TestExtract(dir);
    TestCancelCleansUpOnlyWhatItCreated(dir);
    TestCancelInsideABigFile(dir);
    TestVerifyAfterCopy(dir);
    TestReadErrorCleansUp(dir);
    TestRefusesNonEmptyTarget(dir);
    TestNonAnsiTarget(dir);
  } catch (const std::exception& e) {
    std::printf("FAIL threw: %s\n", e.what());
    ++g_failures;
  }
  fs::remove_all(dir);
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("gamefiles: all checks passed\n");
  return 0;
}

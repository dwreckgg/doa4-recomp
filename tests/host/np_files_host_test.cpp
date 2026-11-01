// Host test for src/net/np_files.cpp (the game-files fingerprint: one group per KNOWN top-level entry of the disc).
// Built and run by run_np_files_test.bat. Temp trees live under <DOA4_ROOT>/np_runs/t8_<pid>_<tick>/ (Doa4Root():
// the environment variable DOA4_ROOT, by default the parent folder of the repository this file is in) and are
// removed at the end; the first output line names the folder. "np_files_host_test.exe -v" prints each group before
// it runs.
//
// Expected values are derived independently of the code under test: the golden group hashes below come from
// DOA2U's own netplay_files.c (doa2u-release 6c35c7b) compiled with a small driver and run on one-entry trees
// (np_runs/review8_a1/drv.exe; with a single file in the folder its whole-tree hash IS the group hash: path, NUL,
// size, sampled content hash), over the deterministic contents this file writes (Fill: byte(k) = k*7 + (k>>8)*13 +
// seed, mod 256; the generator is np_runs/t8f1_golden.py). default.xex (hashed in full here, sampled by DOA2U) is
// checked against a whole-file FNV model written in Python.
//
// Real data: with DOA4_NP_GAME_DIR set, the real game folder is fingerprinted twice (read-only); without it the
// section prints SKIPPED and the summary says "UNMEASURED: real-data section skipped".

#include "../../src/net/np_files.h"
#include "../../src/net/np_rules.h"
#include "../../src/net/np_wire.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace files = doa4::np::files;
namespace rules = doa4::np::rules;
namespace wire = doa4::np::wire;
using V = std::vector<std::string>;

static int g_checks = 0;
static int g_failures = 0;
static bool g_verbose = false;
static bool g_real_ran = false;
static const char* g_group = "";
#define CHECK(...)                                                                                   \
  do {                                                                                               \
    ++g_checks;                                                                                      \
    if (!(__VA_ARGS__)) {                                                                            \
      ++g_failures;                                                                                  \
      std::printf("FAIL [%s] line %d: %s\n", g_group, __LINE__, #__VA_ARGS__);                       \
    }                                                                                                \
  } while (0)
#define GROUP(name)                                \
  do {                                             \
    g_group = name;                                \
    if (g_verbose) std::printf("-- %s\n", name);   \
  } while (0)

static fs::path g_root;

// The folder that holds the worktrees and np_runs: DOA4_ROOT, by default the parent folder of the repository this
// source file is in (tests/host/ -> tests/ -> the repository -> its parent), as tools/np_run.py derives it. The bat
// compiles this file by its full path, so __FILE__ is absolute.
static fs::path Doa4Root() {
  const char* env = std::getenv("DOA4_ROOT");
  if (env && *env) return fs::absolute(fs::path(env));
  return fs::absolute(fs::path(__FILE__)).parent_path().parent_path().parent_path().parent_path();
}

static const char* const kKnown[] = {"anm.afs", "avt.afs",  "chrone.afs", "chrthree.afs", "chrtwo.afs",
                                     "cmn.afs", "default.xex", "dic",       "mot.afs",      "snd.afs",
                                     "sound",   "spr.afs",   "stg.afs"};
static const size_t kKnownCount = sizeof kKnown / sizeof kKnown[0];

static void Fill(const fs::path& p, uint64_t size, unsigned seed) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  std::vector<char> blk(1 << 20);
  for (uint64_t off = 0; off < size;) {
    uint64_t n = size - off < blk.size() ? size - off : blk.size();
    for (uint64_t i = 0; i < n; ++i) {
      uint64_t k = off + i;
      blk[i] = static_cast<char>((k * 7 + (k >> 8) * 13 + seed) & 0xFF);
    }
    f.write(blk.data(), static_cast<std::streamsize>(n));
    off += n;
  }
}

// The standard tree: every known entry present (snd.afs 40 MB, mot.afs 1 KB, default.xex 300 KB so its middle lies
// outside both 64 KB samples - a 100 KB file would be covered whole by the first and last samples and could not tell
// full from sampled hashing; sound is spelled SOUND; dic holds three files), plus movie/.
static void MakeTree(const fs::path& d) {
  Fill(d / "default.xex", 300 * 1024, 3);
  Fill(d / "snd.afs", 40ull * 1024 * 1024, 1);
  Fill(d / "mot.afs", 1024, 2);
  Fill(d / "SOUND" / "x.bin", 5000, 4);
  Fill(d / "movie" / "m.sfd", 3000, 5);
  unsigned seed = 20;
  for (const char* n : {"anm.afs", "avt.afs", "chrone.afs", "chrthree.afs", "chrtwo.afs", "cmn.afs", "spr.afs", "stg.afs"})
    Fill(d / n, 2000, seed++);
  for (const char* n : {"d1.bin", "d2.bin", "d3.bin"}) Fill(d / "dic" / n, 700, seed++);
}

static void Flip(const fs::path& p, uint64_t off) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  unsigned char b = 0;
  DWORD n = 0;
  LARGE_INTEGER pos;
  pos.QuadPart = static_cast<LONGLONG>(off);
  bool ok = h != INVALID_HANDLE_VALUE && SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) &&
            ReadFile(h, &b, 1, &n, nullptr) && n == 1;
  b = static_cast<unsigned char>(b ^ 0xFF);
  ok = ok && SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && WriteFile(h, &b, 1, &n, nullptr) && n == 1;
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  if (!ok) {
    std::printf("FIXTURE ERROR: cannot flip byte %llu of %s\n", static_cast<unsigned long long>(off),
                p.string().c_str());
    std::exit(2);
  }
}

static std::string T(const fs::path& d) { return files::Text(files::Fingerprint(d)); }

static const files::Group* Find(const std::vector<files::Group>& g, const char* name) {
  for (const auto& x : g)
    if (x.name == name) return &x;
  return nullptr;
}

static std::string Hex16(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

// Flips one byte, fingerprints, flips it back and checks the tree reads as the baseline again (the control that
// the difference came from the flip, not from the tree changing under us). Returns the differing groups.
static V FlipDiff(const fs::path& d, const std::string& base, const char* rel, uint64_t off) {
  Flip(d / rel, off);
  V diff = rules::DifferingGroups(base, T(d));
  Flip(d / rel, off);
  CHECK(T(d) == base);
  return diff;
}

static void TestStandardTree() {
  GROUP("standard tree");
  fs::path d = g_root / "std";
  MakeTree(d);
  uint32_t ignored = 99;
  auto g1 = files::Fingerprint(d, &ignored);
  auto g2 = files::Fingerprint(d);
  std::string base = files::Text(g1);

  CHECK(ignored == 0);  // movie is left out and is not counted as an unknown entry
  CHECK(g1.size() == kKnownCount);
  CHECK(Find(g1, "movie") == nullptr);
  for (size_t i = 0; i < g1.size() && i < kKnownCount; ++i) {
    CHECK(g1[i].name == kKnown[i]);  // the disc's list, sorted
    CHECK(g1[i].hash != 0 && !g1[i].unreadable && !g1[i].missing);
    CHECK(g1[i].files == (g1[i].name == "dic" ? 3u : 1u));
  }
  CHECK(Find(g1, "SOUND") == nullptr);  // names lower case
  CHECK(Find(g1, "snd.afs") && Find(g1, "mot.afs") && Find(g1, "default.xex") && Find(g1, "sound"));
  CHECK(Find(g1, "snd.afs")->hash != Find(g1, "mot.afs")->hash);
  CHECK(T(d) == base);  // twice equal
  CHECK(files::Text(g2) == base);
  CHECK(rules::DifferingGroups(base, base).empty());
  size_t lines = 0;
  for (char c : base) lines += c == '\n';
  CHECK(lines == kKnownCount);
  CHECK(base.rfind("anm.afs=", 0) == 0);

  // Golden hashes: DOA2U's own C (see the header) for the sampled files, a whole-file model for default.xex.
  CHECK(Hex16(Find(g1, "snd.afs")->hash) == "44d2f038b887e645");
  CHECK(Hex16(Find(g1, "mot.afs")->hash) == "f3c03745403d9ddb");
  CHECK(Hex16(Find(g1, "default.xex")->hash) == "1b9834d066e3f651");
  CHECK(Hex16(Find(g1, "sound")->hash) == "22482c19ab927436");

  GROUP("sampled file: what is and is not covered");
  const char* a = "snd.afs";
  const uint64_t MB = 1024ull * 1024;
  const uint64_t A = 40 * MB;
  CHECK(FlipDiff(d, base, a, 10) == V({"snd.afs"}));                    // inside the first 64 KB
  CHECK(FlipDiff(d, base, a, 60 * 1024) == V({"snd.afs"}));             // late in the first sample (64 KB, not 32 KB)
  CHECK(FlipDiff(d, base, a, 16 * MB + 10) == V({"snd.afs"}));          // the 16 MB sample
  CHECK(FlipDiff(d, base, a, 16 * MB + 64 * 1024 - 1) == V({"snd.afs"}));  // its last covered byte
  CHECK(FlipDiff(d, base, a, 32 * MB + 10) == V({"snd.afs"}));          // the 32 MB sample (stride is 16 MB)
  CHECK(FlipDiff(d, base, a, A - 1) == V({"snd.afs"}));                 // the last byte: the last-64-KB sample
  CHECK(FlipDiff(d, base, a, A - 64 * 1024) == V({"snd.afs"}));         // the first byte of the last sample
  // The documented blind spot: bytes outside every sample are not seen. Pinned on purpose; a change to full hashing
  // must update these checks consciously.
  CHECK(FlipDiff(d, base, a, 20 * MB + 100 * 1024).empty());
  CHECK(FlipDiff(d, base, a, 16 * MB + 64 * 1024).empty());           // one past the 16 MB sample
  CHECK(FlipDiff(d, base, a, 64 * 1024).empty());                     // one past the first sample

  GROUP("size, default.xex, movie");
  {
    std::ofstream f(d / "mot.afs", std::ios::binary | std::ios::app);
    f.put('x');
  }
  CHECK(rules::DifferingGroups(base, T(d)) == V({"mot.afs"}));  // size is part of the hash
  fs::resize_file(d / "mot.afs", 1024);
  CHECK(T(d) == base);
  CHECK(FlipDiff(d, base, "default.xex", 0) == V({"default.xex"}));
  CHECK(FlipDiff(d, base, "default.xex", 150 * 1024) == V({"default.xex"}));  // outside both samples: hashed in full
  CHECK(FlipDiff(d, base, "default.xex", 300 * 1024 - 1) == V({"default.xex"}));
  CHECK(FlipDiff(d, base, "movie/m.sfd", 100).empty());
  Fill(d / "movie" / "extra.sfd", 10, 9);
  CHECK(T(d) == base);
  fs::remove(d / "movie" / "extra.sfd");

  GROUP("extra files inside a known folder; ignored names");
  Fill(d / "SOUND" / "y.bin", 10, 6);
  CHECK(rules::DifferingGroups(base, T(d)) == V({"sound"}));
  fs::remove(d / "SOUND" / "y.bin");
  CHECK(T(d) == base);
  Fill(d / "SOUND" / "desktop.ini", 10, 6);
  Fill(d / "Thumbs.db", 10, 6);
  CHECK(T(d) == base);
  fs::remove(d / "SOUND" / "desktop.ini");
  fs::remove(d / "Thumbs.db");

  GROUP("share modes");
  {
    // M2: a file held open for WRITING by someone else is still read (FILE_SHARE_WRITE in the open).
    HANDLE w = CreateFileW((d / "mot.afs").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    CHECK(w != INVALID_HANDLE_VALUE);
    auto gw = files::Fingerprint(d);
    CHECK(Find(gw, "mot.afs") && !Find(gw, "mot.afs")->unreadable);
    CHECK(files::Text(gw) == base);
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    // M4: a file held with DELETE access (an installer or launcher about to rename it) is still read
    // (FILE_SHARE_DELETE in the open), and the holder can rename it while it is held.
    HANDLE h = CreateFileW((d / "mot.afs").c_str(), GENERIC_READ | DELETE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    auto gd = files::Fingerprint(d);
    CHECK(Find(gd, "mot.afs") && !Find(gd, "mot.afs")->unreadable);
    CHECK(files::Text(gd) == base);
    bool moved = MoveFileW((d / "mot.afs").c_str(), (d / "mot.afs.moved").c_str()) != 0;
    CHECK(moved);  // the fixture itself: the held file can be renamed by the test
    if (moved) {
      CHECK(Find(files::Fingerprint(d), "mot.afs")->missing);
      CHECK(MoveFileW((d / "mot.afs.moved").c_str(), (d / "mot.afs").c_str()) != 0);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(T(d) == base);
  }

  GROUP("unreadable");
  {
    HANDLE h = CreateFileW((d / "SOUND" / "x.bin").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);  // the fixture holds the file exclusively
    auto g = files::Fingerprint(d);
    std::string t = files::Text(g);
    const files::Group* s = Find(g, "sound");
    CHECK(s && s->unreadable && !s->missing && s->hash == 0 && s->files == 0);
    CHECK(t.find("sound=unreadable\n") != std::string::npos);
    CHECK(Find(g, "snd.afs") && !Find(g, "snd.afs")->unreadable);  // the others still read
    CHECK(rules::DifferingGroups(base, t) == V({"sound"}));
    CHECK(rules::DifferingGroups(t, t) == V({"sound"}));  // an unreadable group agrees with nothing, itself included
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(T(d) == base);  // released: back to the baseline
    // a top-level file
    h = CreateFileW((d / "mot.afs").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    auto g3 = files::Fingerprint(d);
    CHECK(Find(g3, "mot.afs") && Find(g3, "mot.afs")->unreadable);
    CHECK(files::Text(g3).find("mot.afs=unreadable\n") != std::string::npos);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }

  GROUP("no such folder");
  CHECK(files::Fingerprint(g_root / "does_not_exist").empty());
  CHECK(files::Text({}).empty());
}

// R18 / review I1: only the disc's own top-level entries are fingerprinted; anything else beside the game is
// ignored (counted for a log line), so extra files cannot block a match or overflow Hello::files.
static void TestExtrasAndMissing() {
  GROUP("extra top-level entries");
  fs::path d = g_root / "ex", e = g_root / "ex_plain";
  MakeTree(d);
  MakeTree(e);
  std::string base = T(e);
  for (int i = 0; i < 100; ++i) {
    char n[32];
    std::snprintf(n, sizeof n, "extra%03d", i);
    if (i % 2) {
      Fill(d / (std::string(n) + ".bin"), 100 + i, 30);
    } else {
      Fill(d / n / "inner.bin", 100 + i, 31);
    }
  }
  Fill(d / "a=b", 50, 32);  // M3: a name with '=' cannot collapse two groups: it is not a group at all
  Fill(d / "Readme.txt", 50, 33);
  uint32_t ignored = 0;
  auto g = files::Fingerprint(d, &ignored);
  CHECK(ignored == 102);
  CHECK(g.size() == kKnownCount);
  CHECK(files::Text(g) == base);  // the same Text as the tree without them
  CHECK(rules::DifferingGroups(files::Text(g), base).empty());
  CHECK(base.size() < 1000);
  // Hello::files is capped (np_wire kMaxFilesText = 1,000 bytes): the Text of the full tree, and the worst case
  // for 13 groups (every value unreadable or 16 hex digits wide, every name its own length), must encode.
  wire::Hello hello;
  hello.files = files::Text(g);
  CHECK(!wire::EncodeHello(hello).empty());
  std::vector<files::Group> worst;
  for (const char* n : kKnown) {
    files::Group x;
    x.name = n;
    x.hash = 0xFFFFFFFFFFFFFFFFull;
    worst.push_back(x);
  }
  hello.files = files::Text(worst);
  CHECK(hello.files.size() <= 1000);
  CHECK(!wire::EncodeHello(hello).empty());
  // a changed loose file changes nothing; a changed game file still does
  Fill(d / "extra001.bin", 777, 99);
  CHECK(T(d) == base);
  Flip(d / "cmn.afs", 5);
  CHECK(rules::DifferingGroups(base, T(d)) == V({"cmn.afs"}));
  Flip(d / "cmn.afs", 5);
  // a known entry spelled in another case is the known one
  fs::rename(d / "stg.afs", d / "STG.AFS");
  CHECK(T(d) == base);
  // the plain tree has no unknown entry
  ignored = 7;
  files::Fingerprint(e, &ignored);
  CHECK(ignored == 0);

  GROUP("missing entries");
  fs::remove(d / "chrone.afs");
  auto gm = files::Fingerprint(d);
  const files::Group* m = Find(gm, "chrone.afs");
  CHECK(m && m->missing && !m->unreadable && m->hash == 0 && m->files == 0);
  std::string tm = files::Text(gm);
  CHECK(tm.find("chrone.afs=missing\n") != std::string::npos);
  CHECK(gm.size() == kKnownCount);
  CHECK(rules::DifferingGroups(base, tm) == V({"chrone.afs"}));
  CHECK(rules::DifferingGroups(tm, tm) == V({"chrone.afs"}));  // missing agrees with nothing, itself included
  // a file where a known folder should be is still an entry
  fs::remove_all(d / "dic");
  Fill(d / "dic", 10, 40);
  auto gd = files::Fingerprint(d);
  CHECK(Find(gd, "dic") && !Find(gd, "dic")->missing && Find(gd, "dic")->files == 1);
  // an empty known folder is a group with no files, neither missing nor unreadable
  fs::remove(d / "dic");
  fs::create_directories(d / "dic");
  auto ge = files::Fingerprint(d);
  const files::Group* de = Find(ge, "dic");
  CHECK(de && de->files == 0 && !de->unreadable && !de->missing && de->hash != 0);
  // an empty folder: every known entry is missing
  fs::path z = g_root / "zero";
  fs::create_directories(z);
  uint32_t zi = 5;
  auto gz = files::Fingerprint(z, &zi);
  CHECK(gz.size() == kKnownCount && zi == 0);
  for (const auto& x : gz) CHECK(x.missing && x.hash == 0);
}

static void TestCaseAndOrder() {
  GROUP("case and order");
  fs::path a = g_root / "ca", b = g_root / "cb";
  // Same content, different letter case in every name and a different creation order.
  Fill(a / "SOUND" / "x.bin", 700, 11);
  Fill(a / "SOUND" / "Sub" / "z.bin", 800, 12);
  Fill(a / "SOUND" / "a.bin", 900, 13);
  Fill(a / "SPR.AFS", 100, 14);
  Fill(b / "spr.afs", 100, 14);
  Fill(b / "sound" / "a.bin", 900, 13);
  Fill(b / "Sound" / "sub" / "Z.BIN", 800, 12);
  Fill(b / "SOUND" / "X.BIN", 700, 11);
  auto ga = files::Fingerprint(a), gb = files::Fingerprint(b);
  CHECK(ga.size() == kKnownCount && gb.size() == kKnownCount);
  const files::Group* sa = Find(ga, "sound");
  CHECK(sa && sa->files == 3 && sa->hash != 0);
  CHECK(Find(ga, "spr.afs") && Find(ga, "spr.afs")->hash != 0 && !Find(ga, "spr.afs")->missing);
  CHECK(T(a) == T(b));
  // different content under the same names does differ (the equality above is not vacuous)
  Fill(b / "sound" / "sub" / "z.bin", 801, 12);
  CHECK(T(a) != T(b));
  CHECK(Find(files::Fingerprint(b), "sound")->hash != sa->hash);
  CHECK(Find(files::Fingerprint(b), "spr.afs")->hash == Find(ga, "spr.afs")->hash);
  // the same bytes under a different file name differ too
  Fill(b / "sound" / "sub" / "z.bin", 800, 12);
  CHECK(T(a) == T(b));
  fs::rename(b / "sound" / "sub" / "z.bin", b / "sound" / "sub" / "w.bin");
  CHECK(T(a) != T(b));
}

static void TestText() {
  GROUP("text");
  std::vector<files::Group> g(4);
  g[0].name = "z";
  g[0].hash = 0xAB;
  g[1].name = "a";
  g[1].unreadable = true;
  g[2].name = "m";
  g[2].hash = 0xFFFFFFFFFFFFFFFFull;
  g[3].name = "c";
  g[3].missing = true;
  CHECK(files::Text(g) == "a=unreadable\nc=missing\nm=ffffffffffffffff\nz=00000000000000ab\n");
}

// M1: the sampling boundaries, against DOA2U's own C (goldens from np_runs/review8_a1/drv.exe on a folder holding
// only snd.afs of this size, seed 1). A ">" in place of ">=" in the last-sample test, an off-by-one in the 64 KB
// sample or in the 16 MB stride goes red here.
static void TestBoundarySizes() {
  GROUP("boundary sizes");
  const uint64_t MB = 1024ull * 1024;
  struct Case { uint64_t size; const char* hash; };
  const Case cases[] = {
      {65535, "5a88415a23ad3193"},          {65536, "0c6e4fa93e3ada28"},          {65537, "44cf72b088a1719e"},
      {16 * MB - 1, "cb1bbd7fa0725af3"},    {16 * MB, "c22c6362887fa373"},        {16 * MB + 65535, "2acf3334fd0fcb34"},
      {16 * MB + 65536, "30ff95e2cbd43750"}, {16 * MB + 65537, "21d567fb68897c40"}, {32 * MB + 65536, "479308237c961cfa"},
      {32 * MB + 65537, "b640cd4b8376cb13"},
  };
  fs::path d = g_root / "bnd";
  for (const Case& c : cases) {
    Fill(d / "snd.afs", c.size, 1);
    auto g = files::Fingerprint(d);
    const files::Group* x = Find(g, "snd.afs");
    CHECK(x && !x->unreadable && !x->missing && x->files == 1);
    CHECK(x && Hex16(x->hash) == c.hash);
    if (x && Hex16(x->hash) != c.hash)
      std::printf("  size %llu: got %s want %s\n", static_cast<unsigned long long>(c.size), Hex16(x->hash).c_str(),
                  c.hash);
  }
}

static void TestRealData() {
  GROUP("real data");
  const char* env = std::getenv("DOA4_NP_GAME_DIR");
  if (!env || !*env) {
    std::printf("SKIPPED: real-data section (DOA4_NP_GAME_DIR not set)\n");
    return;
  }
  g_real_ran = true;
  fs::path d = env;
  using clock = std::chrono::steady_clock;
  uint32_t ignored = 99;
  auto t0 = clock::now();
  auto g1 = files::Fingerprint(d, &ignored);
  auto t1 = clock::now();
  auto g2 = files::Fingerprint(d);
  auto t2 = clock::now();
  double s1 = std::chrono::duration<double>(t1 - t0).count();
  double s2 = std::chrono::duration<double>(t2 - t1).count();
  std::printf("real data %s: first %.3f s, second %.3f s, ignored top-level entries %u\n", env, s1, s2, ignored);
  std::string text = files::Text(g1);
  std::printf("%s", text.c_str());
  for (const auto& g : g1) std::printf("  %s files=%u\n", g.name.c_str(), g.files);
  CHECK(files::Text(g2) == text);
  CHECK(g1.size() == kKnownCount);
  for (const char* n : kKnown) {
    const files::Group* x = Find(g1, n);
    CHECK(x && !x->unreadable && !x->missing && x->hash != 0 && x->files >= 1);
  }
  CHECK(Find(g1, "movie") == nullptr);
  CHECK(ignored == 0);  // the real folder holds exactly the disc's entries (and movie)
  CHECK(s2 < 2.0);      // warm: under 2 s
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
  g_root = Doa4Root() / "np_runs" /
           ("t8_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
  std::printf("temp root: %s\n", reinterpret_cast<const char*>(g_root.u8string().c_str()));
  std::error_code ec;
  fs::create_directories(g_root, ec);
  TestStandardTree();
  TestExtrasAndMissing();
  TestCaseAndOrder();
  TestText();
  TestBoundarySizes();
  TestRealData();
  fs::remove_all(g_root, ec);
  CHECK(!fs::exists(g_root));
  if (!g_real_ran) std::printf("UNMEASURED: real-data section skipped\n");
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

// Host test for src/net/np_files.cpp (the game-files fingerprint, one group per top-level file and folder).
// Built and run by run_np_files_test.bat. Temp trees live under C:/Users/fsop1/doa4/np_runs/t8_<pid>_<tick>/ and
// are removed at the end. "np_files_host_test.exe -v" prints each group before it runs.
//
// Expected values are derived independently of the code under test: the golden group hashes below come from
// scratch/t8_model.py, a Python model of DOA2U's netplay_files.c sampling written from its source (FNV-1a 64; the
// content hash of a file is FNV over (offset, bytes) per sample, the group hash FNV over (path, NUL, size, content)),
// run over the same deterministic file contents this file writes (Fill: byte(i) = i*7 + (i>>8)*13 + seed).
//
// Real data: with DOA4_NP_GAME_DIR set, the real game folder is fingerprinted twice (read-only); without it the
// section prints SKIPPED and the summary line says "UNMEASURED: real-data section skipped".

#include "../../src/net/np_files.h"
#include "../../src/net/np_rules.h"

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

// The standard tree (a.afs 40 MB, default.xex 300 KB so its middle lies outside both 64 KB samples; the brief's
// 100 KB would be covered whole by the first and last samples and could not tell full from sampled hashing).
static void MakeTree(const fs::path& d) {
  Fill(d / "default.xex", 300 * 1024, 3);
  Fill(d / "a.afs", 40ull * 1024 * 1024, 1);
  Fill(d / "b.afs", 1024, 2);
  Fill(d / "SOUND" / "x.bin", 5000, 4);
  Fill(d / "movie" / "m.sfd", 3000, 5);
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
  auto g1 = files::Fingerprint(d);
  auto g2 = files::Fingerprint(d);
  std::string base = files::Text(g1);

  CHECK(g1.size() == 4);  // movie is left out
  CHECK(Find(g1, "movie") == nullptr);
  CHECK(Find(g1, "a.afs") && Find(g1, "b.afs") && Find(g1, "default.xex") && Find(g1, "sound"));
  CHECK(Find(g1, "SOUND") == nullptr);  // names lower case
  CHECK(!g1.empty() && g1[0].name == "a.afs" && g1[1].name == "b.afs" && g1[2].name == "default.xex" &&
        g1[3].name == "sound");  // sorted by name
  for (const auto& g : g1) CHECK(g.hash != 0 && !g.unreadable && g.files == 1);
  CHECK(Find(g1, "a.afs")->hash != Find(g1, "b.afs")->hash);
  CHECK(T(d) == base);  // twice equal
  CHECK(files::Text(g2) == base);
  CHECK(rules::DifferingGroups(base, base).empty());
  // Text: four lines, name=16 lower-case hex
  size_t lines = 0;
  for (char c : base) lines += c == '\n';
  CHECK(lines == 4);
  CHECK(base.rfind("a.afs=", 0) == 0);

  // Golden hashes from the Python model (scratch/t8_model.py).
  CHECK(Hex16(Find(g1, "a.afs")->hash) == "f8e46082ba2b2e59");
  CHECK(Hex16(Find(g1, "b.afs")->hash) == "e7d4badaf962715d");
  CHECK(Hex16(Find(g1, "default.xex")->hash) == "1b9834d066e3f651");
  CHECK(Hex16(Find(g1, "sound")->hash) == "22482c19ab927436");

  GROUP("sampled file: what is and is not covered");
  const char* a = "a.afs";
  const uint64_t MB = 1024ull * 1024;
  const uint64_t A = 40 * MB;
  CHECK(FlipDiff(d, base, a, 10) == V({"a.afs"}));                    // inside the first 64 KB
  CHECK(FlipDiff(d, base, a, 60 * 1024) == V({"a.afs"}));             // late in the first sample (64 KB, not 32 KB)
  CHECK(FlipDiff(d, base, a, 16 * MB + 10) == V({"a.afs"}));          // the 16 MB sample
  CHECK(FlipDiff(d, base, a, 16 * MB + 64 * 1024 - 1) == V({"a.afs"}));  // its last covered byte
  CHECK(FlipDiff(d, base, a, 32 * MB + 10) == V({"a.afs"}));          // the 32 MB sample (stride is 16 MB)
  CHECK(FlipDiff(d, base, a, A - 1) == V({"a.afs"}));                 // the last byte: the last-64-KB sample
  CHECK(FlipDiff(d, base, a, A - 64 * 1024) == V({"a.afs"}));         // the first byte of the last sample
  // The documented blind spot: bytes outside every sample are not seen. Pinned on purpose; a change to full hashing
  // must update these checks consciously.
  CHECK(FlipDiff(d, base, a, 20 * MB + 100 * 1024).empty());
  CHECK(FlipDiff(d, base, a, 16 * MB + 64 * 1024).empty());           // one past the 16 MB sample
  CHECK(FlipDiff(d, base, a, 64 * 1024).empty());                     // one past the first sample

  GROUP("size, default.xex, movie");
  {
    std::ofstream f(d / "b.afs", std::ios::binary | std::ios::app);
    f.put('x');
  }
  CHECK(rules::DifferingGroups(base, T(d)) == V({"b.afs"}));  // size is part of the hash
  fs::resize_file(d / "b.afs", 1024);
  CHECK(T(d) == base);
  CHECK(FlipDiff(d, base, "default.xex", 0) == V({"default.xex"}));
  CHECK(FlipDiff(d, base, "default.xex", 150 * 1024) == V({"default.xex"}));  // outside both samples: hashed in full
  CHECK(FlipDiff(d, base, "default.xex", 300 * 1024 - 1) == V({"default.xex"}));
  CHECK(FlipDiff(d, base, "movie/m.sfd", 100).empty());
  Fill(d / "movie" / "extra.sfd", 10, 9);
  CHECK(T(d) == base);
  fs::remove(d / "movie" / "extra.sfd");

  GROUP("extra files and ignored names");
  Fill(d / "SOUND" / "y.bin", 10, 6);
  CHECK(rules::DifferingGroups(base, T(d)) == V({"sound"}));
  fs::remove(d / "SOUND" / "y.bin");
  CHECK(T(d) == base);
  Fill(d / "SOUND" / "desktop.ini", 10, 6);
  Fill(d / "Thumbs.db", 10, 6);
  CHECK(T(d) == base);
  fs::remove(d / "SOUND" / "desktop.ini");
  fs::remove(d / "Thumbs.db");
  Fill(d / "c.afs", 10, 6);  // a new top-level file is a new group
  CHECK(rules::DifferingGroups(base, T(d)) == V({"c.afs"}));
  fs::remove(d / "c.afs");
  CHECK(T(d) == base);
  fs::create_directories(d / "emptydir");  // an empty folder is a group with no files
  {
    auto g = files::Fingerprint(d);
    const files::Group* e = Find(g, "emptydir");
    CHECK(e && e->files == 0 && !e->unreadable);
    CHECK(rules::DifferingGroups(base, files::Text(g)) == V({"emptydir"}));
  }
  fs::remove(d / "emptydir");

  GROUP("unreadable");
  {
    HANDLE h = CreateFileW((d / "SOUND" / "x.bin").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);  // the fixture holds the file exclusively
    auto g = files::Fingerprint(d);
    std::string t = files::Text(g);
    const files::Group* s = Find(g, "sound");
    CHECK(s && s->unreadable && s->hash == 0 && s->files == 0);
    CHECK(t.find("sound=unreadable\n") != std::string::npos);
    CHECK(Find(g, "a.afs") && !Find(g, "a.afs")->unreadable);  // the others still read
    CHECK(rules::DifferingGroups(base, t) == V({"sound"}));
    CHECK(rules::DifferingGroups(t, t) == V({"sound"}));  // an unreadable group agrees with nothing, itself included
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(T(d) == base);  // released: back to the baseline
    // a top-level file
    h = CreateFileW((d / "b.afs").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    auto g3 = files::Fingerprint(d);
    CHECK(Find(g3, "b.afs") && Find(g3, "b.afs")->unreadable);
    CHECK(files::Text(g3).find("b.afs=unreadable\n") != std::string::npos);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }

  GROUP("no such folder");
  CHECK(files::Fingerprint(g_root / "does_not_exist").empty());
  CHECK(files::Text({}).empty());
}

static void TestCaseAndOrder() {
  GROUP("case and order");
  fs::path a = g_root / "ca", b = g_root / "cb";
  // Same content, different letter case in every name and a different creation order.
  Fill(a / "SOUND" / "x.bin", 700, 11);
  Fill(a / "SOUND" / "Sub" / "z.bin", 800, 12);
  Fill(a / "SOUND" / "a.bin", 900, 13);
  Fill(a / "Z.AFS", 100, 14);
  Fill(b / "z.afs", 100, 14);
  Fill(b / "sound" / "a.bin", 900, 13);
  Fill(b / "Sound" / "sub" / "Z.BIN", 800, 12);
  Fill(b / "SOUND" / "X.BIN", 700, 11);
  auto ga = files::Fingerprint(a), gb = files::Fingerprint(b);
  CHECK(ga.size() == 2 && gb.size() == 2);
  CHECK(!ga.empty() && ga[0].name == "sound" && ga[1].name == "z.afs");
  CHECK(ga.size() == 2 && ga[0].files == 3 && ga[0].hash != 0);
  CHECK(T(a) == T(b));
  CHECK(rules::DifferingGroups(files::Text(ga), files::Text(gb)).empty());
  // a different content under the same names does differ (the equality above is not vacuous)
  Fill(b / "sound" / "sub" / "z.bin", 801, 12);
  CHECK(rules::DifferingGroups(T(a), T(b)) == V({"sound"}));
  // the same bytes under a different file name differ too
  Fill(b / "sound" / "sub" / "z.bin", 800, 12);
  CHECK(T(a) == T(b));
  fs::rename(b / "sound" / "sub" / "z.bin", b / "sound" / "sub" / "w.bin");
  CHECK(rules::DifferingGroups(T(a), T(b)) == V({"sound"}));
}

static void TestText() {
  GROUP("text");
  std::vector<files::Group> g(3);
  g[0].name = "z";
  g[0].hash = 0xAB;
  g[1].name = "a";
  g[1].unreadable = true;
  g[2].name = "m";
  g[2].hash = 0xFFFFFFFFFFFFFFFFull;
  CHECK(files::Text(g) == "a=unreadable\nm=ffffffffffffffff\nz=00000000000000ab\n");
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
  auto t0 = clock::now();
  auto g1 = files::Fingerprint(d);
  auto t1 = clock::now();
  auto g2 = files::Fingerprint(d);
  auto t2 = clock::now();
  double s1 = std::chrono::duration<double>(t1 - t0).count();
  double s2 = std::chrono::duration<double>(t2 - t1).count();
  std::printf("real data %s: first %.3f s, second %.3f s\n", env, s1, s2);
  std::string text = files::Text(g1);
  std::printf("%s", text.c_str());
  for (const auto& g : g1) std::printf("  %s files=%u\n", g.name.c_str(), g.files);
  CHECK(files::Text(g2) == text);
  const char* expect[] = {"anm.afs", "avt.afs", "chrone.afs", "chrthree.afs", "chrtwo.afs", "cmn.afs", "default.xex",
                          "dic",     "mot.afs", "snd.afs",    "sound",        "spr.afs",    "stg.afs"};
  CHECK(g1.size() == 13);
  for (const char* n : expect) {
    const files::Group* x = Find(g1, n);
    CHECK(x && !x->unreadable && x->hash != 0 && x->files >= 1);
  }
  CHECK(Find(g1, "movie") == nullptr);
  CHECK(s2 < 2.0);  // warm: under 2 s
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
  g_root = fs::path("C:/Users/fsop1/doa4/np_runs") /
           ("t8_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
  std::error_code ec;
  fs::create_directories(g_root, ec);
  TestStandardTree();
  TestCaseAndOrder();
  TestText();
  TestRealData();
  fs::remove_all(g_root, ec);
  CHECK(!fs::exists(g_root));
  if (!g_real_ran) std::printf("UNMEASURED: real-data section skipped\n");
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

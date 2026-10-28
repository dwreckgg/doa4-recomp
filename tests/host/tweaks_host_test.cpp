// Host test for src/doa4_tweaks.cpp, against the Xenia patch file's bytes and
// the image's originals (doa4_tweak_fixtures.h). Built and run by
// run_tweaks_test.bat.

#include "../../src/doa4_tweaks.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "doa4_tweak_fixtures.h"

static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using namespace doa4::tweaks;
namespace fx = doa4_fixtures;

const std::vector<fx::FixturePatch>& FixtureFor(size_t tweak) {
  switch (tweak) {
    case kTagAllStages:
      return fx::kTagAllStages;
    case kMpHiddenCharacters:
      return fx::kMpHiddenCharacters;
    default:
      return fx::kAlpha152;
  }
}

// Guest memory holding the image's original bytes at every patch address.
// Writes to read-only addresses fault (as on the host) unless made writable.
struct FakeGuest {
  std::map<uint32_t, uint8_t> bytes;
  std::set<uint32_t> writable_read_only;  // read-only addresses made writable
  int writes = 0;
  int faults = 0;
  std::vector<std::pair<uint32_t, bool>> protect_calls;
  bool protect_fails = false;

  doa4::GuestBytes guest{[this](uint32_t a) { return bytes[a]; },
                         [this](uint32_t a, uint8_t v) {
                           if (IsReadOnly(a) && !writable_read_only.count(a)) {
                             ++faults;
                             return;
                           }
                           bytes[a] = v;
                           ++writes;
                         }};
  ProtectFn protect = [this](uint32_t address, uint32_t size, bool writable) {
    protect_calls.emplace_back(address, writable);
    if (protect_fails) return false;
    for (uint32_t a = address; a < address + size; ++a) {
      if (writable) {
        writable_read_only.insert(a);
      } else {
        writable_read_only.erase(a);
      }
    }
    return true;
  };

  FakeGuest() {
    for (size_t t = 0; t < AllTweaks().size(); ++t) {
      for (const auto& p : FixtureFor(t)) {
        for (size_t i = 0; i < p.original.size(); ++i) bytes[p.address + uint32_t(i)] = p.original[i];
      }
    }
  }
  bool Holds(const std::vector<fx::FixturePatch>& patches, bool on) {
    for (const auto& p : patches) {
      const auto& want = on ? p.on : p.original;
      for (size_t i = 0; i < want.size(); ++i) {
        if (bytes[p.address + uint32_t(i)] != want[i]) return false;
      }
    }
    return true;
  }
};

static void TestListsMatchThePatchFile() {
  for (size_t t = 0; t < AllTweaks().size(); ++t) {
    std::vector<Patch> mine = AllTweaks()[t].patches;
    std::sort(mine.begin(), mine.end(),
              [](const Patch& a, const Patch& b) { return a.address < b.address; });
    const auto& file = FixtureFor(t);
    CHECK(mine.size() == file.size());
    for (size_t i = 0; i < std::min(mine.size(), file.size()); ++i) {
      CHECK(mine[i].address == file[i].address);
      CHECK(mine[i].bytes == file[i].on);
    }
  }
  CHECK(std::string(AllTweaks()[kTagAllStages].cvar) == "doa4_tag_all_stages");
  CHECK(std::string(AllTweaks()[kMpHiddenCharacters].cvar) == "doa4_mp_hidden_characters");
  CHECK(std::string(AllTweaks()[kAlpha152].cvar) == "doa4_alpha152");
  CHECK(!AllTweaks()[kTagAllStages].needs_restart);
  CHECK(!AllTweaks()[kMpHiddenCharacters].needs_restart);
  CHECK(AllTweaks()[kAlpha152].needs_restart);
}

static void TestReadOnlyRanges() {
  CHECK(IsReadOnly(0x821B52A7));  // the Alpha-152 slot byte
  CHECK(IsReadOnly(0x82000000));
  CHECK(IsReadOnly(0x822EFFFF));
  CHECK(!IsReadOnly(0x822F0000));  // CODE starts: never patched here
  CHECK(!IsReadOnly(0x82B70000));  // DATA
  CHECK(!IsReadOnly(0x82C346D0));
  CHECK(!IsReadOnly(0x82F5F2A0));
  CHECK(!IsReadOnly(0x835AFFFF));
  CHECK(IsReadOnly(0x835B0000));
  CHECK(IsReadOnly(0x835FFFFF));
  CHECK(!IsReadOnly(0x83600000));
}

static void TestOnThenOffRestoresEveryByte() {
  for (size_t t = 0; t < AllTweaks().size(); ++t) {
    FakeGuest g;
    TweakPatcher patcher;
    patcher.CaptureOriginals(g.guest);
    CHECK(!TweakPatcher::IsOn(g.guest, t));
    CHECK(patcher.Apply(g.guest, t, true, g.protect).empty());
    CHECK(g.Holds(FixtureFor(t), true));
    CHECK(TweakPatcher::IsOn(g.guest, t));
    CHECK(patcher.Apply(g.guest, t, false, g.protect).empty());
    CHECK(g.Holds(FixtureFor(t), false));
    CHECK(!TweakPatcher::IsOn(g.guest, t));
    CHECK(g.faults == 0);
    CHECK(g.writable_read_only.empty());  // protection restored every time
    // The other tweaks' bytes were never touched.
    for (size_t other = 0; other < AllTweaks().size(); ++other) {
      if (other != t) CHECK(g.Holds(FixtureFor(other), false));
    }
  }
}

static void TestReadOnlyByteGoesThroughProtect() {
  FakeGuest g;
  TweakPatcher patcher;
  patcher.CaptureOriginals(g.guest);
  CHECK(patcher.Apply(g.guest, kAlpha152, true, g.protect).empty());
  // Exactly one read-only patch: made writable, then read-only again.
  CHECK(g.protect_calls.size() == 2);
  if (g.protect_calls.size() == 2) {
    CHECK(g.protect_calls[0] == std::make_pair(uint32_t(0x821B52A7), true));
    CHECK(g.protect_calls[1] == std::make_pair(uint32_t(0x821B52A7), false));
  }
  CHECK(g.bytes[0x821B52A7] == 0x01);
  // DATA-only tweaks never call it.
  FakeGuest g2;
  TweakPatcher p2;
  p2.CaptureOriginals(g2.guest);
  CHECK(p2.Apply(g2.guest, kTagAllStages, true, g2.protect).empty());
  CHECK(p2.Apply(g2.guest, kMpHiddenCharacters, true, g2.protect).empty());
  CHECK(g2.protect_calls.empty());
}

static void TestFailedProtectWritesNothing() {
  FakeGuest g;
  g.protect_fails = true;
  TweakPatcher patcher;
  patcher.CaptureOriginals(g.guest);
  std::string error = patcher.Apply(g.guest, kAlpha152, true, g.protect);
  CHECK(!error.empty());
  CHECK(g.writes == 0);
  CHECK(g.faults == 0);
  CHECK(g.Holds(fx::kAlpha152, false));
}

static void TestOffNeedsOriginals() {
  FakeGuest g;
  TweakPatcher patcher;
  CHECK(!patcher.captured());
  CHECK(!patcher.Apply(g.guest, kTagAllStages, false, g.protect).empty());
  CHECK(!patcher.Apply(g.guest, kTagAllStages, true, g.protect).empty());
  CHECK(g.writes == 0);
  patcher.CaptureOriginals(g.guest);
  CHECK(patcher.captured());
  CHECK(!patcher.Apply(g.guest, 3, true, g.protect).empty());  // no such tweak
}

// A second capture must not replace the originals with patched bytes.
static void TestCaptureOnlyOnce() {
  FakeGuest g;
  TweakPatcher patcher;
  patcher.CaptureOriginals(g.guest);
  CHECK(patcher.Apply(g.guest, kTagAllStages, true, g.protect).empty());
  patcher.CaptureOriginals(g.guest);
  CHECK(patcher.Apply(g.guest, kTagAllStages, false, g.protect).empty());
  CHECK(g.Holds(fx::kTagAllStages, false));
}

int main() {
  TestListsMatchThePatchFile();
  TestReadOnlyRanges();
  TestOnThenOffRestoresEveryByte();
  TestReadOnlyByteGoesThroughProtect();
  TestFailedProtectWritesNothing();
  TestOffNeedsOriginals();
  TestCaptureOnlyOnce();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("tweaks: all checks passed\n");
  return 0;
}

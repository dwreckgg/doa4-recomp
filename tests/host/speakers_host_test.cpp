// Host test for src/doa4_speakers.cpp (the Audio tab's speaker choices).
// Built and run by run_speakers_test.bat.

#include "../../src/doa4_config.h"
#include "../../src/doa4_speakers.h"

#include <toml++/toml.hpp>

#include <cstdio>
#include <string>
#include <string_view>

static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using namespace doa4::speakers;

// The SDK's accepted values: rexglue-sdk (branch doa4-surround)
// src/audio/sdl/sdl_audio_driver.cpp  audio_speakers .allowed({"auto", "stereo", "surround"})
// and include/rex/audio/speaker_mode.h ParseSpeakerMode (any other text is "auto").
static bool SdkAccepts(std::string_view value) {
  return value == "auto" || value == "stereo" || value == "surround";
}

static void TestChoices() {
  CHECK(kChoices.size() == 3);
  CHECK(std::string_view(kChoices[0].value) == "auto");  // default row = today's behaviour
  CHECK(std::string_view(kChoices[1].value) == "stereo");
  CHECK(std::string_view(kChoices[2].value) == "surround");
  for (size_t i = 0; i < kChoices.size(); ++i) {
    CHECK(SdkAccepts(kChoices[i].value));
    CHECK(ChoiceIndex(kChoices[i].value) == int(i));
    // Saved to the toml as a string and read back by the loader.
    const std::string text = doa4::UpsertTomlKey("a = 1\n[GPU]\nb = 2\n", "audio_speakers",
                                                 doa4::TomlString(kChoices[i].value));
    const toml::table table = toml::parse(text);
    CHECK(table["audio_speakers"].value_or(std::string()) == kChoices[i].value);
    CHECK(table["a"].value_or(0) == 1);  // top level, not inside [GPU]
    CHECK(!table["GPU"].as_table()->contains("audio_speakers"));
  }
  CHECK(std::string_view(kChoices[0].label) == "Automatic");
  CHECK(std::string_view(kChoices[2].label) == "5.1 surround");
  // Unknown or hand-edited values show as Automatic, as the SDK treats them.
  CHECK(ChoiceIndex("") == 0);
  CHECK(ChoiceIndex("5.1") == 0);
  CHECK(ChoiceIndex("Surround") == 0);
}

static void TestDescribe() {
  // Not opened yet (the cvar line exists but the driver has not run).
  CHECK(DescribeOutput(0, 0) == "Output: not opened yet");
  CHECK(DescribeOutput(2, 2) == "Output: 2 channels (Windows speaker setup is stereo)");
  CHECK(DescribeOutput(6, 6) == "Output: 6 channels (Windows speaker setup is 5.1)");
  CHECK(DescribeOutput(8, 6) == "Output: 8 channels (Windows speaker setup is 7.1)");
  CHECK(DescribeOutput(1, 2) == "Output: 1 channel (Windows speaker setup is mono)");
  CHECK(DescribeOutput(4, 6) == "Output: 4 channels");
}

static void TestHint() {
  // Stereo device, game sent stereo: the user may want Windows set to 5.1.
  CHECK(!Hint(2, 2, "auto").empty());
  CHECK(Hint(2, 2, "auto").find("Windows") != std::string::npos);
  // Stereo device but the user asked for 5.1: SDL folds it down; say so.
  CHECK(Hint(2, 6, "surround").find("stereo") != std::string::npos);
  CHECK(Hint(2, 6, "surround") != Hint(2, 2, "auto"));
  // 5.1 device, 5.1 stream: nothing to add.
  CHECK(Hint(6, 6, "auto").empty());
  // 5.1 device but the user chose Stereo: say the speakers get two channels.
  CHECK(!Hint(6, 2, "stereo").empty());
  CHECK(Hint(6, 2, "stereo") != Hint(2, 2, "auto"));
  // Not opened yet: no hint.
  CHECK(Hint(0, 0, "auto").empty());
}

int main() {
  TestChoices();
  TestDescribe();
  TestHint();
  if (g_failures) {
    std::printf("%d FAILED\n", g_failures);
    return 1;
  }
  std::printf("all speakers checks passed\n");
  return 0;
}

// The Audio tab's speaker choices: what each row shows, the audio_speakers
// value it saves, and the read-out of the output device. No SDK headers, so a
// host test can check the values against the SDK's accepted ones.

#pragma once

#include <array>
#include <string>
#include <string_view>

namespace doa4::speakers {

struct Choice {
  const char* label;
  const char* value;  // as saved to dead_or_alive_4.toml
};

// audio_speakers (rexglue-sdk src/audio/sdl/sdl_audio_driver.cpp), read once
// when the audio driver opens at start.
inline constexpr std::array<Choice, 3> kChoices = {{
    {"Automatic", "auto"},
    {"Stereo", "stereo"},
    {"5.1 surround", "surround"},
}};

// The kChoices row for an audio_speakers value; 0 (Automatic) when unknown,
// as the SDK treats it.
int ChoiceIndex(std::string_view value);

// "Output: 2 channels (Windows speaker setup is stereo)" for the channel count
// the device reports; "Output: not opened yet" for 0.
std::string DescribeOutput(int device_channels, int stream_channels);

// One more line of advice for this device/stream/choice, or "" when there is
// nothing to add.
std::string Hint(int device_channels, int stream_channels, std::string_view value);

}  // namespace doa4::speakers

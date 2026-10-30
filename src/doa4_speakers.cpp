// The Audio tab's speaker choices. See doa4_speakers.h.

#include "doa4_speakers.h"

namespace doa4::speakers {

int ChoiceIndex(std::string_view value) {
  for (size_t i = 0; i < kChoices.size(); ++i) {
    if (value == kChoices[i].value) return int(i);
  }
  return 0;
}

std::string DescribeOutput(int device_channels, int /*stream_channels*/) {
  switch (device_channels) {
    case 0:
      return "Output: not opened yet";
    case 1:
      return "Output: 1 channel (Windows speaker setup is mono)";
    case 2:
      return "Output: 2 channels (Windows speaker setup is stereo)";
    case 6:
      return "Output: 6 channels (Windows speaker setup is 5.1)";
    case 8:
      return "Output: 8 channels (Windows speaker setup is 7.1)";
    default:
      return "Output: " + std::to_string(device_channels) + " channels";
  }
}

std::string Hint(int device_channels, int stream_channels, std::string_view value) {
  if (device_channels == 0) return "";
  if (device_channels <= 2 && stream_channels == 6) {
    return "The game sends 5.1 but Windows is set to stereo, so it is folded down to two "
           "speakers. Set the speakers to 5.1 in Windows sound settings for real surround.";
  }
  if (device_channels <= 2) {
    return "For real surround, set the speakers to 5.1 in Windows sound settings, then choose "
           "Automatic or 5.1 surround here.";
  }
  if (stream_channels == 2) {
    return value == "stereo" ? "Stereo is chosen, so only the front two speakers play."
                             : "The game is sending stereo.";
  }
  return "";
}

}  // namespace doa4::speakers

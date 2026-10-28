// A path as UTF-8 text, for logs, messages and the menu.
//
// std::filesystem::path::string() converts to the Windows ANSI code page and
// throws when a character has no mapping (a Japanese or Chinese folder name on
// a Western Windows, for example); a throw on a worker thread ends the game.

#pragma once

#include <filesystem>
#include <string>

namespace doa4 {

inline std::string Utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

}  // namespace doa4

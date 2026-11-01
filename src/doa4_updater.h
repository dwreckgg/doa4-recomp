// src/doa4_updater.h - Check for Updates in the Online tab (netplay plan 1b, Task 4; spec 6 "In game").
// Ported from DOA2U (doa2u-release netplay-release 6c35c7b, doa2u/src/ui/doa2u_ui.cpp 282-454: HttpGet over WinHTTP,
// CheckForUpdates, DrawUpdateButton), with plan 1b's ruling B1: the release's files are fetched one by one (no zip),
// each is checked against the release's SHA256SUMS.txt before anything is renamed, all three game files are swapped,
// and a failed step renames back everything done so far. A running exe and its loaded DLLs can be renamed on Windows,
// not overwritten (DOA2U relies on the same for its exe); the renamed copies (<name>.old) are deleted when the game
// next starts (DeleteOldFiles, from the app's OnPreSetup).
// No SDK and no ImGui here: the pure file steps are host-tested (tests/host/updater_host_test.cpp); the network part
// is not (no release exists while it is written; plan 1b Task 5 runs it against GitHub).
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace doa4::updater {

// The game files a release replaces, in swap order, and the checksum file published beside them.
inline constexpr const char* kFiles[] = {"dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll"};
inline constexpr int kFileCount = 3;
inline constexpr const char* kSumsName = "SHA256SUMS.txt";
// The folder beside the exe the downloads go to (removed after every check).
inline constexpr const char* kStagingName = "update_new";
// Shown beside the disabled button, and the answer to an np_ctl.on "update" request in online play.
inline constexpr const char* kOnlineText = "Disconnect from online play first.";

struct Result {
  bool ok = false;  // the files were swapped
  std::string tag;  // the release it looked at ("" when none was picked)
  std::string message;
};

// Background thread only (it blocks for the whole download). GET https://api.github.com/repos/<DOA4_RELEASE_REPO>/
// releases?per_page=30; Choose; downloads the three files and SHA256SUMS.txt into <exe dir>/update_new/; deletes
// stale .old files; VerifyAndSwap; removes update_new. After one successful swap, later calls in the same process
// download nothing and say so (the running game is still the old one until it restarts).
Result CheckAndUpdate();

// The decision once the releases JSON arrived (pure). `json`: GitHub's release list; `channel`: DOA4_RELEASE_CHANNEL;
// `current`: DOA4_RELEASE_TAG. update = true when the highest release with kFiles + SHA256SUMS.txt (rules::PickRelease)
// is newer than `current` (CompareVersions > 0), or `current` is a self-built game (rules::IsDevBuild): any release.
// update = false: `message` says why ("No <channel> release found yet", "Already on the latest <channel> release
// (<current>)"). `urls` holds the four names -> their download URLs.
struct Choice {
  bool update = false;
  bool replaces_dev = false;  // `current` is a self-built game
  std::string tag;
  std::map<std::string, std::string> urls;
  std::string message;
};
Choice Choose(std::string_view json, std::string_view channel, std::string_view current);
// The message after a successful swap: "Updated to <tag>. Restart the game to use it.", with
// " (it replaces the self-built game)" after the tag when `replaces_dev`.
std::string UpdatedText(std::string_view tag, bool replaces_dev);

// Pure file steps (host-tested). First every kFiles[i] is checked: SHA256SUMS.txt (`sha256sums`) must name it exactly
// once and <staging>/<f> must exist with that SHA-256; anything else refuses before any rename. Then for each file in
// kFiles order: step 2i renames <dir>/<f> -> <dir>/<f>.old (nothing to rename when <dir>/<f> does not exist), step
// 2i+1 renames <staging>/<f> -> <dir>/<f>. No rename replaces an existing file (a leftover <f>.old makes step 2i
// fail). Any failed step renames back, in reverse order, everything done so far. `fail_at` (tests only) makes step n
// fail without trying it. Returns "" when all six steps were done, else why it failed (naming the file).
std::string VerifyAndSwap(const std::filesystem::path& dir, const std::filesystem::path& staging,
                          std::string_view sha256sums, int fail_at = -1);

// At start (OnPreSetup, after the previous instance has exited): deletes <dir>/<f>.old for every kFiles[i] whose
// <dir>/<f> exists (an .old whose file is missing may be the only copy left by an interrupted swap: kept). Nothing
// else is touched; a file that cannot be deleted (still in use) is left for the next start.
void DeleteOldFiles(const std::filesystem::path& dir);

// SHA256SUMS.txt lines: "<64 hex digits>  <name>" (sha256sum's text mode) or "<64 hex> *<name>" (binary mode); CRLF or
// LF; a UTF-8 BOM is skipped; other lines are ignored. `hex`: name -> lower-case hex (the last line for a name);
// `count`: how many lines named it (VerifyAndSwap refuses a required name whose count is not 1).
struct Sums {
  std::map<std::string, std::string> hex;
  std::map<std::string, int> count;
};
Sums ParseSums(std::string_view text);
// SHA-256 of a file (BCrypt), lower-case hex; "" with *error set when it cannot be read.
std::string Sha256File(const std::filesystem::path& file, std::string* error);

// The folder of the running exe.
std::filesystem::path ExeDir();

// One check at a time, shared by the Online tab's button and the np_ctl.on "update" request (any thread).
// Start: runs CheckAndUpdate on its own thread and returns true; false when one is already running (nothing changes).
// Nothing is logged on the calling thread: `log` (may be empty) gets "[doa4][UPDATE] <text>" lines from the check's
// own thread, one per message.
bool Start(std::function<void(const std::string&)> log);
// A request its caller refused (online play): the message becomes `why`, unless a check is running.
void Refuse(std::string_view why);
bool Busy();
// "" before the first Start or Refuse; "Checking for updates...", "Downloading <tag>...", then the Result's message.
std::string Message();

}  // namespace doa4::updater

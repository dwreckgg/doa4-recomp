// src/doa4_updater.h - Check for Updates in the Online tab (netplay plan 1b, Task 4; spec 6 "In game").
// Ported from DOA2U (doa2u-release netplay-release 6c35c7b, doa2u/src/ui/doa2u_ui.cpp 282-454: HttpGet over WinHTTP,
// CheckForUpdates, DrawUpdateButton), with plan 1b's ruling B1: the release's files are fetched one by one (no zip),
// each is checked against the release's SHA256SUMS.txt before anything is renamed, all three game files are swapped,
// and a failed step renames back everything done so far. A running exe and its loaded DLLs can be renamed on Windows,
// not overwritten (DOA2U relies on the same for its exe); the renamed copies (<name>.old) are deleted when the game
// next starts (CleanUpAtStart, from the app's OnPreSetup).
// Online play and an update exclude each other (review fix round I-1): a check starts only offline; while it runs, and
// after it swapped files in this process (RestartPending), the lobby refuses Host and Join; and just before the swap the
// caller's may_swap predicate is asked again.
// No SDK and no ImGui here. The network and the folder come in through Env, so the whole check is host-tested on temp
// folders with a fake HTTP source (tests/host/updater_host_test.cpp); only GameEnv's WinHTTP source is not.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace doa4::updater {

// The game files a release replaces, in swap order, and the checksum file published beside them.
// FROZEN (review M-4): every updater already deployed reads exactly this list and installs exactly these files from the
// releases it fetches. A release that needs a fourth file cannot be installed by them; adding one needs a new
// mechanism (a manifest the updater reads, or a new channel), never a longer list here.
inline constexpr const char* kFiles[] = {"dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll"};
inline constexpr int kFileCount = 3;
inline constexpr const char* kSumsName = "SHA256SUMS.txt";
// The folder beside the exe the downloads go to (removed after every check, and at start).
inline constexpr const char* kStagingName = "update_new";
// Shown beside the disabled button, and the answer to an np_ctl.on "update" request in online play.
inline constexpr const char* kOnlineText = "Disconnect from online play first.";
// Why Host and Join are refused (the lobby's event line) and disabled (the Online tab).
inline constexpr const char* kBusyText = "An update is downloading.";
inline constexpr const char* kRestartText = "Restart the game to finish the update.";
// Download caps (review M-3): a game file, SHA256SUMS.txt, the release list.
inline constexpr uint64_t kMaxFileBytes = uint64_t(256) << 20;
inline constexpr uint64_t kMaxSumsBytes = uint64_t(1) << 20;
inline constexpr uint64_t kMaxListBytes = uint64_t(16) << 20;

struct Result {
  bool ok = false;  // the files were swapped
  std::string tag;  // the release it looked at ("" when none was picked)
  std::string message;
};

// What a check gets from whoever asked for it.
struct Request {
  std::function<void(const std::string&)> log;  // "[doa4][UPDATE] <text>" lines, from the check's own thread; may be empty
  std::function<bool()> may_swap;               // asked just before the swap: still offline and no session (empty = yes)
  bool confirm = false;                         // the second press that installs a release over a self-built game
};

// Where a check reads from and writes to: the game's own (GameEnv), or the host test's fakes and temp folder.
using Sink = std::function<bool(const char* data, size_t n)>;  // false = stop the transfer
struct Env {
  // GET `url` over HTTPS: the body goes to `sink`; false with *error set when it failed. `api`: the GitHub API request.
  std::function<bool(const std::string& url, bool api, const Sink& sink, std::string* error)> get;
  std::filesystem::path dir;           // the game's folder (absolute)
  std::string repo, channel, current;  // DOA4_RELEASE_REPO, DOA4_RELEASE_CHANNEL, DOA4_RELEASE_TAG
  uint64_t max_file_bytes = kMaxFileBytes;
};
// WinHTTP (system proxy; redirects followed only from HTTPS to HTTPS; the asked-for and the final host must pass
// HostAllowed; only a 200 answer; Content-Length, when sent, must equal the bytes received), ExeDir(), the build's
// DOA4_RELEASE_* values.
Env GameEnv();

// Blocks for the whole download (call it from Start's thread). GET https://api.github.com/repos/<repo>/releases?
// per_page=30; Choose. A self-built game (Choose's replaces_dev) is only told "Release <tag> replaces your self-built
// game. Press again to install." until a request with `confirm` finds that same tag offered by an earlier check; its
// files are then kept as <name>.dev, never .old. Downloads the three files and SHA256SUMS.txt into <dir>/update_new/
// (each game file at most max_file_bytes); deletes stale .old files; asks may_swap (a no leaves everything as it was);
// VerifyAndSwap; removes update_new in every case. After one successful swap, later checks in the same process download
// nothing and say so (RestartPending). After Shutdown, nothing more is downloaded or swapped.
Result CheckAndUpdate(const Request& request, const Env& env);

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
// " (your self-built files are kept as .dev)" after the tag when `replaces_dev`.
std::string UpdatedText(std::string_view tag, bool replaces_dev);
// The first press over a self-built game: "Release <tag> replaces your self-built game. Press again to install."
std::string OfferText(std::string_view tag);

// Pure file steps (host-tested). First every kFiles[i] is checked: SHA256SUMS.txt (`sha256sums`) must name it exactly
// once and <staging>/<f> must exist with that SHA-256; anything else refuses before any rename. Then for each file in
// kFiles order: step 2i renames <dir>/<f> -> <dir>/<f><backup> (nothing to rename when <dir>/<f> does not exist), step
// 2i+1 renames <staging>/<f> -> <dir>/<f>. No rename replaces an existing file: an existing <f><backup> fails step 2i
// with "an old copy of <f> is still in use: close other copies of the game and try again" (backup ".old"; review M-5)
// or a "move it out of the game's folder" text (".dev"). Any failed step renames back, in reverse order, everything done
// so far. `fail_at` (tests only) makes step n fail without trying it. Returns "" when all six steps were done, else why
// it failed (naming the file). `backup` is ".old" or ".dev".
std::string VerifyAndSwap(const std::filesystem::path& dir, const std::filesystem::path& staging,
                          std::string_view sha256sums, int fail_at = -1, std::string_view backup = ".old");

// Deletes <dir>/<f>.old for every kFiles[i] whose <dir>/<f> exists (an .old whose file is missing may be the only copy
// left by an interrupted swap: kept). Never touches <f>.dev or anything else; a file that cannot be deleted (still in
// use) is left for the next start.
void DeleteOldFiles(const std::filesystem::path& dir);
// At start (OnPreSetup, after the previous instance has exited): DeleteOldFiles and removes a leftover <dir>/update_new
// (review M-2). Nothing happens for an empty or relative `dir`.
void CleanUpAtStart(const std::filesystem::path& dir);

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
// Review M-3: the hosts a request may go to or end at (ASCII case ignored): github.com, api.github.com, or a download
// host <name>.githubusercontent.com. Nothing else (no other subdomain, no trailing dot, no lookalike).
bool HostAllowed(std::string_view host);

// The folder of the running exe.
std::filesystem::path ExeDir();

// One check at a time, shared by the Online tab's button and the np_ctl.on "update" request (any thread).
using CheckFn = std::function<Result(const Request&)>;
// Runs `check` (empty: CheckAndUpdate(request, GameEnv())) on its own thread and returns true; false when one is
// already running, or after Shutdown (nothing changes). Nothing is logged on the calling thread. An exception out of
// `check` ends the check with the message "The update check stopped unexpectedly".
bool Start(Request request, CheckFn check = {});
// A request its caller refused (online play): the message becomes `why`, unless a check is running.
void Refuse(std::string_view why);
bool Busy();
bool RestartPending();  // this process swapped a release in: it must restart before playing online
bool OfferPending();    // a self-built game was offered a release: the next press confirms it
// "" before the first Start or Refuse; "Checking for updates...", "Downloading <tag>...", then the Result's message.
std::string Message();
// Review M-1, first thing at shutdown: no further download or swap starts; waits at most `max_wait_ms` for a swap in
// progress to finish. A running download stops at its next chunk.
void Shutdown(uint32_t max_wait_ms);

// Tests only: forgets the swap, the offer, the cancel flag and the message (Busy must be false).
void ResetForTest();

}  // namespace doa4::updater

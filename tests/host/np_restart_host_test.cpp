// Host test for src/net/np_restart (Task 12): the session folders' names, the session-folder cleaner (reparse points
// are never followed), the save writer (NameOk names only, below the folder only) and the relaunch plan (the child's
// environment and show state). Built and run by run_np_restart_test.bat. Files go under %TEMP%\doa4_np_restart_<pid>\
// and only that folder is removed at the end. A junction is made with "cmd /c mklink /J" (no console window).

#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../src/net/np_restart.h"

namespace fs = std::filesystem;
using namespace doa4::np;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      std::fflush(stdout);                                             \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

static fs::path g_root;

static void WriteFile(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream(p, std::ios::binary) << text;
}

static bool Exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

static std::string ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A directory junction at `link` pointing to `target` (cmd's mklink /J; no window). True if the link exists afterwards
// as a reparse point.
static bool MakeJunction(const fs::path& link, const fs::path& target) {
  std::wstring cmd = L"cmd.exe /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return false;
  }
  WaitForSingleObject(pi.hProcess, 10000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  const DWORD a = GetFileAttributesW(link.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

static size_t CountEntries(const fs::path& dir) {
  std::error_code ec;
  size_t n = 0;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) ++n;
  return n;
}

static void TestPaths() {
  const fs::path u = L"C:/ud";
  CHECK(restart::HandoffPath(u) == fs::path(L"C:/ud") / "netplay" / "handoff.d4h");
  CHECK(restart::SessionUserData(u) == fs::path(L"C:/ud") / "netplay" / "session");
  // a fixed LOCAL time (mktime reads tm as local time): 2026-10-31 23:59:58, both roles
  std::tm t{};
  t.tm_year = 126;
  t.tm_mon = 9;
  t.tm_mday = 31;
  t.tm_hour = 23;
  t.tm_min = 59;
  t.tm_sec = 58;
  t.tm_isdst = -1;
  const std::time_t now = std::mktime(&t);
  CHECK(restart::SessionDir(u, {}, true, now) == fs::path(L"C:/ud") / "netplay" / "sessions" / "20261031-235958_host");
  CHECK(restart::SessionDir(u, {}, false, now) ==
        fs::path(L"C:/ud") / "netplay" / "sessions" / "20261031-235958_joiner");
  // single-digit fields are zero-padded (a month 1 and a day 2: not "202612" or "2026-1-2")
  std::tm t2{};
  t2.tm_year = 127;
  t2.tm_mon = 0;
  t2.tm_mday = 2;
  t2.tm_hour = 3;
  t2.tm_min = 4;
  t2.tm_sec = 5;
  t2.tm_isdst = -1;
  CHECK(restart::SessionDir(u, {}, true, std::mktime(&t2)) ==
        fs::path(L"C:/ud") / "netplay" / "sessions" / "20270102-030405_host");
  // with a test dir: <T>/session, whatever the role and time
  CHECK(restart::SessionDir(u, L"C:/run/ctl", true, now) == fs::path(L"C:/run/ctl") / "session");
  CHECK(restart::SessionDir(u, L"C:/run/ctl", false, 0) == fs::path(L"C:/run/ctl") / "session");
}

static void TestClearFolder() {
  std::string error;
  // (1) a missing folder is created, empty
  const fs::path fresh = g_root / "fresh" / "netplay" / "session";
  CHECK(restart::ClearFolder(fresh, &error));
  CHECK(fs::is_directory(fresh) && CountEntries(fresh) == 0);

  // (2) a populated folder: files, a read-only file, a nested folder, and a junction to a folder OUTSIDE it that
  // holds a file. After the clear: the folder exists and is empty; the junction's target and its file survive.
  const fs::path sess = g_root / "u1" / "netplay" / "session";
  const fs::path outside = g_root / "outside";
  WriteFile(outside / "keep.txt", "keep");
  WriteFile(outside / "deep" / "keep2.txt", "keep2");
  WriteFile(sess / "B13EBABEBABEBABE" / "544307D1" / "00000001" / "ups.dat", "old save");
  WriteFile(sess / "loose.txt", "x");
  WriteFile(sess / "ro.txt", "ro");
  SetFileAttributesW((sess / "ro.txt").c_str(), FILE_ATTRIBUTE_READONLY);
  const bool junction = MakeJunction(sess / "link", outside);
  CHECK(junction);  // the fixture's precondition: a real reparse point inside the session folder
  CHECK(Exists(sess / "link" / "keep.txt"));  // and it resolves to the outside folder
  CHECK(restart::ClearFolder(sess, &error));
  CHECK(error.empty());
  CHECK(fs::is_directory(sess));
  CHECK(CountEntries(sess) == 0);
  CHECK(ReadAll(outside / "keep.txt") == "keep");          // NOT deleted through the junction
  CHECK(ReadAll(outside / "deep" / "keep2.txt") == "keep2");
  CHECK(CountEntries(outside) == 2);

  // (3) the session folder itself is a junction: refused; nothing below its target is touched
  const fs::path u2 = g_root / "u2" / "netplay";
  std::error_code ec;
  fs::create_directories(u2, ec);
  const fs::path target = g_root / "target2";
  WriteFile(target / "precious.txt", "precious");
  CHECK(MakeJunction(u2 / "session", target));
  error.clear();
  CHECK(!restart::ClearFolder(u2 / "session", &error));
  CHECK(error.find("link") != std::string::npos);
  CHECK(ReadAll(target / "precious.txt") == "precious");
  CHECK(CountEntries(target) == 1);

  // (4) a file where the folder should be: refused, the file stays
  WriteFile(g_root / "u3" / "netplay" / "session", "a file");
  error.clear();
  CHECK(!restart::ClearFolder(g_root / "u3" / "netplay" / "session", &error));
  CHECK(ReadAll(g_root / "u3" / "netplay" / "session") == "a file");
}

static void TestWriteSaveFiles() {
  const fs::path dir = g_root / "w" / "netplay" / "session";
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::vector<handoff::File> files;
  files.push_back({"B13EBABEBABEBABE/544307D1/00000001/ups.dat", {1, 2, 3}});
  files.push_back({"B13EBABEBABEBABE/544307D1/00000001/sys.dat", {}});  // an empty file is still a file
  files.push_back({"B13EBABEBABEBABE/544307D1/../../../escape.txt", {9}});  // NameOk refuses ".."
  files.push_back({"other/544307D1/x.dat", {7}});                            // outside the save prefix
  files.push_back({"B13EBABEBABEBABE/544307D1/00000001/CON", {7}});          // a device name
  std::string error;
  CHECK(!restart::WriteSaveFiles(dir, files, &error));  // three refused
  CHECK(error.find("escape.txt") != std::string::npos);
  CHECK(error.find("other/544307D1/x.dat") != std::string::npos);
  CHECK(ReadAll(dir / "B13EBABEBABEBABE" / "544307D1" / "00000001" / "ups.dat") == std::string("\x01\x02\x03", 3));
  CHECK(Exists(dir / "B13EBABEBABEBABE" / "544307D1" / "00000001" / "sys.dat"));
  CHECK(!Exists(g_root / "w" / "escape.txt") && !Exists(dir.parent_path() / "escape.txt") &&
        !Exists(g_root / "escape.txt"));
  CHECK(!Exists(dir / "other"));
  CHECK(!Exists(dir / "B13EBABEBABEBABE" / "544307D1" / "00000001" / "CON"));
  // all good: true, error empty
  std::vector<handoff::File> good(files.begin(), files.begin() + 2);
  CHECK(restart::WriteSaveFiles(dir, good, &error) && error.empty());
}

static const std::wstring* Find(const restart::RelaunchPlan& p, const wchar_t* name) {
  for (const restart::EnvVar& v : p.env) {
    if (v.name == name) return &v.value;
  }
  return nullptr;
}

static void TestRelaunchPlan() {
  const std::wstring cmd = L"\"C:\\g\\dead_or_alive_4.exe\" --user_data_root=C:\\r\\ud --headless=true";
  const fs::path h = L"C:/r/ud/netplay/handoff.d4h";
  // into a session, started hidden by the harness (STARTF_USESHOWWINDOW + SW_SHOWMINNOACTIVE = 7)
  restart::RelaunchPlan p = restart::PlanRelaunch(cmd, 4242, &h, false, true, 7);
  CHECK(p.command_line == cmd);  // the same command line (the session finds its user data and test dir from it)
  CHECK(Find(p, L"DOA4_WAIT_PID") && *Find(p, L"DOA4_WAIT_PID") == L"4242");
  CHECK(Find(p, L"DOA4_NP_HANDOFF") && *Find(p, L"DOA4_NP_HANDOFF") == h.wstring());
  CHECK(Find(p, L"DOA4_NP_AFTER_SESSION") && Find(p, L"DOA4_NP_AFTER_SESSION")->empty());  // removed in the child
  CHECK(p.use_show && p.show == 7);
  // out of a session (the offline relaunch): no hand-off, DOA4_NP_AFTER_SESSION=1, a normal start stays normal
  p = restart::PlanRelaunch(cmd, 17, nullptr, true, false, 1);
  CHECK(Find(p, L"DOA4_NP_HANDOFF") && Find(p, L"DOA4_NP_HANDOFF")->empty());  // removed: never a second session
  CHECK(Find(p, L"DOA4_NP_AFTER_SESSION") && *Find(p, L"DOA4_NP_AFTER_SESSION") == L"1");
  CHECK(Find(p, L"DOA4_WAIT_PID") && *Find(p, L"DOA4_WAIT_PID") == L"17");
  CHECK(!p.use_show);  // the parent had no STARTF_USESHOWWINDOW: its wShowWindow (1) is not passed on
  CHECK(p.show == 0);
  // a hidden SW_HIDE (0) start is passed on as such (0 is a value, not "none")
  p = restart::PlanRelaunch(cmd, 17, nullptr, true, true, 0);
  CHECK(p.use_show && p.show == 0);
}

// Fix round I2 / M1: the redirect proof and the names a session may skip.
static void TestRedirectAndNames() {
  const fs::path sess = g_root / "u9" / "netplay" / "session";
  CHECK(restart::RedirectHolds(sess, sess));
  CHECK(restart::RedirectHolds(sess.wstring() + L"\\", sess));                       // a trailing separator
  CHECK(restart::RedirectHolds(fs::path(sess.generic_wstring()), sess));             // '/' against '\'
  std::wstring upper = sess.wstring();
  for (wchar_t& c : upper) c = wchar_t(towupper(c));
  CHECK(restart::RedirectHolds(upper, sess));                                        // one folder on Windows
  CHECK(restart::RedirectHolds(g_root / "u9" / "netplay" / "x" / ".." / "session", sess));
  // the likeliest wrong answers: the real user data (the redirect did not happen), a parent, a sibling, empty
  CHECK(!restart::RedirectHolds(g_root / "u9", sess));
  CHECK(!restart::RedirectHolds(g_root / "u9" / "netplay", sess));
  CHECK(!restart::RedirectHolds(g_root / "u9" / "netplay" / "sessions", sess));
  CHECK(!restart::RedirectHolds(g_root / "u9" / "netplay" / "session2", sess));
  CHECK(!restart::RedirectHolds({}, sess) && !restart::RedirectHolds(sess, {}));
  // exactly the three known unregistered names
  CHECK(restart::KnownUnregisteredSetting("kernel_pix"));
  CHECK(restart::KnownUnregisteredSetting("vulkan_readback_resolve"));
  CHECK(restart::KnownUnregisteredSetting("vulkan_readback_memexport"));
  CHECK(!restart::KnownUnregisteredSetting("kernel_debug_monitor"));
  CHECK(!restart::KnownUnregisteredSetting("d3d12_readback_resolve"));
  CHECK(!restart::KnownUnregisteredSetting("kernel_pix2") && !restart::KnownUnregisteredSetting("kernel_pi"));
  CHECK(!restart::KnownUnregisteredSetting(""));
}

// Task 15 carry f: the lobby-only test switch, only with a test dir and only for exactly "1".
static void TestNoRestartSwitch() {
  CHECK(restart::TestNoRestart(true, "1"));
  CHECK(!restart::TestNoRestart(false, "1"));  // no test dir: a player's environment cannot turn it on
  CHECK(!restart::TestNoRestart(true, nullptr));
  CHECK(!restart::TestNoRestart(true, ""));
  CHECK(!restart::TestNoRestart(true, "0"));
  CHECK(!restart::TestNoRestart(true, "11") && !restart::TestNoRestart(true, "1 ") && !restart::TestNoRestart(true, "yes"));
  CHECK(!restart::TestNoRestart(false, nullptr));
}

int main() {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  g_root = fs::path(tmp) / ("doa4_np_restart_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  fs::create_directories(g_root, ec);
  TestPaths();
  TestClearFolder();
  TestWriteSaveFiles();
  TestRelaunchPlan();
  TestRedirectAndNames();
  TestNoRestartSwitch();
  // clean up only what this run made: remove junctions as links first (never through them)
  std::string error;
  restart::ClearFolder(g_root, &error);
  RemoveDirectoryW(g_root.c_str());
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

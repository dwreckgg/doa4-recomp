// Check for Updates. See doa4_updater.h.

#include "doa4_updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "net/np_rules.h"

#ifdef DOA4_UPDATER_HOST_TEST
// tests/host/run_updater_test.bat: the release identity CMake gives the game target, as a stand-in (the test never
// calls GameEnv, so it never uses them)
#define DOA4_RELEASE_TAG "dev"
#define DOA4_RELEASE_REPO "none/none"
#define DOA4_RELEASE_CHANNEL "netplay"
#endif
#if !defined(DOA4_RELEASE_TAG) || !defined(DOA4_RELEASE_REPO) || !defined(DOA4_RELEASE_CHANNEL)
#error "doa4_updater.cpp needs DOA4_RELEASE_TAG, DOA4_RELEASE_REPO and DOA4_RELEASE_CHANNEL (CMakeLists.txt)"
#endif

namespace fs = std::filesystem;

namespace doa4::updater {

namespace {

std::mutex g_mu;  // the four strings and g_busy
bool g_busy = false;
std::string g_message;
std::string g_installed;  // the tag swapped in by this process ("" = none yet): RestartPending
std::string g_offered;    // the tag offered to a self-built game ("" = none): OfferPending
std::atomic<bool> g_cancel{false};  // Shutdown: nothing more is downloaded or swapped
// Held from the may_swap question to the end of the swap, so that Shutdown can wait for a swap in progress.
std::timed_mutex g_swap_mu;

void SetMessage(const Request& request, const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_message = text;
  }
  if (request.log) request.log("[doa4][UPDATE] " + text);
}

std::string ErrorText(DWORD e) { return "error " + std::to_string(e); }

std::string SizeText(uint64_t bytes) {
  return bytes % (uint64_t(1) << 20) == 0 ? std::to_string(bytes >> 20) + " MB" : std::to_string(bytes) + " bytes";
}

bool IsMissing(const fs::path& p) {
  if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
  const DWORD e = GetLastError();
  return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

std::wstring Widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string AsciiOf(std::wstring_view w) {  // host names: anything outside ASCII becomes '?' (HostAllowed refuses it)
  std::string s;
  for (wchar_t ch : w) s += (ch > 0 && ch < 128) ? char(ch) : '?';
  return s;
}

// An https URL's host, or "" when `url` is not https.
std::string HttpsHost(const std::wstring& url, std::wstring* path) {
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  uc.dwSchemeLength = DWORD(-1);
  uc.dwHostNameLength = DWORD(-1);
  uc.dwUrlPathLength = DWORD(-1);
  uc.dwExtraInfoLength = DWORD(-1);
  if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return {};
  if (path) {
    path->assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) path->append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  }
  return AsciiOf(std::wstring_view(uc.lpszHostName, uc.dwHostNameLength));
}

// GameEnv's GET: WinHTTP over the system proxy (DOA2U's HttpGet) with review M-3's rules: https only, redirects only
// from HTTPS to HTTPS, the asked-for host and the final host must pass HostAllowed, only a 200 answer, and the bytes
// received must equal Content-Length when the server sends one.
bool WinHttpGet(const std::string& url_text, bool api, const Sink& sink, std::string* error) {
  const std::wstring url = Widen(url_text);
  std::wstring path;
  const std::string host = HttpsHost(url, &path);
  if (host.empty()) {
    *error = "not an https address";
    return false;
  }
  if (!HostAllowed(host)) {
    *error = "not a GitHub address (" + host + ")";
    return false;
  }
  bool ok = false;
  HINTERNET s = WinHttpOpen(L"DOA4-netplay-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (s) {
    WinHttpSetTimeouts(s, 30000, 30000, 30000, 60000);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;  // the default, set explicitly (M-3)
    WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
  }
  HINTERNET c = s ? WinHttpConnect(s, Widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE)
                  : nullptr;
  const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
  if (!r) {
    *error = "cannot open a connection (" + ErrorText(GetLastError()) + ")";
  } else if (!WinHttpSendRequest(r, headers, api ? DWORD(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
             !WinHttpReceiveResponse(r, nullptr)) {
    *error = "no answer (" + ErrorText(GetLastError()) + ")";
  } else {
    // where the redirects ended (WINHTTP_OPTION_URL: the final URL)
    DWORD len = 0;
    WinHttpQueryOption(r, WINHTTP_OPTION_URL, nullptr, &len);
    std::wstring final_url(len / sizeof(wchar_t) + 1, L'\0');
    std::string final_host;
    if (len && WinHttpQueryOption(r, WINHTTP_OPTION_URL, final_url.data(), &len)) {
      final_url.resize(len / sizeof(wchar_t));
      final_host = HttpsHost(final_url, nullptr);
    }
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &sz, WINHTTP_NO_HEADER_INDEX);
    wchar_t cl[32] = {};
    DWORD cl_size = sizeof cl;
    const bool have_length = WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl,
                                                 &cl_size, WINHTTP_NO_HEADER_INDEX) != 0;
    const uint64_t want = have_length ? _wcstoui64(cl, nullptr, 10) : 0;
    if (final_host.empty() || !HostAllowed(final_host)) {
      *error = "redirected to a host that is not GitHub's (" + (final_host.empty() ? std::string("?") : final_host) + ")";
    } else if (status != 200) {
      *error = "HTTP " + std::to_string(status);
    } else {
      std::vector<char> buf(1 << 16);
      uint64_t got = 0;
      ok = true;
      for (;;) {
        DWORD n = 0;
        if (!WinHttpReadData(r, buf.data(), DWORD(buf.size()), &n)) {
          *error = "the transfer broke off (" + ErrorText(GetLastError()) + ")";
          ok = false;
          break;
        }
        if (n == 0) break;
        got += n;
        if (!sink(buf.data(), n)) {
          *error = "stopped";
          ok = false;
          break;
        }
      }
      if (ok && have_length && got != want) {
        *error = "the transfer ended early (" + std::to_string(got) + " of " + std::to_string(want) + " bytes)";
        ok = false;
      }
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  if (s) WinHttpCloseHandle(s);
  return ok;
}

// Downloads `url` into the new file `path` (it must not exist), at most `cap` bytes; a copy of the bytes goes to `copy`
// when not null. The file is flushed before it is closed.
bool Download(const Env& env, const std::string& url, const fs::path& path, uint64_t cap, std::string* copy,
              std::string* error) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    *error = "cannot create it (" + ErrorText(GetLastError()) + ")";
    return false;
  }
  uint64_t got = 0;
  bool over = false, write_failed = false, cancelled = false;
  std::string get_error;
  const bool ok = env.get && env.get(url, false, [&](const char* data, size_t n) {
    if (g_cancel.load()) {
      cancelled = true;
      return false;
    }
    if (got + n > cap) {
      over = true;
      return false;
    }
    DWORD wrote = 0;
    if (!WriteFile(f, data, DWORD(n), &wrote, nullptr) || wrote != n) {
      write_failed = true;
      return false;
    }
    got += n;
    if (copy) copy->append(data, n);
    return true;
  }, &get_error);
  const bool flushed = FlushFileBuffers(f) != 0;
  const bool closed = CloseHandle(f) != 0;
  if (cancelled) {
    *error = "the game is closing";
  } else if (over) {
    *error = "larger than " + SizeText(cap);
  } else if (write_failed || (ok && (!flushed || !closed))) {
    *error = "cannot write it";
  } else if (!ok) {
    *error = get_error.empty() ? std::string("failed") : get_error;
  } else {
    return true;
  }
  return false;
}

}  // namespace

// ---- the decision ----

Choice Choose(std::string_view json, std::string_view channel, std::string_view current) {
  Choice c;
  std::vector<std::string_view> required;
  for (const char* f : kFiles) required.push_back(f);
  required.push_back(kSumsName);
  np::rules::ReleasePick pick;
  if (!np::rules::PickRelease(json, channel, required, &pick)) {
    c.message = "No " + std::string(channel) + " release found yet";
    return c;
  }
  c.tag = pick.tag;
  c.replaces_dev = np::rules::IsDevBuild(current);
  if (!c.replaces_dev && np::rules::CompareVersions(pick.tag, current) <= 0) {
    c.message = "Already on the latest " + std::string(channel) + " release (" + std::string(current) + ")";
    return c;
  }
  c.update = true;
  c.urls = std::move(pick.asset_urls);
  c.message = "Downloading " + c.tag + "...";
  return c;
}

std::string UpdatedText(std::string_view tag, bool replaces_dev) {
  return "Updated to " + std::string(tag) + (replaces_dev ? " (your self-built files are kept as .dev)" : "") +
         ". Restart the game to use it.";
}

std::string OfferText(std::string_view tag) {
  return "Release " + std::string(tag) + " replaces your self-built game. Press again to install.";
}

// ---- the file steps ----

Sums ParseSums(std::string_view text) {
  Sums s;
  if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    // "<64 hex> <' ' or '*'><name>"
    if (line.size() < 67 || line[64] != ' ' || (line[65] != ' ' && line[65] != '*')) continue;
    std::string hex;
    bool ok = true;
    for (size_t i = 0; i < 64 && ok; ++i) {
      const char ch = line[i];
      if (ch >= '0' && ch <= '9') hex += ch;
      else if (ch >= 'a' && ch <= 'f') hex += ch;
      else if (ch >= 'A' && ch <= 'F') hex += char(ch - 'A' + 'a');
      else ok = false;
    }
    if (!ok) continue;
    const std::string name(line.substr(66));
    s.hex[name] = hex;
    ++s.count[name];
  }
  return s;
}

std::string Sha256File(const fs::path& file, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return std::string();
  };
  HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return fail("cannot open it (" + ErrorText(GetLastError()) + ")");
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
            BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
  std::string why = ok ? "" : "no SHA-256 provider";
  std::vector<uint8_t> buf(1 << 16);
  while (ok) {
    DWORD got = 0;
    if (!ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr)) {
      why = "cannot read it (" + ErrorText(GetLastError()) + ")";
      ok = false;
      break;
    }
    if (got == 0) break;
    ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), got, 0));
    if (!ok) why = "hashing failed";
  }
  uint8_t digest[32] = {};
  if (ok && !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof digest, 0))) {
    ok = false;
    why = "hashing failed";
  }
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  CloseHandle(f);
  if (!ok) return fail(why);
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  for (uint8_t b : digest) {
    out += kHex[b >> 4];
    out += kHex[b & 15];
  }
  return out;
}

bool HostAllowed(std::string_view host) {
  std::string h;
  for (char ch : host) h += (ch >= 'A' && ch <= 'Z') ? char(ch - 'A' + 'a') : ch;
  if (h == "github.com" || h == "api.github.com") return true;
  const std::string_view suffix = ".githubusercontent.com";
  if (h.size() <= suffix.size() || h.compare(h.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
  for (size_t i = 0; i < h.size() - suffix.size(); ++i) {  // one label: letters, digits, '-'
    const char ch = h[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
  }
  return true;
}

std::string VerifyAndSwap(const fs::path& dir, const fs::path& staging, std::string_view sha256sums, int fail_at,
                          std::string_view backup_suffix) {
  // 1. every file checked before anything is renamed
  const Sums sums = ParseSums(sha256sums);
  for (const char* name : kFiles) {
    const std::string f = name;
    const auto n = sums.count.find(f);
    if (n == sums.count.end()) return f + " has no line in SHA256SUMS.txt: nothing was changed";
    if (n->second != 1) return f + " has " + std::to_string(n->second) + " lines in SHA256SUMS.txt: nothing was changed";
    std::string err;
    const std::string got = Sha256File(staging / f, &err);
    if (got.empty()) return "the downloaded " + f + " cannot be read (" + err + "): nothing was changed";
    const auto want = sums.hex.find(f);
    if (want == sums.hex.end() || got != want->second)
      return "the downloaded " + f + " does not match SHA256SUMS.txt (a broken download?): nothing was changed";
  }
  // 2. the six renames; each one done is remembered so that a failure can put it back
  struct Move {
    fs::path from, to;
  };
  const std::string suffix(backup_suffix);
  std::vector<Move> done;
  std::string why;
  for (int step = 0; step < 2 * kFileCount; ++step) {
    const std::string f = kFiles[step / 2];
    const bool backup = step % 2 == 0;
    const fs::path from = backup ? dir / f : staging / f;
    const fs::path to = backup ? dir / (f + suffix) : dir / f;
    const std::string what = backup ? "renaming " + f + " to " + f + suffix : "putting the new " + f + " in place";
    if (step == fail_at) {
      why = what + " failed (test switch, step " + std::to_string(step) + ")";
      break;
    }
    if (backup && IsMissing(from)) continue;  // nothing installed under this name: the new one is simply placed
    if (backup && !IsMissing(to)) {           // review M-5: say what is in the way (CheckAndUpdate deleted unused .old)
      why = suffix == ".dev" ? f + ".dev (a self-built game kept by an earlier update) is in the way: move it out of the "
                                   "game's folder and try again"
                             : "an old copy of " + f + " is still in use: close other copies of the game and try again";
      break;
    }
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {  // never replaces
      why = what + " failed (" + ErrorText(GetLastError()) + ")";
      break;
    }
    done.push_back({from, to});
  }
  if (why.empty()) return "";
  // 3. rollback, newest first
  std::string stuck;
  for (auto it = done.rbegin(); it != done.rend(); ++it) {
    if (!MoveFileExW(it->to.c_str(), it->from.c_str(), 0))
      stuck += (stuck.empty() ? "" : ", ") + it->to.filename().string() + " (" + ErrorText(GetLastError()) + ")";
  }
  if (!stuck.empty())
    return why + "; putting back " + stuck + " failed too: reinstall the game from the GitHub Releases page";
  return why + ": everything was put back";
}

void DeleteOldFiles(const fs::path& dir) {
  for (const char* name : kFiles) {
    const fs::path file = dir / name, old = dir / (std::string(name) + ".old");
    if (IsMissing(file)) continue;  // the .old may be the only copy left by an interrupted swap
    DeleteFileW(old.c_str());       // in use (an instance still running from it) or absent: left as it is
  }
}

void CleanUpAtStart(const fs::path& dir) {
  if (dir.empty() || !dir.is_absolute()) return;
  DeleteOldFiles(dir);
  std::error_code ec;
  fs::remove_all(dir / kStagingName, ec);  // an interrupted check's downloads (review M-2)
}

fs::path ExeDir() {
  std::wstring buf(512, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      return fs::path(buf).parent_path();
    }
    if (buf.size() >= 32768) return {};
    buf.resize(buf.size() * 2);
  }
}

// ---- the check ----

Env GameEnv() {
  Env e;
  e.get = WinHttpGet;
  e.dir = ExeDir();
  e.repo = DOA4_RELEASE_REPO;
  e.channel = DOA4_RELEASE_CHANNEL;
  e.current = DOA4_RELEASE_TAG;
  return e;
}

Result CheckAndUpdate(const Request& request, const Env& env) {
  Result r;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_installed.empty()) {
      r.tag = g_installed;
      r.message = UpdatedText(g_installed, false);  // already swapped in by this process: nothing to download
      return r;
    }
  }
  if (g_cancel.load()) {
    r.message = "The update check was stopped: the game is closing";
    return r;
  }
  std::string json, err;
  const bool listed = env.get && env.get("https://api.github.com/repos/" + env.repo + "/releases?per_page=30", true,
                                         [&](const char* data, size_t n) {
                                           if (g_cancel.load() || json.size() + n > kMaxListBytes) return false;
                                           json.append(data, n);
                                           return true;
                                         },
                                         &err);
  if (!listed) {
    r.message = "Could not reach GitHub (" + (err.empty() ? std::string("no answer") : err) + ")";
    return r;
  }
  const Choice c = Choose(json, env.channel, env.current);
  r.tag = c.tag;
  if (!c.update) {
    r.message = c.message;
    return r;
  }
  if (c.replaces_dev) {  // review I-2: a self-built game is replaced only on a second, confirming press
    bool confirmed = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      confirmed = request.confirm && g_offered == c.tag;
      if (!confirmed) g_offered = c.tag;
    }
    if (!confirmed) {
      r.message = OfferText(c.tag);
      return r;
    }
  }
  SetMessage(request, c.message);
  const fs::path& dir = env.dir;
  if (dir.empty() || !dir.is_absolute()) {
    r.message = "Update to " + c.tag + " failed: the game's own folder could not be read";
    return r;
  }
  const fs::path staging = dir / kStagingName;
  std::error_code ec;
  fs::remove_all(staging, ec);  // a leftover of an interrupted check
  ec.clear();
  fs::create_directories(staging, ec);
  if (ec) {
    r.message = "Update to " + c.tag + " failed: cannot create " + std::string(kStagingName) + " beside the game (" +
                ec.message() + ")";
    return r;
  }
  auto fail = [&](const std::string& text) {
    fs::remove_all(staging, ec);
    r.message = "Update to " + c.tag + " failed: " + text;
    return r;
  };
  std::string sums;
  std::vector<std::string> names(std::begin(kFiles), std::end(kFiles));
  names.push_back(kSumsName);
  for (const std::string& name : names) {
    const auto url = c.urls.find(name);
    std::string why = url == c.urls.end() ? "no download address" : "";
    const bool is_sums = name == kSumsName;
    if (why.empty())
      Download(env, url->second, staging / name, is_sums ? kMaxSumsBytes : env.max_file_bytes, is_sums ? &sums : nullptr,
               &why);
    if (!why.empty()) return fail("could not download " + name + " (" + why + "); nothing was changed");
  }
  DeleteOldFiles(dir);  // leftovers of an earlier update that are no longer in use
  std::string why;
  {
    std::unique_lock<std::timed_mutex> swap(g_swap_mu);  // Shutdown waits for this block (review M-1)
    if (g_cancel.load()) return fail("the game is closing; nothing was changed");
    if (request.may_swap && !request.may_swap())  // review I-1: still offline, no session?
      return fail("online play started during the download; nothing was changed");
    if (g_cancel.load()) return fail("the game is closing; nothing was changed");
    why = VerifyAndSwap(dir, staging, sums, -1, c.replaces_dev ? ".dev" : ".old");
    if (why.empty()) {
      std::lock_guard<std::mutex> lock(g_mu);
      g_installed = c.tag;
      g_offered.clear();
    }
  }
  fs::remove_all(staging, ec);
  if (!why.empty()) {
    r.message = "Update to " + c.tag + " failed: " + why;
    return r;
  }
  r.ok = true;
  r.message = UpdatedText(c.tag, c.replaces_dev);
  return r;
}

// ---- one check at a time ----

bool Start(Request request, CheckFn check) {
  if (g_cancel.load()) return false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_busy) return false;
    g_busy = true;
    g_message = "Checking for updates...";
  }
  try {
    std::thread([request = std::move(request), check = std::move(check)] {
      Result r;
      try {  // review M-1: nothing escapes this thread; Busy always clears
        if (request.log) request.log("[doa4][UPDATE] Checking for updates...");
        r = check ? check(request) : CheckAndUpdate(request, GameEnv());
      } catch (...) {
        r = Result{};
        r.message = "The update check stopped unexpectedly";
      }
      {
        std::lock_guard<std::mutex> lock(g_mu);
        g_message = r.message;
      }
      try {
        if (request.log) request.log("[doa4][UPDATE] " + r.message);
      } catch (...) {
      }
      std::lock_guard<std::mutex> lock(g_mu);  // last: once Busy is false, the check has logged everything
      g_busy = false;
    }).detach();
  } catch (...) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_busy = false;
    g_message = "The update check could not start";
    return false;
  }
  return true;
}

void Refuse(std::string_view why) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_busy) g_message = std::string(why);
}

bool Busy() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_busy;
}

bool RestartPending() {
  std::lock_guard<std::mutex> lock(g_mu);
  return !g_installed.empty();
}

bool OfferPending() {
  std::lock_guard<std::mutex> lock(g_mu);
  return !g_offered.empty();
}

std::string Message() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_message;
}

void Shutdown(uint32_t max_wait_ms) {
  g_cancel.store(true);
  if (g_swap_mu.try_lock_for(std::chrono::milliseconds(max_wait_ms))) g_swap_mu.unlock();
}

void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_installed.clear();
  g_offered.clear();
  g_message.clear();
  g_cancel.store(false);
}

}  // namespace doa4::updater

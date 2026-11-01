// Check for Updates. See doa4_updater.h.

#include "doa4_updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

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
// reaches the network, so it never uses them)
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

std::mutex g_mu;  // everything below
bool g_busy = false;
std::string g_message;
std::string g_installed;  // the tag swapped in by this process ("" = none yet)
std::function<void(const std::string&)> g_log;

void SetMessage(const std::string& text) {
  std::function<void(const std::string&)> log;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_message = text;
    log = g_log;
  }
  if (log) log("[doa4][UPDATE] " + text);
}

std::string ErrorText(DWORD e) { return "error " + std::to_string(e); }

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

// GET over HTTPS (WinHTTP, the system proxy; redirects followed, never from HTTPS to HTTP). Only a 200 answer counts;
// its body goes to `sink` (false = stop). DOA2U's HttpGet, with the read failure and the status reported.
bool HttpGet(const std::wstring& url, bool github_api, const std::function<bool(const char*, size_t)>& sink,
             std::string* error) {
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  uc.dwSchemeLength = DWORD(-1);
  uc.dwHostNameLength = DWORD(-1);
  uc.dwUrlPathLength = DWORD(-1);
  uc.dwExtraInfoLength = DWORD(-1);
  if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
    *error = "not an https address";
    return false;
  }
  const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

  bool ok = false;
  HINTERNET s = WinHttpOpen(L"DOA4-netplay-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (s) WinHttpSetTimeouts(s, 30000, 30000, 30000, 60000);
  HINTERNET c = s ? WinHttpConnect(s, host.c_str(), uc.nPort, 0) : nullptr;
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE)
                  : nullptr;
  const wchar_t* headers = github_api ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
  if (!r) {
    *error = "cannot open a connection (" + ErrorText(GetLastError()) + ")";
  } else if (!WinHttpSendRequest(r, headers, github_api ? DWORD(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
             !WinHttpReceiveResponse(r, nullptr)) {
    *error = "no answer (" + ErrorText(GetLastError()) + ")";
  } else {
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
      *error = "HTTP " + std::to_string(status);
    } else {
      std::vector<char> buf(1 << 16);
      ok = true;
      for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(r, buf.data(), DWORD(buf.size()), &got)) {
          *error = "the transfer broke off (" + ErrorText(GetLastError()) + ")";
          ok = false;
          break;
        }
        if (got == 0) break;
        if (!sink(buf.data(), got)) {
          *error = "cannot store it";
          ok = false;
          break;
        }
      }
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  if (s) WinHttpCloseHandle(s);
  return ok;
}

// Downloads `url` into the new file `path` (it must not exist); a copy of the bytes goes to `copy` when not null.
bool Download(const std::string& url, const fs::path& path, std::string* copy, std::string* error) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    *error = "cannot create it (" + ErrorText(GetLastError()) + ")";
    return false;
  }
  const bool ok = HttpGet(Widen(url), false, [&](const char* data, size_t n) {
    DWORD wrote = 0;
    if (!WriteFile(f, data, DWORD(n), &wrote, nullptr) || wrote != n) return false;
    if (copy && copy->size() < (size_t(1) << 20)) copy->append(data, n);
    return true;
  }, error);
  const bool closed = CloseHandle(f) != 0;
  if (ok && !closed) *error = "cannot close it";
  return ok && closed;
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
  return "Updated to " + std::string(tag) + (replaces_dev ? " (it replaces the self-built game)" : "") +
         ". Restart the game to use it.";
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

std::string VerifyAndSwap(const fs::path& dir, const fs::path& staging, std::string_view sha256sums, int fail_at) {
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
  std::vector<Move> done;
  std::string why;
  for (int step = 0; step < 2 * kFileCount; ++step) {
    const std::string f = kFiles[step / 2];
    const bool backup = step % 2 == 0;
    const fs::path from = backup ? dir / f : staging / f;
    const fs::path to = backup ? dir / (f + ".old") : dir / f;
    const std::string what = backup ? "renaming " + f + " to " + f + ".old" : "putting the new " + f + " in place";
    if (step == fail_at) {
      why = what + " failed (test switch, step " + std::to_string(step) + ")";
      break;
    }
    if (backup && IsMissing(from)) continue;  // nothing installed under this name: the new one is simply placed
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {  // never replaces: a leftover .old fails here
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

Result CheckAndUpdate() {
  Result r;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_installed.empty()) {
      r.tag = g_installed;
      r.message = UpdatedText(g_installed, false);  // already swapped in by this process: nothing to download
      return r;
    }
  }
  std::string json, err;
  const std::wstring api = L"https://api.github.com/repos/" + Widen(DOA4_RELEASE_REPO) + L"/releases?per_page=30";
  if (!HttpGet(api, true, [&](const char* data, size_t n) {
        if (json.size() + n > (size_t(16) << 20)) return false;  // no release list is that large
        json.append(data, n);
        return true;
      }, &err)) {
    r.message = "Could not reach GitHub (" + err + ")";
    return r;
  }
  const Choice c = Choose(json, DOA4_RELEASE_CHANNEL, DOA4_RELEASE_TAG);
  r.tag = c.tag;
  if (!c.update) {
    r.message = c.message;
    return r;
  }
  SetMessage(c.message);
  const fs::path dir = ExeDir();
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
  std::string sums;
  std::vector<std::string> names(std::begin(kFiles), std::end(kFiles));
  names.push_back(kSumsName);
  for (const std::string& name : names) {
    const auto url = c.urls.find(name);
    std::string why = url == c.urls.end() ? "no download address" : "";
    if (why.empty() && !Download(url->second, staging / name, name == kSumsName ? &sums : nullptr, &why) && why.empty())
      why = "failed";
    if (!why.empty()) {
      fs::remove_all(staging, ec);
      r.message = "Update to " + c.tag + " failed: could not download " + name + " (" + why + "); nothing was changed";
      return r;
    }
  }
  DeleteOldFiles(dir);  // leftovers of an earlier update that are no longer in use
  const std::string why = VerifyAndSwap(dir, staging, sums);
  fs::remove_all(staging, ec);
  if (!why.empty()) {
    r.message = "Update to " + c.tag + " failed: " + why;
    return r;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_installed = c.tag;
  }
  r.ok = true;
  r.message = UpdatedText(c.tag, c.replaces_dev);
  return r;
}

// ---- one check at a time ----

bool Start(std::function<void(const std::string&)> log) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_busy) return false;
    g_busy = true;
    g_message = "Checking for updates...";
    g_log = std::move(log);
  }
  try {
    std::thread([] {
      std::function<void(const std::string&)> log;
      {
        std::lock_guard<std::mutex> lock(g_mu);
        log = g_log;
      }
      if (log) log("[doa4][UPDATE] Checking for updates...");
      const Result r = CheckAndUpdate();
      {
        std::lock_guard<std::mutex> lock(g_mu);
        g_message = r.message;
        g_busy = false;
        log = g_log;
      }
      if (log) log("[doa4][UPDATE] " + r.message);
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

std::string Message() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_message;
}

}  // namespace doa4::updater

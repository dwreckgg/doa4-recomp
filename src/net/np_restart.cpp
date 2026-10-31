// See np_restart.h.
#include "np_restart.h"

#include <cstdio>
#include <cwctype>
#include <fstream>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace doa4::np::restart {
namespace fs = std::filesystem;

fs::path HandoffPath(const fs::path& user_data) { return user_data / "netplay" / "handoff.d4h"; }

fs::path SessionUserData(const fs::path& user_data) { return user_data / "netplay" / "session"; }

fs::path SessionDir(const fs::path& user_data, const fs::path& test_dir, bool host, std::time_t now) {
  if (!test_dir.empty()) return test_dir / "session";
  std::tm local{};
  localtime_s(&local, &now);
  char name[48];
  std::snprintf(name, sizeof name, "%04d%02d%02d-%02d%02d%02d_%s", local.tm_year + 1900, local.tm_mon + 1,
                local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, host ? "host" : "joiner");
  return user_data / "netplay" / "sessions" / name;
}

bool RedirectHolds(const fs::path& runtime_user_data, const fs::path& session_user_data) {
  if (runtime_user_data.empty() || session_user_data.empty()) return false;
  auto norm = [](const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) a = p;
    std::wstring s = a.lexically_normal().generic_wstring();
    while (s.size() > 3 && s.back() == L'/') s.pop_back();
    for (wchar_t& c : s) c = wchar_t(towlower(c));
    return s;
  };
  return norm(runtime_user_data) == norm(session_user_data);
}

bool KnownUnregisteredSetting(std::string_view name) {
  return name == "kernel_pix" || name == "vulkan_readback_resolve" || name == "vulkan_readback_memexport";
}

bool TestNoRestart(bool test_dir_set, const char* value) {
  return test_dir_set && value != nullptr && std::string_view(value) == "1";
}

namespace {

std::string Text(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

// The attributes of the entry itself (a link is not followed: GetFileAttributesW reports the link's own attributes).
DWORD Attributes(const fs::path& p) { return GetFileAttributesW(p.c_str()); }

bool IsReparse(DWORD a) { return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT); }

// Removes everything inside `dir` (a real folder), depth first. A reparse point is removed as the link itself.
bool Empty(const fs::path& dir, std::string* error) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir / L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND) return true;
    *error = "cannot list " + Text(dir) + " (Windows error " + std::to_string(e) + ")";
    return false;
  }
  bool ok = true;
  do {
    const std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    const fs::path p = dir / name;
    const DWORD a = fd.dwFileAttributes;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), a & ~DWORD(FILE_ATTRIBUTE_READONLY));
    BOOL removed;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
      // a junction or a directory link: RemoveDirectoryW removes the link, not the target; a file link: DeleteFileW
      removed = (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(p.c_str()) : DeleteFileW(p.c_str());
    } else if (a & FILE_ATTRIBUTE_DIRECTORY) {
      if (!Empty(p, error)) {
        ok = false;
        break;
      }
      removed = RemoveDirectoryW(p.c_str());
    } else {
      removed = DeleteFileW(p.c_str());
    }
    if (!removed) {
      *error = "cannot remove " + Text(p) + " (Windows error " + std::to_string(GetLastError()) + ")";
      ok = false;
      break;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return ok;
}

}  // namespace

bool ClearFolder(const fs::path& dir, std::string* error) {
  error->clear();
  const DWORD a = Attributes(dir);
  if (a == INVALID_FILE_ATTRIBUTES) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
      *error = "cannot create " + Text(dir);
      return false;
    }
    // a parent of the folder may itself be a link the creation went through: refuse a folder that is not real now
    if (IsReparse(Attributes(dir))) {
      *error = Text(dir) + " is a link, not a folder";
      return false;
    }
    return true;
  }
  if (IsReparse(a)) {
    *error = Text(dir) + " is a link (reparse point), not a folder: it is not cleared or used";
    return false;
  }
  if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
    *error = Text(dir) + " is a file, not a folder";
    return false;
  }
  return Empty(dir, error);
}

bool WriteSaveFiles(const fs::path& dir, const std::vector<handoff::File>& files, std::string* error) {
  error->clear();
  bool ok = true;
  for (const handoff::File& f : files) {
    if (!handoff::NameOk(f.name)) {
      *error += (error->empty() ? "" : "; ") + std::string("refused save file name '") + f.name + "'";
      ok = false;
      continue;
    }
    const fs::path p = dir / fs::path(std::u8string(f.name.begin(), f.name.end()));
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (out) out.write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
    if (!out) {
      *error += (error->empty() ? "" : "; ") + std::string("cannot write ") + Text(p);
      ok = false;
    }
  }
  return ok;
}

RelaunchPlan PlanRelaunch(const std::wstring& command_line, unsigned long parent_pid, const fs::path* handoff,
                          bool after_session, bool parent_use_show, unsigned short parent_show) {
  RelaunchPlan plan;
  plan.command_line = command_line;
  plan.env.push_back({L"DOA4_WAIT_PID", std::to_wstring(parent_pid)});
  plan.env.push_back({L"DOA4_NP_HANDOFF", handoff ? handoff->wstring() : std::wstring()});
  plan.env.push_back({L"DOA4_NP_AFTER_SESSION", after_session ? L"1" : L""});
  plan.use_show = parent_use_show;
  plan.show = parent_use_show ? parent_show : 0;
  return plan;
}

bool Relaunch(const fs::path* handoff, bool after_session, std::string* error) {
  wchar_t exe[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (length == 0 || length == MAX_PATH) {
    *error = "cannot find the game's executable";
    return false;
  }
  STARTUPINFOW parent{};
  parent.cb = sizeof parent;
  GetStartupInfoW(&parent);
  const RelaunchPlan plan = PlanRelaunch(GetCommandLineW(), GetCurrentProcessId(), handoff, after_session,
                                         (parent.dwFlags & STARTF_USESHOWWINDOW) != 0, parent.wShowWindow);
  // set for the child, restored right after: this process keeps its own values
  std::vector<std::pair<std::wstring, std::wstring>> saved;  // name, old value ("" + flag below = unset)
  std::vector<bool> had;
  for (const EnvVar& v : plan.env) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(v.name.c_str(), buf, 32768);
    had.push_back(n > 0 && n < 32768);
    saved.push_back({v.name, had.back() ? std::wstring(buf, n) : std::wstring()});
    SetEnvironmentVariableW(v.name.c_str(), v.value.empty() ? nullptr : v.value.c_str());
  }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  if (plan.use_show) {
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = plan.show;
  }
  PROCESS_INFORMATION pi{};
  std::wstring command_line = plan.command_line;
  const BOOL created =
      CreateProcessW(exe, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
  const DWORD e = GetLastError();
  for (size_t i = 0; i < saved.size(); ++i) {
    SetEnvironmentVariableW(saved[i].first.c_str(), had[i] ? saved[i].second.c_str() : nullptr);
  }
  if (!created) {
    *error = "CreateProcessW failed (Windows error " + std::to_string(e) + ")";
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

}  // namespace doa4::np::restart

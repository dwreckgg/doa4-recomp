// See np_files.h.
#include "np_files.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <system_error>

namespace doa4::np::files {
namespace {

constexpr uint32_t kSample = 64u * 1024u;
constexpr uint64_t kStride = 16ull * 1024u * 1024u;
constexpr uint64_t kFnvInit = 1469598103934665603ull;
constexpr uint64_t kFnvMul = 1099511628211ull;

uint64_t Fnv(uint64_t h, const void* p, size_t n) {
  const unsigned char* b = static_cast<const unsigned char*>(p);
  while (n--) {
    h ^= *b++;
    h *= kFnvMul;
  }
  return h;
}

std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

bool Skipped(const std::string& lower_name) {
  return lower_name == "desktop.ini" || lower_name == "thumbs.db";
}

bool ReadAt(HANDLE h, uint64_t off, unsigned char* dst, DWORD len, DWORD* got) {
  LARGE_INTEGER pos;
  pos.QuadPart = static_cast<LONGLONG>(off);
  *got = 0;
  if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
  if (len == 0) return true;
  return ReadFile(h, dst, len, got, nullptr) && *got == len;
}

// 64 KB at every 16 MB and the last 64 KB, each hashed with its offset. False when a read fails.
bool SampledHash(HANDLE h, uint64_t len, unsigned char* buf, uint64_t* hash) {
  for (uint64_t at = 0;; at += kStride) {
    uint64_t off = at;
    bool last = false;
    if (off + kSample >= len) {  // the end: the last 64 KB
      off = len > kSample ? len - kSample : 0;
      last = true;
    }
    DWORD want = static_cast<DWORD>(len - off < kSample ? len - off : kSample);
    DWORD got = 0;
    if (!ReadAt(h, off, buf, want, &got)) return false;
    *hash = Fnv(*hash, &off, sizeof off);
    *hash = Fnv(*hash, buf, got);
    if (last) break;
  }
  return true;
}

// Every byte of the file. False when a read fails.
bool FullHash(HANDLE h, uint64_t len, unsigned char* buf, uint64_t* hash) {
  for (uint64_t off = 0; off < len; off += kSample) {
    DWORD want = static_cast<DWORD>(len - off < kSample ? len - off : kSample);
    DWORD got = 0;
    if (!ReadAt(h, off, buf, want, &got)) return false;
    *hash = Fnv(*hash, buf, got);
  }
  return true;
}

// Folds one file (path lower case with '/', size, content) into `hash`. False when it cannot be read.
bool FoldFile(const std::filesystem::path& full, const std::string& rel, bool whole, unsigned char* buf,
              uint64_t* hash) {
  HANDLE h = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz;
  uint64_t content = kFnvInit;
  bool ok = GetFileSizeEx(h, &sz) != 0;
  uint64_t size = ok ? static_cast<uint64_t>(sz.QuadPart) : 0;
  if (ok) ok = whole ? FullHash(h, size, buf, &content) : SampledHash(h, size, buf, &content);
  CloseHandle(h);
  if (!ok) return false;
  *hash = Fnv(*hash, rel.c_str(), rel.size() + 1);
  *hash = Fnv(*hash, &size, sizeof size);
  *hash = Fnv(*hash, &content, sizeof content);
  return true;
}

struct Entry {
  std::string lower;
  std::filesystem::path full;
  bool dir = false;
};

// Entries of one folder in lower-case name order; false when it cannot be listed.
bool List(const std::filesystem::path& dir, std::vector<Entry>* out) {
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec), end;
  if (ec) return false;
  for (; it != end; it.increment(ec)) {
    if (ec) return false;
    Entry e;
    e.lower = Lower(Utf8(it->path().filename().wstring()));
    if (e.lower.empty() || Skipped(e.lower)) continue;
    std::error_code dec;
    e.dir = it->is_directory(dec);
    if (dec) return false;
    e.full = it->path();
    out->push_back(std::move(e));
  }
  if (ec) return false;
  std::sort(out->begin(), out->end(), [](const Entry& a, const Entry& b) { return a.lower < b.lower; });
  return true;
}

// Folds every file under `dir` (recursing); `rel` is the path so far. False on anything unreadable.
bool Walk(const std::filesystem::path& dir, const std::string& rel, unsigned char* buf, uint64_t* hash,
          uint32_t* files, int depth) {
  if (depth > 16) return false;
  std::vector<Entry> list;
  if (!List(dir, &list)) return false;
  for (const Entry& e : list) {
    std::string path = rel + "/" + e.lower;
    if (e.dir) {
      if (!Walk(e.full, path, buf, hash, files, depth + 1)) return false;
    } else {
      if (!FoldFile(e.full, path, false, buf, hash)) return false;
      ++*files;
    }
  }
  return true;
}

}  // namespace

std::vector<Group> Fingerprint(const std::filesystem::path& dir) {
  std::vector<Group> groups;
  std::vector<Entry> top;
  if (!List(dir, &top)) return groups;
  std::unique_ptr<unsigned char[]> buf(new unsigned char[kSample]);
  for (const Entry& e : top) {
    if (e.lower == "movie") continue;
    Group g;
    g.name = e.lower;
    uint64_t hash = kFnvInit;
    bool ok;
    if (e.dir) {
      ok = Walk(e.full, e.lower, buf.get(), &hash, &g.files, 0);
    } else {
      ok = FoldFile(e.full, e.lower, e.lower == "default.xex", buf.get(), &hash);
      if (ok) g.files = 1;
    }
    if (ok) {
      g.hash = hash;
    } else {
      g.unreadable = true;
      g.files = 0;
    }
    groups.push_back(std::move(g));
  }
  std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.name < b.name; });
  return groups;
}

std::string Text(const std::vector<Group>& groups) {
  std::vector<const Group*> sorted;
  for (const Group& g : groups) sorted.push_back(&g);
  std::sort(sorted.begin(), sorted.end(), [](const Group* a, const Group* b) { return a->name < b->name; });
  std::string t;
  for (const Group* g : sorted) {
    t += g->name;
    t += '=';
    if (g->unreadable) {
      t += "unreadable";
    } else {
      char b[24];
      std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(g->hash));
      t += b;
    }
    t += '\n';
  }
  return t;
}

}  // namespace doa4::np::files

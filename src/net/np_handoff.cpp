// DOA4 netplay: the session hand-off (see np_handoff.h for the contract).
// Pure: no SDK, no Windows, no files, no sockets. All integers are packed and read byte by byte, little-endian.
// A joiner unpacks bytes a REMOTE host sent, so every check here is a security boundary: the size is checked before
// anything is read, every length before the bytes it announces, every file name before it can become a path.

#include "np_handoff.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <optional>

#include "../doa4_tweaks.h"

namespace doa4::np::handoff {
namespace {

constexpr uint16_t kVersion = 1;
constexpr char kMagic[4] = {'D', '4', 'H', 'O'};
// Everything but the release text, the guest settings text and the files: magic 4, version 2, build 8, release
// length 2, role 1, delay 1, local port 2, peer ip 4, peer port 2, token 4, seed 8, guest length 4, file count 4.
constexpr size_t kFixedBytes = 46;
constexpr uint8_t kMinDelay = 1;
constexpr uint8_t kMaxDelay = 8;

// The SDK cvars the census (Task 4 report, Step 1) found can reach the guest, in this order. Each name is a cvar
// defined in sdk-np1 (the host test checks that against the sources). Why each is here is in the report's table.
constexpr const char* kSdkSettings[] = {
    // guest kernel and system state: what an export returns to the guest, what the guest image or heap holds
    "user_language", "user_country", "license_mask", "cl", "xex_apply_patches", "kernel_debug_monitor",
    "kernel_cert_monitor", "kernel_pix", "rexcrt_heap_enable", "rexcrt_heap_size_mb", "protect_zero",
    "protect_on_release", "scribble_heap",
    // guest threads, files and clocks
    "ignore_thread_priorities", "ignore_thread_affinities", "allow_game_relative_writes", "clock_no_scaling",
    "clock_source_raw",
    // the guest video mode (window_width/height feed it while video_mode_width/height are at their defaults) and
    // the vblank cadence
    "video_mode_width", "video_mode_height", "video_mode_refresh_rate", "resolution", "window_width", "window_height",
    "vsync",
    // what the GPU writes back into guest memory
    "readback_resolve", "readback_resolve_half_pixel_offset", "readback_memexport", "readback_memexport_fast",
    "d3d12_readback_resolve", "d3d12_readback_memexport", "vulkan_readback_resolve", "vulkan_readback_memexport",
    "query_occlusion_fake_sample_count", "occlusion_query_enable", "clear_memory_page_state",
    // guest audio callbacks
    "audio_maxqframes",
};

// ---- bytes ----

class Writer {
 public:
  explicit Writer(std::vector<uint8_t>* out) : out_(out) {}
  void U8(uint8_t v) { out_->push_back(v); }
  void U16(uint16_t v) {
    U8(uint8_t(v & 0xFF));
    U8(uint8_t(v >> 8));
  }
  void U32(uint32_t v) {
    U16(uint16_t(v & 0xFFFF));
    U16(uint16_t(v >> 16));
  }
  void U64(uint64_t v) {
    U32(uint32_t(v & 0xFFFFFFFFu));
    U32(uint32_t(v >> 32));
  }
  void Bytes(const uint8_t* p, size_t n) { out_->insert(out_->end(), p, p + n); }
  void Text(std::string_view s) { out_->insert(out_->end(), s.begin(), s.end()); }

 private:
  std::vector<uint8_t>* out_;
};

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> in) : in_(in) {}
  size_t Left() const { return in_.size() - pos_; }
  bool AtEnd() const { return pos_ == in_.size(); }
  bool U8(uint8_t* v) {
    if (Left() < 1) return false;
    *v = in_[pos_++];
    return true;
  }
  bool U16(uint16_t* v) {
    uint8_t lo = 0, hi = 0;
    if (!U8(&lo) || !U8(&hi)) return false;
    *v = uint16_t(lo | (hi << 8));
    return true;
  }
  bool U32(uint32_t* v) {
    uint16_t lo = 0, hi = 0;
    if (!U16(&lo) || !U16(&hi)) return false;
    *v = uint32_t(lo) | (uint32_t(hi) << 16);
    return true;
  }
  bool U64(uint64_t* v) {
    uint32_t lo = 0, hi = 0;
    if (!U32(&lo) || !U32(&hi)) return false;
    *v = uint64_t(lo) | (uint64_t(hi) << 32);
    return true;
  }
  // The next n bytes (a view into the input), false if fewer remain. Never advances past the end: n is compared
  // with what is left, not added to the position.
  bool Take(size_t n, std::span<const uint8_t>* out) {
    if (n > Left()) return false;
    *out = in_.subspan(pos_, n);
    pos_ += n;
    return true;
  }

 private:
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
};

// ---- names ----

char Upper(char c) { return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c; }

bool IsNameChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

// A Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9) opens the device instead of a file, with any
// extension: the base is the text before the first '.', compared without regard to case.
bool IsDeviceName(std::string_view comp) {
  const std::string_view base = comp.substr(0, comp.find('.'));
  if (base.size() == 3) {
    const char a = Upper(base[0]), b = Upper(base[1]), c = Upper(base[2]);
    return (a == 'C' && b == 'O' && c == 'N') || (a == 'P' && b == 'R' && c == 'N') ||
           (a == 'A' && b == 'U' && c == 'X') || (a == 'N' && b == 'U' && c == 'L');
  }
  if (base.size() == 4) {
    const char a = Upper(base[0]), b = Upper(base[1]), c = Upper(base[2]);
    const bool com = a == 'C' && b == 'O' && c == 'M';
    const bool lpt = a == 'L' && b == 'P' && c == 'T';
    return (com || lpt) && base[3] >= '1' && base[3] <= '9';
  }
  return false;
}

bool ComponentOk(std::string_view comp) {
  if (comp.empty()) return false;
  // "." and ".." name the current and the parent folder. (The trailing-dot rule below refuses both as well;
  // each rule is kept, and a probe mutates the two together.)
  if (comp == "." || comp == "..") return false;
  if (comp.back() == '.') return false;  // Windows drops a trailing dot: "a." is the file "a"
  return !IsDeviceName(comp);
}

// The name folded to lower case: Windows file names are not case-sensitive.
std::string Fold(std::string_view s) {
  std::string f(s);
  for (char& c : f) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
  return f;
}

// Two names that are one file, or a name that is also a folder of another: null if the files are fine.
const char* FileCollision(const std::vector<File>& files) {
  std::vector<std::string> folded;
  folded.reserve(files.size());
  for (const File& f : files) folded.push_back(Fold(f.name));
  for (size_t i = 0; i < folded.size(); ++i) {
    for (size_t j = i + 1; j < folded.size(); ++j) {
      const std::string& a = folded[i];
      const std::string& b = folded[j];
      if (a == b) return "a duplicate file name";
      const std::string& shorter = a.size() < b.size() ? a : b;
      const std::string& longer = a.size() < b.size() ? b : a;
      if (longer.size() > shorter.size() && longer.compare(0, shorter.size(), shorter) == 0 &&
          longer[shorter.size()] == '/') {
        return "a file name is also a folder";
      }
    }
  }
  return nullptr;
}

// ---- the fields' ranges, shared by Pack and Unpack ----

bool RoleOk(uint8_t role) { return role == uint8_t(Role::kHost) || role == uint8_t(Role::kJoiner); }
bool DelayOk(uint8_t delay) { return delay >= kMinDelay && delay <= kMaxDelay; }

size_t PackedSize(const Handoff& h) {
  size_t n = kFixedBytes + h.release.size() + h.guest_settings.size();
  for (const File& f : h.files) n += 2 + f.name.size() + 4 + f.data.size();
  return n;
}

// True if Pack may write `h`: every limit and range Unpack enforces.
bool Packable(const Handoff& h) {
  if (!RoleOk(uint8_t(h.role)) || !DelayOk(h.delay) || h.session_token == 0) return false;
  if (h.release.size() > kMaxReleaseBytes || h.guest_settings.size() > kMaxGuestBytes) return false;
  if (h.files.size() > kMaxFiles) return false;
  size_t total = kFixedBytes + h.release.size() + h.guest_settings.size();
  for (const File& f : h.files) {
    if (!NameOk(f.name) || f.data.size() > kMaxFileBytes) return false;
    total += 2 + f.name.size() + 4 + f.data.size();
  }
  if (total > kMaxPackedBytes) return false;
  return FileCollision(h.files) == nullptr;
}

// A name from outside as it may appear in a message: printable ASCII, at most 64 characters.
std::string Shown(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size() && i < 64; ++i) out += (s[i] >= 0x20 && s[i] <= 0x7E) ? s[i] : '?';
  if (s.size() > 64) out += "...";
  return out;
}

}  // namespace

bool NameOk(std::string_view name) {
  if (name.size() > kMaxNameBytes) return false;
  if (!name.starts_with(std::string_view(kSavePrefix))) return false;
  size_t components = 0, start = 0;
  for (size_t i = 0; i <= name.size(); ++i) {
    if (i < name.size() && name[i] != '/') {
      if (!IsNameChar(name[i])) return false;
      continue;
    }
    if (!ComponentOk(name.substr(start, i - start))) return false;
    ++components;
    start = i + 1;
  }
  return components <= kMaxNameComponents;
}

std::vector<uint8_t> Pack(const Handoff& h) {
  if (!Packable(h)) return {};
  std::vector<uint8_t> out;
  out.reserve(PackedSize(h));
  Writer w(&out);
  w.Bytes(reinterpret_cast<const uint8_t*>(kMagic), sizeof(kMagic));
  w.U16(kVersion);
  w.U64(h.build);
  w.U16(uint16_t(h.release.size()));
  w.Text(h.release);
  w.U8(uint8_t(h.role));
  w.U8(h.delay);
  w.U16(h.local_port);
  w.Bytes(h.peer.ip.data(), h.peer.ip.size());
  w.U16(h.peer.port);
  w.U32(h.session_token);
  w.U64(h.seed);
  w.U32(uint32_t(h.guest_settings.size()));
  w.Text(h.guest_settings);
  w.U32(uint32_t(h.files.size()));
  for (const File& f : h.files) {
    w.U16(uint16_t(f.name.size()));
    w.Text(f.name);
    w.U32(uint32_t(f.data.size()));
    w.Bytes(f.data.data(), f.data.size());
  }
  return out;
}

std::string Unpack(std::span<const uint8_t> bytes, Handoff* out) {
  if (!out) return "no destination";
  *out = Handoff{};
  constexpr const char* kCut = "the hand-off is cut short";
  if (bytes.size() > kMaxPackedBytes) return "the hand-off is too large";

  Reader r(bytes);
  Handoff h;
  std::span<const uint8_t> view;
  if (!r.Take(sizeof(kMagic), &view)) return kCut;
  if (std::memcmp(view.data(), kMagic, sizeof(kMagic)) != 0) return "not a session hand-off";
  uint16_t version = 0;
  if (!r.U16(&version)) return kCut;
  if (version != kVersion) return "a different hand-off version";
  if (!r.U64(&h.build)) return kCut;

  uint16_t release_len = 0;
  if (!r.U16(&release_len)) return kCut;
  if (release_len > kMaxReleaseBytes) return "the release text is too long";
  if (!r.Take(release_len, &view)) return kCut;
  h.release.assign(view.begin(), view.end());

  uint8_t role = 0, delay = 0;
  if (!r.U8(&role)) return kCut;
  if (!RoleOk(role)) return "an unknown role";
  h.role = Role(role);
  if (!r.U8(&delay)) return kCut;
  if (!DelayOk(delay)) return "a bad input delay";
  h.delay = delay;
  if (!r.U16(&h.local_port)) return kCut;
  if (!r.Take(h.peer.ip.size(), &view)) return kCut;
  std::copy(view.begin(), view.end(), h.peer.ip.begin());
  if (!r.U16(&h.peer.port)) return kCut;
  if (!r.U32(&h.session_token)) return kCut;
  if (h.session_token == 0) return "a zero session token";
  if (!r.U64(&h.seed)) return kCut;

  uint32_t guest_len = 0;
  if (!r.U32(&guest_len)) return kCut;
  if (guest_len > kMaxGuestBytes) return "the guest settings are too long";
  if (!r.Take(guest_len, &view)) return kCut;
  h.guest_settings.assign(view.begin(), view.end());

  uint32_t count = 0;
  if (!r.U32(&count)) return kCut;
  if (count > kMaxFiles) return "too many files";
  for (uint32_t i = 0; i < count; ++i) {
    uint16_t name_len = 0;
    if (!r.U16(&name_len)) return kCut;
    // An over-long name is refused before its bytes are looked for (a lying length is "a refused file name", not
    // "cut short"). The empty name is refused here and again by NameOk below with the same reason: the two rules
    // overlap by design, and the probe's mutant that drops the "== 0" half survives for that reason alone.
    if (name_len == 0 || name_len > kMaxNameBytes) return "a refused file name";
    if (!r.Take(name_len, &view)) return kCut;
    File f;
    f.name.assign(view.begin(), view.end());
    if (!NameOk(f.name)) return "a refused file name";
    uint32_t data_len = 0;
    if (!r.U32(&data_len)) return kCut;
    if (data_len > kMaxFileBytes) return "a file too large";
    if (!r.Take(data_len, &view)) return kCut;
    f.data.assign(view.begin(), view.end());
    h.files.push_back(std::move(f));
  }
  if (!r.AtEnd()) return "extra bytes after the hand-off";
  if (const char* why = FileCollision(h.files)) return why;

  *out = std::move(h);
  return {};
}

// ---- guest-visible settings ----

const std::vector<std::string>& GuestSettingNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v(std::begin(kSdkSettings), std::end(kSdkSettings));
    for (const doa4::tweaks::Tweak& t : doa4::tweaks::AllTweaks()) v.push_back(t.cvar);
    return v;
  }();
  return names;
}

std::string CaptureGuestSettings(const std::function<std::string(const std::string& name)>& query) {
  std::string text;
  for (const std::string& name : GuestSettingNames()) {
    const std::string value = query(name);
    if (value.find_first_of("\r\n") != std::string::npos) return {};
    text += name;
    text += '=';
    text += value;
    text += '\n';
  }
  return text;
}

std::string ApplyGuestSettings(std::string_view text,
                               const std::function<bool(const std::string& name, const std::string& value)>& set) {
  if (text.find('\r') != std::string_view::npos) return "a carriage return in the guest settings";
  const std::vector<std::string>& names = GuestSettingNames();
  std::vector<std::optional<std::string_view>> values(names.size());
  for (size_t pos = 0; pos < text.size();) {
    size_t end = text.find('\n', pos);
    const bool unterminated = end == std::string_view::npos;  // only the last line can be
    if (unterminated) end = text.size();
    const std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos || eq == 0) return "a malformed guest setting line";
    const std::string_view name = line.substr(0, eq);
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) return "unknown guest setting " + Shown(name);
    std::optional<std::string_view>& slot = values[size_t(it - names.begin())];
    if (slot) return "guest setting " + *it + " given twice";
    slot = line.substr(eq + 1);
    if (unterminated) return "the last guest setting line has no newline";
  }
  for (size_t i = 0; i < names.size(); ++i) {
    if (!values[i]) return "guest setting " + names[i] + ": missing";
  }
  for (size_t i = 0; i < names.size(); ++i) {
    if (!set(names[i], std::string(*values[i]))) return "the game refused guest setting " + names[i];
  }
  return {};
}

}  // namespace doa4::np::handoff

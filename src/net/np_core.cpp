// See np_core.h.

#include "np_core.h"

#include <algorithm>
#include <cstring>

namespace doa4::np {

namespace {

void PutBe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
uint16_t GetBe16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

void PutLe32(std::string* s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s->push_back(char(uint8_t(v >> (8 * i))));
}
void PutLe64(std::string* s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s->push_back(char(uint8_t(v >> (8 * i))));
}
uint32_t GetLe32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t GetLe64(const uint8_t* p) { return uint64_t(GetLe32(p)) | uint64_t(GetLe32(p + 4)) << 32; }

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

std::vector<std::string_view> Words(std::string_view s) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

bool ParseInt(std::string_view s, long long lo, long long hi, long long* out) {
  if (s.empty()) return false;
  bool neg = false;
  size_t i = 0;
  if (s[0] == '-' || s[0] == '+') {
    neg = s[0] == '-';
    i = 1;
  }
  if (i >= s.size()) return false;
  long long v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
    if (v > 4000000000ll) return false;
  }
  if (neg) v = -v;
  if (v < lo || v > hi) return false;
  *out = v;
  return true;
}

std::string Upper(std::string_view s) {
  std::string u(s);
  for (char& c : u) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  }
  return u;
}

// One user's item list: "-" or "A+UP+LX=-100".
bool ParsePad(std::string_view text, Pad* pad, std::string* why) {
  *pad = Pad{};
  if (text == "-") return true;
  size_t i = 0;
  while (i <= text.size()) {
    size_t j = text.find('+', i);
    if (j == std::string_view::npos) j = text.size();
    const std::string item = Upper(text.substr(i, j - i));
    i = j + 1;
    if (item.empty()) {
      *why = "empty item";
      return false;
    }
    static constexpr struct {
      const char* name;
      uint16_t bit;
    } kButtons[] = {{"A", kA},         {"B", kB},       {"X", kX},           {"Y", kY},
                    {"UP", kUp},       {"DOWN", kDown}, {"LEFT", kLeft},     {"RIGHT", kRight},
                    {"START", kStart}, {"BACK", kBack}, {"LB", kLB},         {"RB", kRB},
                    {"LS", kLThumb},   {"RS", kRThumb}};
    bool done = false;
    for (const auto& b : kButtons) {
      if (item == b.name) {
        pad->buttons |= b.bit;
        done = true;
      }
    }
    if (done) continue;
    const size_t eq = item.find('=');
    if (eq == std::string::npos) {
      *why = "unknown item '" + item + "'";
      return false;
    }
    const std::string key = item.substr(0, eq);
    const std::string_view value = std::string_view(item).substr(eq + 1);
    long long v = 0;
    if (key == "LT" || key == "RT") {
      if (!ParseInt(value, 0, 255, &v)) {
        *why = key + " needs 0..255";
        return false;
      }
      (key == "LT" ? pad->lt : pad->rt) = uint8_t(v);
    } else if (key == "LX" || key == "LY" || key == "RX" || key == "RY") {
      if (!ParseInt(value, -32768, 32767, &v)) {
        *why = key + " needs -32768..32767";
        return false;
      }
      int16_t* axis = key == "LX" ? &pad->lx : key == "LY" ? &pad->ly : key == "RX" ? &pad->rx : &pad->ry;
      *axis = int16_t(v);
    } else {
      *why = "unknown item '" + item + "'";
      return false;
    }
    if (j == text.size()) break;
  }
  return true;
}

// xorshift64*: the random fighting input of a script, a pure function of its seed.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull) {
    if (!s) s = 1;
  }
  uint32_t Next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return uint32_t((s * 0x2545F4914F6CDD1Dull) >> 32);
  }
  uint32_t Below(uint32_t n) { return Next() % n; }
};

}  // namespace

void PadToGuest(const Pad& pad, uint8_t out[12]) {
  PutBe16(out + 0, pad.buttons);
  out[2] = pad.lt;
  out[3] = pad.rt;
  PutBe16(out + 4, uint16_t(pad.lx));
  PutBe16(out + 6, uint16_t(pad.ly));
  PutBe16(out + 8, uint16_t(pad.rx));
  PutBe16(out + 10, uint16_t(pad.ry));
}

Pad PadFromGuest(const uint8_t in[12]) {
  Pad pad;
  pad.buttons = GetBe16(in + 0);
  pad.lt = in[2];
  pad.rt = in[3];
  pad.lx = int16_t(GetBe16(in + 4));
  pad.ly = int16_t(GetBe16(in + 6));
  pad.rx = int16_t(GetBe16(in + 8));
  pad.ry = int16_t(GetBe16(in + 10));
  return pad;
}

bool ParseScript(std::string_view text, Script* out, std::string* error) {
  *out = Script{};
  error->clear();
  uint8_t connected = 1;
  bool connect_seen = false;
  int line_no = 0;
  auto fail = [&](const std::string& why) {
    *error = "line " + std::to_string(line_no) + ": " + why;
    return false;
  };
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) nl = text.size();
    std::string_view line = text.substr(pos, nl - pos);
    pos = nl + 1;
    ++line_no;
    const size_t hash = line.find('#');
    if (hash != std::string_view::npos) line = line.substr(0, hash);
    line = Trim(line);
    if (line.empty()) {
      if (nl == text.size()) break;
      continue;
    }
    const std::vector<std::string_view> w = Words(line);
    const std::string head = Upper(w[0]);
    const uint32_t now = uint32_t(out->frames.size());
    if (head == "CONNECT") {
      uint8_t mask = 0;
      for (size_t i = 1; i < w.size(); ++i) {
        long long u = 0;
        if (!ParseInt(w[i], 0, kUsers - 1, &u)) return fail("connect takes users 0-3");
        mask |= uint8_t(1u << u);
      }
      connected = mask;
      if (!connect_seen) out->inherit_frames = out->frames.size();
      connect_seen = true;
    } else if (head == "SHOT" || head == "QUIT") {
      if (w.size() != 1) return fail(head + " takes no arguments");
      out->events.push_back({head == "SHOT" ? ScriptEvent::kShot : ScriptEvent::kQuit, now});
    } else if (head == "RANDOM") {
      long long frames = 0, seed = 0;
      if (w.size() < 4 || !ParseInt(w[1], 1, 10000000, &frames) || !ParseInt(w[2], 0, 4000000000ll, &seed)) {
        return fail("random <frames> <seed> <user> [<user> ...]");
      }
      uint8_t users = 0;
      for (size_t i = 3; i < w.size(); ++i) {
        long long u = 0;
        if (!ParseInt(w[i], 0, kUsers - 1, &u)) return fail("random: users are 0-3");
        users |= uint8_t(1u << u);
      }
      static constexpr uint16_t kDirs[] = {0,          kUp,           kDown,         kLeft,         kRight,
                                           kUp | kLeft, kUp | kRight, kDown | kLeft, kDown | kRight};
      static constexpr uint16_t kAttacks[] = {0, kA, kB, kX, kY, kX | kY, kA | kB};
      Rng rng{uint64_t(seed)};
      struct Held {
        uint16_t buttons = 0;
        uint32_t left = 0;
      } held[kUsers];
      for (long long f = 0; f < frames; ++f) {
        FrameInput in;
        in.connected = connected;
        for (int u = 0; u < kUsers; ++u) {
          if (!(users & (1u << u))) continue;
          if (held[u].left == 0) {
            held[u].buttons = uint16_t(kDirs[rng.Below(9)] | kAttacks[rng.Below(7)]);
            held[u].left = 2 + rng.Below(11);
          }
          --held[u].left;
          in.pad[u].buttons = held[u].buttons;
        }
        out->frames.push_back(in);
      }
    } else {
      long long frames = 0;
      if (!ParseInt(w[0], 1, 10000000, &frames)) return fail("unknown command '" + std::string(w[0]) + "'");
      if (w.size() > 1 + kUsers) return fail("more than four users");
      FrameInput in;
      in.connected = connected;
      for (size_t i = 1; i < w.size(); ++i) {
        std::string why;
        if (!ParsePad(w[i], &in.pad[i - 1], &why)) return fail(why);
      }
      out->frames.insert(out->frames.end(), size_t(frames), in);
    }
    if (nl == text.size()) break;
  }
  if (!connect_seen) out->inherit_frames = out->frames.size();
  return true;
}

uint32_t InputQueue::Append(const Script& script, uint32_t now) {
  const uint32_t start = std::max(now, end_);
  if (end_ <= now) tail_connected_ = connected_;  // nothing still queued
  for (size_t i = 0; i < script.frames.size(); ++i) {
    FrameInput in = script.frames[i];
    if (i < script.inherit_frames) in.connected = tail_connected_;
    items_.push_back({start + uint32_t(i), in});
  }
  if (!items_.empty() && !script.frames.empty()) tail_connected_ = items_.back().input.connected;
  for (const ScriptEvent& e : script.events) {
    events_.push_back({e.kind, start + e.frame});
  }
  std::stable_sort(events_.begin() + ptrdiff_t(next_event_), events_.end(),
                   [](const ScriptEvent& a, const ScriptEvent& b) { return a.frame < b.frame; });
  end_ = start + uint32_t(script.frames.size());
  return start;
}

FrameInput InputQueue::Next(uint32_t frame, std::vector<ScriptEvent::Kind>* fired) {
  while (next_event_ < events_.size() && events_[next_event_].frame <= frame) {
    fired->push_back(events_[next_event_].kind);
    ++next_event_;
  }
  while (next_item_ < items_.size() && items_[next_item_].frame < frame) ++next_item_;
  if (next_item_ < items_.size() && items_[next_item_].frame == frame) {
    const FrameInput in = items_[next_item_++].input;
    connected_ = in.connected;
    return in;
  }
  FrameInput neutral;
  neutral.connected = connected_;
  return neutral;
}

std::string EncodeRecHeader(uint64_t seed, uint32_t flags, std::string_view meta) {
  std::string s = "D4NPREC1";
  PutLe32(&s, 2);
  PutLe32(&s, uint32_t(kRecFrameBytes));
  PutLe64(&s, seed);
  PutLe32(&s, flags);
  PutLe32(&s, uint32_t(meta.size()));
  s.append(meta);
  return s;
}

std::string EncodeRecFrame(const FrameInput& input) {
  std::string s(kRecFrameBytes, '\0');
  s[0] = char(input.connected);
  for (int u = 0; u < kUsers; ++u) {
    PadToGuest(input.pad[u], reinterpret_cast<uint8_t*>(s.data()) + 4 + 12 * u);
  }
  return s;
}

bool DecodeRecording(std::string_view bytes, Recording* out, std::string* error, std::string* note) {
  *out = Recording{};
  if (note) note->clear();
  const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
  if (bytes.size() < kRecHeaderBytes || bytes.substr(0, 8) != "D4NPREC1") {
    *error = "not a DOA4 netplay recording";
    return false;
  }
  const uint32_t version = GetLe32(p + 8);
  if ((version != 1 && version != 2) || GetLe32(p + 12) != kRecFrameBytes) {
    *error = "unsupported recording version";
    return false;
  }
  out->seed = GetLe64(p + 16);
  out->flags = GetLe32(p + 24);
  size_t head = kRecHeaderBytes;
  if (version == 2) {
    const uint32_t meta = GetLe32(p + 28);
    if (bytes.size() < kRecHeaderBytes + size_t(meta)) {
      *error = "recording header cut short";
      return false;
    }
    out->meta.assign(bytes.substr(kRecHeaderBytes, meta));
    head += meta;
  }
  const size_t body = bytes.size() - head;
  const size_t n = body / kRecFrameBytes;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* f = p + head + i * kRecFrameBytes;
    FrameInput in;
    in.connected = f[0];
    for (int u = 0; u < kUsers; ++u) in.pad[u] = PadFromGuest(f + 4 + 12 * u);
    out->frames.push_back(in);
  }
  if (body % kRecFrameBytes && note) {
    *note = "dropped a partial last frame (" + std::to_string(body % kRecFrameBytes) + " bytes)";
  }
  return true;
}

namespace {
// The value of the line "<key>=<value>" in `meta`, or "(none)" when there is no such line.
std::string MetaValue(std::string_view meta, std::string_view key) {
  size_t at = 0;
  while (at < meta.size()) {
    size_t end = meta.find('\n', at);
    if (end == std::string_view::npos) end = meta.size();
    const std::string_view line = meta.substr(at, end - at);
    if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == '=') {
      return std::string(line.substr(key.size() + 1));
    }
    at = end + 1;
  }
  return "(none)";
}
}  // namespace

std::string MetaMismatch(std::string_view recorded, std::string_view current) {
  std::string out;
  for (std::string_view key : {"build", "data", "fixes", "serial", "sweep", "tweaks"}) {
    const std::string a = MetaValue(recorded, key), b = MetaValue(current, key);
    // (a value "unreadable" - the build or the game data could not be hashed - agrees with nothing)
    if (a != b || a == "unreadable") out += (out.empty() ? "" : "; ") + std::string(key) + " " + a + " vs " + b;
  }
  return out;
}

uint32_t ParseFixSet(const char* text) {
  if (!text || !*text || std::string_view(text) == "all") return ~0u;
  if (std::string_view(text) == "none") return 0;
  std::string_view s(text);
  uint32_t bits = 0;
  char sep = ',';
  bool minus = false;
  if (s.substr(0, 4) == "all-") {  // "all-x-y": everything but x and y
    bits = ~0u;
    s.remove_prefix(4);
    sep = '-';
    minus = true;
  }
  while (!s.empty()) {
    const size_t at = s.find(sep);
    const std::string_view item = s.substr(0, at);
    for (int i = 0; i < kFixCount; ++i) {
      if (item == kFixNames[i]) bits = minus ? (bits & ~(1u << i)) : (bits | (1u << i));
    }
    if (at == std::string_view::npos) break;
    s.remove_prefix(at + 1);
  }
  return bits;
}

std::string FixSetText(uint32_t bits) {
  std::string names;
  for (int i = 0; i < kFixCount; ++i) {
    if (bits & (1u << i)) names += std::string(names.empty() ? "" : ",") + kFixNames[i];
  }
  return names.empty() ? std::string("none") : names;
}

void MaskedCopy(const Region& region, const uint8_t* bytes, std::vector<uint8_t>* out) {
  out->assign(bytes, bytes + region.length);
  for (const Mask& m : region.masks) {
    if (m.offset >= region.length) continue;
    const uint32_t n = std::min(m.length, region.length - m.offset);
    std::memset(out->data() + m.offset, 0, n);
  }
}

std::string EncodeDigestHeader(const std::vector<Region>& regions) {
  std::string s = "D4NPDIG1";
  PutLe32(&s, 1);
  PutLe32(&s, uint32_t(regions.size()));
  for (const Region& r : regions) {
    PutLe32(&s, r.address);
    PutLe32(&s, r.length);
    char name[24] = {};
    std::memcpy(name, r.name, std::min(std::strlen(r.name), sizeof(name) - 1));
    s.append(name, sizeof(name));
  }
  return s;
}

std::string EncodeSweepHeader(uint32_t lo, uint32_t hi, uint32_t block, uint32_t per_frame) {
  std::string s = "D4NPSWP1";
  PutLe32(&s, 1);
  PutLe32(&s, lo);
  PutLe32(&s, hi);
  PutLe32(&s, block);
  PutLe32(&s, per_frame);
  PutLe32(&s, 0);
  return s;
}

uint32_t SweepFirst(uint32_t frame, uint32_t per_frame, uint32_t nblocks) {
  return nblocks ? uint32_t((uint64_t(frame) * per_frame) % nblocks) : 0;
}

std::vector<std::vector<Mask>> BlockMasks(uint32_t lo, uint32_t block, uint32_t nblocks,
                                          const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  std::vector<std::vector<Mask>> out(nblocks);
  const uint64_t hi = uint64_t(lo) + uint64_t(block) * nblocks;
  for (const auto& [address, length] : ranges) {
    uint64_t a = std::max<uint64_t>(address, lo);
    const uint64_t end = std::min<uint64_t>(uint64_t(address) + length, hi);
    while (a < end) {
      const uint32_t i = uint32_t((a - lo) / block);
      const uint64_t block_end = uint64_t(lo) + uint64_t(i + 1) * block;
      const uint64_t stop = std::min(end, block_end);
      out[i].push_back({uint32_t(a - (uint64_t(lo) + uint64_t(i) * block)), uint32_t(stop - a)});
      a = stop;
    }
  }
  return out;
}

std::vector<Region> BlockRegions(uint32_t lo, uint32_t block, uint32_t nblocks,
                                 const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  std::vector<std::vector<Mask>> masks = BlockMasks(lo, block, nblocks, ranges);
  std::vector<Region> out;
  out.reserve(nblocks);
  for (uint32_t k = 0; k < nblocks; ++k) out.push_back(Region{"", lo + k * block, block, std::move(masks[k])});
  return out;
}

namespace {
bool IsStashSignature(const uint8_t* p) { return std::memcmp(p, kStashSignature, 4) == 0; }
}  // namespace

bool HasStashWords(const uint8_t* bytes, size_t length, const uint8_t* before) {
  if (length < 4) return false;
  if (before && IsStashSignature(before)) return true;
  uint32_t sig, count = 0;
  std::memcpy(&sig, kStashSignature, 4);
  // Every word but the last (whose follower lies beyond `bytes`). No early exit and an index loop: a count the
  // compiler vectorises - 0.32 us for a 64 KB block against 4.9 us for a byte-offset OR loop (the sweep runs it on 4
  // blocks a frame; measured with -march=x86-64-v3 on a real dump, Task 11c).
  const size_t words = length / 4 - 1;
  for (size_t i = 0; i < words; ++i) {
    uint32_t w;
    std::memcpy(&w, bytes + 4 * i, 4);
    count += w == sig;
  }
  return count != 0;
}

uint32_t ZeroStashWords(uint8_t* bytes, size_t length, const uint8_t* before) {
  uint32_t n = 0;
  bool after_signature = before && IsStashSignature(before);
  for (size_t at = 0; at + 4 <= length; at += 4) {
    const bool signature = IsStashSignature(bytes + at);  // as the word was, before it may be zeroed below
    if (after_signature) {
      std::memset(bytes + at, 0, 4);
      ++n;
    }
    after_signature = signature;
  }
  return n;
}

uint32_t SweepBlockCopy(const Region& region, const uint8_t* bytes, const uint8_t* before, std::vector<uint8_t>* out) {
  out->assign(bytes, bytes + region.length);
  const uint32_t n = ZeroStashWords(out->data(), out->size(), before);
  for (const Mask& m : region.masks) {
    if (m.offset >= region.length) continue;
    std::memset(out->data() + m.offset, 0, std::min(m.length, region.length - m.offset));
  }
  return n;
}

std::string EncodeCountersHeader(const std::vector<std::string>& names) {
  std::string s = "D4NPCNT1";
  PutLe32(&s, 1);
  PutLe32(&s, uint32_t(names.size()));
  for (const std::string& n : names) {
    char name[24] = {};
    std::memcpy(name, n.data(), std::min(n.size(), sizeof(name) - 1));
    s.append(name, sizeof(name));
  }
  return s;
}

std::string EncodeCountersFrame(uint32_t frame, const std::vector<uint32_t>& values) {
  std::string s;
  PutLe32(&s, frame);
  PutLe32(&s, 0);
  for (uint32_t v : values) PutLe32(&s, v);
  return s;
}

namespace {
void StartFrame(ThreadClock* c, uint32_t frame) {
  if (c->frame != frame) {
    c->frame = frame;
    c->tb_reads = c->tick_reads = c->ft_reads = 0;
  }
}
}  // namespace

uint64_t VirtualTimebase(ThreadClock* c, uint32_t frame) {
  StartFrame(c, frame);
  const uint64_t v =
      kTimebaseBase + uint64_t(frame) * kTimebaseHz / 60 + uint64_t(++c->tb_reads) * (kTimebaseHz / 100000);
  c->last_tb = std::max(v, c->last_tb + 1);
  return c->last_tb;
}

uint32_t VirtualTickCount(ThreadClock* c, uint32_t frame) {
  StartFrame(c, frame);
  const uint32_t v = kTickBase + uint32_t(uint64_t(frame) * 1000 / 60) + (c->tick_reads++ >> 4);
  c->last_tick = std::max(v, c->last_tick);
  return c->last_tick;
}

uint64_t VirtualFileTime(ThreadClock* c, uint32_t frame) {
  StartFrame(c, frame);
  const uint64_t v = kFileTimeBase + uint64_t(frame) * 10000000 / 60 + uint64_t(++c->ft_reads) * 100;
  c->last_ft = std::max(v, c->last_ft + 1);
  return c->last_ft;
}

uint64_t AudioTimebase(uint64_t callback) { return kTimebaseBase + callback * 256 * kTimebaseHz / 48000; }
uint32_t AudioTickCount(uint64_t callback) { return kTickBase + uint32_t(callback * 256 * 1000 / 48000); }
uint64_t AudioFileTime(uint64_t callback) { return kFileTimeBase + callback * 256 * 10000000 / 48000; }

uint32_t MtSeed(uint64_t session_seed, uint32_t state, uint32_t k) {
  uint64_t x = session_seed ^ (uint64_t(state) << 32) ^ (uint64_t(k) * 0x9E3779B97F4A7C15ull);
  x += 0x9E3779B97F4A7C15ull;  // splitmix64 finaliser
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return uint32_t(x ^ (x >> 31));
}

uint32_t SeedSeries::Next(uint64_t session_seed, uint32_t state) {
  uint32_t k;
  {
    std::lock_guard lk(mu_);
    k = seeded_[state]++;
  }
  return MtSeed(session_seed, state, k);
}

void SeedSeries::Reset() {
  std::lock_guard lk(mu_);
  seeded_.clear();
}

uint32_t KeepAliveLr(KeepAliveFn fn) {
  // sub_8275BD18: "bl 0x8275ad70" at 0x8275BD98, "bl 0x8275aee0" at 0x8275BDA8 (the timeout branch)
  return fn == KeepAliveFn::kAd70 ? 0x8275BD9C : 0x8275BDAC;
}

KeepAliveCall ClassifyKeepAlive(KeepAliveFn fn, uint32_t thread_start, uint32_t lr) {
  if (thread_start != kD3dKickThreadStart) return KeepAliveCall::kPass;
  return lr == KeepAliveLr(fn) ? KeepAliveCall::kSkip : KeepAliveCall::kUnexpected;
}

ReaderWait ReaderOutcome(int32_t ret, bool found, bool ended) {
  if (ret != 1) return ReaderWait::kNoThread;
  return found && ended ? ReaderWait::kEnded : ReaderWait::kFallback;
}

}  // namespace doa4::np

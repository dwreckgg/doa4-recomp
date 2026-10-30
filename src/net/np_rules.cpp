// DOA4 netplay lobby rules (see np_rules.h for the contract). Pure: no SDK, no Windows, no sockets.
// Join codes and the release-channel helpers are ported from DOA2U (join_code.c and release_channel.c at
// doa2u-release 6c35c7b, which adapted parkerallan/doa3-recomp's join_code.c, MIT); the rest is new.

#include "np_rules.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace doa4::np::rules {
namespace {

constexpr size_t kNpos = std::string_view::npos;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// ---- join codes ----

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static_assert(kAlphabet.size() == 64);

// The packed value is shuffled before it becomes text, so a code does not show the address to whoever sees it (a
// stream, a screenshot): a 6-round Feistel network over the code's 36 or 48 bits with a fixed key. It hides the
// address from a glance or a base64 decoder, not from someone who reads this file. (DOA2U join_code.c.)
constexpr uint32_t kRoundKey[6] = {0x9E3779B9u, 0x7F4A7C15u, 0xF39CC060u, 0x5CEDC834u, 0xB5297A4Du, 0x68E31DA4u};

uint32_t RoundFn(uint32_t x, int r, uint32_t mask) {
  x ^= kRoundKey[r];
  x ^= x >> 16; x *= 0x7FEB352Du;
  x ^= x >> 15; x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x & mask;
}

uint64_t Scramble(uint64_t v, int bits, bool forward) {
  const int half = bits / 2;
  const uint32_t mask = (1u << half) - 1u;
  uint32_t l = uint32_t(v >> half) & mask, rt = uint32_t(v) & mask, t;
  if (forward) {
    for (int r = 0; r < 6; ++r) { t = rt; rt = l ^ RoundFn(rt, r, mask); l = t; }
  } else {
    for (int r = 5; r >= 0; --r) { t = l; l = rt ^ RoundFn(l, r, mask); rt = t; }
  }
  return (uint64_t(l) << half) | rt;
}

int SymbolIndex(char c) {
  const size_t at = kAlphabet.find(c);
  return at == kNpos ? -1 : int(at);
}

bool AllZero(const std::array<uint8_t, 4>& ip) { return (ip[0] | ip[1] | ip[2] | ip[3]) == 0; }

// Four decimal groups of 1-3 digits (each <= 255) separated by dots, from s[*pos]; *pos moves past the last digit.
bool ParseIpv4(std::string_view s, size_t* pos, std::array<uint8_t, 4>* ip) {
  size_t i = *pos;
  std::array<uint8_t, 4> b{};
  for (size_t k = 0; k < 4; ++k) {
    int v = 0, n = 0;
    while (i < s.size() && IsDigit(s[i]) && n < 3) { v = v * 10 + (s[i] - '0'); ++i; ++n; }
    if (n == 0 || v > 255) return false;
    b[k] = uint8_t(v);
    if (k < 3) {
      if (i >= s.size() || s[i] != '.') return false;
      ++i;
    }
  }
  *ip = b;
  *pos = i;
  return true;
}

// ---- STUN ----

constexpr size_t kStunHeader = 20;
constexpr unsigned kStunBindingSuccess = 0x0101;
constexpr unsigned kAttrMapped = 0x0001;
constexpr unsigned kAttrXorMapped = 0x0020;
constexpr uint8_t kStunCookie[4] = {0x21, 0x12, 0xA4, 0x42};

unsigned Be16(std::span<const uint8_t> d, size_t at) { return (unsigned(d[at]) << 8) | d[at + 1]; }

// ---- release list reader (DOA2U release_channel.c: a small reader for GitHub's JSON, over indices) ----

// Index of the closing quote of the string that opens at s[i], or kNpos.
size_t StrEnd(std::string_view s, size_t i) {
  for (++i; i < s.size(); ++i) {
    if (s[i] == '\\') ++i;  // an escape: the next character is not the end
    else if (s[i] == '"') return i;
  }
  return kNpos;
}

// Index of the bracket closing the object or array that opens at s[i], or kNpos. Iterative: nesting cannot overflow
// the stack.
size_t BlockEnd(std::string_view s, size_t i) {
  int depth = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') {
      i = StrEnd(s, i);
      if (i == kNpos) return kNpos;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      if (--depth == 0) return i;
    }
  }
  return kNpos;
}

// In the object s[obj..end] (the braces included), the index of the first character of the top-level member
// `key`'s value (a quote for a string), or kNpos.
size_t Member(std::string_view s, size_t obj, size_t end, std::string_view key) {
  size_t p = obj + 1;
  while (p < end) {
    while (p < end && s[p] != '"') ++p;  // the next key
    if (p >= end) return kNpos;
    const size_t k = p, ke = StrEnd(s, k);
    if (ke == kNpos || ke >= end) return kNpos;
    size_t v = ke + 1;
    while (v < end && (s[v] == ' ' || s[v] == '\t' || s[v] == '\r' || s[v] == '\n' || s[v] == ':')) ++v;
    if (v >= end) return kNpos;
    if (ke - k - 1 == key.size() && s.substr(k + 1, key.size()) == key) return v;
    if (s[v] == '"') {  // skip the value
      p = StrEnd(s, v);
    } else if (s[v] == '{' || s[v] == '[') {
      p = BlockEnd(s, v);
    } else {
      p = v;
      while (p < end && s[p] != ',' && s[p] != '}') ++p;
      continue;
    }
    if (p == kNpos) return kNpos;
    ++p;
  }
  return kNpos;
}

// The string value that opens at s[v], without its quotes; false for no value, no string, or one with escapes.
bool ReadString(std::string_view s, size_t v, std::string* out) {
  if (v == kNpos || v >= s.size() || s[v] != '"') return false;
  const size_t e = StrEnd(s, v);
  if (e == kNpos) return false;
  const std::string_view body = s.substr(v + 1, e - v - 1);
  if (body.find('\\') != kNpos) return false;
  out->assign(body);
  return true;
}

std::string Bare(std::string_view version) {  // "netplay-v1.0.14" -> "v1.0.14"
  const size_t d = version.rfind('-');
  return std::string(d == kNpos ? version : version.substr(d + 1));
}

// "name=value" lines of an np_files text (see DifferingGroups).
std::map<std::string, std::string> ParseGroups(std::string_view text) {
  std::map<std::string, std::string> groups;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    const std::string_view line = text.substr(pos, nl == kNpos ? kNpos : nl - pos);
    pos = nl == kNpos ? text.size() : nl + 1;
    if (line.empty()) continue;
    const size_t eq = line.find('=');
    if (eq == kNpos) groups[std::string(line)] = "";
    else groups[std::string(line.substr(0, eq))] = std::string(line.substr(eq + 1));
  }
  return groups;
}

}  // namespace

// =====================================================================================================
// Join codes
// =====================================================================================================

std::string JoinCodeTextOf(uint64_t packed, int symbols) {
  if (symbols != 6 && symbols != 8) return {};
  packed = Scramble(packed, symbols * 6, true);
  std::string out(size_t(symbols), ' ');
  for (int i = symbols - 1; i >= 0; --i) {
    out[size_t(i)] = kAlphabet[size_t(packed & 63)];
    packed >>= 6;
  }
  return out;
}

std::string MakeJoinCode(const Endpoint& ep) {
  if (ep.port == 0) return {};
  uint64_t v = 0;
  for (uint8_t b : ep.ip) v = (v << 8) | b;
  if (ep.port >= kPortBase && ep.port < kPortBase + kPorts) return JoinCodeTextOf((v << 4) | uint64_t(ep.port - kPortBase), 6);
  return JoinCodeTextOf((v << 16) | ep.port, 8);
}

bool ParseJoinCode(std::string_view text, Endpoint* out, uint16_t default_port) {
  if (!out) return false;
  size_t i = 0;
  while (i < text.size() && IsBlank(text[i])) ++i;
  {  // a dotted address, with or without :port
    std::array<uint8_t, 4> ip{};
    size_t e = i;
    if (ParseIpv4(text, &e, &ip)) {
      unsigned port = default_port;
      if (e < text.size() && text[e] == ':') {
        port = 0;
        ++e;
        if (e >= text.size() || !IsDigit(text[e])) return false;
        while (e < text.size() && IsDigit(text[e])) {
          port = port * 10 + unsigned(text[e] - '0');
          ++e;
          if (port > 65535) return false;
        }
      }
      while (e < text.size() && IsBlank(text[e])) ++e;
      if (e != text.size() || port == 0 || AllZero(ip)) return false;
      out->ip = ip;
      out->port = uint16_t(port);
      return true;
    }
  }
  char clean[8];
  size_t n = 0;
  for (; i < text.size(); ++i) {
    if (IsBlank(text[i])) continue;
    if (n >= 8) return false;
    clean[n++] = text[i];
  }
  if (n != 6 && n != 8) return false;
  uint64_t v = 0;
  for (size_t k = 0; k < n; ++k) {
    const int idx = SymbolIndex(clean[k]);
    if (idx < 0) return false;
    v = (v << 6) | uint64_t(idx);
  }
  v = Scramble(v, int(n) * 6, false);
  Endpoint ep;
  if (n == 6) {
    ep.port = uint16_t(kPortBase + unsigned(v & 15));
    v >>= 4;
  } else {
    ep.port = uint16_t(v & 0xFFFF);
    v >>= 16;
  }
  for (size_t k = 0; k < 4; ++k) ep.ip[k] = uint8_t(v >> (24 - 8 * k));
  if (ep.port == 0 || AllZero(ep.ip)) return false;
  *out = ep;
  return true;
}

std::string EndpointText(const Endpoint& ep) {
  return std::to_string(ep.ip[0]) + "." + std::to_string(ep.ip[1]) + "." + std::to_string(ep.ip[2]) + "." +
         std::to_string(ep.ip[3]) + ":" + std::to_string(ep.port);
}

// =====================================================================================================
// STUN and NAT class
// =====================================================================================================

std::array<uint8_t, 20> StunRequest(const std::array<uint8_t, 12>& transaction) {
  std::array<uint8_t, 20> r{0x00, 0x01, 0x00, 0x00, kStunCookie[0], kStunCookie[1], kStunCookie[2], kStunCookie[3]};
  std::copy(transaction.begin(), transaction.end(), r.begin() + 8);
  return r;
}

bool IsStun(std::span<const uint8_t> d) {
  return d.size() >= kStunHeader && d[4] == kStunCookie[0] && d[5] == kStunCookie[1] && d[6] == kStunCookie[2] &&
         d[7] == kStunCookie[3];
}

bool StunMapped(std::span<const uint8_t> r, const std::array<uint8_t, 12>& transaction, Endpoint* mapped) {
  if (!mapped || !IsStun(r)) return false;
  if (Be16(r, 0) != kStunBindingSuccess) return false;
  if (!std::equal(transaction.begin(), transaction.end(), r.begin() + 8)) return false;
  const size_t message_len = Be16(r, 2);
  if (message_len > r.size() - kStunHeader) return false;  // the message is cut
  const size_t end = kStunHeader + message_len;
  Endpoint xored, plain;
  bool have_xored = false, have_plain = false;
  size_t p = kStunHeader;
  while (p < end) {
    if (end - p < 4) return false;  // a stray attribute header
    const unsigned type = Be16(r, p);
    const size_t len = Be16(r, p + 2), v = p + 4;
    if (len > end - v) return false;  // the value passes the message's end
    if (type == kAttrXorMapped || type == kAttrMapped) {
      if (len < 4) return false;
      if (r[v + 1] == 0x01) {  // IPv4 (an IPv6 address is skipped)
        if (len < 8) return false;
        Endpoint e;
        unsigned port = Be16(r, v + 2);
        if (type == kAttrXorMapped) port ^= (unsigned(kStunCookie[0]) << 8) | kStunCookie[1];
        e.port = uint16_t(port);
        for (size_t k = 0; k < 4; ++k) e.ip[k] = uint8_t(r[v + 4 + k] ^ (type == kAttrXorMapped ? kStunCookie[k] : 0));
        if (type == kAttrXorMapped) {
          if (!have_xored) { xored = e; have_xored = true; }
        } else if (!have_plain) {
          plain = e;
          have_plain = true;
        }
      }
    }
    p = v + ((len + 3) & ~size_t(3));  // values are padded to a multiple of 4
  }
  if (have_xored) *mapped = xored;
  else if (have_plain) *mapped = plain;
  else return false;
  return true;
}

Nat ClassifyNat(std::span<const Endpoint> answers) {
  if (answers.empty()) return Nat::kNoAnswer;
  if (answers.size() == 1) return Nat::kOneAnswer;
  for (size_t i = 1; i < answers.size(); ++i)
    if (!(answers[i] == answers[0])) return Nat::kSymmetric;
  return Nat::kCone;
}

const char* NatNote(Nat nat) {
  switch (nat) {
    case Nat::kNoAnswer: return "Local network only (no answer from the STUN servers)";
    case Nat::kOneAnswer: return "Public address found (one STUN server answered)";
    case Nat::kCone: return "Public address found";
    case Nat::kSymmetric:
      return "Your network changes ports per destination (symmetric NAT): players outside it may not reach you";
  }
  return "";
}

std::string ConnectTimeoutText(Nat nat) {
  std::string text = "A direct connection is not possible between your networks; relay arrives with lobbies.";
  if (nat == Nat::kSymmetric) text += " (Your network changes ports per destination.)";
  return text;
}

// =====================================================================================================
// Input delay and pacing
// =====================================================================================================

int DelayFromRtt(int rtt_ms) {
  long long frames;
  if (rtt_ms < 0) frames = 10;  // retail: no QoS measurement -> 10
  else if (rtt_ms <= 33) frames = 2;
  else frames = (static_cast<long long>(rtt_ms) * 2997 + 199999) / 100000;
  return int(std::clamp(frames, 1LL, 8LL));
}

int PacerExtraUs(float frames_ahead) {
  if (!(frames_ahead >= 1.0f)) return 0;  // below 1.0 frame, negative, or NaN
  const double us = double(frames_ahead) * 16667.0 / 60.0;
  if (us >= 1000.0) return 1000;  // the cap (also keeps lround away from values it cannot hold)
  return int(std::lround(us));
}

// =====================================================================================================
// Release channel
// =====================================================================================================

bool ParseVersion(std::string_view tag, std::array<int, 3>* out) {
  if (tag.empty()) return false;
  const size_t dash = tag.rfind('-');  // "<channel>-v1.2.3"
  size_t p = dash == kNpos ? 0 : dash + 1;
  if (p < tag.size() && tag[p] == 'v') ++p;
  std::array<int, 3> v{};
  for (size_t i = 0; i < 3; ++i) {
    if (p >= tag.size() || !IsDigit(tag[p])) return false;
    int n = 0;
    while (p < tag.size() && IsDigit(tag[p])) {
      if (n > 99999999) return false;  // at most 9 digits
      n = n * 10 + (tag[p++] - '0');
    }
    v[i] = n;
    if (i < 2) {
      if (p >= tag.size() || tag[p] != '.') return false;
      ++p;
    }
  }
  if (p != tag.size()) return false;
  if (out) *out = v;
  return true;
}

int CompareVersions(std::string_view a, std::string_view b) {
  std::array<int, 3> va{}, vb{};
  const int pa = ParseVersion(a, &va) ? 1 : 0, pb = ParseVersion(b, &vb) ? 1 : 0;
  if (!pa || !pb) return pa - pb;  // an unparsable tag sorts lowest
  for (size_t i = 0; i < 3; ++i)
    if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
  return 0;
}

bool IsDevBuild(std::string_view tag) {
  std::array<int, 3> v{};
  return !ParseVersion(tag, &v) || v[2] == 0;
}

bool PickRelease(std::string_view json, std::string_view channel, std::span<const std::string_view> required,
                 ReleasePick* out) {
  if (!out || channel.empty()) return false;
  std::string prefix(channel);
  prefix += "-v";
  size_t p = 0;
  while (p < json.size() && IsBlank(json[p])) ++p;
  if (p >= json.size() || json[p] != '[') return false;  // an error object, not a list
  const size_t list_end = BlockEnd(json, p);
  if (list_end == kNpos) return false;                    // a cut transfer
  bool found = false;
  ReleasePick best;
  std::array<int, 3> best_version{};
  for (size_t i = p + 1; i < list_end; ++i) {
    if (json[i] != '{') continue;
    const size_t rel_end = BlockEnd(json, i);
    if (rel_end == kNpos) return false;
    const size_t release = i;
    i = rel_end;
    std::string tag;
    std::array<int, 3> version{};
    if (!ReadString(json, Member(json, release, rel_end, "tag_name"), &tag)) continue;
    if (tag.compare(0, prefix.size(), prefix) != 0 || !ParseVersion(tag, &version)) continue;
    const size_t draft = Member(json, release, rel_end, "draft");
    if (draft != kNpos && json.substr(draft, 4) == "true") continue;
    const size_t assets = Member(json, release, rel_end, "assets");
    if (assets == kNpos || json[assets] != '[') continue;
    const size_t assets_end = BlockEnd(json, assets);
    if (assets_end == kNpos) continue;
    std::map<std::string, std::string> urls;  // the required assets of this release
    for (size_t a = assets + 1; a < assets_end; ++a) {
      if (json[a] != '{') continue;
      const size_t asset_end = BlockEnd(json, a);
      if (asset_end == kNpos) break;
      std::string name, url;
      if (ReadString(json, Member(json, a, asset_end, "name"), &name) &&
          ReadString(json, Member(json, a, asset_end, "browser_download_url"), &url)) {
        for (std::string_view want : required)
          if (name == want) urls.emplace(name, url);
      }
      a = asset_end;
    }
    bool complete = true;
    for (std::string_view want : required) complete = complete && urls.count(std::string(want)) != 0;
    if (!complete) continue;
    // GitHub documents no order for this list: keep the highest version
    if (!found || std::lexicographical_compare(best_version.begin(), best_version.end(), version.begin(), version.end())) {
      best.tag = tag;
      best.asset_urls = std::move(urls);
      best_version = version;
      found = true;
    }
  }
  if (!found) return false;
  *out = std::move(best);
  return true;
}

std::string MismatchAdvice(std::string_view mine, std::string_view theirs) {
  if (theirs.empty())  // its own updater cannot fetch netplay releases
    return "The other player has an older game: they need the newest netplay release from the GitHub Releases page.";
  if (IsDevBuild(mine))
    return "You run a self-built exe: both of you should use the latest netplay release (press Check for Updates).";
  if (IsDevBuild(theirs))
    return "The other player runs a self-built exe: both of you should use the latest netplay release (Check for Updates).";
  const int c = CompareVersions(mine, theirs);
  if (c < 0)
    return "You have an older release (" + Bare(mine) + ", the other player " + Bare(theirs) +
           "): press Check for Updates, then restart the game.";
  if (c > 0)
    return "The other player has an older release (" + Bare(theirs) + ", you " + Bare(mine) +
           "): they need to press Check for Updates.";
  return "Both games say the same version (" + Bare(mine) +
         ") but the builds differ: both download it again from the GitHub Releases page.";
}

// =====================================================================================================
// Handshake verdict
// =====================================================================================================

std::vector<std::string> DifferingGroups(std::string_view files_a, std::string_view files_b) {
  const std::map<std::string, std::string> a = ParseGroups(files_a), b = ParseGroups(files_b);
  std::set<std::string> names;  // sorted
  for (const auto& kv : a) names.insert(kv.first);
  for (const auto& kv : b) names.insert(kv.first);
  std::vector<std::string> differing;
  for (const std::string& name : names) {
    const auto ia = a.find(name), ib = b.find(name);
    if (ia == a.end() || ib == b.end() || ia->second != ib->second || ia->second == "unreadable" ||
        ib->second == "unreadable")
      differing.push_back(name);
  }
  return differing;
}

Verdict Judge(const Identity& host, const Identity& joiner, bool host_busy) {
  Verdict v;
  if (host_busy) {
    v.refusal = wire::Refusal::kBusy;
    v.text = "The host is already in an online session.";
    return v;
  }
  if (host.build == 0 || joiner.build == 0) {
    v.refusal = wire::Refusal::kBuild;
    v.text = "This game's build could not be identified, so it cannot be matched against the other player's.";
    return v;
  }
  if (host.build != joiner.build) {
    v.refusal = wire::Refusal::kBuild;
    v.text = MismatchAdvice(host.release, joiner.release);
    return v;
  }
  const std::vector<std::string> groups = DifferingGroups(host.files, joiner.files);
  if (!groups.empty()) {
    v.refusal = wire::Refusal::kFiles;
    v.text = "The game files differ (";
    for (size_t i = 0; i < groups.size(); ++i) v.text += (i ? ", " : "") + groups[i];
    v.text += "): both players need the same game files.";
  }
  return v;
}

}  // namespace doa4::np::rules

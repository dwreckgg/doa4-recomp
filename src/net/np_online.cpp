// src/net/np_online.cpp - see np_online.h. Ported from DOA2U (doa2u-release 6c35c7b, doa2u/src/online/netplay.c):
// handle_control (1093-1291) -> HandlePacket, poll_network (1350-1432) -> Tick, on_connected (1061-1091), the session
// start (2100-2231) -> StartSessionNow / HostRestart / OnBootMessage / Answer. Changed from DOA2U: the lobby runs on the
// transport's I/O thread (DOA2U polls from the game loop); every packet but kGekko and Hello/Welcome/Punch is taken only
// from the peer with its random token (DOA2U checks the address only); the joiner composes its own build advice; the
// hand-off's peer is the address the socket saw; replies to strangers are bounded (count and size); a game of another
// protocol version gets an answer it can read (any version reads the header's proto byte); a stalled partial transfer
// is reset after 5 s; the session process (after the restart) meets its peer with the hand-off's token.
#include "np_online.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "../doa4_updater.h"  // its texts only: the updater itself is reached through Providers::updater
#include "np_upnp.h"

namespace doa4::np::online {

namespace fs = std::filesystem;

const char* StateName(State s) {
  switch (s) {
    case State::kOffline: return "offline";
    case State::kHosting: return "hosting";
    case State::kConnecting: return "connecting";
    case State::kConnected: return "connected";
    case State::kStarting: return "starting";
    case State::kRestarting: return "restarting";
    case State::kInSession: return "in_session";
  }
  return "?";
}

std::string Sanitize(std::string_view text, size_t max) {
  std::string out;
  out.reserve(std::min(text.size(), max));
  for (char c : text) {
    if (out.size() >= max) break;
    const unsigned char u = static_cast<unsigned char>(c);
    out.push_back(u >= 0x20 && u <= 0x7E ? c : '?');
  }
  return out;
}

std::string SaltFiles(const std::string& files, const std::string& group) {
  if (group.empty()) return files;
  std::string out;
  size_t pos = 0;
  while (pos < files.size()) {
    size_t end = files.find('\n', pos);
    const bool nl = end != std::string::npos;
    if (!nl) end = files.size();
    std::string line = files.substr(pos, end - pos);
    const size_t eq = line.find('=');
    if (eq != std::string::npos && line.compare(0, eq, group) == 0 && eq == group.size()) {
      const std::string v = line.substr(eq + 1);
      bool hex = v.size() == 16;
      uint64_t h = 0;
      for (char c : v) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) hex = false;
        h = h << 4 | uint64_t(d < 0 ? 0 : d);
      }
      if (hex) {
        char b[24];
        std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h + 1));
        line = group + "=" + b;
      }
    }
    out += line;
    if (nl) out += '\n';
    pos = end + (nl ? 1 : 0);
  }
  return out;
}

namespace {

using Clock = std::chrono::steady_clock;

// carry 2a (Task 2 review): reliable message ids come from one counter per process that is never reset, so a message
// of a new lobby is never mistaken for a duplicate of an earlier one by a joiner that is still running.
std::atomic<uint32_t> g_msg_id{0};
uint32_t NextMsgId() {
  uint32_t v;
  do v = ++g_msg_id;
  while (v == 0);
  return v;
}

uint64_t Random64() {
  static std::mutex mu;
  static std::random_device rd;
  std::lock_guard<std::mutex> lock(mu);
  return uint64_t(rd()) << 32 ^ uint64_t(rd());
}
uint32_t RandomToken() {
  uint32_t t;
  do t = uint32_t(Random64());
  while (t == 0);
  return t;
}

bool ParseU64(std::string_view s, uint64_t* out) {
  if (s.empty()) return false;
  int base = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s.remove_prefix(2);
  }
  uint64_t v = 0;
  for (char c : s) {
    int d = c >= '0' && c <= '9' ? c - '0' : base == 16 && c >= 'a' && c <= 'f' ? c - 'a' + 10 : base == 16 && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0 || d >= base) return false;
    if (v > (~0ull - uint64_t(d)) / uint64_t(base)) return false;
    v = v * uint64_t(base) + uint64_t(d);
  }
  *out = v;
  return true;
}

bool IsPrivate(const std::array<uint8_t, 4>& ip) {
  return ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xF0) == 16) || (ip[0] == 192 && ip[1] == 168);
}

// The first IPv4 address of an adapter that is up and not loopback (the LAN code when STUN has no answer).
std::optional<rules::Endpoint> LanAddress() {
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf(size);
  ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                 reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  if (r == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                             reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  }
  if (r != NO_ERROR) return std::nullopt;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
      rules::Endpoint e;
      std::memcpy(e.ip.data(), &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr, 4);
      if (e.ip[0] == 169 && e.ip[1] == 254) continue;  // link-local
      return e;
    }
  }
  return std::nullopt;
}

// Written to <path>.tmp, then renamed over <path>, so a reader never sees half a file. "" or why not.
std::string WriteAtomic(const fs::path& path, const void* data, size_t n) {
  std::error_code ec;
  if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return "cannot create " + tmp.filename().string();
    f.write(static_cast<const char*>(data), std::streamsize(n));
    f.close();
    if (!f) return "cannot write " + tmp.filename().string();
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return "cannot rename onto " + path.filename().string() + " (error " + std::to_string(GetLastError()) + ")";
  return "";
}

// The groups of this side's files text that are unreadable or missing, as the player reads it ("" when none).
std::string LocalFilesProblem(const std::string& files) {
  std::vector<std::string> unreadable, missing;
  size_t pos = 0;
  while (pos < files.size()) {
    size_t end = files.find('\n', pos);
    if (end == std::string::npos) end = files.size();
    const std::string line = files.substr(pos, end - pos);
    const size_t eq = line.find('=');
    if (eq != std::string::npos) {
      const std::string v = line.substr(eq + 1);
      if (v == "unreadable") unreadable.push_back(line.substr(0, eq));
      if (v == "missing") missing.push_back(line.substr(0, eq));
    }
    pos = end + 1;
  }
  auto join = [](const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + v[i];
    return s;
  };
  std::string out;
  if (!unreadable.empty()) out = "your game files: " + join(unreadable) + " cannot be read";
  if (!missing.empty())
    out += (out.empty() ? "" : "; ") + std::string("your game files: ") + join(missing) + (missing.size() == 1 ? " is missing" : " are missing");
  return out;
}

// Status::refusal / status.txt refusal= (Task 10's harness judges refusal kinds by it, never by the prose).
const char* KindName(wire::Refusal r) {
  switch (r) {
    case wire::Refusal::kNone: return "none";
    case wire::Refusal::kBuild: return "build";
    case wire::Refusal::kFiles: return "files";
    case wire::Refusal::kBusy: return "busy";
    case wire::Refusal::kProto: return "proto";
    case wire::Refusal::kSession: return "session";
  }
  return "other";
}

std::string ProtoAdvice(uint8_t theirs) {
  return "The other player's game speaks network protocol " + std::to_string(theirs) + ", yours " + std::to_string(wire::kProto) + ": " +
         (theirs > wire::kProto ? "you have the older game - press Check for Updates, then restart the game."
                                : "the other player has the older game - they need to press Check for Updates.");
}

enum class Cmd : uint8_t { kHost, kJoin, kReach, kDisconnect, kStart, kDelay, kLeave, kRestartFailed };
struct Command {
  Cmd cmd = Cmd::kHost;
  uint16_t port = 0;
  std::string text;
  bool flag = false;
  int n = 0;
  wire::Leave leave;
};

}  // namespace

// =====================================================================================================
// State
// =====================================================================================================
struct Lobby::Impl {
  Providers p;
  net::Config cfg;
  bool started = false;
  bool session = false;  // StartInSession
  Clock::time_point t0 = Clock::now();
  std::atomic<DWORD> io_tid{0};
  mutable std::mutex mu;  // everything below; the I/O thread holds it while it handles a packet or a tick
  std::deque<Command> cmds;
  std::vector<std::function<void()>> deferred;  // run on the I/O thread after the mutex is released

  // the files text (helper thread)
  std::thread files_thread;
  bool files_ready = false;
  std::string files_text;
  std::optional<rules::Endpoint> lan;

  State state = State::kOffline;
  bool is_host = false;
  std::string event, hint_override;
  std::string events_written;  // the last event appended to events.txt
  // the peer (lobby: after the Welcome; session: the hand-off's)
  bool has_peer = false;
  bool peer_confirmed = false;  // R2-I3: the peer answered a Ping (an authenticated Pong); until then it is tentative
  rules::Endpoint peer;
  uint32_t token = 0;
  std::string peer_release;
  uint32_t peer_last_rx = 0;
  // joiner
  rules::Endpoint target;
  uint32_t connect_t0 = 0, hello_sent = 0;
  bool hello_ever = false;
  bool punch_logged = false;
  // host
  bool punching = false;
  rules::Endpoint punch_to;
  uint32_t punch_sent = 0;
  bool punch_ever = false;
  bool upnp_started = false;
  uint32_t upnp_polled = 0;     // R2-I1: when upnp::Get() was last read
  uint32_t upnp_lost_seen = 0;  // the Result::lost already turned into an event
  // pings
  uint32_t ping_sent = 0;
  bool ping_ever = false;
  float rtt = 0;
  int rtt_n = 0;
  // delay
  bool delay_auto = true;
  int delay_manual = 2;
  // the lobby's own drops (control packets); the transport counts the rest
  std::array<uint32_t, size_t(wire::Drop::kCount)> drops{};
  // carry 2d: the times of the replies to strangers in the last second
  std::deque<uint32_t> replies;
  // host: the session start
  wire::ReliableSender tx;
  handoff::Handoff boot;
  bool booting = false;
  uint32_t boot_t0 = 0;
  // joiner: the session start
  wire::ReliableReceiver rx;
  bool rx_partial = false, have_completed = false;
  uint32_t rx_last = 0, last_completed_id = 0;
  bool reply_pending = false, reply_ever = false;
  wire::BootReply reply;
  uint32_t reply_first = 0, reply_last = 0;
  int reply_left = 0;  // a refusal is sent 3 times
  bool handoff_written = false;  // M-2: the joiner's hand-off is on disk and its restart has not fired yet
  bool boot_late = false;        // I-1b: the host's boot timed out but is still answerable (no newer start, same peer)
  uint64_t epoch = 0;            // bumped whenever the connection is left or replaced (a pending hand-off write checks it)
  bool session_refused = false;  // I-1a: the hand-off's peer answered our session Hello as a lobby
  std::string refusal = "none";  // Status::refusal: the latest refusal's kind (status.txt refusal=)
  std::map<int, uint32_t> stranger_events;  // I-3: per refusal kind, when its last event was shown
  // session
  handoff::Handoff sh;
  std::function<void(int32_t, wire::LeaveReason)> on_left;
  bool left_called = false;
  int leave_repeat = 0;
  uint64_t leave_sends = 0;  // Leaves sent so far (QuitSession waits on it; under mu)
  wire::Leave leave_out;
  uint32_t session_reply_sent = 0;
  bool session_reply_ever = false, wrong_source_logged = false;
  bool session_refusal_seen = false;  // R1-1: a kSession answer came from the peer; final after kSessionRefusalGraceMs
  uint32_t session_refusal_t0 = 0;
  // request files
  uint32_t ctl_last = 0, status_last = 0;
  bool status_dirty = true, status_ever = false;

  std::mutex stop_mu;  // M-6: Stop is serialised
  net::Transport t;  // last: destroyed first (its destructor stops the I/O thread before the state above goes)

  uint32_t Now() const { return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()); }
  // M-1: called with `mu` held; the line goes out once the mutex is released (RunDeferred), in order with the other
  // effects queued there (file writes, the restart). LogNow is for the few places that do not hold `mu`.
  void Log(const std::string& line) {
    if (!p.log) return;
    auto fn = p.log;
    deferred.push_back([fn, line] { fn(line); });
  }
  void LogNow(const std::string& line) const {
    if (p.log) p.log(line);
  }
  bool StrangerEvent(int kind, uint32_t now) {  // I-3: one event (and its log line) per refusal kind per 10 s
    const auto it = stranger_events.find(kind);
    if (it != stranger_events.end() && now - it->second < kStrangerEventMs) return false;
    stranger_events[kind] = now;
    return true;
  }
  std::string Test(const char* name) const {
    if (p.test_dir.empty()) return "";
    const auto it = p.test.find(name);
    return it == p.test.end() ? "" : it->second;
  }
  uint64_t Build() const {
    uint64_t b = p.build ? p.build() : 0;
    if (b != 0 && Test("DOA4_NP_TEST_BUILD_SALT") == "1") b += 1;
    return b;
  }
  int Delay() const {
    uint64_t v = 0;
    if (ParseU64(Test("DOA4_NP_DELAY"), &v) && v >= 1 && v <= 8) return int(v);
    if (!delay_auto) return delay_manual;
    return rules::DelayFromRtt(rtt_n ? int(std::lround(rtt)) : -1);
  }
  void Drop(wire::Drop why) { ++drops[size_t(why)]; }
  // Fix round I-1: Host and Join are refused while a check downloads and once one swapped files in this process (the
  // relaunch into a session would start the new exe, whose hand-off build check refuses the session). True = refused:
  // the event line says why, the lobby stays as it was.
  bool UpdateRefuses(const char* action) {
    const char* why = (p.updater.restart_pending && p.updater.restart_pending()) ? updater::kRestartText
                      : (p.updater.busy && p.updater.busy())                    ? updater::kBusyText
                                                                                : nullptr;
    if (!why) return false;
    Log(std::string("[doa4][NET] ") + action + " refused: " + why);
    SetEvent(why);
    return true;
  }
  void SetEvent(const std::string& text, const char* refusal_kind = "none");
  Status SnapshotLocked() const;
  void WriteStatus(uint32_t now);
  void ApplyRequests(const std::string& text, uint32_t now);
  void Apply(const Command& c, uint32_t now);
  void GoOffline();
  void DropPeer(const std::string& why);
  void SendTo(const rules::Endpoint& to, wire::Type type, uint32_t tok, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> d = wire::Encode(type, tok, body);
    if (!d.empty()) t.SendTo(to, std::move(d));
  }
  void SendPeer(wire::Type type, const std::vector<uint8_t>& body) { SendTo(peer, type, token, body); }
  bool ReplyWelcome(const rules::Endpoint& to, wire::Welcome w, size_t trigger_bytes, uint32_t now, bool stranger);
  wire::Hello OwnHello(uint32_t session_token) const;
  void Connected(const rules::Endpoint& from, uint32_t tok, const std::string& release, uint32_t now);

  void OnPacket(const rules::Endpoint& from, const wire::Packet& pk);
  void OnForeign(const rules::Endpoint& from, uint8_t proto, uint8_t type, size_t bytes);
  void ForeignLocked(const rules::Endpoint& from, uint8_t proto, uint8_t type, size_t bytes);
  void OnTick();
  void HandlePacket(const rules::Endpoint& from, const wire::Packet& pk, uint32_t now);
  void HandleHello(const rules::Endpoint& from, const wire::Packet& pk, uint32_t now);
  void HandleSessionHello(const rules::Endpoint& from, const wire::Packet& pk, uint32_t now);
  void HandleWelcome(const rules::Endpoint& from, const wire::Packet& pk, uint32_t now);
  void HandleSessionWelcome(const rules::Endpoint& from, const wire::Packet& pk);
  void Tick(uint32_t now);
  void StartSessionNow(uint32_t now);
  void HostRestart();
  void OnBootMessage(const std::vector<uint8_t>& msg, uint8_t type);
  void Answer(uint32_t tok, bool ok, const std::string& text);
  void PeerLeft(int32_t frame, wire::LeaveReason r);
  void RunDeferred();
  bool StartTransport(std::string* error);
};

void Lobby::Impl::SetEvent(const std::string& text, const char* refusal_kind) {
  event = text;
  refusal = refusal_kind;
  status_dirty = true;
  if (text == events_written) return;
  events_written = text;
  Log("[doa4][NET] event: " + text);
  if (p.events_path.empty()) return;
  const long long unix_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  const fs::path path = p.events_path;
  const std::string line = std::to_string(unix_ms) + ' ' + Sanitize(text, 1000) + '\n';
  deferred.push_back([path, line] {  // M-1: no disk write while `mu` is held
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (f) f << line;
  });
}

Status Lobby::Impl::SnapshotLocked() const {
  Status s;
  s.state = state;
  s.is_host = is_host;
  s.event = event;
  s.peer_release = peer_release;
  s.ping_ms = rtt;
  s.ping_samples = rtt_n;
  s.delay = Delay();
  s.delay_auto = delay_auto;
  s.port = t.port();
  const net::Stats ts = t.stats();
  s.nat = ts.nat;
  s.code_note = rules::NatNote(ts.nat);
  if (ts.mapped) {
    s.my_code = rules::MakeJoinCode(*ts.mapped);
  } else if (cfg.loopback) {
    rules::Endpoint e;
    e.ip = {127, 0, 0, 1};
    e.port = s.port;
    s.my_code = rules::MakeJoinCode(e);
  } else if (lan) {
    rules::Endpoint e = *lan;
    e.port = s.port;
    s.my_code = rules::MakeJoinCode(e);
  }
  s.upnp = cfg.upnp ? (upnp_started ? upnp::Get().message : "UPnP: not asked yet") : "UPnP off";
  s.session_refused = session_refused;
  s.refusal = refusal;
  for (size_t r = 0; r < s.drops.size(); ++r) {
    s.drops[r] = ts.dropped[r] + drops[r];
    s.dropped += s.drops[r];
  }
  s.gekko_early = ts.gekko_early;
  s.peer_port = has_peer && (state == State::kConnected || state == State::kStarting || state == State::kRestarting ||
                             state == State::kInSession)
                    ? peer.port
                    : 0;
  s.peer_address = s.peer_port ? rules::EndpointText(peer) : "";  // R2-I3: the Connected line names the peer
  s.can_start = is_host && state == State::kConnected && rtt_n >= 4;
  switch (state) {
    case State::kOffline: s.hint = ""; break;
    case State::kHosting: s.hint = rules::HostWaitingHint(s.port); break;  // R2-I2: the likely causes if nobody gets in
    case State::kConnecting: s.hint = session ? "Waiting for the other player's game to come back." : "Waiting for the host to answer."; break;
    case State::kConnected:
      s.hint = is_host ? (s.can_start ? "Press Start online session (both games restart)." : "Measuring the ping...")
                       : "The host starts the online session; both games then restart.";
      break;
    case State::kStarting: s.hint = "Starting the online session: both games restart."; break;
    case State::kRestarting: s.hint = "Restarting into the online session..."; break;
    case State::kInSession: s.hint = ""; break;
  }
  return s;
}

// status.txt's nat= value (Task 15 close-out (b), the harness's S11 rule): the NAT class the transport's STUN found.
static const char* NatKey(rules::Nat nat) {
  switch (nat) {
    case rules::Nat::kNoAnswer: return "no_answer";
    case rules::Nat::kOneAnswer: return "one_answer";
    case rules::Nat::kCone: return "cone";
    case rules::Nat::kSymmetric: return "symmetric";
  }
  return "other";
}

void Lobby::Impl::WriteStatus(uint32_t now) {
  const Status s = SnapshotLocked();
  char ping[32];
  std::snprintf(ping, sizeof ping, "%.1f", double(s.ping_ms));
  const std::string text = "state=" + std::string(StateName(s.state)) + "\nport=" + std::to_string(s.port) +
                           "\npeer_port=" + std::to_string(s.peer_port) + "\nping=" + ping + "\ndelay=" + std::to_string(s.delay) +
                           "\ncan_start=" + (s.can_start ? "1" : "0") + "\nevent=" + Sanitize(s.event, 1000) +
                           "\ncode=" + Sanitize(s.my_code, 64) + "\nnat=" + NatKey(s.nat) +  // Task 15 close-out (b)
                           "\nupdate=" + Sanitize(p.updater.message ? p.updater.message() : "", 1000) +  // plan 1b Task 4
                           "\nrefusal=" + s.refusal + "\n";
  status_dirty = false;
  status_last = now;
  status_ever = true;
  // M-1: written once `mu` is released. A reader holding status.txt open can make the rename fail: it is marked dirty
  // again and retried at the next tick.
  const fs::path path = p.test_dir / "status.txt";
  deferred.push_back([this, path, text] {
    if (WriteAtomic(path, text.data(), text.size()).empty()) return;
    std::lock_guard<std::mutex> lock(mu);
    status_dirty = true;
  });
}

// The request file, taken (renamed, read, deleted) by the I/O thread without `mu` (M-1). False when there is none.
static bool TakeCtl(const fs::path& dir, std::string* text) {
  const fs::path on = dir / "np_ctl.on", taking = dir / "np_ctl.taking";
  if (!MoveFileExW(on.c_str(), taking.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
  {
    std::ifstream f(taking, std::ios::binary);
    text->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  std::error_code ec;
  fs::remove(taking, ec);
  return true;
}

void Lobby::Impl::ApplyRequests(const std::string& text, uint32_t now) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    const size_t sp = line.find(' ');
    const std::string verb = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
    Command c;
    uint64_t v = 0;
    bool ok = true;
    if (verb == "host" && (arg.empty() || (ParseU64(arg, &v) && v <= 65535))) {
      c.cmd = Cmd::kHost;
      c.port = uint16_t(v);
    } else if (verb == "join" && !arg.empty()) {
      c.cmd = Cmd::kJoin;
      c.text = arg;
    } else if (verb == "reach" && !arg.empty()) {
      c.cmd = Cmd::kReach;
      c.text = arg;
    } else if (verb == "start" && arg.empty()) {
      c.cmd = Cmd::kStart;
    } else if (verb == "disconnect" && arg.empty()) {
      c.cmd = Cmd::kDisconnect;
    } else if (verb == "delay" && arg == "auto") {
      c.cmd = Cmd::kDelay;
      c.flag = true;
    } else if (verb == "delay" && ParseU64(arg, &v) && v <= 100) {
      c.cmd = Cmd::kDelay;
      c.flag = false;
      c.n = int(v);
    } else if (verb == "status" && arg.empty()) {
      WriteStatus(now);
      continue;
    } else if (verb == "update" && (arg.empty() || arg == "confirm")) {
      // Plan 1b Task 4: the Online tab's Check for Updates, under the same rule as its button (online_view: only when
      // the lobby is Offline and this is no session process); "update confirm" is the second press that installs over a
      // self-built game (fix round I-2). The updater's start logs nothing on this thread (M-1).
      if (!p.updater.start) {
        Log("[doa4][NET] update: this game has no updater");
      } else if (session || state != State::kOffline) {
        if (p.updater.refuse) p.updater.refuse(updater::kOnlineText);
        Log(std::string("[doa4][NET] update refused: ") + (session ? "a session process" : StateName(state)));
      } else if (p.updater.start(arg == "confirm")) {
        Log("[doa4][NET] update: Check for Updates started (np_ctl.on)");
      } else {
        Log("[doa4][NET] update: a check is already running");
      }
      WriteStatus(now);
      continue;
    } else {
      ok = false;
    }
    if (!ok) {
      Log("[doa4][NET] unknown request '" + Sanitize(line) + "' in np_ctl.on (ignored)");
      continue;
    }
    Apply(c, now);
    WriteStatus(now);
  }
}

void Lobby::Impl::GoOffline() {
  state = State::kOffline;
  is_host = false;
  has_peer = false;
  peer_confirmed = false;
  token = 0;
  peer_release.clear();
  punching = false;
  booting = false;
  tx.Reset();
  rx.Reset();
  rx_partial = false;
  reply_pending = false;
  rtt = 0;
  rtt_n = 0;
  ping_ever = false;
  status_dirty = true;
  ++epoch;
  boot_late = false;
  if (handoff_written) {  // M-2: the session start is cancelled before the restart fired: the hand-off goes too
    handoff_written = false;
    const fs::path path = p.handoff_path;
    deferred.push_back([path] {
      std::error_code ec;
      fs::remove(path, ec);
    });
    Log("[doa4][NET] the session start was cancelled before the restart: hand-off withdrawn");
  }
}

// The peer went away (Bye or silence): the host keeps hosting, the joiner is Offline.
void Lobby::Impl::DropPeer(const std::string& why) {
  const bool host = is_host;
  GoOffline();
  if (host) {
    is_host = true;
    state = State::kHosting;
  }
  SetEvent(why);
}

wire::Hello Lobby::Impl::OwnHello(uint32_t session_token) const {
  wire::Hello h;
  h.build = Build();
  h.release = Sanitize(p.release);
  h.files = session ? "" : files_text;
  h.delay_auto = delay_auto ? 1 : 0;
  h.delay_frames = uint8_t(std::clamp(delay_manual, 1, 8));
  h.session_token = session_token;
  return h;
}

// carry 2d: a reply to a datagram from anyone not authenticated - one per datagram (the caller), never larger than it
// (the text is shortened; nothing is sent when even an empty text does not fit), at most 10 in any 1,000 ms. M-4: the
// Welcome to the joiner being accepted, or resent to the known peer, keeps the size rule but not the stranger budget,
// so a flood of junk Hellos cannot keep the real joiner out.
bool Lobby::Impl::ReplyWelcome(const rules::Endpoint& to, wire::Welcome w, size_t trigger_bytes, uint32_t now, bool stranger) {
  w.release = Sanitize(w.release);
  w.text = Sanitize(w.text);
  const size_t fixed = wire::kHeaderBytes + 1 + 8 + 2 + w.release.size() + 2 + 4;
  if (fixed > trigger_bytes) return false;
  if (w.text.size() > trigger_bytes - fixed) w.text.resize(trigger_bytes - fixed);
  if (stranger) {
    while (!replies.empty() && now - replies.front() >= 1000) replies.pop_front();
    if (replies.size() >= size_t(kUnauthRepliesPerSecond)) return false;
    replies.push_back(now);
  }
  SendTo(to, wire::Type::kWelcome, 0, wire::EncodeWelcome(w));
  return true;
}

void Lobby::Impl::Connected(const rules::Endpoint& from, uint32_t tok, const std::string& release, uint32_t now) {
  has_peer = true;
  peer_confirmed = false;  // R2-I3: tentative until its first authenticated Pong
  peer = from;  // carry 2c: the address the socket saw, never the code typed
  token = tok;
  peer_release = Sanitize(release);
  peer_last_rx = now;
  rtt = 0;
  rtt_n = 0;
  ping_ever = false;
  punching = false;
  booting = false;
  tx.Reset();
  rx.Reset();
  rx_partial = false;
  have_completed = false;
  reply_pending = false;
  boot_late = false;
  ++epoch;
  state = State::kConnected;
  SetEvent("Connected to the other player.");
}

// =====================================================================================================
// Packets (I/O thread)
// =====================================================================================================
void Lobby::Impl::OnPacket(const rules::Endpoint& from, const wire::Packet& pk) {
  io_tid = GetCurrentThreadId();
  {
    std::lock_guard<std::mutex> lock(mu);
    HandlePacket(from, pk, Now());
  }
  RunDeferred();
}

void Lobby::Impl::OnForeign(const rules::Endpoint& from, uint8_t proto, uint8_t type, size_t bytes) {
  io_tid = GetCurrentThreadId();
  {
    std::lock_guard<std::mutex> lock(mu);
    ForeignLocked(from, proto, type, bytes);
  }
  RunDeferred();
}

void Lobby::Impl::ForeignLocked(const rules::Endpoint& from, uint8_t proto, uint8_t type, size_t bytes) {
  if (session) return;
  const uint32_t now = Now();
  if (is_host && state != State::kOffline && type == uint8_t(wire::Type::kHello)) {
    wire::Welcome w;
    w.refusal = wire::Refusal::kProto;
    w.build = Build();
    w.release = p.release;
    w.text = ProtoAdvice(proto);
    ReplyWelcome(from, w, bytes, now, true);
    refusal = "proto";  // the kind is kept even when the event line is not changed (I-3)
    status_dirty = true;
    if (state == State::kHosting && StrangerEvent(1000, now))
      SetEvent("Refused a player whose game speaks network protocol " + std::to_string(proto) + " (yours " + std::to_string(wire::kProto) + ").",
               "proto");
  } else if (!is_host && state == State::kConnecting && from == target) {
    GoOffline();
    SetEvent(ProtoAdvice(proto), "proto");
  }
}

void Lobby::Impl::HandlePacket(const rules::Endpoint& from, const wire::Packet& pk, uint32_t now) {
  switch (pk.type) {
    case wire::Type::kHello:
      if (session) HandleSessionHello(from, pk, now);
      else HandleHello(from, pk, now);
      return;
    case wire::Type::kWelcome:
      if (!session) HandleWelcome(from, pk, now);
      else HandleSessionWelcome(from, pk);
      return;
    case wire::Type::kPunch:  // the host reaching out (strict routers): no reply to a stranger; a Hello sooner
      if (!punch_logged) {
        punch_logged = true;
        Log("[doa4][NET] punch received (a host reaching out)");
      }
      if (!session && !is_host && state == State::kConnecting && from == target && hello_ever && now - hello_sent >= 100)
        hello_ever = false;  // the next tick sends the Hello at once
      return;
    case wire::Type::kGekko:
      return;  // the transport's
    default:
      break;
  }
  // ---- the rest: only from the peer, with its token ----
  if (!has_peer || !(from == peer)) {
    Drop(wire::Drop::kSource);
    return;
  }
  if (pk.token != token) {
    Drop(wire::Drop::kToken);
    return;
  }
  peer_last_rx = now;
  switch (pk.type) {
    case wire::Type::kPing: {
      wire::Ping pg;
      if (!wire::DecodePing(pk.body, &pg)) {
        Drop(wire::Drop::kBody);
        return;
      }
      SendPeer(wire::Type::kPong, wire::EncodePing(pg));
      return;
    }
    case wire::Type::kPong: {
      wire::Ping pg;
      if (!wire::DecodePing(pk.body, &pg)) {
        Drop(wire::Drop::kBody);
        return;
      }
      const uint32_t r = now - pg.t_ms;
      if (r > 60000) return;  // not one of ours
      rtt = rtt_n ? rtt * 0.8f + float(r) * 0.2f : float(r);
      ++rtt_n;
      peer_confirmed = true;  // R2-I3: the peer answers from the address it holds, with its token
      status_dirty = true;
      return;
    }
    case wire::Type::kChunk: {
      if (session || is_host || (state != State::kConnected && state != State::kRestarting)) return;
      wire::Chunk c;
      if (!wire::DecodeChunk(pk.body, &c)) {
        Drop(wire::Drop::kBody);
        return;
      }
      wire::Ack ack;
      const wire::ReliableReceiver::Result r = rx.OnChunk(c, &ack);
      if (r == wire::ReliableReceiver::Result::kIgnored) return;
      SendPeer(wire::Type::kAck, wire::EncodeAck(ack));
      rx_last = now;
      if (r == wire::ReliableReceiver::Result::kAck) {
        if (!(have_completed && c.msg_id == last_completed_id)) rx_partial = true;
        return;
      }
      rx_partial = false;
      have_completed = true;
      last_completed_id = c.msg_id;
      uint8_t type = 0;
      const std::vector<uint8_t> msg = rx.Take(&type);
      if (state == State::kConnected) OnBootMessage(msg, type);
      return;
    }
    case wire::Type::kAck: {
      if (!is_host || !booting) return;
      wire::Ack a;
      if (!wire::DecodeAck(pk.body, &a)) {
        Drop(wire::Drop::kBody);
        return;
      }
      tx.OnAck(a);  // the acks complete the transfer; only the BootReply decides
      return;
    }
    case wire::Type::kBootReply: {
      // I-1b: in kStarting, or Connected after the 15 s timeout while that boot is still the current one
      if (!is_host || !(state == State::kStarting || (state == State::kConnected && boot_late))) return;
      wire::BootReply br;
      if (!wire::DecodeBootReply(pk.body, &br)) {
        Drop(wire::Drop::kBody);
        return;
      }
      const bool ok = br.ok == 1;  // carry 2a: any other value is not a yes
      if (!(br.session_token == boot.session_token || (!ok && br.session_token == 0))) return;
      if (ok) {
        if (state == State::kConnected) Log("[doa4][NET] a late yes to the session start (after the 15 s): restarting");
        HostRestart();
        return;
      }
      if (state != State::kStarting) return;  // a late refusal changes nothing
      booting = false;
      tx.Reset();
      state = State::kConnected;
      SetEvent("The other player's game could not take the session: " + Sanitize(br.text.empty() ? "no reason given" : br.text));
      return;
    }
    case wire::Type::kLeave: {
      wire::Leave l;
      if (!wire::DecodeLeave(pk.body, &l)) {
        Drop(wire::Drop::kBody);
        return;
      }
      if (session) PeerLeft(l.frame < 0 ? -1 : l.frame, l.reason);  // carry 2a: a negative frame means "none"
      return;
    }
    case wire::Type::kBye:
      if (session) {
        PeerLeft(-1, wire::LeaveReason::kLeft);
      } else if (state == State::kConnected || state == State::kStarting) {
        DropPeer("The other player disconnected.");
      }
      return;
    default:
      return;
  }
}

void Lobby::Impl::PeerLeft(int32_t frame, wire::LeaveReason r) {
  if (left_called) return;
  left_called = true;
  Log("[doa4][NET] the other player left the session (frame " + std::to_string(frame) + ", reason " + std::to_string(int(r)) + ")");
  if (on_left) {
    auto fn = on_left;
    deferred.push_back([fn, frame, r] { fn(frame, r); });
  }
}

void Lobby::Impl::HandleHello(const rules::Endpoint& from, const wire::Packet& pk, uint32_t now) {
  const bool hosting = is_host && state != State::kOffline && state != State::kConnecting;
  wire::Hello hl;
  if (!wire::DecodeHello(pk.body, &hl)) {
    if (hosting) Drop(wire::Drop::kBody);
    return;
  }
  if (is_host && has_peer && from == peer && hl.session_token != 0 && hl.session_token == boot.session_token &&
      pk.token == boot.session_token && (state == State::kStarting || (state == State::kConnected && boot_late))) {
    // Task 12 carry 2h (the Task 9 suggestion): a session Hello carrying the CURRENT boot's token from our peer proves
    // the joiner took the hand-off and restarted (its BootReply was lost): a late yes. Restart into the session with
    // the same hand-off instead of answering kSession. Any other token keeps the kSession answer below.
    Log("[doa4][NET] the other player's game is already in the session (its session Hello carries this boot's token): "
        "taken as a late yes, restarting");
    HostRestart();
    return;
  }
  if (pk.token != 0 || hl.session_token != 0) {
    Drop(wire::Drop::kToken);
    // Final review R1-1 / R2-M4: a lobby starting a session, or restarting into one (either role), stays silent - it is
    // about to become that session's peer (its own session process answers soon), and a kSession answer would end the
    // other side's session at its start.
    if (state == State::kStarting || state == State::kRestarting || booting) return;
    // I-1a: a session process's Hello (the other game restarted into a session this one never started): say so, so it
    // gives up (after its grace) instead of waiting for np_live's frame-0 limit. A stranger reply like any other.
    wire::Welcome w;
    w.refusal = wire::Refusal::kSession;
    w.build = Build();  // no release: a kSession answer must fit any Hello (2d size rule)
    w.text = "That game is not in the online session.";
    ReplyWelcome(from, w, pk.body.size() + wire::kHeaderBytes, now, true);
    refusal = "session";
    status_dirty = true;
    return;
  }
  if (!hosting) return;
  wire::Welcome w;
  w.build = Build();
  w.release = p.release;
  if (has_peer && from == peer) {  // the joiner did not get the Welcome: the same one again
    if (state != State::kConnected) return;
    w.session_token = token;
    ReplyWelcome(from, w, pk.body.size() + wire::kHeaderBytes, now, false);
    return;
  }
  if (!files_ready) return;  // the joiner repeats its Hello every 400 ms
  // R2-I3: a tentative peer (Connected, no authenticated Pong yet) does not hold the slot against a newer valid Hello
  const bool tentative = has_peer && !peer_confirmed && state == State::kConnected;
  const rules::Identity host_id{w.build, p.release, files_text}, joiner_id{hl.build, hl.release, hl.files};
  const rules::Verdict v = rules::Judge(host_id, joiner_id, has_peer && !tentative);
  const size_t trigger = pk.body.size() + wire::kHeaderBytes;
  if (v.refusal == wire::Refusal::kNone) {
    if (tentative)
      Log("[doa4][NET] a valid Hello from " + rules::EndpointText(from) + " replaces the tentative peer " + rules::EndpointText(peer) +
          " (it never answered a ping)");
    Connected(from, RandomToken(), hl.release, now);
    w.session_token = token;
    Log("[doa4][NET] joiner connected: release " + peer_release + ", delay preference " +
        (hl.delay_auto ? std::string("auto") : std::to_string(std::clamp(int(hl.delay_frames), 1, 8))));  // carry 2a: clamped
    ReplyWelcome(from, w, trigger, now, false);
    return;
  }
  w.refusal = v.refusal;
  w.text = v.text;
  ReplyWelcome(from, w, trigger, now, true);
  const char* kind = KindName(v.refusal);
  refusal = kind;  // kept even when the event line is not changed (busy; I-3)
  status_dirty = true;
  if (!StrangerEvent(int(v.refusal), now)) return;  // I-3: a stranger cannot flood the event line, game.log or events.txt
  switch (v.refusal) {
    case wire::Refusal::kBusy: break;  // the pair's own event stays
    case wire::Refusal::kBuild: SetEvent("Refused a player with a different build. " + Sanitize(v.text), kind); break;
    case wire::Refusal::kFiles: {
      const std::string local = LocalFilesProblem(files_text);
      SetEvent("Refused a player: " + (local.empty() ? Sanitize(v.text) : local + "."), kind);
      break;
    }
    default: SetEvent("Refused a player: " + Sanitize(v.text), kind); break;
  }
}

void Lobby::Impl::HandleWelcome(const rules::Endpoint& from, const wire::Packet& pk, uint32_t now) {
  if (is_host || state != State::kConnecting) return;
  const bool lan_answer = cfg.lan_broadcast && !cfg.loopback && IsPrivate(from.ip) && from.port >= rules::kPortBase &&
                          from.port < rules::kPortBase + rules::kPorts;  // the answer to our LAN call (DOA2U)
  if (!(from == target) && !lan_answer) {
    Drop(wire::Drop::kSource);
    return;
  }
  wire::Welcome w;
  if (!wire::DecodeWelcome(pk.body, &w)) {
    Drop(wire::Drop::kBody);
    return;
  }
  if (w.refusal == wire::Refusal::kNone) {
    if (w.session_token == 0) {
      Drop(wire::Drop::kBody);
      return;
    }
    Connected(from, w.session_token, w.release, now);
    return;
  }
  std::string text;
  switch (w.refusal) {
    case wire::Refusal::kBuild: {  // carry 2b: the joiner's own advice, never the host-seat text
      const uint64_t own = Build();
      if (own == 0) text = "Your game could not read its own build - reinstall it from the newest release.";
      else if (w.build == 0) text = "The host's game could not read its own build - they need to reinstall it from the newest release.";
      else text = rules::MismatchAdvice(Sanitize(p.release), Sanitize(w.release));
      break;
    }
    case wire::Refusal::kFiles: {  // carry 2g: this side's own unreadable or missing files are named as such
      const std::string local = LocalFilesProblem(files_text);
      text = local.empty() ? (w.text.empty() ? "The host's game files differ from yours." : Sanitize(w.text))
                           : "Could not join: " + local + " (check your game folder).";
      break;
    }
    case wire::Refusal::kProto: text = "The host's game speaks another network protocol: update both games to the newest release."; break;
    default: text = w.text.empty() ? "The host refused the connection." : Sanitize(w.text); break;
  }
  GoOffline();
  SetEvent(text, KindName(w.refusal));
}

void Lobby::Impl::HandleSessionHello(const rules::Endpoint& from, const wire::Packet& pk, uint32_t now) {
  wire::Hello hl;
  if (!wire::DecodeHello(pk.body, &hl)) {
    Drop(wire::Drop::kBody);
    return;
  }
  if (pk.token != sh.session_token || hl.session_token != sh.session_token) {  // another session (or a lobby)
    Drop(wire::Drop::kToken);
    wire::Welcome w;
    w.refusal = wire::Refusal::kSession;
    w.build = Build();  // no release: a kSession answer must fit any Hello (2d size rule)
    w.text = "That game is in another online session.";
    ReplyWelcome(from, w, pk.body.size() + wire::kHeaderBytes, now, true);
    return;
  }
  if (!(from == sh.peer)) {  // carry 2c: the peer is the address the lobby's socket saw (the hand-off's), only
    Drop(wire::Drop::kSource);
    if (!wrong_source_logged) {
      wrong_source_logged = true;
      Log("[doa4][NET] a Hello with this session's token came from another address than the hand-off's peer (" +
          rules::EndpointText(from) + ", expected " + rules::EndpointText(sh.peer) + "): ignored");
    }
    return;
  }
  peer_last_rx = now;
  if (state != State::kInSession) {
    state = State::kInSession;
    t.SetPeer(sh.peer, sh.session_token);
    // carry 2g: the source of the first session Hello next to the hand-off's peer (NAT rebinding evidence, plan 1b)
    Log("[doa4][NET] the other player's game is back: in session (first session Hello from " + rules::EndpointText(from) +
        ", hand-off peer " + rules::EndpointText(sh.peer) + ")");
    SetEvent("The other player's game is back: the session runs.");
  }
  if (!session_reply_ever || now - session_reply_sent >= 100) {  // so it knows we are here too
    session_reply_ever = true;
    session_reply_sent = now;
    SendTo(sh.peer, wire::Type::kHello, sh.session_token, wire::EncodeHello(OwnHello(sh.session_token)));
  }
}

// I-1a: the session process's Hello reached a lobby (the other game did not restart into the session): give up now and
// say so through Status::session_refused, so the app (Task 12) ends the session and restarts offline.
void Lobby::Impl::HandleSessionWelcome(const rules::Endpoint& from, const wire::Packet& pk) {
  if (!(from == sh.peer)) {
    Drop(wire::Drop::kSource);
    return;
  }
  wire::Welcome w;
  if (!wire::DecodeWelcome(pk.body, &w)) {
    Drop(wire::Drop::kBody);
    return;
  }
  if (w.refusal != wire::Refusal::kSession || state != State::kConnecting) return;
  // R1-1: final only after the grace (Tick); the Hello keeps going meanwhile, so a game still on its way into the session
  // (whose old lobby answered) is met when it comes back
  if (session_refusal_seen) return;
  session_refusal_seen = true;
  session_refusal_t0 = Now();
  Log("[doa4][NET] the hand-off's peer answered as a lobby: waiting " + std::to_string(kSessionRefusalGraceMs / 1000) +
      " s for its game to come back into the session");
}

// =====================================================================================================
// Session start
// =====================================================================================================
void Lobby::Impl::StartSessionNow(uint32_t now) {
  boot_late = false;  // I-1b: a newer start ends the window for a late yes to the old one
  if (!is_host || state != State::kConnected) {
    SetEvent("Connect to the other player first.");
    return;
  }
  if (rtt_n < 4) {
    SetEvent("Wait a moment: the ping is still being measured.");
    return;
  }
  std::string err;
  std::vector<handoff::File> files = p.save_files ? p.save_files(&err) : std::vector<handoff::File>{};
  if (!err.empty()) {  // R6: refused before anything is sent
    SetEvent("The session cannot start: your save could not be read (" + Sanitize(err) + ").");
    return;
  }
  if (files.size() > handoff::kMaxFiles) {
    SetEvent("The session cannot start: your save has " + std::to_string(files.size()) + " files; a session carries at most " +
             std::to_string(handoff::kMaxFiles) + " (the hand-off limit).");
    return;
  }
  const std::string guest = p.guest_settings ? p.guest_settings() : "";
  const std::string gerr = handoff::ApplyGuestSettings(guest, [](const std::string&, const std::string&) { return true; });
  if (!gerr.empty()) {
    SetEvent("The session cannot start: a setting cannot be carried (" + Sanitize(gerr) + ").");
    return;
  }
  handoff::Handoff h;
  h.build = Build();
  h.release = p.release;
  h.role = handoff::Role::kHost;
  h.delay = uint8_t(Delay());
  h.local_port = t.port();
  h.peer = peer;
  h.session_token = RandomToken();
  uint64_t seed = 0;
  h.seed = ParseU64(Test("DOA4_NP_SEED"), &seed) ? seed : Random64();
  h.guest_settings = guest;
  h.files = std::move(files);
  const std::vector<uint8_t> bytes = handoff::Pack(h);
  if (bytes.empty()) {
    SetEvent("The session cannot start: the save breaks a hand-off limit (at most " + std::to_string(handoff::kMaxFiles) +
             " files of " + std::to_string(handoff::kMaxFileBytes / 1024) + " KB each, " + std::to_string(handoff::kMaxPackedBytes) +
             " bytes in all).");
    return;
  }
  tx.Reset();
  if (!tx.Begin(NextMsgId(), wire::kMsgBoot, bytes, now)) {
    SetEvent("The session cannot start: the session data is too large to send.");
    return;
  }
  boot = std::move(h);
  booting = true;
  boot_t0 = now;
  state = State::kStarting;
  Log("[doa4][NET] session start sent: " + std::to_string(bytes.size()) + " bytes, " + std::to_string(boot.files.size()) +
      " save files, delay " + std::to_string(boot.delay));
  SetEvent("Starting the online session (" + std::to_string(boot.files.size()) + " save files): both games restart...");
}

// The host: the joiner said yes with the hand-off's token. Write our side, then restart.
// M-1: the file is written once `mu` is released; the outcome (restart, or back to Connected) is applied under it
// again, in the same order as before: write, log, event, restart.
void Lobby::Impl::HostRestart() {
  booting = false;
  boot_late = false;
  tx.Reset();
  handoff::Handoff h = boot;
  h.role = handoff::Role::kHost;
  h.local_port = t.port();
  h.peer = peer;
  const std::vector<uint8_t> bytes = handoff::Pack(h);
  state = State::kRestarting;  // no second yes is taken while the file is written
  const uint64_t at = epoch;
  const fs::path path = p.handoff_path;
  deferred.push_back([this, bytes, path, at] {
    const std::string err = bytes.empty() ? "it does not pack" : WriteAtomic(path, bytes.data(), bytes.size());
    std::lock_guard<std::mutex> lock(mu);
    if (at != epoch || state != State::kRestarting) return;  // left meanwhile
    if (!err.empty()) {
      state = State::kConnected;
      SetEvent("Could not write the session hand-off (" + err + ").");
      return;
    }
    Log("[doa4][NET] hand-off written; restarting into the session");
    SetEvent("Starting the online session: restarting...");
    if (p.restart_into_session) deferred.push_back(p.restart_into_session);
  });
}

// The joiner: the host's hand-off arrived complete.
void Lobby::Impl::OnBootMessage(const std::vector<uint8_t>& msg, uint8_t type) {
  if (type != wire::kMsgBoot) return;
  handoff::Handoff h;
  const std::string err = handoff::Unpack(msg, &h);
  if (!err.empty()) {
    const std::string text = "The session from the host was refused (" + Sanitize(err) + ").";
    Answer(0, false, text);
    SetEvent(text);
    return;
  }
  const uint64_t own = Build();
  if (own == 0 || h.build != own) {
    const std::string text =
        "The host runs a different build of the game: both players need the same netplay release (Check for Updates in the Online tab).";
    Answer(h.session_token, false, text);
    SetEvent(text);
    return;
  }
  h.role = handoff::Role::kJoiner;
  h.local_port = t.port();
  h.peer = peer;  // carry 2c: the host as this socket saw it
  const std::vector<uint8_t> bytes = handoff::Pack(h);
  const uint32_t tok = h.session_token;
  const uint64_t at = epoch;
  const fs::path path = p.handoff_path;
  // M-1: written once `mu` is released; then, under it again, the answer (a yes only once the file is on disk)
  deferred.push_back([this, bytes, path, tok, at] {
    const std::string werr = bytes.empty() ? "it does not pack" : WriteAtomic(path, bytes.data(), bytes.size());
    std::lock_guard<std::mutex> lock(mu);
    if (at != epoch || state != State::kConnected) {  // M-2: cancelled while it was written
      if (werr.empty())
        deferred.push_back([path] {
          std::error_code ec;
          fs::remove(path, ec);
        });
      return;
    }
    if (!werr.empty()) {
      const std::string text = "Could not write the session hand-off (" + werr + ").";
      Answer(tok, false, text);
      SetEvent(text);
      return;
    }
    handoff_written = true;
    Answer(tok, true, "");
    state = State::kRestarting;
    SetEvent("The host started the online session: restarting...");
  });
}

void Lobby::Impl::Answer(uint32_t tok, bool ok, const std::string& text) {
  reply = wire::BootReply{};
  reply.session_token = tok;
  reply.ok = ok ? 1 : 0;
  reply.text = Sanitize(text);
  reply_pending = true;
  reply_ever = false;
  reply_left = ok ? -1 : 3;
}

// =====================================================================================================
// The tick (I/O thread, every 5 ms)
// =====================================================================================================
void Lobby::Impl::OnTick() {
  io_tid = GetCurrentThreadId();
  const uint32_t now = Now();
  std::string ctl;
  bool have_ctl = false;
  if (!p.test_dir.empty() && now - ctl_last >= kCtlPollMs) {  // ctl_last: the I/O thread's own
    ctl_last = now;
    have_ctl = TakeCtl(p.test_dir, &ctl);
  }
  {
    std::lock_guard<std::mutex> lock(mu);
    while (!cmds.empty()) {
      const Command c = cmds.front();
      cmds.pop_front();
      Apply(c, now);
    }
    Tick(now);
    if (have_ctl) ApplyRequests(ctl, now);
    if (!p.test_dir.empty() && (status_dirty || !status_ever || now - status_last >= kStatusEveryMs)) WriteStatus(now);
  }
  RunDeferred();
}

// Runs the effects queued under `mu` (log lines, file writes, the restart, on_peer_left), in order, without it; an effect
// may queue more (a hand-off write queues the restart), which run in the same call.
void Lobby::Impl::RunDeferred() {
  for (;;) {
    std::vector<std::function<void()>> d;
    {
      std::lock_guard<std::mutex> lock(mu);
      d.swap(deferred);
    }
    if (d.empty()) return;
    for (auto& f : d) f();
  }
}

void Lobby::Impl::Apply(const Command& c, uint32_t now) {
  status_dirty = true;
  switch (c.cmd) {
    case Cmd::kHost:
      if (session || UpdateRefuses("host")) return;
      if (c.port != 0 && c.port != t.port())
        Log("[doa4][NET] host on port " + std::to_string(c.port) + " asked; this lobby's socket is on " + std::to_string(t.port()) +
            " (a new port takes a new lobby)");
      GoOffline();
      is_host = true;
      state = State::kHosting;
      if (cfg.upnp && !upnp_started) {
        upnp_started = true;
        upnp::MapAsync(t.port());
      }
      SetEvent("Hosting: give the other player your code.");
      return;
    case Cmd::kJoin: {
      if (session || UpdateRefuses("join")) return;
      rules::Endpoint ep;
      GoOffline();
      if (!rules::ParseJoinCode(c.text, &ep)) {
        SetEvent(rules::kBadCodeText);
        return;
      }
      target = ep;
      state = State::kConnecting;
      connect_t0 = now;
      hello_ever = false;
      SetEvent("Connecting to the host...");
      return;
    }
    case Cmd::kReach: {
      if (session || !is_host || state != State::kHosting) {
        SetEvent("Reach out works while hosting.");
        return;
      }
      rules::Endpoint ep;
      if (!rules::ParseJoinCode(c.text, &ep)) {
        SetEvent(rules::kBadCodeText);
        return;
      }
      punching = true;
      punch_to = ep;
      punch_ever = false;
      Log("[doa4][NET] reaching out to " + rules::EndpointText(ep));
      return;
    }
    case Cmd::kDisconnect:
      if (session) return;
      if (has_peer)
        for (int i = 0; i < 3; ++i) SendPeer(wire::Type::kBye, {});
      if (state != State::kOffline) {
        GoOffline();
        SetEvent("Disconnected.");
      }
      return;
    case Cmd::kStart:
      if (!session) StartSessionNow(now);
      return;
    case Cmd::kDelay:
      delay_auto = c.flag;
      if (!c.flag) delay_manual = std::clamp(c.n, 1, 8);
      return;
    case Cmd::kLeave:
      if (!session) return;
      leave_out = c.leave;
      leave_repeat = 3;
      return;
    case Cmd::kRestartFailed: {
      if (session || state != State::kRestarting) return;
      const bool host = is_host;
      GoOffline();
      if (host) {
        is_host = true;
        state = State::kHosting;
      }
      Log("[doa4][NET] the relaunch into the session failed (" + Sanitize(c.text) + "): back in the lobby");
      SetEvent("Could not restart into the online session (" + Sanitize(c.text) + "): still in the lobby.");
      return;
    }
  }
}

void Lobby::Impl::Tick(uint32_t now) {
  // the joiner's reply to the session start: every 100 ms; the restart 700 ms after the first ok (DOA2U 2230)
  if (reply_pending) {
    if (!reply_ever || now - reply_last >= kBootReplyRepeatMs) {
      SendPeer(wire::Type::kBootReply, wire::EncodeBootReply(reply));
      if (!reply_ever) {
        reply_first = now;
        Log(reply.ok ? "[doa4][NET] boot reply ok sent" : "[doa4][NET] boot reply refusal sent: " + reply.text);
      }
      reply_ever = true;
      reply_last = now;
      if (reply_left > 0 && --reply_left == 0) reply_pending = false;
    }
    if (reply_pending && reply.ok && now - reply_first >= kJoinerRestartMs) {
      reply_pending = false;
      handoff_written = false;  // the restart owns the file now
      Log("[doa4][NET] hand-off written; restarting into the session");
      if (p.restart_into_session) deferred.push_back(p.restart_into_session);
    }
  }
  // carry 2a: a partial transfer with no chunk for 5 s is abandoned, so the host's next one is taken
  if (rx_partial && now - rx_last >= kRxIdleResetMs) {
    rx.Reset();
    rx_partial = false;
    have_completed = false;
    Log("[doa4][NET] a partial transfer had no chunk for 5 s: reset");
  }
  if (cfg.upnp && upnp_started && now - upnp_polled >= kUpnpPollMs) {  // R2-I1: a renewal that gave up is an event
    upnp_polled = now;
    const upnp::Result r = upnp::Get();
    if (r.lost != upnp_lost_seen) {
      upnp_lost_seen = r.lost;
      Log("[doa4][NET] UPnP: " + Sanitize(r.message, 1000));
      SetEvent(r.message);
    }
  }
  if (session) {
    if (state == State::kConnecting && session_refusal_seen && now - session_refusal_t0 >= kSessionRefusalGraceMs) {
      state = State::kOffline;  // R1-1: the grace is over and the peer's session Hello never came
      session_refused = true;
      Log("[doa4][NET] the hand-off's peer answered as a lobby: the other game did not start the session; giving up");
      SetEvent("The other player's game did not start the session.", "session");
      return;
    }
    if (state == State::kConnecting && (!hello_ever || now - hello_sent >= kHelloMs)) {
      hello_ever = true;
      hello_sent = now;
      SendTo(sh.peer, wire::Type::kHello, sh.session_token, wire::EncodeHello(OwnHello(sh.session_token)));
      SendTo(sh.peer, wire::Type::kPunch, 0, {});
    }
    if (state == State::kInSession && (!ping_ever || now - ping_sent >= kPingMs)) {
      ping_ever = true;
      ping_sent = now;
      SendPeer(wire::Type::kPing, wire::EncodePing(wire::Ping{now}));
    }
    if (leave_repeat > 0) {
      --leave_repeat;
      SendPeer(wire::Type::kLeave, wire::EncodeLeave(leave_out));
      ++leave_sends;
    }
    return;
  }
  switch (state) {
    case State::kHosting:
      if (punching && (!punch_ever || now - punch_sent >= kPunchMs)) {
        punch_ever = true;
        punch_sent = now;
        SendTo(punch_to, wire::Type::kPunch, 0, {});
      }
      break;
    case State::kConnecting:
      if (files_ready && (!hello_ever || now - hello_sent >= kHelloMs)) {
        hello_ever = true;
        hello_sent = now;
        const std::vector<uint8_t> body = wire::EncodeHello(OwnHello(0));
        SendTo(target, wire::Type::kHello, 0, body);
        if (cfg.lan_broadcast && !cfg.loopback) {  // also call out on the local network (DOA2U)
          const std::vector<uint8_t> d = wire::Encode(wire::Type::kHello, 0, body);
          for (uint16_t i = 0; i < rules::kPorts; ++i) t.Broadcast(uint16_t(rules::kPortBase + i), d);
        }
      }
      if (now - connect_t0 >= p.connect_timeout_ms) {
        const rules::Nat nat = t.stats().nat;
        GoOffline();
        SetEvent(rules::ConnectTimeoutText(nat, target.port));  // R2-I2: the port of the code typed
      }
      break;
    case State::kConnected:
    case State::kStarting:
      if (!ping_ever || now - ping_sent >= kPingMs) {
        ping_ever = true;
        ping_sent = now;
        SendPeer(wire::Type::kPing, wire::EncodePing(wire::Ping{now}));
      }
      if (booting) {
        for (const auto& c : tx.Due(now, kRetxMs)) SendPeer(wire::Type::kChunk, c);
        if (now - boot_t0 >= kBootAnswerMs) {
          booting = false;
          tx.Reset();
          state = State::kConnected;
          boot_late = true;  // I-1b: a yes that arrives later for this boot still starts it
          SetEvent("The other player's game did not answer the session start.");
        }
      }
      if (now - peer_last_rx > kPeerTimeoutMs) DropPeer("Connection to the other player lost.");
      break;
    default:
      break;
  }
}

bool Lobby::Impl::StartTransport(std::string* error) {
  t.on_packet = [this](const rules::Endpoint& from, const wire::Packet& pk) { OnPacket(from, pk); };
  t.on_tick = [this](uint32_t) { OnTick(); };
  t.on_foreign_proto = [this](const rules::Endpoint& from, uint8_t proto, uint8_t type, size_t bytes) { OnForeign(from, proto, type, bytes); };
  auto log = p.log;  // a copy: the transport's sink outlives anything Stop tears down
  std::string e;
  if (!t.Start(cfg, [log](const std::string& line) {
        if (log) log(line);
      }, &e)) {
    if (error) *error = e;
    return false;
  }
  return true;
}

// =====================================================================================================
// Lobby
// =====================================================================================================
Lobby::Lobby() : impl_(std::make_unique<Impl>()) {}
Lobby::~Lobby() { Stop(); }

bool Lobby::Start(Providers providers, const net::Config& transport, std::string* error) {
  Impl& m = *impl_;
  if (m.started) {
    if (error) *error = "the lobby was already started (a Lobby starts once)";
    return false;
  }
  m.started = true;
  m.p = std::move(providers);
  m.cfg = transport;
  if (!m.cfg.loopback) m.lan = LanAddress();
  if (!m.StartTransport(error)) return false;
  // carry 2g: the files text once, off the game, UI and I/O threads; Hello waits for it
  m.files_thread = std::thread([&m] {
    std::string f = m.p.files ? m.p.files() : "";
    const std::string salt = m.Test("DOA4_NP_TEST_FILES_SALT");
    if (!salt.empty()) f = SaltFiles(f, salt);
    std::lock_guard<std::mutex> lock(m.mu);
    m.files_text = std::move(f);
    m.files_ready = true;
    m.Log("[doa4][NET] game files fingerprinted (" + std::to_string(std::count(m.files_text.begin(), m.files_text.end(), '\n')) + " groups)");
  });
  m.LogNow("[doa4][NET] lobby started on port " + std::to_string(m.t.port()) + " (release " + Sanitize(m.p.release) + ")");
  return true;
}

bool Lobby::StartInSession(Providers providers, const net::Config& transport, const handoff::Handoff& h,
                           std::function<void(int32_t frame, wire::LeaveReason reason)> on_peer_left, std::string* error) {
  Impl& m = *impl_;
  if (m.started) {
    if (error) *error = "the lobby was already started (a Lobby starts once)";
    return false;
  }
  if (h.session_token == 0 || h.local_port == 0 || h.peer.port == 0) {
    if (error) *error = "the hand-off has no session token, local port or peer";
    return false;
  }
  m.started = true;
  m.session = true;
  m.p = std::move(providers);
  m.cfg = transport;
  m.cfg.port = h.local_port;
  m.cfg.exact_port = true;
  m.sh = h;
  m.on_left = std::move(on_peer_left);
  {
    std::lock_guard<std::mutex> lock(m.mu);
    m.is_host = h.role == handoff::Role::kHost;
    m.has_peer = true;  // the lobby's packets are taken from the hand-off's peer with its token; kGekko after its Hello
    m.peer = h.peer;
    m.token = h.session_token;
    m.peer_release = Sanitize(h.release);
    m.delay_auto = false;
    m.delay_manual = h.delay;
    m.state = State::kConnecting;
  }
  if (!m.StartTransport(error)) return false;
  if (m.cfg.upnp) {  // M-3: the session process maps its port again (the lobby's mapping went with the old process)
    std::lock_guard<std::mutex> lock(m.mu);
    m.upnp_started = true;
    upnp::MapAsync(m.t.port());
  }
  m.LogNow("[doa4][NET] session process: port " + std::to_string(m.t.port()) + ", peer " + rules::EndpointText(h.peer) + ", " +
        (m.is_host ? "host" : "joiner"));
  return true;
}

void Lobby::Stop() {
  Impl& m = *impl_;
  if (GetCurrentThreadId() == m.io_tid.load()) {  // carry 2d (Task 7 review M-2): joining the I/O thread from itself
    m.LogNow("[doa4][NET] WARN Stop called on the I/O thread (from a callback); ignored");
    return;
  }
  std::lock_guard<std::mutex> stop_lock(m.stop_mu);  // M-6: concurrent Stop calls are safe
  m.t.Stop();
  m.io_tid = 0;
  if (m.files_thread.joinable()) m.files_thread.join();
  bool unmap = false;
  {
    std::lock_guard<std::mutex> lock(m.mu);
    unmap = m.upnp_started;
    m.upnp_started = false;
  }
  if (unmap) upnp::Unmap();
}

static void Push(Lobby::Impl& m, Command c) {
  std::lock_guard<std::mutex> lock(m.mu);
  m.cmds.push_back(std::move(c));
}

void Lobby::Host(uint16_t port) {
  Command c;
  c.cmd = Cmd::kHost;
  c.port = port;
  Push(*impl_, c);
}
void Lobby::Join(std::string code_or_address) {
  Command c;
  c.cmd = Cmd::kJoin;
  c.text = std::move(code_or_address);
  Push(*impl_, c);
}
void Lobby::ReachOut(std::string code) {
  Command c;
  c.cmd = Cmd::kReach;
  c.text = std::move(code);
  Push(*impl_, c);
}
void Lobby::Disconnect() {
  Command c;
  c.cmd = Cmd::kDisconnect;
  Push(*impl_, c);
}
void Lobby::StartSession() {
  Command c;
  c.cmd = Cmd::kStart;
  Push(*impl_, c);
}
void Lobby::RestartFailed(std::string why) {
  Command c;
  c.cmd = Cmd::kRestartFailed;
  c.text = std::move(why);
  Push(*impl_, c);
}
void Lobby::SetDelay(bool automatic, int frames) {
  Command c;
  c.cmd = Cmd::kDelay;
  c.flag = automatic;
  c.n = frames;
  Push(*impl_, c);
}
void Lobby::SendLeave(int32_t frame, wire::LeaveReason reason) {
  Command c;
  c.cmd = Cmd::kLeave;
  c.leave.frame = frame;
  c.leave.reason = reason;
  Push(*impl_, c);
}

int Lobby::QuitSession(int32_t frame, uint32_t max_wait_ms) {
  // Review M1: a Leave already queued or still being repeated (e.g. a desync's) keeps its reason and frame: no second
  // Leave replaces it; this call only waits for it to go out. Otherwise the quit Leave (kLeft) is queued.
  uint64_t base = 0;
  int expected = 3;
  bool pending = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    base = impl_->leave_sends;
    const bool queued = std::any_of(impl_->cmds.begin(), impl_->cmds.end(), [](const Command& c) { return c.cmd == Cmd::kLeave; });
    pending = queued || impl_->leave_repeat > 0;
    if (!queued && impl_->leave_repeat > 0) expected = impl_->leave_repeat;  // the rest of a Leave already being sent
  }
  if (!pending) SendLeave(frame, wire::LeaveReason::kLeft);
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
  uint64_t done = 0;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(impl_->mu);
      done = impl_->leave_sends - base;
    }
    if (done >= uint64_t(expected) || std::chrono::steady_clock::now() >= end) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  impl_->t.Stop();  // only now: the Leave had its chance to go out (Task 15 G2)
  // Review M2: a send here is a sendto on the lobby's socket at once (Transport::SendTo -> SendOut -> RawSend,
  // np_transport.cpp:263-266), or a hold/drop of the network simulator when one is configured (harness only).
  return int(std::min<uint64_t>(done, uint64_t(expected))) + (3 - expected);
}


Status Lobby::status() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->SnapshotLocked();
}

net::Transport& Lobby::transport() { return impl_->t; }

}  // namespace doa4::np::online

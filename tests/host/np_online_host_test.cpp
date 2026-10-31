// Host test for src/net/np_online (the lobby: join, handshake, pings, session start, restart-on-join, the session
// process's Hello/Leave). Built and run by run_np_online_test.bat. Two or more Lobby instances run in this one process,
// each with its own Transport on 127.0.0.1 port 0 (STUN off unless a group runs fake STUN servers on loopback; UPnP
// off: np_upnp is never called). Scripted peers (FakePeer: a plain UDP socket speaking the wire format) stand in for
// one side wherever a test must send what a real lobby never sends (a wrong token, a third address, another protocol).
// Real time, bounded waits. A watchdog thread ends the run with a FAIL line after 240 s or above 1 GiB of private
// memory. Files go under %TEMP%\doa4_np_online_<pid>\ and only that folder is removed at the end.
// "np_online_host_test.exe -v" prints each group before it runs; "-only <substring>" runs only the matching groups.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../src/net/np_handoff.h"
#include "../../src/net/np_online.h"
#include "../../src/net/np_rules.h"
#include "../../src/net/np_transport.h"
#include "../../src/net/np_upnp.h"
#include "../../src/net/np_wire.h"

using namespace doa4::np;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using online::State;

static int g_checks = 0;
static int g_failures = 0;
static bool g_verbose = false;
static std::string g_only;
static std::mutex g_print_mu;

#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

#define CHECKF(cond, ...)                                           \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                     \
      std::printf("\n");                                            \
      ++g_failures;                                                 \
    }                                                               \
  } while (0)

static bool Group(const char* name) {
  if (!g_only.empty() && std::string(name).find(g_only) == std::string::npos) return false;
  if (g_verbose) std::printf("group: %s\n", name);
  return true;
}

using Clock = std::chrono::steady_clock;
static int64_t NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }
static void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

template <class F>
static bool WaitFor(F f, int ms) {
  const int64_t t0 = NowUs();
  while (NowUs() - t0 < int64_t(ms) * 1000) {
    if (f()) return true;
    SleepMs(5);
  }
  return f();
}

static rules::Endpoint Loop(uint16_t port) {
  rules::Endpoint e;
  e.ip = {127, 0, 0, 1};
  e.port = port;
  return e;
}
static std::string Addr(uint16_t port) { return "127.0.0.1:" + std::to_string(port); }

// ---- np_upnp is not linked: these stand-ins count calls per function. Only the UPnP group (net::Config::upnp on)
// may reach them; the run asserts every call was made there (the router is the user's) ----
static std::atomic<int> g_upnp_map{0}, g_upnp_get{0}, g_upnp_unmap{0}, g_upnp_shutdown{0};
static std::atomic<uint16_t> g_upnp_port{0};
static int UpnpCalls() { return g_upnp_map + g_upnp_get + g_upnp_unmap + g_upnp_shutdown; }
static int g_upnp_expected = 0;  // the calls the UPnP group made
namespace doa4::np::upnp {
void MapAsync(uint16_t port) {
  g_upnp_port = port;
  ++g_upnp_map;
}
Result Get() {
  ++g_upnp_get;
  return {};
}
void Unmap() { ++g_upnp_unmap; }
void Shutdown(uint32_t) { ++g_upnp_shutdown; }
}  // namespace doa4::np::upnp

// ---- per-run folder ----
static fs::path g_root;
static int g_dir_n = 0;
static fs::path NewDir(const char* tag) {
  const fs::path d = g_root / (std::string(tag) + "_" + std::to_string(++g_dir_n));
  fs::create_directories(d);
  return d;
}
static bool ReadFile(const fs::path& p, Bytes* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}
static std::string ReadText(const fs::path& p) {
  Bytes b;
  if (!ReadFile(p, &b)) return "";
  return std::string(b.begin(), b.end());
}

// ---- the log sink: every line with the time it was logged ----
struct LogSink {
  std::mutex mu;
  std::vector<std::pair<int64_t, std::string>> lines;
  std::vector<DWORD> tids;  // the thread of each line
  std::function<void(const std::string&)> Fn() {
    return [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(mu);
      lines.emplace_back(NowUs(), line);
      tids.push_back(GetCurrentThreadId());
    };
  }
  DWORD TidOf(const std::string& needle) {
    std::lock_guard<std::mutex> lock(mu);
    for (size_t i = 0; i < lines.size(); ++i)
      if (lines[i].second.find(needle) != std::string::npos) return tids[i];
    return 0;
  }
  int Count(const std::string& needle) {
    std::lock_guard<std::mutex> lock(mu);
    int n = 0;
    for (const auto& l : lines) n += l.second.find(needle) != std::string::npos ? 1 : 0;
    return n;
  }
  int64_t FirstUs(const std::string& needle) {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& l : lines)
      if (l.second.find(needle) != std::string::npos) return l.first;
    return -1;
  }
  void Dump(const char* who) {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& l : lines) std::printf("  log %s: %s\n", who, l.second.c_str());
  }
};

// ---- a plain UDP socket of the test, bound to 127.0.0.1 ----
struct Udp {
  SOCKET s = INVALID_SOCKET;
  uint16_t port = 0;
  explicit Udp(uint16_t want = 0) {
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(want);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
      if (s != INVALID_SOCKET) closesocket(s);
      s = INVALID_SOCKET;
      return;
    }
    int l = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
    port = ntohs(a.sin_port);
    DWORD b = FALSE, ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &b, sizeof b, nullptr, 0, &ret, nullptr, nullptr);
  }
  ~Udp() { Close(); }
  void Close() {
    if (s != INVALID_SOCKET) closesocket(s);
    s = INVALID_SOCKET;
  }
  Udp(const Udp&) = delete;
  Udp& operator=(const Udp&) = delete;
  bool ok() const { return s != INVALID_SOCKET; }
  int SendTo(uint16_t to_port, const Bytes& d) const {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(to_port);
    return sendto(s, reinterpret_cast<const char*>(d.data()), int(d.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
  }
  bool Recv(int ms, Bytes* out, uint16_t* from_port = nullptr) const {
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(s, &fs);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    if (select(0, &fs, nullptr, nullptr, &tv) <= 0) return false;
    char buf[4096];
    sockaddr_in from{};
    int fl = sizeof from;
    const int n = recvfrom(s, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 0) return false;
    out->assign(buf, buf + n);
    if (from_port) *from_port = ntohs(from.sin_port);
    return true;
  }
};

// A port nobody serves right now (bound, then closed).
static uint16_t DeadPort() {
  Udp u;
  return u.port;
}

// ---- a scripted peer ----
struct Rx {
  int64_t us = 0;
  uint16_t from = 0;
  uint8_t proto = 0;
  wire::Type type = wire::Type::kPing;
  uint32_t token = 0;
  Bytes body;
  size_t bytes = 0;  // the whole datagram
};
struct FakePeer {
  Udp sock;
  std::thread th;
  std::atomic<bool> stop{false};
  std::mutex mu;
  std::vector<Rx> rx;
  std::atomic<uint32_t> pong_token{0};  // non-zero: every kPing is answered with a kPong carrying this token
  FakePeer() { Begin(); }
  ~FakePeer() { End(); }
  void Begin() {
    th = std::thread([this] {
      while (!stop.load()) {
        Bytes d;
        uint16_t from = 0;
        if (!sock.Recv(5, &d, &from)) continue;
        if (d.size() < wire::kHeaderBytes) continue;
        Rx r;
        r.us = NowUs();
        r.from = from;
        r.bytes = d.size();
        r.proto = d[4];
        r.type = wire::Type(d[5]);
        r.token = uint32_t(d[8]) | uint32_t(d[9]) << 8 | uint32_t(d[10]) << 16 | uint32_t(d[11]) << 24;
        r.body.assign(d.begin() + wire::kHeaderBytes, d.end());
        if (r.proto == wire::kProto && r.type == wire::Type::kPing && pong_token.load())
          sock.SendTo(from, wire::Encode(wire::Type::kPong, pong_token.load(), r.body));
        std::lock_guard<std::mutex> lock(mu);
        rx.push_back(std::move(r));
      }
    });
  }
  void End() {
    stop = true;
    if (th.joinable()) th.join();
  }
  void Send(uint16_t to, wire::Type t, uint32_t token, const Bytes& body) { sock.SendTo(to, wire::Encode(t, token, body)); }
  std::vector<Rx> Of(wire::Type t) {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<Rx> v;
    for (const Rx& r : rx)
      if (r.type == t && r.proto == wire::kProto) v.push_back(r);
    return v;
  }
  int Count(wire::Type t) { return int(Of(t).size()); }
  bool Wait(wire::Type t, int ms, Rx* out = nullptr, size_t nth = 0) {
    const bool ok = WaitFor([&] { return Of(t).size() > nth; }, ms);
    if (ok && out) *out = Of(t)[nth];
    return ok;
  }
  std::vector<Rx> All() {
    std::lock_guard<std::mutex> lock(mu);
    return rx;
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mu);
    rx.clear();
  }
};

// ---- fixtures ----
static const std::string kFilesText =
    "anm.afs=0123456789abcdef\nchrone.afs=00000000000000aa\ndefault.xex=1111111111111111\n";
static const std::string kPre = handoff::kSavePrefix;

static std::string GuestText() {
  return handoff::CaptureGuestSettings([](const std::string& name) { return name == "vsync" ? "false" : "1"; });
}
static std::vector<handoff::File> SaveFixture() {
  handoff::File a{kPre + "00000001/ups.dat/ups.dat", Bytes(4000)};
  for (size_t i = 0; i < a.data.size(); ++i) a.data[i] = uint8_t(i * 7 + 3);
  handoff::File b{kPre + "Headers/00000001/ups.dat.header", Bytes(100, 0x5A)};
  return {a, b};
}

struct Side {
  fs::path dir;
  std::atomic<uint64_t> build{0x0DA4000000000001ull};
  std::string release = "dev";
  std::string files = kFilesText;
  std::string guest = GuestText();
  std::mutex save_mu;
  std::vector<handoff::File> save = SaveFixture();
  std::string save_error;
  std::atomic<int> restarts{0};
  std::atomic<int64_t> restart_us{0};
  std::atomic<DWORD> files_tid{0};  // the thread Providers::files ran on
  std::function<void()> on_restart;
  LogSink log;
  uint32_t timeout_ms = 2000;
  fs::path test_dir;
  std::map<std::string, std::string> test;
  explicit Side(const char* tag) : dir(NewDir(tag)) {}
  online::Providers P() {
    online::Providers p;
    p.build = [this] { return build.load(); };
    p.connect_timeout_ms = timeout_ms;
    p.release = release;
    p.files = [this] {
      files_tid = GetCurrentThreadId();
      return files;
    };
    p.guest_settings = [this] { return guest; };
    p.save_files = [this](std::string* e) {
      std::lock_guard<std::mutex> lock(save_mu);
      if (!save_error.empty()) {
        *e = save_error;
        return std::vector<handoff::File>{};
      }
      return save;
    };
    p.handoff_path = dir / "netplay" / "handoff.d4h";
    p.restart_into_session = [this] {
      restart_us = NowUs();
      ++restarts;
      if (on_restart) on_restart();
    };
    p.events_path = dir / "netplay" / "events.txt";
    p.test_dir = test_dir;
    p.test = test;
    p.log = log.Fn();
    return p;
  }
  fs::path HandoffPath() const { return dir / "netplay" / "handoff.d4h"; }
  std::string Events() const { return ReadText(dir / "netplay" / "events.txt"); }
};

static net::Config LoopCfg() {
  net::Config c;
  c.port = 0;
  c.loopback = true;
  c.stun = false;
  c.lan_broadcast = false;
  c.upnp = false;
  return c;
}

static bool StartLobby(online::Lobby& l, Side& s, const net::Config& c = LoopCfg()) {
  std::string e;
  const bool ok = l.Start(s.P(), c, &e);
  CHECKF(ok, "Lobby::Start: %s", e.c_str());
  return ok;
}
static bool WaitState(online::Lobby& l, State s, int ms) {
  return WaitFor([&] { return l.status().state == s; }, ms);
}
static bool Contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static bool ConnectPair(online::Lobby& host, online::Lobby& joiner) {
  host.Host(0);
  if (!WaitState(host, State::kHosting, 1000)) return false;
  joiner.Join(Addr(host.status().port));
  return WaitState(host, State::kConnected, 3000) && WaitState(joiner, State::kConnected, 3000);
}
static bool WaitCanStart(online::Lobby& host, int ms = 3000) {
  return WaitFor([&] { return host.status().can_start; }, ms);
}
static uint32_t Drops(const online::Status& s, wire::Drop d) { return s.drops[size_t(d)]; }

static Bytes HelloBody(uint64_t build, const std::string& release, const std::string& files, uint8_t delay_auto = 1,
                       uint8_t delay_frames = 2, uint32_t token = 0) {
  wire::Hello h;
  h.build = build;
  h.release = release;
  h.files = files;
  h.delay_auto = delay_auto;
  h.delay_frames = delay_frames;
  h.session_token = token;
  return wire::EncodeHello(h);
}
static Bytes WelcomeBody(wire::Refusal r, uint64_t build, const std::string& release, const std::string& text, uint32_t token) {
  wire::Welcome w;
  w.refusal = r;
  w.build = build;
  w.release = release;
  w.text = text;
  w.session_token = token;
  return wire::EncodeWelcome(w);
}
static Bytes BootReplyBody(uint32_t token, uint8_t ok, const std::string& text) {
  wire::BootReply b;
  b.session_token = token;
  b.ok = ok;
  b.text = text;
  return wire::EncodeBootReply(b);
}
static Bytes LeaveBody(int32_t frame, wire::LeaveReason r) {
  wire::Leave l;
  l.frame = frame;
  l.reason = r;
  return wire::EncodeLeave(l);
}

// A fake joiner joins a real hosting lobby: Hello, then the Welcome's token; it answers pings from then on.
static uint32_t FakeJoin(FakePeer& f, online::Lobby& host, uint64_t build, const std::string& release = "dev",
                         uint8_t delay_auto = 1, uint8_t delay_frames = 2) {
  f.Clear();
  f.Send(host.status().port, wire::Type::kHello, 0, HelloBody(build, release, kFilesText, delay_auto, delay_frames));
  Rx w;
  if (!f.Wait(wire::Type::kWelcome, 1000, &w)) return 0;
  wire::Welcome m;
  if (!wire::DecodeWelcome(w.body, &m) || m.refusal != wire::Refusal::kNone) return 0;
  f.pong_token = m.session_token;
  return m.session_token;
}

// A valid packed hand-off as a host would send it.
static Bytes PackedHandoff(uint16_t host_port, uint16_t joiner_port, uint32_t token) {
  handoff::Handoff h;
  h.build = 0x0DA4000000000001ull;
  h.release = "dev";
  h.role = handoff::Role::kHost;
  h.delay = 3;
  h.local_port = host_port;
  h.peer = Loop(joiner_port);
  h.session_token = token;
  h.seed = 0x1234567890ull;
  h.guest_settings = GuestText();
  h.files = SaveFixture();
  return handoff::Pack(h);
}
static std::vector<Bytes> Chunks(const Bytes& msg, uint32_t msg_id) {
  wire::ReliableSender s;
  s.Begin(msg_id, wire::kMsgBoot, msg, 0);
  return s.Due(0, online::kRetxMs);
}

// =====================================================================================================
// Pure helpers
// =====================================================================================================

static void TestPure() {
  if (!Group("pure: Sanitize, SaltFiles, StateName, the UPnP renewal schedule")) return;
  CHECK(online::Sanitize("a\x01" "b\nc\x7F" "d") == "a?b?c?d");
  CHECK(online::Sanitize(std::string(300, 'x')).size() == wire::kMaxText);
  CHECK(online::Sanitize("abc", 2) == "ab");
  CHECK(online::Sanitize(std::string("\xC3\xA9", 2)) == "??");
  CHECK(online::Sanitize(" ~") == " ~");  // both ends of the printable range survive
  CHECK(online::SaltFiles(kFilesText, "chrone.afs") ==
        "anm.afs=0123456789abcdef\nchrone.afs=00000000000000ab\ndefault.xex=1111111111111111\n");
  CHECK(online::SaltFiles("a=ffffffffffffffff\n", "a") == "a=0000000000000000\n");
  CHECK(online::SaltFiles("a=unreadable\n", "a") == "a=unreadable\n");
  CHECK(online::SaltFiles(kFilesText, "snd.afs") == kFilesText);
  CHECK(online::SaltFiles(kFilesText, "anm") == kFilesText);  // a name prefix is not the group
  CHECK(std::string(online::StateName(State::kInSession)) == "in_session");
  CHECK(std::string(online::StateName(State::kHosting)) == "hosting");
  // carry 2f: the lease is renewed at half its length (a permanent lease, 0, never); always before it runs out.
  CHECK(upnp::RenewDelayMs(3600) == 1800000u);
  CHECK(upnp::RenewDelayMs(0) == 0u);
  bool all = true;
  for (uint32_t lease = 1; lease <= 7200; ++lease) {
    const uint32_t d = upnp::RenewDelayMs(lease);
    all = all && d > 0 && d < lease * 1000u && d >= lease * 1000u / 2;
  }
  CHECK(all);
}

// =====================================================================================================
// Connect: every Status field by name
// =====================================================================================================

static void TestConnect() {
  if (!Group("connect: host + join on loopback, pings, every Status field")) return;
  Side hs("conn_host"), js("conn_join");
  hs.release = "netplay-v1.0.5";
  js.release = "netplay-v1.0.6";  // another tag, same build: the tags are advice only (and peer_release can tell them apart)
  online::Lobby host, joiner;
  if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
  online::Status h0 = host.status();
  CHECK(h0.state == State::kOffline && !h0.is_host && h0.ping_samples == 0 && !h0.can_start);
  CHECKF(h0.port != 0 && h0.port == host.transport().port(), "port %u, transport %u", h0.port, host.transport().port());
  const int64_t t0 = NowUs();
  host.Host(0);
  CHECK(WaitState(host, State::kHosting, 1000));
  CHECK(host.status().is_host);
  CHECK(!host.status().hint.empty());
  joiner.Join(Addr(host.status().port));
  CHECK(WaitState(host, State::kConnected, 3000));
  CHECK(WaitState(joiner, State::kConnected, 3000));
  std::printf("info: connect: both connected %lld ms after Host\n", (long long)((NowUs() - t0) / 1000));
  CHECKF(WaitFor([&] { return host.status().ping_samples >= 4 && joiner.status().ping_samples >= 4; }, int(3000 - (NowUs() - t0) / 1000)),
         "ping samples host %d joiner %d 3 s after Host", host.status().ping_samples, joiner.status().ping_samples);
  CHECK(WaitCanStart(host, 100));
  const online::Status h = host.status(), j = joiner.status();
  CHECK(h.is_host && !j.is_host);
  CHECK(h.can_start && !j.can_start);
  rules::Endpoint ep;
  CHECKF(rules::ParseJoinCode(h.my_code, &ep) && ep == Loop(h.port), "host code '%s' does not parse to 127.0.0.1:%u", h.my_code.c_str(), h.port);
  CHECKF(rules::ParseJoinCode(j.my_code, &ep) && ep == Loop(j.port), "joiner code '%s'", j.my_code.c_str());
  CHECK(h.port == host.transport().port() && j.port == joiner.transport().port() && h.port != j.port);
  CHECK(h.peer_port == j.port && j.peer_port == h.port);
  CHECKF(h.peer_release == "netplay-v1.0.6" && j.peer_release == "netplay-v1.0.5", "peer_release host '%s' joiner '%s'",
         h.peer_release.c_str(), j.peer_release.c_str());
  CHECKF(h.delay_auto && h.delay == rules::DelayFromRtt(int(std::lround(h.ping_ms))), "host delay %d (auto %d) at ping %.2f", h.delay,
         int(h.delay_auto), h.ping_ms);
  CHECK(j.delay_auto && j.delay == rules::DelayFromRtt(int(std::lround(j.ping_ms))));
  CHECK(h.ping_ms >= 0 && h.ping_ms < 100);
  CHECK(h.nat == rules::Nat::kNoAnswer && h.code_note == rules::NatNote(rules::Nat::kNoAnswer));
  CHECK(j.nat == rules::Nat::kNoAnswer && j.code_note == rules::NatNote(rules::Nat::kNoAnswer));
  CHECKF(Contains(h.upnp, "off") && Contains(j.upnp, "off"), "upnp '%s' / '%s'", h.upnp.c_str(), j.upnp.c_str());
  CHECKF(!j.hint.empty() && !h.hint.empty(), "hints '%s' / '%s'", h.hint.c_str(), j.hint.c_str());
  CHECKF(Contains(h.event, "Connected") && Contains(j.event, "Connected"), "events '%s' / '%s'", h.event.c_str(), j.event.c_str());
  CHECKF(h.dropped == 0 && j.dropped == 0 && h.gekko_early == 0, "dropped %u / %u", h.dropped, j.dropped);
  CHECKF(h.refusal == "none" && j.refusal == "none", "refusal %s / %s", h.refusal.c_str(), j.refusal.c_str());
  // carry 2g: Providers::files ran once, on neither the thread that called Start nor the I/O thread (whose lines the
  // "event: Connected" log line shows)
  const DWORD io = js.log.TidOf("event: Connected");
  CHECKF(js.files_tid.load() != 0 && io != 0 && js.files_tid.load() != GetCurrentThreadId() && js.files_tid.load() != io,
         "files ran on thread %lu (main %lu, I/O %lu)", js.files_tid.load(), GetCurrentThreadId(), io);
  // the events file (T4.6: the wiring of events_path)
  CHECKF(Contains(hs.Events(), "Connected to the other player.") && Contains(js.Events(), "Connected to the other player."),
         "events files: '%s' / '%s'", hs.Events().c_str(), js.Events().c_str());
  // SetDelay: manual, clamped 1..8, back to auto
  host.SetDelay(false, 5);
  CHECKF(WaitFor([&] { return host.status().delay == 5 && !host.status().delay_auto; }, 200), "delay %d auto %d after SetDelay(false, 5)",
         host.status().delay, int(host.status().delay_auto));
  host.SetDelay(false, 99);
  CHECK(WaitFor([&] { return host.status().delay == 8; }, 200));
  host.SetDelay(false, 0);
  CHECK(WaitFor([&] { return host.status().delay == 1; }, 200));
  host.SetDelay(true, 5);
  CHECK(WaitFor([&] { const auto s = host.status(); return s.delay_auto && s.delay == rules::DelayFromRtt(int(std::lround(s.ping_ms))); }, 200));
  // Disconnect: a Bye; both Offline.
  joiner.Disconnect();
  CHECK(WaitState(joiner, State::kOffline, 500));
  CHECKF(WaitState(host, State::kOffline, 1000) || WaitState(host, State::kHosting, 10), "host state %s after the joiner's Bye",
         online::StateName(host.status().state));
  CHECK(Contains(host.status().event, "disconnected"));
  host.Stop();
  joiner.Stop();
}

// =====================================================================================================
// Refusals at the handshake
// =====================================================================================================

static void TestBuildDiffers() {
  if (!Group("handshake: build differs -> the joiner composes its own advice")) return;
  {
    Side hs("build_host"), js("build_join");
    hs.release = "netplay-v1.0.5";
    js.release = "dev";
    js.build = hs.build + 1;
    online::Lobby host, joiner;
    if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
    host.Host(0);
    CHECK(WaitState(host, State::kHosting, 1000));
    joiner.Join(Addr(host.status().port));
    CHECK(WaitFor([&] { return Contains(joiner.status().event, rules::MismatchAdvice("dev", "netplay-v1.0.5")); }, 2000));
    const online::Status j = joiner.status();
    CHECK(j.state == State::kOffline);
    // carry 2b: the host-seat text (MismatchAdvice(host, joiner)) is never shown on the joiner
    CHECKF(!Contains(j.event, rules::MismatchAdvice("netplay-v1.0.5", "dev")), "joiner shows the host-seat text: '%s'", j.event.c_str());
    CHECK(rules::MismatchAdvice("dev", "netplay-v1.0.5") != rules::MismatchAdvice("netplay-v1.0.5", "dev"));  // the fixture tells them apart
    CHECKF(Contains(host.status().event, "Refused a player with a different build"), "host event '%s'", host.status().event.c_str());
    CHECKF(j.refusal == "build" && host.status().refusal == "build", "refusal %s / %s", j.refusal.c_str(), host.status().refusal.c_str());
    CHECK(host.status().state == State::kHosting);
  }
  {  // build 0 (unreadable) on both sides: refused kBuild all the same
    Side hs("build0_host"), js("build0_join");
    hs.build = 0;
    js.build = 0;
    online::Lobby host, joiner;
    if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
    host.Host(0);
    CHECK(WaitState(host, State::kHosting, 1000));
    joiner.Join(Addr(host.status().port));
    CHECKF(WaitFor([&] { return joiner.status().state == State::kOffline && !joiner.status().event.empty(); }, 2000), "joiner %s '%s'",
           online::StateName(joiner.status().state), joiner.status().event.c_str());
    CHECKF(Contains(joiner.status().event, "could not read its own build"), "joiner event '%s'", joiner.status().event.c_str());
    CHECK(joiner.status().refusal == "build");
    CHECK(host.status().state == State::kHosting);
  }
  {  // DOA4_NP_TEST_BUILD_SALT=1 on the joiner (with a test dir): refused; the same map without a test dir: ignored
    Side hs("salt_host"), js("salt_join"), ks("salt_nodir");
    js.test_dir = js.dir / "ctl";
    fs::create_directories(js.test_dir);
    js.test["DOA4_NP_TEST_BUILD_SALT"] = "1";
    ks.test["DOA4_NP_TEST_BUILD_SALT"] = "1";
    online::Lobby host, joiner, nodir;
    if (!StartLobby(host, hs) || !StartLobby(joiner, js) || !StartLobby(nodir, ks)) return;
    host.Host(0);
    CHECK(WaitState(host, State::kHosting, 1000));
    joiner.Join(Addr(host.status().port));
    CHECK(WaitFor([&] { return joiner.status().state == State::kOffline && !joiner.status().event.empty(); }, 2000));
    CHECKF(Contains(host.status().event, "different build"), "host event '%s'", host.status().event.c_str());
    nodir.Join(Addr(host.status().port));
    CHECK(host.status().refusal == "build");
    CHECKF(WaitState(nodir, State::kConnected, 2000), "the salt without a test dir was not ignored: %s '%s'",
           online::StateName(nodir.status().state), nodir.status().event.c_str());
  }
}

static void TestFilesDiffer() {
  if (!Group("handshake: files differ -> both name the group; a local problem is the joiner's own")) return;
  {  // DOA4_NP_TEST_FILES_SALT=chrone.afs on the joiner
    Side hs("files_host"), js("files_join");
    js.test_dir = js.dir / "ctl";
    fs::create_directories(js.test_dir);
    js.test["DOA4_NP_TEST_FILES_SALT"] = "chrone.afs";
    online::Lobby host, joiner;
    if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
    host.Host(0);
    CHECK(WaitState(host, State::kHosting, 1000));
    joiner.Join(Addr(host.status().port));
    CHECK(WaitFor([&] { return joiner.status().state == State::kOffline && !joiner.status().event.empty(); }, 2000));
    CHECKF(Contains(joiner.status().event, "chrone.afs") && !Contains(joiner.status().event, "anm.afs"), "joiner event '%s'",
           joiner.status().event.c_str());
    CHECK(joiner.status().refusal == "files" && host.status().refusal == "files");
    CHECKF(Contains(host.status().event, "chrone.afs") && !Contains(host.status().event, "anm.afs"), "host event '%s'",
           host.status().event.c_str());
  }
  for (const char* value : {"unreadable", "missing"}) {  // carry 2g: the joiner's own unreadable / missing group
    Side hs("files_local_host"), js("files_local_join");
    js.files = "anm.afs=0123456789abcdef\nchrone.afs=" + std::string(value) + "\ndefault.xex=1111111111111111\n";
    online::Lobby host, joiner;
    if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
    host.Host(0);
    CHECK(WaitState(host, State::kHosting, 1000));
    joiner.Join(Addr(host.status().port));
    CHECK(WaitFor([&] { return joiner.status().state == State::kOffline && !joiner.status().event.empty(); }, 2000));
    const std::string want = std::string("your game files: chrone.afs ") + (value[0] == 'u' ? "cannot be read" : "is missing");
    CHECKF(Contains(joiner.status().event, want), "joiner event '%s' (want '%s')", joiner.status().event.c_str(), want.c_str());
    CHECK(joiner.status().refusal == "files");
    CHECKF(!Contains(joiner.status().event, "both players need the same game files"), "joiner event blames the other side: '%s'",
           joiner.status().event.c_str());
  }
  {  // the host's own unreadable group: the host's event says so
    Side hs("files_hostlocal_host"), js("files_hostlocal_join");
    hs.files = "anm.afs=0123456789abcdef\nchrone.afs=unreadable\ndefault.xex=1111111111111111\n";
    online::Lobby host, joiner;
    if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
    host.Host(0);
    CHECK(WaitState(host, State::kHosting, 1000));
    joiner.Join(Addr(host.status().port));
    // (the joiner is Offline before its Join is applied too: wait on the host's refusal, then on the joiner's event)
    CHECKF(WaitFor([&] { return Contains(host.status().event, "Refused"); }, 2000), "host event '%s'", host.status().event.c_str());
    CHECKF(Contains(host.status().event, "your game files: chrone.afs cannot be read"), "host event '%s'", host.status().event.c_str());
    CHECK(WaitFor([&] { return joiner.status().state == State::kOffline && !joiner.status().event.empty(); }, 1000));
    CHECKF(!Contains(joiner.status().event, "your game files"), "the joiner blames its own files: '%s'", joiner.status().event.c_str());
    CHECK(joiner.status().refusal == "files" && host.status().refusal == "files");
  }
}

static void TestBusy() {
  if (!Group("handshake: a third player is refused kBusy; the pair stays connected")) return;
  Side hs("busy_host"), js("busy_join"), ts("busy_third");
  online::Lobby host, joiner, third;
  if (!StartLobby(host, hs) || !StartLobby(joiner, js) || !StartLobby(third, ts)) return;
  CHECK(ConnectPair(host, joiner));
  const int samples = host.status().ping_samples;
  third.Join(Addr(host.status().port));
  CHECK(WaitFor([&] { return third.status().state == State::kOffline && !third.status().event.empty(); }, 2000));
  CHECKF(Contains(third.status().event, "already in an online session"), "third's event '%s'", third.status().event.c_str());
  CHECKF(third.status().refusal == "busy" && host.status().refusal == "busy" && joiner.status().refusal == "none", "refusal %s / %s / %s",
         third.status().refusal.c_str(), host.status().refusal.c_str(), joiner.status().refusal.c_str());
  CHECK(host.status().state == State::kConnected && joiner.status().state == State::kConnected);
  CHECK(host.status().peer_port == joiner.status().port);
  CHECK(WaitFor([&] { return host.status().ping_samples >= samples + 2; }, 1500));  // the pair still talks
}

// =====================================================================================================
// Tokens and sources
// =====================================================================================================

static void TestStaleToken() {
  if (!Group("tokens: after the Welcome a kPing with token 0 or another token is dropped (kToken), state unchanged")) return;
  Side js("stale_join");
  online::Lobby joiner;
  if (!StartLobby(joiner, js)) return;
  FakePeer fake;
  joiner.Join(Addr(fake.sock.port));
  Rx hello;
  CHECK(fake.Wait(wire::Type::kHello, 1000, &hello));
  const uint32_t T = 0x7A11C0DEu;
  fake.pong_token = T;
  fake.Send(hello.from, wire::Type::kWelcome, 0, WelcomeBody(wire::Refusal::kNone, js.build, "dev\x01v2", "", T));
  CHECK(WaitState(joiner, State::kConnected, 1000));
  CHECKF(joiner.status().peer_release == "dev?v2", "peer_release '%s' (a received text is sanitised)", joiner.status().peer_release.c_str());
  SleepMs(100);
  const online::Status s0 = joiner.status();
  const uint16_t jp = s0.port;
  const Bytes ping = wire::EncodePing(wire::Ping{42});
  fake.Clear();
  fake.Send(jp, wire::Type::kPing, 0, ping);
  SleepMs(150);
  online::Status s1 = joiner.status();
  CHECKF(s1.dropped == s0.dropped + 1 && Drops(s1, wire::Drop::kToken) == Drops(s0, wire::Drop::kToken) + 1, "token 0: dropped %u -> %u",
         s0.dropped, s1.dropped);
  fake.Send(jp, wire::Type::kPing, 0, ping);  // no. 2 below uses another token; this one checks "one per packet" twice
  fake.Send(jp, wire::Type::kPing, T ^ 0x10, ping);
  SleepMs(150);
  online::Status s2 = joiner.status();
  CHECKF(s2.dropped == s1.dropped + 2 && Drops(s2, wire::Drop::kToken) == Drops(s1, wire::Drop::kToken) + 2, "dropped %u -> %u",
         s1.dropped, s2.dropped);
  CHECK(s2.state == State::kConnected);
  // no Pong went back for the dropped pings; the positive control: a ping with T gets one, and is not a drop
  int pong_ours = 0;
  for (const Rx& r : fake.Of(wire::Type::kPong)) {
    wire::Ping p;
    pong_ours += wire::DecodePing(r.body, &p) && p.t_ms == 42 ? 1 : 0;
  }
  CHECK(pong_ours == 0);
  fake.Send(jp, wire::Type::kPing, T, ping);
  CHECK(WaitFor([&] {
    for (const Rx& r : fake.Of(wire::Type::kPong)) {
      wire::Ping p;
      if (wire::DecodePing(r.body, &p) && p.t_ms == 42 && r.token == T) return true;
    }
    return false;
  }, 500));
  CHECK(joiner.status().dropped == s2.dropped);
}

// Spoofing (S2a): a packet that changes state is accepted only from the peer with its token. Host side: the host is in
// kStarting with a fake joiner; Leave, BootReply, Chunk and Ack each come once with a wrong token from the fake's own
// address (kToken) and once with the right token from a third socket (kSource): nothing changes. Then the real reply.
static uint32_t g_first_msg_id = 0;
static void TestSpoofHost() {
  if (!Group("spoofing, host side: BootReply/Ack/Leave/Chunk need the peer's address and token; then the real reply")) return;
  Side hs("spoof_host");
  hs.test["DOA4_NP_SEED"] = "7";  // no test dir: ignored
  online::Lobby host;
  if (!StartLobby(host, hs)) return;
  std::atomic<bool> stop_refused{false};
  hs.on_restart = nullptr;
  host.Host(0);
  CHECK(WaitState(host, State::kHosting, 1000));
  FakePeer fake;
  const uint32_t T = FakeJoin(fake, host, hs.build, "dev", /*delay_auto=*/0, /*delay_frames=*/200);
  CHECK(T != 0);
  CHECK(WaitState(host, State::kConnected, 500));
  // carry 2a: the joiner's delay preference (200) is clamped before it is logged
  CHECKF(hs.log.Count("delay preference 8") == 1, "no clamped delay preference line");
  CHECK(WaitCanStart(host));
  const uint16_t hp = host.status().port;
  host.StartSession();
  CHECK(WaitState(host, State::kStarting, 500));
  // collect the hand-off from the chunks (the fake does not ack: the host resends every 150 ms, which is fine)
  wire::ReliableReceiver rr;
  bool complete = false;
  WaitFor([&] {
    for (const Rx& r : fake.Of(wire::Type::kChunk)) {
      wire::Chunk c;
      wire::Ack a;
      if (r.token == T && wire::DecodeChunk(r.body, &c)) {
        if (!g_first_msg_id) g_first_msg_id = c.msg_id;
        complete = rr.OnChunk(c, &a) == wire::ReliableReceiver::Result::kComplete || complete;
      }
    }
    return complete;
  }, 1000);
  CHECK(complete);
  uint8_t type = 0;
  const Bytes msg = rr.Take(&type);
  handoff::Handoff h;
  CHECKF(type == wire::kMsgBoot && handoff::Unpack(msg, &h).empty(), "the boot message does not unpack");
  CHECK(h.seed != 7);  // the seed switch without a test dir is ignored
  CHECK(h.session_token != 0 && h.peer == Loop(fake.sock.port) && h.local_port == hp);

  Udp third;
  const online::Status s0 = host.status();
  auto spoof = [&](wire::Type t, const Bytes& body) {
    fake.Send(hp, t, T ^ 0x01000000u, body);  // the peer's address, a wrong token
    third.SendTo(hp, wire::Encode(t, T, body));  // the right token, a third address
  };
  spoof(wire::Type::kBootReply, BootReplyBody(h.session_token, 1, ""));
  wire::Ack ack;
  ack.msg_id = g_first_msg_id;
  ack.have = {~0ull, ~0ull, ~0ull, ~0ull};
  spoof(wire::Type::kAck, wire::EncodeAck(ack));
  spoof(wire::Type::kLeave, LeaveBody(10, wire::LeaveReason::kLeft));
  wire::Chunk ch;
  const Bytes payload(10, 1);
  ch.msg_id = 99;
  ch.msg_type = wire::kMsgBoot;
  ch.count = 1;
  ch.index = 0;
  ch.payload = payload;
  spoof(wire::Type::kChunk, wire::EncodeChunk(ch));
  spoof(wire::Type::kBye, Bytes{});
  SleepMs(300);
  const online::Status s1 = host.status();
  CHECKF(s1.state == State::kStarting, "state %s after the spoofs", online::StateName(s1.state));
  CHECK(hs.restarts == 0 && !fs::exists(hs.HandoffPath()));
  CHECKF(Drops(s1, wire::Drop::kToken) == Drops(s0, wire::Drop::kToken) + 5 && Drops(s1, wire::Drop::kSource) == Drops(s0, wire::Drop::kSource) + 5,
         "kToken %u -> %u, kSource %u -> %u", Drops(s0, wire::Drop::kToken), Drops(s1, wire::Drop::kToken), Drops(s0, wire::Drop::kSource),
         Drops(s1, wire::Drop::kSource));
  CHECK(s1.dropped == s0.dropped + 10);
  // fix round I-2: the right packet token but another body token (the hand-off's ^ 1): not this boot's yes
  fake.Send(hp, wire::Type::kBootReply, T, BootReplyBody(h.session_token ^ 1, 1, ""));
  SleepMs(300);
  CHECKF(host.status().state == State::kStarting, "state %s after a yes with the wrong hand-off token",
         online::StateName(host.status().state));
  CHECK(hs.restarts == 0 && !fs::exists(hs.HandoffPath()));
  // the real reply: the restart, once; the restart callback tries Stop on the I/O thread, which refuses (carry 2d)
  hs.on_restart = [&] { host.Stop(); };
  fake.Send(hp, wire::Type::kBootReply, T, BootReplyBody(h.session_token, 1, ""));
  CHECK(WaitFor([&] { return hs.restarts.load() == 1; }, 1000));
  CHECK(WaitState(host, State::kRestarting, 200));
  CHECK(fs::exists(hs.HandoffPath()));
  CHECKF(WaitFor([&] { return hs.log.Count("Stop called on the I/O thread") == 1; }, 500), "no refusal line for Stop from a callback");
  hs.on_restart = nullptr;
  fake.Send(hp, wire::Type::kBootReply, T, BootReplyBody(h.session_token, 1, ""));  // the joiner repeats it: still one restart
  SleepMs(300);
  CHECK(hs.restarts == 1);
  Bytes written;
  handoff::Handoff hw;
  CHECK(ReadFile(hs.HandoffPath(), &written) && handoff::Unpack(written, &hw).empty());
  CHECK(hw.role == handoff::Role::kHost && hw.session_token == h.session_token && hw.peer == Loop(fake.sock.port));
  host.Stop();  // from the main thread: works
  (void)stop_refused;
}

// carry 2a: msg_ids come from one per-process counter: a second host lobby in this process does not reuse the first's.
// Also: a BootReply whose ok is neither 0 nor 1 counts as a refusal, and its text is sanitised before it is shown.
static void TestMsgIdsAndSanitise() {
  if (!Group("msg ids from a per-process counter; BootReply ok/text sanitised")) return;
  Side hs("msgid_host");
  online::Lobby host;
  if (!StartLobby(host, hs)) return;
  host.Host(0);
  CHECK(WaitState(host, State::kHosting, 1000));
  FakePeer fake;
  const uint32_t T = FakeJoin(fake, host, hs.build);
  CHECK(T != 0 && WaitCanStart(host));
  const uint16_t hp = host.status().port;
  host.StartSession();
  Rx r;
  CHECK(fake.Wait(wire::Type::kChunk, 1000, &r));
  wire::Chunk c;
  CHECK(wire::DecodeChunk(r.body, &c));
  CHECKF(g_first_msg_id != 0 && c.msg_id != g_first_msg_id && c.msg_id > g_first_msg_id, "msg ids %u then %u", g_first_msg_id, c.msg_id);
  // the joiner "refuses" with ok = 2 and a text holding control characters
  fake.Send(hp, wire::Type::kBootReply, T, BootReplyBody(0, 2, "bad\x01\nreason"));
  CHECK(WaitState(host, State::kConnected, 1000));
  CHECKF(Contains(host.status().event, "bad??reason"), "event '%s'", host.status().event.c_str());
  CHECK(hs.restarts == 0 && !fs::exists(hs.HandoffPath()));
}

// Joiner side with a fake host: chunks with a wrong token or from a third address are dropped; a partial transfer with
// no chunk for 5 s is Reset (carry 2a), so the next transfer is taken; the reply, then the restart 700 ms later.
static void TestSpoofJoinerAndRxReset() {
  if (!Group("spoofing, joiner side; a stalled partial transfer is reset after 5 s; the joiner's restart timing")) return;
  Side js("spoofj_join");
  online::Lobby joiner;
  if (!StartLobby(joiner, js)) return;
  FakePeer fake;
  joiner.Join(Addr(fake.sock.port));
  Rx hello;
  CHECK(fake.Wait(wire::Type::kHello, 1000, &hello));
  const uint32_t T = 0x51DE0001u;
  fake.pong_token = T;
  fake.Send(hello.from, wire::Type::kWelcome, 0, WelcomeBody(wire::Refusal::kNone, js.build, "dev", "", T));
  CHECK(WaitState(joiner, State::kConnected, 1000));
  const uint16_t jp = joiner.status().port;
  const uint32_t tok = 0xB007B007u;
  const Bytes msg = PackedHandoff(fake.sock.port, jp, tok);
  CHECK(!msg.empty());
  const std::vector<Bytes> chunks = Chunks(msg, 1000);
  CHECKF(chunks.size() >= 2, "the fixture hand-off needs >= 2 chunks (has %zu)", chunks.size());
  Udp third;
  const online::Status s0 = joiner.status();
  for (const Bytes& b : chunks) {
    fake.Send(jp, wire::Type::kChunk, T ^ 0x00010000u, b);
    third.SendTo(jp, wire::Encode(wire::Type::kChunk, T, b));
  }
  fake.Send(jp, wire::Type::kLeave, T ^ 1, LeaveBody(1, wire::LeaveReason::kLeft));
  third.SendTo(jp, wire::Encode(wire::Type::kLeave, T, LeaveBody(1, wire::LeaveReason::kLeft)));
  SleepMs(300);
  const online::Status s1 = joiner.status();
  CHECK(s1.state == State::kConnected && js.restarts == 0 && !fs::exists(js.HandoffPath()));
  CHECK(fake.Count(wire::Type::kBootReply) == 0 && fake.Count(wire::Type::kAck) == 0);
  const uint32_t n = uint32_t(chunks.size()) + 1;
  CHECKF(Drops(s1, wire::Drop::kToken) == Drops(s0, wire::Drop::kToken) + n && Drops(s1, wire::Drop::kSource) == Drops(s0, wire::Drop::kSource) + n,
         "kToken +%u kSource +%u (want +%u each)", Drops(s1, wire::Drop::kToken) - Drops(s0, wire::Drop::kToken),
         Drops(s1, wire::Drop::kSource) - Drops(s0, wire::Drop::kSource), n);
  // a partial transfer (first chunk of msg 1001), then silence for 5.5 s, then msg 1002 in full
  fake.Send(jp, wire::Type::kChunk, T, Chunks(msg, 1001)[0]);
  CHECK(fake.Wait(wire::Type::kAck, 500));
  SleepMs(5500);
  CHECKF(js.log.Count("partial transfer") == 1, "no reset line after 5 s without a chunk");
  fake.Clear();
  for (const Bytes& b : Chunks(msg, 1002)) fake.Send(jp, wire::Type::kChunk, T, b);
  Rx reply;
  CHECKF(fake.Wait(wire::Type::kBootReply, 1000, &reply), "no BootReply to the second transfer (the stalled one was not reset)");
  wire::BootReply br;
  CHECK(wire::DecodeBootReply(reply.body, &br) && br.ok == 1 && br.session_token == tok && reply.token == T);
  CHECK(WaitFor([&] { return js.restarts.load() == 1; }, 1500));
  const int64_t gap_ms = (js.restart_us.load() - reply.us) / 1000;
  CHECKF(gap_ms >= 700 - 5, "the joiner restarted %lld ms after its first reply (>= 700)", (long long)gap_ms);
  CHECKF(gap_ms < 1000, "the joiner restarted only %lld ms after its first reply", (long long)gap_ms);
  const int replies = fake.Count(wire::Type::kBootReply);
  CHECKF(replies >= 5 && replies <= 9, "%d replies in the 700 ms (every 100 ms)", replies);
  SleepMs(300);
  CHECK(fake.Count(wire::Type::kBootReply) == replies);  // none after the restart
  CHECK(js.restarts == 1 && joiner.status().state == State::kRestarting);
  Bytes written;
  handoff::Handoff hw;
  CHECK(ReadFile(js.HandoffPath(), &written) && handoff::Unpack(written, &hw).empty());
  CHECK(hw.role == handoff::Role::kJoiner && hw.local_port == jp && hw.peer == Loop(fake.sock.port) && hw.session_token == tok);
}

// =====================================================================================================
// Reach out, start, refusals of the start
// =====================================================================================================

static void TestReachOut() {
  if (!Group("reach out: the host punches the joiner's code while the joiner joins")) return;
  Side hs("reach_host"), js("reach_join");
  online::Lobby host, joiner;
  if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
  host.Host(0);
  CHECK(WaitState(host, State::kHosting, 1000));
  host.ReachOut(joiner.status().my_code);
  CHECKF(WaitFor([&] { return js.log.Count("punch") >= 1; }, 1000), "the joiner saw no punch from the host");
  joiner.Join(Addr(host.status().port));
  CHECK(WaitState(host, State::kConnected, 3000) && WaitState(joiner, State::kConnected, 3000));
}

struct Handoffs {
  Bytes host, joiner;
};
static Handoffs g_handoffs;

static void TestStartSession() {
  if (!Group("start session: both hand-offs written and equal where they must be; one restart each, the joiner's >= 700 ms later")) return;
  Side hs("start_host"), js("start_join");
  hs.test_dir = hs.dir / "ctl";
  fs::create_directories(hs.test_dir);
  hs.test["DOA4_NP_SEED"] = "12345678901";
  hs.test["DOA4_NP_DELAY"] = "6";
  online::Lobby host, joiner;
  if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
  CHECK(ConnectPair(host, joiner));
  CHECK(WaitCanStart(host));
  CHECK(host.status().delay == 6);  // the switch is what Start would use
  const uint16_t hp = host.status().port, jp = joiner.status().port;
  host.StartSession();
  CHECK(WaitFor([&] { return hs.restarts.load() == 1 && js.restarts.load() == 1; }, 3000));
  CHECK(host.status().state == State::kRestarting && joiner.status().state == State::kRestarting);
  const int64_t reply_us = js.log.FirstUs("boot reply ok sent");
  CHECKF(reply_us > 0, "no 'boot reply ok sent' line on the joiner");
  CHECKF(js.restart_us.load() - reply_us >= 695'000, "joiner restart %lld ms after its reply", (long long)((js.restart_us.load() - reply_us) / 1000));
  CHECKF(hs.restart_us.load() >= reply_us, "the host restarted before the joiner's reply");
  SleepMs(1200);
  CHECK(hs.restarts == 1 && js.restarts == 1);
  Bytes hb, jb;
  handoff::Handoff h, j;
  CHECK(ReadFile(hs.HandoffPath(), &hb) && ReadFile(js.HandoffPath(), &jb));
  CHECK(handoff::Unpack(hb, &h).empty());
  CHECK(handoff::Unpack(jb, &j).empty());
  CHECK(h.build == j.build && h.build == hs.build.load());
  CHECK(h.release == j.release && h.release == hs.release);
  CHECK(h.delay == 6 && j.delay == 6);
  CHECK(h.seed == 12345678901ull && j.seed == h.seed);
  CHECK(h.session_token != 0 && j.session_token == h.session_token);
  CHECK(h.guest_settings == hs.guest && j.guest_settings == hs.guest);
  bool files_equal = h.files.size() == hs.save.size() && j.files.size() == hs.save.size();
  for (size_t i = 0; files_equal && i < hs.save.size(); ++i)
    files_equal = h.files[i].name == hs.save[i].name && h.files[i].data == hs.save[i].data && j.files[i].name == hs.save[i].name &&
                  j.files[i].data == hs.save[i].data;
  CHECK(files_equal);
  CHECK(h.role == handoff::Role::kHost && j.role == handoff::Role::kJoiner);
  CHECK(h.local_port == hp && j.local_port == jp);
  CHECK(h.peer == Loop(jp) && j.peer == Loop(hp));
  g_handoffs.host = hb;
  g_handoffs.joiner = jb;
  host.Stop();
  joiner.Stop();
}

static void TestJoinerRefusesBoot() {
  if (!Group("start session: the joiner refuses (its build changed) -> host stays Connected with its reason")) return;
  Side hs("refuse_host"), js("refuse_join");
  online::Lobby host, joiner;
  if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
  CHECK(ConnectPair(host, joiner));
  CHECK(WaitCanStart(host));
  js.build = js.build + 1;
  host.StartSession();
  CHECK(WaitFor([&] { return Contains(host.status().event, "different build"); }, 3000));
  CHECKF(host.status().state == State::kConnected, "host state %s", online::StateName(host.status().state));
  CHECK(joiner.status().state == State::kConnected);
  SleepMs(900);
  CHECK(hs.restarts == 0 && js.restarts == 0);
  CHECK(!fs::exists(hs.HandoffPath()) && !fs::exists(js.HandoffPath()));
}

static void TestJoinerSilent() {
  if (!Group("start session: the joiner is silent -> the host is Connected again after 15 s with 'did not answer'")) return;
  Side hs("silent_host"), js("silent_join");
  online::Lobby host, joiner;
  if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
  CHECK(ConnectPair(host, joiner));
  CHECK(WaitCanStart(host));
  joiner.transport().Stop();
  const int64_t t0 = NowUs();
  host.StartSession();
  CHECK(WaitState(host, State::kStarting, 500));
  CHECK(WaitFor([&] { return host.status().state != State::kStarting; }, 16500));
  const int64_t ms = (NowUs() - t0) / 1000;
  std::printf("info: silent joiner: the host left kStarting after %lld ms\n", (long long)ms);
  CHECKF(ms >= 14500 && ms <= 15500, "after %lld ms (15 s +- 0.5 s)", (long long)ms);
  CHECKF(host.status().state == State::kConnected && Contains(host.status().event, "did not answer"), "host %s '%s'",
         online::StateName(host.status().state), host.status().event.c_str());
  CHECK(hs.restarts == 0 && !fs::exists(hs.HandoffPath()));
}

static void TestStartRefusals() {
  if (!Group("start session: no save, or a save over the hand-off limit -> refused before anything is sent")) return;
  Side hs("nosave_host");
  online::Lobby host;
  if (!StartLobby(host, hs)) return;
  host.Host(0);
  CHECK(WaitState(host, State::kHosting, 1000));
  FakePeer fake;
  CHECK(FakeJoin(fake, host, hs.build) != 0 && WaitCanStart(host));
  {
    std::lock_guard<std::mutex> lock(hs.save_mu);
    hs.save_error = "ups.dat cannot be read (sharing violation)";
  }
  host.StartSession();
  CHECK(WaitFor([&] { return Contains(host.status().event, "ups.dat cannot be read (sharing violation)"); }, 1000));
  CHECK(host.status().state == State::kConnected);
  {
    std::lock_guard<std::mutex> lock(hs.save_mu);
    hs.save_error.clear();
    hs.save.clear();
    for (int i = 0; i < 17; ++i) hs.save.push_back(handoff::File{kPre + "00000001/f" + std::to_string(i) + ".dat", Bytes(10, uint8_t(i))});
  }
  host.StartSession();
  CHECK(WaitFor([&] { return Contains(host.status().event, "hand-off limit"); }, 1000));
  CHECKF(Contains(host.status().event, "17") && Contains(host.status().event, "16"), "event '%s'", host.status().event.c_str());
  CHECK(host.status().state == State::kConnected);
  SleepMs(300);
  CHECK(fake.Count(wire::Type::kChunk) == 0);
  CHECK(hs.restarts == 0 && !fs::exists(hs.HandoffPath()));
  // positive control: with a good save the same fake gets chunks (so "no chunk" above was the refusal)
  {
    std::lock_guard<std::mutex> lock(hs.save_mu);
    hs.save = SaveFixture();
  }
  host.StartSession();
  CHECK(fake.Wait(wire::Type::kChunk, 1000));
}

// =====================================================================================================
// The session process
// =====================================================================================================

static void TestSession() {
  if (!Group("session: two processes from the hand-offs reach kInSession; kGekko flows; Leave ends it once")) return;
  handoff::Handoff H, J;
  CHECK(!g_handoffs.host.empty() && handoff::Unpack(g_handoffs.host, &H).empty() && handoff::Unpack(g_handoffs.joiner, &J).empty());
  if (g_handoffs.host.empty()) return;
  Side hs("sess_host"), js("sess_join");
  std::mutex mu;
  std::vector<std::pair<int32_t, wire::LeaveReason>> left_h, left_j;
  online::Lobby host, joiner;
  std::string e;
  const int64_t t0 = NowUs();
  CHECKF(host.StartInSession(hs.P(), LoopCfg(), H, [&](int32_t f, wire::LeaveReason r) {
    std::lock_guard<std::mutex> lock(mu);
    left_h.emplace_back(f, r);
  }, &e), "host StartInSession: %s", e.c_str());
  CHECK(host.status().port == H.local_port);
  CHECK(host.status().state == State::kConnecting);
  // before the joiner is back: an early kGekko (gekko_early, not a drop), a Hello with the token from another address
  // (kSource, never taken: carry 2c), a Hello with another token (Welcome kSession, kToken)
  Udp third;
  third.SendTo(H.local_port, wire::Encode(wire::Type::kGekko, H.session_token, Bytes{1, 2, 3}));
  third.SendTo(H.local_port, wire::Encode(wire::Type::kHello, H.session_token, HelloBody(H.build, "dev", "", 1, 2, H.session_token)));
  const Bytes wrong_hello = wire::Encode(wire::Type::kHello, H.session_token + 1, HelloBody(H.build, "dev", kFilesText, 1, 2, H.session_token + 1));
  third.SendTo(H.local_port, wrong_hello);
  Bytes got;
  bool welcome_session = false;
  WaitFor([&] {
    Bytes d;
    while (third.Recv(5, &d)) {
      wire::Packet pk;
      wire::Welcome w;
      if (wire::Decode(d, &pk) == wire::Drop::kNone && pk.type == wire::Type::kWelcome && wire::DecodeWelcome(pk.body, &w) &&
          w.refusal == wire::Refusal::kSession && d.size() <= wrong_hello.size())
        welcome_session = true;
    }
    return welcome_session;
  }, 1000);
  CHECK(welcome_session);
  SleepMs(100);
  online::Status s = host.status();
  CHECKF(s.gekko_early == 1 && Drops(s, wire::Drop::kSource) == 1 && Drops(s, wire::Drop::kToken) == 1 && s.dropped == 2,
         "gekko_early %u kSource %u kToken %u dropped %u", s.gekko_early, Drops(s, wire::Drop::kSource), Drops(s, wire::Drop::kToken), s.dropped);
  CHECK(s.state == State::kConnecting);
  CHECK(hs.log.Count("another address") == 1);
  // the joiner comes back 5 s after the host
  while (NowUs() - t0 < 5'000'000) SleepMs(10);
  CHECKF(joiner.StartInSession(js.P(), LoopCfg(), J, [&](int32_t f, wire::LeaveReason r) {
    std::lock_guard<std::mutex> lock(mu);
    left_j.emplace_back(f, r);
  }, &e), "joiner StartInSession: %s", e.c_str());
  CHECK(WaitFor([&] { return host.status().state == State::kInSession && joiner.status().state == State::kInSession; },
                int(7000 - (NowUs() - t0) / 1000)));
  std::printf("info: session: both in session %lld ms after the host started\n", (long long)((NowUs() - t0) / 1000));
  CHECK(host.status().is_host && !joiner.status().is_host);
  // GekkoNet traffic flows
  CHECK(host.transport().PushGekko(Bytes{9, 8, 7}));
  CHECK(WaitFor([&] { return joiner.transport().PopGekko(&got); }, 1000) && got == (Bytes{9, 8, 7}));
  CHECK(WaitFor([&] { return host.status().ping_samples >= 2 && joiner.status().ping_samples >= 2; }, 2000));
  // a third socket's kGekko with the right token: kSource at the joiner
  const uint32_t src0 = Drops(joiner.status(), wire::Drop::kSource);
  third.SendTo(J.local_port, wire::Encode(wire::Type::kGekko, J.session_token, Bytes{1}));
  SleepMs(150);
  CHECK(Drops(joiner.status(), wire::Drop::kSource) == src0 + 1);
  CHECK(!joiner.transport().PopGekko(&got));
  // Leave: on_peer_left once on the joiner, nothing on the host
  host.SendLeave(1234, wire::LeaveReason::kLeft);
  CHECK(WaitFor([&] { std::lock_guard<std::mutex> lock(mu); return !left_j.empty(); }, 1000));
  SleepMs(500);
  {
    std::lock_guard<std::mutex> lock(mu);
    CHECKF(left_j.size() == 1, "on_peer_left ran %zu times on the joiner", left_j.size());
    if (!left_j.empty()) CHECK(left_j[0].first == 1234 && left_j[0].second == wire::LeaveReason::kLeft);
    CHECK(left_h.empty());
  }
  host.Stop();
  joiner.Stop();
}

static void TestSessionSpoof() {
  if (!Group("session spoofing: Leave/Bye need the peer's address and token; a negative frame is clamped to -1")) return;
  Side ss("sspoof");
  FakePeer fake;
  handoff::Handoff h;
  h.build = 1;
  h.release = "dev";
  h.role = handoff::Role::kHost;
  h.delay = 2;
  h.local_port = DeadPort();
  h.peer = Loop(fake.sock.port);
  h.session_token = 0x5E55AAAAu;
  h.seed = 1;
  h.guest_settings = GuestText();
  std::mutex mu;
  std::vector<std::pair<int32_t, wire::LeaveReason>> left;
  online::Lobby lobby;
  std::string e;
  CHECKF(lobby.StartInSession(ss.P(), LoopCfg(), h, [&](int32_t f, wire::LeaveReason r) {
    std::lock_guard<std::mutex> lock(mu);
    left.emplace_back(f, r);
  }, &e), "StartInSession: %s", e.c_str());
  const uint16_t lp = h.local_port;
  CHECK(fake.Wait(wire::Type::kHello, 1000));  // the session's Hello reaches the hand-off's peer
  fake.pong_token = h.session_token;
  fake.Send(lp, wire::Type::kHello, h.session_token, HelloBody(1, "dev", "", 1, 2, h.session_token));
  CHECK(WaitState(lobby, State::kInSession, 1000));
  Udp third;
  const online::Status s0 = lobby.status();
  fake.Send(lp, wire::Type::kLeave, h.session_token ^ 4, LeaveBody(5, wire::LeaveReason::kDesync));
  third.SendTo(lp, wire::Encode(wire::Type::kLeave, h.session_token, LeaveBody(5, wire::LeaveReason::kDesync)));
  fake.Send(lp, wire::Type::kBye, h.session_token ^ 4, Bytes{});
  third.SendTo(lp, wire::Encode(wire::Type::kBye, h.session_token, Bytes{}));
  SleepMs(300);
  const online::Status s1 = lobby.status();
  {
    std::lock_guard<std::mutex> lock(mu);
    CHECKF(left.empty(), "a spoofed Leave/Bye called on_peer_left (%zu)", left.size());
  }
  CHECK(s1.state == State::kInSession);
  CHECK(Drops(s1, wire::Drop::kToken) == Drops(s0, wire::Drop::kToken) + 2 && Drops(s1, wire::Drop::kSource) == Drops(s0, wire::Drop::kSource) + 2);
  fake.Send(lp, wire::Type::kLeave, h.session_token, LeaveBody(-5, wire::LeaveReason::kDesync));
  CHECK(WaitFor([&] { std::lock_guard<std::mutex> lock(mu); return !left.empty(); }, 1000));
  fake.Send(lp, wire::Type::kBye, h.session_token, Bytes{});
  fake.Send(lp, wire::Type::kLeave, h.session_token, LeaveBody(9, wire::LeaveReason::kLeft));
  SleepMs(300);
  std::lock_guard<std::mutex> lock(mu);
  CHECKF(left.size() == 1, "on_peer_left ran %zu times", left.size());
  if (!left.empty()) CHECKF(left[0].first == -1 && left[0].second == wire::LeaveReason::kDesync, "frame %d reason %d", left[0].first, int(left[0].second));
}

// =====================================================================================================
// Request files (the harness contract of Task 10)
// =====================================================================================================

static std::map<std::string, std::string> ParseStatus(const std::string& text, bool* ok) {
  static const char* const kKeys[] = {"state", "port", "peer_port", "ping", "delay", "can_start", "event", "refusal"};
  std::map<std::string, std::string> m;
  *ok = !text.empty() && text.back() == '\n';
  std::istringstream in(text);
  std::string line;
  size_t i = 0;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (i >= 8 || eq == std::string::npos || line.substr(0, eq) != kKeys[i]) {
      *ok = false;
      break;
    }
    m[line.substr(0, eq)] = line.substr(eq + 1);
    ++i;
  }
  *ok = *ok && i == 8;
  return m;
}
static void WriteCtl(const fs::path& dir, const std::string& text) {
  const fs::path tmp = dir / "np_ctl.tmp", on = dir / "np_ctl.on";
  {
    std::ofstream f(tmp, std::ios::binary);
    f << text;
  }
  for (int i = 0; i < 200; ++i) {  // the lobby may not have taken the previous one yet
    if (MoveFileExW(tmp.c_str(), on.c_str(), 0)) return;
    SleepMs(5);
  }
}

static void TestRequestFiles() {
  if (!Group("request files: np_ctl.on commands, status.txt rewritten atomically")) return;
  Side s("ctl");
  s.test_dir = s.dir / "ctl";
  fs::create_directories(s.test_dir);
  online::Lobby lobby;
  if (!StartLobby(lobby, s)) return;
  const fs::path st = s.test_dir / "status.txt";
  const int64_t t0 = NowUs();
  WriteCtl(s.test_dir, "host 0\n");
  bool ok = false;
  CHECKF(WaitFor([&] {
    bool p = false;
    const auto m = ParseStatus(ReadText(st), &p);
    ok = p && m.count("state") && m.at("state") == "hosting";
    return ok;
  }, 1000), "status.txt did not say state=hosting within 1 s: '%s'", ReadText(st).c_str());
  std::printf("info: request file: state=hosting in status.txt %lld ms after np_ctl.on\n", (long long)((NowUs() - t0) / 1000));
  CHECK(WaitFor([&] { return !fs::exists(s.test_dir / "np_ctl.on"); }, 500));  // taken
  {
    bool p = false;
    const auto m = ParseStatus(ReadText(st), &p);
    CHECK(p && m.at("port") == std::to_string(lobby.status().port) && m.at("can_start") == "0" && m.at("peer_port") == "0");
  }
  // the refusal key (Task 10's harness reads it): none, then build after a refused Hello, none again after a new host
  CHECK(WaitFor([&] { bool p = false; const auto m = ParseStatus(ReadText(st), &p); return p && m.at("refusal") == "none"; }, 1000));
  {
    Udp u;
    u.SendTo(lobby.status().port, wire::Encode(wire::Type::kHello, 0, HelloBody(s.build + 1, "dev", kFilesText)));
  }
  CHECKF(WaitFor([&] { bool p = false; const auto m = ParseStatus(ReadText(st), &p); return p && m.at("refusal") == "build"; }, 1500),
         "status.txt after a refused Hello: '%s'", ReadText(st).c_str());
  WriteCtl(s.test_dir, "host 0\n");
  CHECK(WaitFor([&] { bool p = false; const auto m = ParseStatus(ReadText(st), &p); return p && m.at("refusal") == "none"; }, 1500));
  // joiner side: a lobby with its own test dir joins a fake host that answers kBusy -> refusal=busy
  {
    Side js("ctl_join");
    js.test_dir = js.dir / "ctl";
    fs::create_directories(js.test_dir);
    online::Lobby joiner;
    if (StartLobby(joiner, js)) {
      FakePeer fake;
      joiner.Join(Addr(fake.sock.port));
      Rx hello;
      CHECK(fake.Wait(wire::Type::kHello, 1000, &hello));
      fake.Send(hello.from, wire::Type::kWelcome, 0, WelcomeBody(wire::Refusal::kBusy, js.build, "dev", "busy", 0));
      const fs::path jst = js.test_dir / "status.txt";
      CHECKF(WaitFor([&] { bool p = false; const auto m = ParseStatus(ReadText(jst), &p); return p && m.at("refusal") == "busy"; }, 1500),
             "joiner status.txt: '%s'", ReadText(jst).c_str());
    }
  }
  WriteCtl(s.test_dir, "delay 5\n");
  CHECK(WaitFor([&] { bool p = false; const auto m = ParseStatus(ReadText(st), &p); return p && m.at("delay") == "5"; }, 1000));
  // 200 reads while the writer rewrites it (bursts of 40 "status" commands every 100 ms): every read that opened parses
  std::atomic<bool> done{false};
  std::string burst;
  for (int i = 0; i < 40; ++i) burst += "status\n";
  std::thread writer([&] {
    while (!done.load()) {
      WriteCtl(s.test_dir, burst);
      SleepMs(100);
    }
  });
  int opened = 0, parsed = 0;
  for (int i = 0; i < 200; ++i) {
    std::ifstream f(st, std::ios::binary);
    if (f) {
      std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      ++opened;
      bool p = false;
      ParseStatus(text, &p);
      if (p) ++parsed;
      else std::printf("info: half-written read: '%s'\n", text.c_str());
    }
    SleepMs(10);
  }
  done = true;
  writer.join();
  std::printf("info: status.txt: %d of 200 reads opened, %d parsed; %d status rewrites logged\n", opened, parsed,
              s.log.Count("status.txt"));
  CHECKF(opened >= 100 && parsed == opened, "%d opened, %d parsed", opened, parsed);
  // a bad command is logged, not fatal
  WriteCtl(s.test_dir, "fly away\nstatus\n");
  CHECK(WaitFor([&] { return s.log.Count("unknown request") == 1; }, 1000));
  CHECK(lobby.status().state == State::kHosting);
}

// =====================================================================================================
// Connect timeout, with fake STUN servers on loopback
// =====================================================================================================

static Bytes StunSuccess(const uint8_t* tid, const rules::Endpoint& e) {
  Bytes r = {0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42};
  r.insert(r.end(), tid, tid + 12);
  const uint16_t xp = uint16_t(e.port ^ 0x2112);
  const uint8_t ck[4] = {0x21, 0x12, 0xA4, 0x42};
  Bytes attr = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, uint8_t(xp >> 8), uint8_t(xp)};
  for (int i = 0; i < 4; ++i) attr.push_back(uint8_t(e.ip[size_t(i)] ^ ck[i]));
  r.insert(r.end(), attr.begin(), attr.end());
  return r;
}
struct FakeStun {
  Udp sock;
  rules::Endpoint answer;
  std::atomic<bool> stop{false};
  std::thread th;
  void Begin() {
    th = std::thread([this] {
      while (!stop.load()) {
        Bytes d;
        uint16_t from = 0;
        if (!sock.Recv(5, &d, &from)) continue;
        if (d.size() != 20 || d[0] != 0 || d[1] != 1 || d[4] != 0x21) continue;
        sock.SendTo(from, StunSuccess(d.data() + 8, answer));
      }
    });
  }
  ~FakeStun() {
    stop = true;
    if (th.joinable()) th.join();
  }
};
static rules::Endpoint Pub(uint16_t port) {
  rules::Endpoint e;
  e.ip = {203, 0, 113, 7};
  e.port = port;
  return e;
}

static void TestConnectTimeout() {
  if (!Group("connect timeout: Offline after connect_timeout_ms with ConnectTimeoutText(nat) (fake STUN: cone, symmetric)")) return;
  for (rules::Nat want : {rules::Nat::kCone, rules::Nat::kSymmetric}) {
    FakeStun f1, f2;
    f1.answer = Pub(51234);
    f2.answer = Pub(want == rules::Nat::kCone ? 51234 : 51235);
    f1.Begin();
    f2.Begin();
    Side js("timeout_join");
    net::Config c = LoopCfg();
    c.stun = true;
    c.stun_servers = {Addr(f1.sock.port), Addr(f2.sock.port)};
    c.stun_refresh_ms = 300;
    online::Lobby joiner;
    if (!StartLobby(joiner, js, c)) return;
    CHECKF(WaitFor([&] { return joiner.status().nat == want; }, 2000), "nat %d, want %d", int(joiner.status().nat), int(want));
    CHECK(joiner.status().code_note == rules::NatNote(want));
    rules::Endpoint ep;
    CHECK(rules::ParseJoinCode(joiner.status().my_code, &ep) && ep == Pub(51234));  // the code carries the mapped address
    const int64_t t0 = NowUs();
    joiner.Join(Addr(DeadPort()));
    CHECK(WaitState(joiner, State::kConnecting, 200));
    CHECK(WaitFor([&] { return joiner.status().state != State::kConnecting; }, 3000));
    const int64_t ms = (NowUs() - t0) / 1000;
    std::printf("info: connect timeout (nat %d): Offline after %lld ms\n", int(want), (long long)ms);
    CHECKF(ms >= 2000 && ms < 2500, "Offline after %lld ms (2000 <= t < 2500)", (long long)ms);
    CHECK(joiner.status().state == State::kOffline);
    CHECKF(joiner.status().event == rules::ConnectTimeoutText(want), "event '%s'", joiner.status().event.c_str());
    if (want == rules::Nat::kSymmetric) CHECK(joiner.status().event != rules::ConnectTimeoutText(rules::Nat::kCone));
  }
}

// =====================================================================================================
// carry 2d: replies to unauthenticated sources - at most 10 per 1,000 ms, never larger than the trigger
// =====================================================================================================

static void TestReplyBounds() {
  if (!Group("replies to strangers: <= 10 per second in total, never larger than the datagram that triggered them")) return;
  Side hs("bounds_host");
  online::Lobby host;
  if (!StartLobby(host, hs)) return;
  host.Host(0);
  CHECK(WaitState(host, State::kHosting, 1000));
  const uint16_t hp = host.status().port;
  Udp a;
  const Bytes bad = wire::Encode(wire::Type::kHello, 0, HelloBody(hs.build + 1, "", kFilesText));  // a build refusal each
  for (int i = 0; i < 50; ++i) a.SendTo(hp, bad);
  int replies = 0, oversize = 0;
  int64_t first_us = 0;
  const int64_t t0 = NowUs();
  while (NowUs() - t0 < 900'000) {
    Bytes d;
    if (!a.Recv(10, &d)) continue;
    if (!first_us) first_us = NowUs();
    ++replies;
    oversize += d.size() > bad.size() ? 1 : 0;
  }
  CHECKF(replies == 10, "%d replies to 50 Hellos within 900 ms (want exactly 10)", replies);
  CHECK(oversize == 0);
  // the window slides: 1.1 s after the first reply, 3 more get 3 replies
  while (NowUs() - first_us < 1'100'000) SleepMs(5);
  for (int i = 0; i < 3; ++i) a.SendTo(hp, bad);
  int more = 0;
  for (Bytes d; a.Recv(300, &d);) ++more;
  CHECKF(more == 3, "%d replies to 3 Hellos after the window", more);
  SleepMs(1100);
  // size: a 30-byte Hello cannot carry back even an empty refusal ("dev": 32 bytes) -> nothing; 36 bytes -> a reply <= 36
  const Bytes tiny = wire::Encode(wire::Type::kHello, 0, HelloBody(hs.build + 1, "", ""));
  CHECK(tiny.size() == 30);
  a.SendTo(hp, tiny);
  Bytes d;
  CHECK(!a.Recv(300, &d));
  const Bytes small = wire::Encode(wire::Type::kHello, 0, HelloBody(hs.build + 1, "", "x=1234"));
  CHECK(small.size() == 36);
  a.SendTo(hp, small);
  CHECK(a.Recv(300, &d));
  CHECKF(d.size() <= small.size(), "reply of %zu bytes to %zu", d.size(), small.size());
  wire::Packet pk;
  wire::Welcome w;
  CHECK(wire::Decode(d, &pk) == wire::Drop::kNone && wire::DecodeWelcome(pk.body, &w) && w.refusal == wire::Refusal::kBuild);
  CHECK(host.status().state == State::kHosting);
}

static void TestProto() {
  if (!Group("another protocol version: the host answers in its own; the joiner goes Offline with advice")) return;
  Side hs("proto_host");
  online::Lobby host;
  if (!StartLobby(host, hs)) return;
  host.Host(0);
  CHECK(WaitState(host, State::kHosting, 1000));
  const uint16_t hp = host.status().port;
  Udp a;
  Bytes foreign = wire::Encode(wire::Type::kHello, 0, Bytes(40, 1));
  foreign[4] = uint8_t(wire::kProto + 1);
  a.SendTo(hp, foreign);
  Bytes d;
  CHECK(a.Recv(500, &d));
  CHECKF(d.size() <= foreign.size() && d.size() >= wire::kHeaderBytes, "reply %zu bytes", d.size());
  wire::Packet pk;
  wire::Welcome w;
  CHECK(wire::Decode(d, &pk) == wire::Drop::kNone && pk.type == wire::Type::kWelcome && wire::DecodeWelcome(pk.body, &w) &&
        w.refusal == wire::Refusal::kProto);
  CHECKF(Contains(host.status().event, "network protocol"), "host event '%s'", host.status().event.c_str());
  CHECK(host.status().refusal == "proto");
  Bytes bare = wire::Encode(wire::Type::kHello, 0, Bytes{});  // 12 bytes: no answer fits
  bare[4] = uint8_t(wire::kProto + 1);
  a.SendTo(hp, bare);
  CHECK(!a.Recv(300, &d));
  // the joiner: its target answers in protocol kProto + 1
  Side js("proto_join");
  online::Lobby joiner;
  if (!StartLobby(joiner, js)) return;
  FakePeer fake;
  Udp stranger;
  joiner.Join(Addr(fake.sock.port));
  Rx hello;
  CHECK(fake.Wait(wire::Type::kHello, 1000, &hello));
  Bytes back = wire::Encode(wire::Type::kWelcome, 0, Bytes(30, 0));
  back[4] = uint8_t(wire::kProto + 1);
  stranger.SendTo(hello.from, back);  // from someone else: ignored (counted kProto by the transport)
  SleepMs(200);
  CHECK(joiner.status().state == State::kConnecting);
  fake.sock.SendTo(hello.from, back);
  CHECK(WaitState(joiner, State::kOffline, 1000));
  const std::string ev = joiner.status().event;
  CHECKF(Contains(ev, "network protocol " + std::to_string(wire::kProto + 1)) && Contains(ev, "yours " + std::to_string(wire::kProto)) &&
             Contains(ev, "Check for Updates") && joiner.status().refusal == "proto",
         "joiner event '%s'", ev.c_str());
}

// =====================================================================================================

// =====================================================================================================
// Fix round 1
// =====================================================================================================

// The hand-off a fake joiner received over the chunks carrying token T (all the chunks it has seen; Clear it before
// a second start). False when no complete boot message arrived within ms.
// With ack_to, the fake also acknowledges the whole message to that port (as a joiner that received it), so the
// host's transfer is complete: only a BootReply may then restart it.
static bool BootFrom(FakePeer& f, uint32_t T, handoff::Handoff* h, int ms = 1000, uint16_t ack_to = 0) {
  std::vector<uint8_t> msg;
  wire::Ack done;
  WaitFor([&] {
    wire::ReliableReceiver rr;
    for (const Rx& r : f.Of(wire::Type::kChunk)) {
      wire::Chunk c;
      wire::Ack a;
      if (r.token == T && wire::DecodeChunk(r.body, &c) && rr.OnChunk(c, &a) == wire::ReliableReceiver::Result::kComplete) {
        uint8_t type = 0;
        msg = rr.Take(&type);
        done = a;
        return true;
      }
    }
    return false;
  }, ms);
  if (!msg.empty() && ack_to) f.Send(ack_to, wire::Type::kAck, T, wire::EncodeAck(done));
  return !msg.empty() && handoff::Unpack(msg, h).empty();
}

// I-1b: every yes is lost, the host gives up at 15 s ('did not answer'); a late yes for that boot still restarts it
// with the same hand-off. A second StartSession ends that window: a yes for the OLD token is then ignored. Two pairs
// run at once, so the 15 s are waited once.
static void TestLateBootReply() {
  if (!Group("fix I-1b: a late yes restarts the host with the same hand-off; a yes for an older boot does not")) return;
  Side as("late_a"), bs("late_b");
  online::Lobby a, b;
  if (!StartLobby(a, as) || !StartLobby(b, bs)) return;
  a.Host(0);
  b.Host(0);
  CHECK(WaitState(a, State::kHosting, 1000) && WaitState(b, State::kHosting, 1000));
  FakePeer fa, fb;
  const uint32_t Ta = FakeJoin(fa, a, as.build), Tb = FakeJoin(fb, b, bs.build);
  CHECK(Ta != 0 && Tb != 0 && WaitCanStart(a) && WaitCanStart(b));
  const uint16_t ap = a.status().port, bp = b.status().port;
  a.StartSession();
  b.StartSession();
  handoff::Handoff ha, hb1;
  // the fakes acknowledge every chunk (the transfers complete) and never answer (every yes lost): neither host may
  // restart on the acks (plan mutant (a), now deterministic)
  CHECK(BootFrom(fa, Ta, &ha, 1000, ap) && BootFrom(fb, Tb, &hb1, 1000, bp));
  SleepMs(500);
  CHECKF(a.status().state == State::kStarting && b.status().state == State::kStarting && as.restarts == 0 && bs.restarts == 0,
         "after the acks: A %s B %s, restarts %d/%d", online::StateName(a.status().state), online::StateName(b.status().state),
         as.restarts.load(), bs.restarts.load());
  CHECK(WaitFor([&] { return a.status().state == State::kConnected && b.status().state == State::kConnected; }, 16500));
  CHECKF(Contains(a.status().event, "did not answer") && Contains(b.status().event, "did not answer"), "events '%s' / '%s'",
         a.status().event.c_str(), b.status().event.c_str());
  // B: a newer start, then a yes for the OLD token
  fb.Clear();
  b.StartSession();
  CHECK(WaitState(b, State::kStarting, 500));
  handoff::Handoff hb2;
  CHECK(BootFrom(fb, Tb, &hb2) && hb2.session_token != hb1.session_token);
  fb.Send(bp, wire::Type::kBootReply, Tb, BootReplyBody(hb1.session_token, 1, ""));
  // A: the late yes for its (only) boot
  fa.Send(ap, wire::Type::kBootReply, Ta, BootReplyBody(ha.session_token, 1, ""));
  CHECKF(WaitFor([&] { return as.restarts.load() == 1; }, 1000), "the late yes did not restart the host (state %s)",
         online::StateName(a.status().state));
  Bytes written;
  handoff::Handoff hw;
  CHECK(ReadFile(as.HandoffPath(), &written) && handoff::Unpack(written, &hw).empty());
  CHECK(hw.session_token == ha.session_token && hw.seed == ha.seed && hw.role == handoff::Role::kHost);
  SleepMs(300);
  CHECKF(b.status().state == State::kStarting && bs.restarts == 0 && !fs::exists(bs.HandoffPath()),
         "a yes for the old boot moved B: %s, restarts %d", online::StateName(b.status().state), bs.restarts.load());
  // positive control: the yes for B's current boot restarts it
  fb.Send(bp, wire::Type::kBootReply, Tb, BootReplyBody(hb2.session_token, 1, ""));
  CHECK(WaitFor([&] { return bs.restarts.load() == 1; }, 1000));
}

static handoff::Handoff SessionHandoff(uint16_t peer_port, uint32_t token) {
  handoff::Handoff h;
  h.build = 0x0DA4000000000001ull;
  h.release = "dev";
  h.role = handoff::Role::kJoiner;
  h.delay = 2;
  h.local_port = DeadPort();
  h.peer = Loop(peer_port);
  h.session_token = token;
  h.seed = 1;
  h.guest_settings = GuestText();
  return h;
}

// I-1a: the restarted side's session Hello reaches a game that did NOT restart (a lobby): the lobby answers kSession and
// the session process gives up at once (Status::session_refused), well before np_live's frame-0 limit.
static void TestSessionRefused() {
  if (!Group("fix I-1a: a lobby answers a session Hello with kSession; the session process gives up within 1 s")) return;
  Side hs("sref_host"), ss("sref_sess");
  online::Lobby host;
  if (!StartLobby(host, hs)) return;
  host.Host(0);
  CHECK(WaitState(host, State::kHosting, 1000));
  std::atomic<int> left{0};
  online::Lobby sess;
  std::string e;
  const int64_t t0 = NowUs();
  CHECKF(sess.StartInSession(ss.P(), LoopCfg(), SessionHandoff(host.status().port, 0x5E55BEEFu),
                             [&](int32_t, wire::LeaveReason) { ++left; }, &e),
         "StartInSession: %s", e.c_str());
  CHECKF(WaitFor([&] { return sess.status().session_refused; }, 1000), "the session process did not give up within 1 s (state %s)",
         online::StateName(sess.status().state));
  std::printf("info: session refused %lld ms after StartInSession\n", (long long)((NowUs() - t0) / 1000));
  const online::Status s = sess.status();
  CHECK(s.state == State::kOffline);
  CHECKF(Contains(s.event, "did not start the session"), "event '%s'", s.event.c_str());
  CHECKF(s.refusal == "session" && host.status().refusal == "session", "refusal %s / %s", s.refusal.c_str(), host.status().refusal.c_str());
  CHECK(host.status().state == State::kHosting && Drops(host.status(), wire::Drop::kToken) >= 1);
  // the source rule: a Welcome kSession from anyone but the hand-off's peer changes nothing
  FakePeer quiet;
  Side qs("sref_quiet");
  online::Lobby q;
  CHECK(q.StartInSession(qs.P(), LoopCfg(), SessionHandoff(quiet.sock.port, 0x5E55BEEFu), nullptr, &e));
  CHECK(quiet.Wait(wire::Type::kHello, 1000));
  Udp third;
  const Bytes ks = WelcomeBody(wire::Refusal::kSession, 1, "", "no", 0);
  third.SendTo(q.status().port, wire::Encode(wire::Type::kWelcome, 0, ks));
  SleepMs(300);
  CHECK(!q.status().session_refused && q.status().state == State::kConnecting);
  CHECK(Drops(q.status(), wire::Drop::kSource) >= 1);
  quiet.sock.SendTo(q.status().port, wire::Encode(wire::Type::kWelcome, 0, ks));  // from the peer: taken
  CHECK(WaitFor([&] { return q.status().session_refused; }, 1000));
}

// I-3: refusals of strangers cannot flood the event line, game.log or events.txt: one per refusal kind per 10 s.
static void TestStrangerEvents() {
  if (!Group("fix I-3: a flood of refused Hellos and foreign datagrams gives at most one event per kind")) return;
  Side hs("flood_host");
  online::Lobby host;
  if (!StartLobby(host, hs)) return;
  host.Host(0);
  CHECK(WaitState(host, State::kHosting, 1000));
  const uint16_t hp = host.status().port;
  Udp a;
  for (int i = 0; i < 200; ++i) {
    a.SendTo(hp, wire::Encode(wire::Type::kHello, 0, HelloBody(hs.build + 1, "netplay-v1.0." + std::to_string(i), kFilesText)));
    char f[64];
    std::snprintf(f, sizeof f, "anm.afs=%016x\n", i);
    a.SendTo(hp, wire::Encode(wire::Type::kHello, 0, HelloBody(hs.build, "", f)));
    Bytes d = wire::Encode(wire::Type::kHello, 0, Bytes(40, 1));
    d[4] = uint8_t(wire::kProto + 1 + i % 50);
    a.SendTo(hp, d);
    if (i % 20 == 19) SleepMs(2);
  }
  SleepMs(500);
  const int build_lines = hs.log.Count("event: Refused a player with a different build");
  const int files_lines = hs.log.Count("event: Refused a player: ");
  const int proto_lines = hs.log.Count("event: Refused a player whose game speaks network protocol");
  std::printf("info: flood of 600: event lines build %d, files %d, protocol %d\n", build_lines, files_lines, proto_lines);
  CHECKF(build_lines == 1 && files_lines == 1 && proto_lines == 1, "event lines build %d files %d proto %d (want 1 each)", build_lines,
         files_lines, proto_lines);
  const std::string ev = hs.Events();
  const int ev_lines = int(std::count(ev.begin(), ev.end(), '\n'));
  CHECKF(ev_lines <= 4, "events.txt has %d lines after the flood", ev_lines);  // Hosting + one per kind
  CHECK(host.status().state == State::kHosting);
}

// M-2: a joiner that cancels inside the 700 ms before its restart deletes the hand-off it wrote, and does not restart.
static void TestJoinerCancel() {
  if (!Group("fix M-2: the joiner withdraws its hand-off when it cancels before the restart")) return;
  Side js("cancel_join");
  online::Lobby joiner;
  if (!StartLobby(joiner, js)) return;
  FakePeer fake;
  joiner.Join(Addr(fake.sock.port));
  Rx hello;
  CHECK(fake.Wait(wire::Type::kHello, 1000, &hello));
  const uint32_t T = 0x51DE0002u;
  fake.pong_token = T;
  fake.Send(hello.from, wire::Type::kWelcome, 0, WelcomeBody(wire::Refusal::kNone, js.build, "dev", "", T));
  CHECK(WaitState(joiner, State::kConnected, 1000));
  const uint16_t jp = joiner.status().port;
  for (const Bytes& b : Chunks(PackedHandoff(fake.sock.port, jp, 0xB007B008u), 2000)) fake.Send(jp, wire::Type::kChunk, T, b);
  CHECK(fake.Wait(wire::Type::kBootReply, 1000));
  CHECK(fs::exists(js.HandoffPath()));  // precondition: written before the yes went out
  joiner.Disconnect();
  CHECK(WaitState(joiner, State::kOffline, 300));
  SleepMs(1000);
  CHECKF(js.restarts == 0 && !fs::exists(js.HandoffPath()), "restarts %d, hand-off still there: %d", js.restarts.load(),
         int(fs::exists(js.HandoffPath())));
}

// M-3: with Config::upnp the lobby maps its port when hosting, and a session process maps its port again; Stop unmaps.
static void TestUpnpCalls() {
  if (!Group("fix M-3: UPnP mapped by Host and by StartInSession, unmapped by Stop (stand-ins)")) return;
  const int before = UpnpCalls();
  net::Config c = LoopCfg();
  c.upnp = true;
  {
    Side s("upnp_lobby");
    online::Lobby l;
    const int m0 = g_upnp_map, u0 = g_upnp_unmap;
    if (!StartLobby(l, s, c)) return;
    l.Host(0);
    CHECK(WaitFor([&] { return g_upnp_map.load() == m0 + 1; }, 1000));
    CHECK(g_upnp_port.load() == l.status().port);
    l.Stop();
    CHECK(g_upnp_unmap.load() == u0 + 1);
  }
  {
    Side s("upnp_sess");
    online::Lobby l;
    const int m0 = g_upnp_map, u0 = g_upnp_unmap;
    const handoff::Handoff h = SessionHandoff(DeadPort(), 0x77u);
    std::string e;
    CHECKF(l.StartInSession(s.P(), c, h, nullptr, &e), "StartInSession: %s", e.c_str());
    CHECKF(g_upnp_map.load() == m0 + 1 && g_upnp_port.load() == h.local_port, "session map calls %d, port %u (want %u)",
           g_upnp_map.load() - m0, g_upnp_port.load(), h.local_port);
    l.Stop();
    CHECK(g_upnp_unmap.load() == u0 + 1);
  }
  g_upnp_expected += UpnpCalls() - before;
}

// M-4: a junk flood of >= 10 Hellos/s fills the stranger budget; the real joiner still connects (its Welcome is not a
// stranger reply).
static void TestJunkFloodJoin() {
  if (!Group("fix M-4: a junk-Hello flood does not keep the real joiner out")) return;
  Side hs("junk_host"), js("junk_join");
  online::Lobby host, joiner;
  if (!StartLobby(host, hs) || !StartLobby(joiner, js)) return;
  host.Host(0);
  CHECK(WaitState(host, State::kHosting, 1000));
  const uint16_t hp = host.status().port;
  std::atomic<bool> stop{false};
  std::atomic<int> sent{0};
  std::thread junk([&] {
    Udp u;
    const Bytes d = wire::Encode(wire::Type::kHello, 0, HelloBody(hs.build + 1, "", kFilesText));
    // about 5,000 a second, paced by a 1 ms spin (Sleep(1) rounds up to the 15.6 ms timer tick here, and the budget
    // refills all 10 slots at once every second: with a 15 ms gap the joiner's Hello slipped into the refill gap)
    while (!stop.load()) {
      for (int i = 0; i < 5; ++i) u.SendTo(hp, d);
      sent += 5;
      const int64_t until = NowUs() + 1000;
      while (NowUs() < until) std::this_thread::yield();
    }
  });
  SleepMs(1200);  // the stranger budget is full from here on
  joiner.Join(Addr(hp));
  const bool ok = WaitState(joiner, State::kConnected, 3000);
  stop = true;
  junk.join();
  std::printf("info: junk flood: %d junk Hellos sent\n", sent.load());
  CHECKF(sent.load() >= 1000, "the flood sent only %d", sent.load());
  CHECKF(ok, "the joiner did not connect under the flood (state %s, '%s')", online::StateName(joiner.status().state),
         joiner.status().event.c_str());
}

// M-6: concurrent Stop calls are safe (a GUARD: the race window is a few instructions; see the report's probe row).
static void TestConcurrentStop() {
  if (!Group("fix M-6: eight threads calling Stop at once")) return;
  for (int round = 0; round < 5; ++round) {
    Side s("stop");
    online::Lobby l;
    if (!StartLobby(l, s)) return;
    l.Host(0);
    WaitState(l, State::kHosting, 500);
    std::vector<std::thread> ts;
    std::atomic<bool> go{false};
    for (int i = 0; i < 8; ++i)
      ts.emplace_back([&] {
        while (!go.load()) std::this_thread::yield();
        l.Stop();
      });
    go = true;
    for (auto& t : ts) t.join();
    l.Stop();
  }
  CHECK(true);  // reaching here is the check (a double join ends the process)
}

// M-1: no log line is written while the lobby's mutex is held. The log sink asks status() on another thread and waits
// up to 500 ms: a sink called with the mutex held makes that call wait for it.
struct ProbeSink {
  std::atomic<online::Lobby*> lobby{nullptr};
  std::atomic<int> lines{0}, blocked{0};
  std::mutex mu;
  std::vector<std::string> blocked_lines;
  std::function<void(const std::string&)> Fn() {
    return [this](const std::string& line) {
      ++lines;
      online::Lobby* l = lobby;
      if (!l) return;
      auto done = std::make_shared<std::atomic<bool>>(false);
      std::thread([l, done] {
        (void)l->status();
        *done = true;
      }).detach();
      if (!WaitFor([&] { return done->load(); }, 500)) {
        ++blocked;
        std::lock_guard<std::mutex> lock(mu);
        blocked_lines.push_back(line);
      }
    };
  }
};
static void TestNoLogUnderLock() {
  if (!Group("fix M-1: no log line while the lobby mutex is held (connect, start, restart on both sides)")) return;
  Side hs("nolock_host"), js("nolock_join");
  ProbeSink ph, pj;
  online::Lobby host, joiner;
  ph.lobby = &host;
  pj.lobby = &joiner;
  online::Providers p1 = hs.P(), p2 = js.P();
  p1.log = ph.Fn();
  p2.log = pj.Fn();
  std::string e;
  CHECK(host.Start(p1, LoopCfg(), &e) && joiner.Start(p2, LoopCfg(), &e));
  CHECK(ConnectPair(host, joiner));
  CHECK(WaitCanStart(host));
  host.StartSession();
  CHECK(WaitFor([&] { return hs.restarts.load() == 1 && js.restarts.load() == 1; }, 8000));
  SleepMs(600);
  std::printf("info: no-lock probe: host %d lines (%d blocked), joiner %d lines (%d blocked)\n", ph.lines.load(), ph.blocked.load(),
              pj.lines.load(), pj.blocked.load());
  CHECK(ph.lines >= 5 && pj.lines >= 5);  // the probe saw the session start's lines
  {
    std::lock_guard<std::mutex> lock(ph.mu);
    CHECKF(ph.blocked == 0, "host: %d lines logged under the mutex, first '%s'", ph.blocked.load(),
           ph.blocked_lines.empty() ? "" : ph.blocked_lines[0].c_str());
  }
  {
    std::lock_guard<std::mutex> lock(pj.mu);
    CHECKF(pj.blocked == 0, "joiner: %d lines logged under the mutex, first '%s'", pj.blocked.load(),
           pj.blocked_lines.empty() ? "" : pj.blocked_lines[0].c_str());
  }
  host.Stop();
  joiner.Stop();
  ph.lobby = nullptr;
  pj.lobby = nullptr;
  SleepMs(100);  // detached status() calls end
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
    if (std::strcmp(argv[i], "-only") == 0 && i + 1 < argc) g_only = argv[++i];
  }
  std::thread([] {  // time and memory limits (from the first run)
    for (int s = 0;; ++s) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      PROCESS_MEMORY_COUNTERS_EX pm{};
      K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
      if (s >= 240 || pm.PrivateUsage > (size_t(1) << 30)) {
        std::printf("FAIL watchdog: %d s, %zu MB private (%d checks, %d failures so far)\n", s, size_t(pm.PrivateUsage >> 20), g_checks,
                    g_failures);
        std::fflush(stdout);
        std::_Exit(3);
      }
    }
  }).detach();
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  g_root = fs::path(tmp) / ("doa4_np_online_" + std::to_string(GetCurrentProcessId()));
  fs::create_directories(g_root);
  const int64_t t0 = NowUs();
  TestPure();
  TestConnect();
  TestBuildDiffers();
  TestFilesDiffer();
  TestBusy();
  TestStaleToken();
  TestSpoofHost();
  TestMsgIdsAndSanitise();
  TestSpoofJoinerAndRxReset();
  TestReachOut();
  TestStartSession();
  TestJoinerRefusesBoot();
  TestJoinerSilent();
  TestStartRefusals();
  TestSession();
  TestSessionSpoof();
  TestRequestFiles();
  TestConnectTimeout();
  TestReplyBounds();
  TestProto();
  TestLateBootReply();
  TestSessionRefused();
  TestStrangerEvents();
  TestJoinerCancel();
  TestUpnpCalls();
  TestJunkFloodJoin();
  TestConcurrentStop();
  TestNoLogUnderLock();
  CHECKF(UpnpCalls() == g_upnp_expected, "np_upnp was called %d times, %d of them by the UPnP group", UpnpCalls(), g_upnp_expected);
  std::error_code ec;
  fs::remove_all(g_root, ec);
  std::printf("info: run time %lld ms\n", (long long)((NowUs() - t0) / 1000));
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  std::fflush(stdout);
  return g_failures ? 1 : 0;
}

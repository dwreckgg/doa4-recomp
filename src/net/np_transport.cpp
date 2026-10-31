// src/net/np_transport.cpp - see np_transport.h.
// Ported from DOA2U (doa2u-release 6c35c7b, doa2u/src/online/netplay.c): the socket setup of sock_open (308-349, with
// SIO_UDP_CONNRESET off, 43-44) and the STUN client (633-747: k_stun_hosts, stun_resolve_thread, stun_send, stun_handle,
// stun_pump; retry 500 ms and refresh 15000 ms, 68-69). Changed from DOA2U: the bind can be 127.0.0.1 only and takes
// the address exclusively (SO_EXCLUSIVEADDRUSE), so a second copy on the same PC moves to the next port instead of
// sharing one; the socket is owned by one I/O thread instead of the game loop; every STUN server's answer is kept and
// compared (rules::ClassifyNat: symmetric-NAT detection); the binding is refreshed also while connected (R20); STUN
// answers are parsed by rules::StunMapped (the codec of Task 3).
#include "np_transport.h"

#ifndef NOMINMAX
#define NOMINMAX  // std::min / std::max below; windows.h's macros must not see them, whatever the build defines
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <wlanapi.h>  // types only: wlanapi.dll is loaded at run time (Config::wifi_streaming), nothing is linked

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include "np_spsc.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)  // mstcpip.h hides it without _WIN32_WINNT (as DOA2U netplay.c:43)
#endif

namespace doa4::np::net {

// =====================================================================================================
// The simulator's pure parts
// =====================================================================================================
namespace sim {

bool Parse(std::string_view text, Params* out) {
  std::array<uint32_t, 3> v = {0, 0, 0};
  size_t field = 0, digits = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == ',') {
      if (digits == 0 || field >= 3) return false;
      ++field;
      digits = 0;
      continue;
    }
    const char c = text[i];
    // field >= 3: a digit of a fourth field (review I-1: the write below ran past the array before the comma check)
    if (c < '0' || c > '9' || field >= 3 || ++digits > 5) return false;
    v[field] = v[field] * 10 + uint32_t(c - '0');
  }
  if (field != 3 || v[0] > 10000 || v[1] > 10000 || v[2] > 100) return false;
  if (out) *out = Params{v[0], v[1], v[2]};
  return true;
}

bool ParseDrop(std::string_view text, std::vector<uint32_t>* out) {
  std::vector<uint32_t> ks;
  if (!text.empty()) {
    uint32_t k = 0;
    size_t digits = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == ',') {
        if (digits == 0 || ks.size() >= 64) return false;
        ks.push_back(k);
        k = 0;
        digits = 0;
        continue;
      }
      const char c = text[i];
      if (c < '0' || c > '9' || ++digits > 9) return false;
      k = k * 10 + uint32_t(c - '0');
    }
  }
  std::sort(ks.begin(), ks.end());
  ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
  if (out) *out = std::move(ks);
  return true;
}

static uint64_t Mix64(uint64_t z) {  // splitmix64's finaliser
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

Decision Decide(const Params& p, uint32_t seed, uint64_t n) {
  const uint64_t h = Mix64(Mix64(uint64_t(seed) ^ 0xD0A4517000000000ull) ^ n);
  Decision d;
  d.drop = (h % 100) < p.loss_pct;
  int64_t delay = p.delay_ms;
  if (p.jitter_ms) {
    const uint64_t h2 = Mix64(h ^ 0x6A09E667F3BCC909ull);
    delay += int64_t(h2 % (2 * uint64_t(p.jitter_ms) + 1)) - int64_t(p.jitter_ms);
  }
  d.delay_ms = uint32_t(std::max<int64_t>(0, delay));
  return d;
}

}  // namespace sim

// =====================================================================================================
// Helpers
// =====================================================================================================
namespace {

sockaddr_in SockOf(const rules::Endpoint& e) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  std::memcpy(&a.sin_addr, e.ip.data(), 4);  // the array holds the address in network order (a.b.c.d)
  a.sin_port = htons(e.port);
  return a;
}

rules::Endpoint EndpointOf(const sockaddr_in& a) {
  rules::Endpoint e;
  std::memcpy(e.ip.data(), &a.sin_addr, 4);
  e.port = ntohs(a.sin_port);
  return e;
}

const char* NatName(rules::Nat n) {
  switch (n) {
    case rules::Nat::kNoAnswer: return "no answer";
    case rules::Nat::kOneAnswer: return "one answer";
    case rules::Nat::kCone: return "cone";
    case rules::Nat::kSymmetric: return "symmetric";
  }
  return "?";
}

constexpr uint32_t kTickMs = 5;
constexpr uint32_t kStunRetryMs = 500;     // DOA2U STUN_RETRY_MS
constexpr int kRecvBuffer = 2048;          // > wire::kMaxDatagram, so a 1,401-byte datagram reaches wire::Decode
constexpr int kSocketBuffer = 1 << 20;     // SO_RCVBUF / SO_SNDBUF: a burst of a full ring fits
constexpr int kMaxReceivesPerLoop = 4096;  // the loop still ticks under a flood

// The STUN servers are resolved on a helper thread (DOA2U stun_resolve_thread): a DNS lookup can take seconds and must
// not hold the I/O thread or Stop. The thread is detached; this shared state outlives whichever side ends first.
struct Resolve {
  std::mutex mu;
  bool done = false;
  std::vector<std::optional<rules::Endpoint>> eps;  // per configured server, in order; nullopt when it did not resolve
};

void ResolveServers(std::shared_ptr<Resolve> res, std::vector<std::string> names) {
  WSADATA w;
  const bool wsa = WSAStartup(MAKEWORD(2, 2), &w) == 0;
  std::vector<std::optional<rules::Endpoint>> eps(names.size());
  for (size_t i = 0; wsa && i < names.size(); ++i) {
    const size_t colon = names[i].rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == names[i].size()) continue;
    const std::string host = names[i].substr(0, colon), port = names[i].substr(colon + 1);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* ai = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &ai) == 0 && ai) {
      eps[i] = EndpointOf(*reinterpret_cast<const sockaddr_in*>(ai->ai_addr));
      freeaddrinfo(ai);
    }
  }
  if (wsa) WSACleanup();
  std::lock_guard<std::mutex> lock(res->mu);
  res->eps = std::move(eps);
  res->done = true;
}

}  // namespace

// =====================================================================================================
// State
// =====================================================================================================
struct Transport::Impl {
  Transport* owner = nullptr;
  Config config;
  mutable std::mutex log_mu;  // every call of `log` holds it; Stop clears `log` under it (no call after Stop returns)
  Log log;
  sim::Params sim;
  bool sim_on = false;

  SOCKET sock = INVALID_SOCKET;
  WSAEVENT sock_event = WSA_INVALID_EVENT;
  HANDLE wake = nullptr;     // auto-reset: PushGekko (a packet to send) and Stop
  HANDLE arrived = nullptr;  // auto-reset: gekko_arrived()
  HANDLE timer = nullptr;    // high-resolution waitable timer: the next 5 ms tick or held datagram (nullptr: 5 ms timeout)
  std::thread thread;
  std::atomic<bool> stop{false};
  std::atomic<DWORD> io_tid{0};
  std::atomic<uint16_t> port{0};
  bool started = false;  // Start succeeded (a Transport starts once)
  bool running = false;  // between that Start and Stop
  bool wsa = false;

  Spsc<std::vector<uint8_t>, kGekkoQueue> out;  // game thread -> I/O thread
  Spsc<std::vector<uint8_t>, kGekkoQueue> in;   // I/O thread -> game thread

  std::mutex peer_mu;  // any thread
  bool has_peer = false;
  rules::Endpoint peer;
  uint32_t token = 0;

  mutable std::mutex stats_mu;  // the I/O thread writes, any thread reads
  Stats st;
  std::atomic<uint32_t> queue_full{0}, wrong_thread{0};

  // ---- I/O thread only ----
  std::chrono::steady_clock::time_point t0;
  uint64_t sim_draws = 0, held_seq = 0;
  struct Held {
    int64_t due_us;
    uint64_t seq;
    sockaddr_in to;
    std::vector<uint8_t> bytes;
  };
  std::vector<Held> held;  // a min-heap on (due_us, seq)
  bool stun_on = false, stun_resolved = false, stun_sent = false, stun_answered = false;
  std::shared_ptr<Resolve> resolve;
  std::vector<std::optional<rules::Endpoint>> stun_eps;  // per configured server
  std::vector<std::optional<rules::Endpoint>> answers;   // per configured server: its latest mapped answer
  std::vector<uint32_t> answer_ms;                       // ... when it arrived (NowMs)
  std::array<uint8_t, 12> tid{};
  uint32_t stun_last_ms = 0;
  uint64_t rng = 0;
  rules::Nat logged_nat = rules::Nat::kNoAnswer;
  std::optional<rules::Endpoint> logged_mapped;
  int64_t next_tick_us = 0;  // on_tick runs at the first loop at or after this
  uint64_t gekko_out = 0;    // kGekko datagrams drained so far: the index Config::sim_drop counts
  HANDLE wlan = nullptr;     // the WLAN client handle while media streaming mode is held (Config::wifi_streaming)
  void* wlan_close = nullptr;  // WlanCloseHandle, from wlanapi.dll

  void Log(const std::string& line) const {
    std::lock_guard<std::mutex> lock(log_mu);
    if (log) log(line);
  }
  void WifiOn();
  void WifiOff();
  uint32_t NowMs() const {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  int64_t NowUs() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  }
  void CountDrop(wire::Drop why) {
    std::lock_guard<std::mutex> lock(stats_mu);
    ++st.dropped[size_t(why)];
  }
  bool OnIoThread() const { return GetCurrentThreadId() == io_tid.load(); }
  void WrongThread(const char* call) {
    ++wrong_thread;
    Log(std::string("WARN transport: ") + call + " called off the I/O thread; nothing sent");
  }

  void RawSend(const sockaddr_in& to, const std::vector<uint8_t>& d) const {
    sendto(sock, reinterpret_cast<const char*>(d.data()), int(d.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
  }
  // Every datagram to a peer passes here: through the simulator when it is on.
  void SendOut(const sockaddr_in& to, std::vector<uint8_t> d) {
    if (!sim_on) {
      RawSend(to, d);
      return;
    }
    const sim::Decision dec = sim::Decide(sim, config.sim_seed, sim_draws++);
    {
      std::lock_guard<std::mutex> lock(stats_mu);
      ++(dec.drop ? st.sim_dropped : st.sim_delayed);
    }
    if (dec.drop) return;
    held.push_back(Held{NowUs() + int64_t(dec.delay_ms) * 1000, held_seq++, to, std::move(d)});
    std::push_heap(held.begin(), held.end(), Later);
  }
  static bool Later(const Held& a, const Held& b) { return a.due_us != b.due_us ? a.due_us > b.due_us : a.seq > b.seq; }
  void ReleaseHeld() {
    const int64_t now = NowUs();
    while (!held.empty() && held.front().due_us <= now) {
      std::pop_heap(held.begin(), held.end(), Later);
      RawSend(held.back().to, held.back().bytes);
      held.pop_back();
    }
  }

  void Run();
  void ReceiveAll(std::vector<uint8_t>& buf);
  void Handle(const rules::Endpoint& from, std::span<const uint8_t> d, bool* arrived_any);
  void DrainOutbound();
  void StunPump();
  void SendStunRound(uint32_t now_ms);
  bool HandleStun(const rules::Endpoint& from, std::span<const uint8_t> d);
  void StunClassify(uint32_t now_ms);
  void Close();
};

// =====================================================================================================
// The I/O thread
// =====================================================================================================
// The loop waits on the socket, the wake event and (when the system has one) a high-resolution timer armed for the next
// 5 ms tick or the next held datagram, whichever is first. A plain 5 ms timeout wakes on the timer resolution of the
// process, 15.6 ms by default in this game (the SDK's vblank worker says the same, graphics_system.cpp:171-176), which
// would make on_tick a 64 Hz tick and add up to 15.6 ms to every simulated delay. Without the timer the 5 ms timeout
// is the fallback.
void Transport::Impl::Run() {
  io_tid = GetCurrentThreadId();
  Log("[doa4][NET] transport started on port " + std::to_string(port.load()));
  if (config.wifi_streaming) WifiOn();
  std::vector<uint8_t> buf(kRecvBuffer);
  HANDLE waits[3] = {sock_event, wake, timer};
  const DWORD n_waits = timer ? 3 : 2;
  next_tick_us = NowUs();
  while (!stop.load()) {
    DWORD timeout = kTickMs;
    if (timer) {
      int64_t next = next_tick_us;
      if (!held.empty()) next = std::min(next, held.front().due_us);
      const int64_t wait_us = next - NowUs();
      LARGE_INTEGER due;
      due.QuadPart = -wait_us * 10;  // relative, in 100 ns
      if (wait_us <= 0) timeout = 0;
      else if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) timeout = 4 * kTickMs;  // a guard only
    }
    WaitForMultipleObjects(n_waits, waits, FALSE, timeout);
    if (stop.load()) break;
    WSAResetEvent(sock_event);  // before the drain: a datagram arriving during it sets the event again
    ReceiveAll(buf);
    DrainOutbound();
    if (!held.empty()) ReleaseHeld();
    if (stun_on) StunPump();
    const int64_t now_us = NowUs();
    if (now_us >= next_tick_us) {
      next_tick_us += int64_t(kTickMs) * 1000;
      if (next_tick_us <= now_us) next_tick_us = now_us + int64_t(kTickMs) * 1000;  // late by a whole tick: no burst
      if (owner->on_tick) owner->on_tick(uint32_t(now_us / 1000));
    }
  }
  WifiOff();  // the mode ends with the handle (Stop joins this thread before it returns)
}

// ---- Wi-Fi media streaming mode: DOA2U recomp by Parker Allan (parkerallan/doa2u-recomp, MIT), nic_net.c 277-302
// (wlan_no_background_scans, commit 361dccf), with the handle closed at the end instead of kept for the process. ----
void Transport::Impl::WifiOn() {
  int32_t set = 0;
  HMODULE m = LoadLibraryW(L"wlanapi.dll");  // kept loaded: WLAN may hold threads of its own in the process
  using OpenFn = DWORD(WINAPI*)(DWORD, PVOID, PDWORD, PHANDLE);
  using EnumFn = DWORD(WINAPI*)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
  using SetFn = DWORD(WINAPI*)(HANDLE, const GUID*, WLAN_INTF_OPCODE, DWORD, const VOID*, PVOID);
  using FreeFn = VOID(WINAPI*)(PVOID);
  OpenFn open = m ? reinterpret_cast<OpenFn>(GetProcAddress(m, "WlanOpenHandle")) : nullptr;
  EnumFn enumi = m ? reinterpret_cast<EnumFn>(GetProcAddress(m, "WlanEnumInterfaces")) : nullptr;
  SetFn seti = m ? reinterpret_cast<SetFn>(GetProcAddress(m, "WlanSetInterface")) : nullptr;
  FreeFn freem = m ? reinterpret_cast<FreeFn>(GetProcAddress(m, "WlanFreeMemory")) : nullptr;
  wlan_close = m ? reinterpret_cast<void*>(GetProcAddress(m, "WlanCloseHandle")) : nullptr;
  DWORD ver = 0;
  if (open && enumi && seti && freem && wlan_close && open(2, nullptr, &ver, &wlan) == ERROR_SUCCESS) {
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (enumi(wlan, nullptr, &list) == ERROR_SUCCESS && list) {
      BOOL on = TRUE;
      for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        if (list->InterfaceInfo[i].isState != wlan_interface_state_connected) continue;
        if (seti(wlan, &list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_media_streaming_mode, sizeof on, &on,
                 nullptr) == ERROR_SUCCESS)
          ++set;
      }
      freem(list);
    }
  } else {
    wlan = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(stats_mu);
    st.wifi_interfaces = set;
  }
  Log("[doa4][NET] Wi-Fi media streaming mode (background scans off) on " + std::to_string(set) + " interface(s)" +
      (m ? "" : "; wlanapi.dll not available"));
}

void Transport::Impl::WifiOff() {
  if (wlan && wlan_close) reinterpret_cast<DWORD(WINAPI*)(HANDLE, PVOID)>(wlan_close)(wlan, nullptr);
  wlan = nullptr;
}

void Transport::Impl::ReceiveAll(std::vector<uint8_t>& buf) {
  bool arrived_any = false;
  for (int i = 0; i < kMaxReceivesPerLoop; ++i) {
    sockaddr_in from{};
    int fl = sizeof from;
    const int n = recvfrom(sock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n == SOCKET_ERROR) {
      const int e = WSAGetLastError();
      if (e == WSAEMSGSIZE) {  // longer than the buffer (and so than kMaxDatagram): cut by the stack, refused here
        CountDrop(wire::Drop::kTooLarge);
        continue;
      }
      if (e == WSAECONNRESET || e == WSAENETRESET) continue;  // an ICMP report; SIO_UDP_CONNRESET keeps them rare
      break;                                                  // WSAEWOULDBLOCK: nothing more queued
    }
    Handle(EndpointOf(from), std::span<const uint8_t>(buf.data(), size_t(n)), &arrived_any);
  }
  if (arrived_any) SetEvent(arrived);
}

void Transport::Impl::Handle(const rules::Endpoint& from, std::span<const uint8_t> d, bool* arrived_any) {
  if (stun_on && rules::IsStun(d) && HandleStun(from, d)) return;
  wire::Packet pk;
  const wire::Drop why = wire::Decode(d, &pk);
  if (why != wire::Drop::kNone) {
    CountDrop(why);
    // kProto: the magic matched and the datagram has a whole header, so its proto and type bytes can be handed on
    if (why == wire::Drop::kProto && owner->on_foreign_proto) owner->on_foreign_proto(from, d[4], d[5], d.size());
    return;
  }
  if (pk.type != wire::Type::kGekko) {
    if (owner->on_packet) owner->on_packet(from, pk);
    return;
  }
  bool has, source_ok, token_ok;
  {
    std::lock_guard<std::mutex> lock(peer_mu);
    has = has_peer;
    source_ok = has_peer && from == peer;
    token_ok = pk.token == token;
  }
  if (!has) {  // carry 2h: before SetPeer (session start), counted apart from the drops
    std::lock_guard<std::mutex> lock(stats_mu);
    ++st.gekko_early;
    return;
  }
  if (!source_ok) {
    CountDrop(wire::Drop::kSource);
    return;
  }
  if (!token_ok) {
    CountDrop(wire::Drop::kToken);
    return;
  }
  if (!in.Push(std::vector<uint8_t>(pk.body.begin(), pk.body.end()))) {
    ++queue_full;
    return;
  }
  *arrived_any = true;
}

void Transport::Impl::DrainOutbound() {
  bool has;
  rules::Endpoint to;
  uint32_t tok;
  {
    std::lock_guard<std::mutex> lock(peer_mu);
    has = has_peer;
    to = peer;
    tok = token;
  }
  const sockaddr_in addr = SockOf(to);
  std::vector<uint8_t> p;
  while (out.Pop(&p)) {
    if (!has) continue;  // no peer: discarded
    std::vector<uint8_t> d = wire::Encode(wire::Type::kGekko, tok, p);
    if (d.empty()) continue;  // too long for one datagram: discarded
    const uint64_t k = gekko_out++;
    if (!config.sim_drop.empty() && std::binary_search(config.sim_drop.begin(), config.sim_drop.end(), k)) {
      {
        std::lock_guard<std::mutex> lock(stats_mu);
        ++st.sim_drop_hits;
      }
      Log("[doa4][NET] sim_drop: kGekko datagram " + std::to_string(k) + " dropped (GekkoNet packet type " +
          std::to_string(p.empty() ? -1 : int(p[0])) + ")");
      continue;
    }
    SendOut(addr, std::move(d));
  }
}

// ---- STUN (DOA2U stun_pump / stun_send / stun_handle) ----
void Transport::Impl::StunPump() {
  if (!stun_resolved) {
    {
      std::lock_guard<std::mutex> lock(resolve->mu);
      if (!resolve->done) return;
      stun_eps = resolve->eps;
    }
    stun_resolved = true;
    answers.assign(stun_eps.size(), std::nullopt);
    answer_ms.assign(stun_eps.size(), 0);
    for (size_t i = 0; i < stun_eps.size(); ++i)
      if (!stun_eps[i]) Log("[doa4][NET] STUN server " + config.stun_servers[i] + " not resolved");
  }
  const uint32_t now = NowMs();
  if (!stun_answered) {
    if (!stun_sent || now - stun_last_ms >= kStunRetryMs) SendStunRound(now);
  } else {
    if (now - stun_last_ms >= config.stun_refresh_ms) SendStunRound(now);  // R20: also while connected, keeps the NAT mapping alive
    StunClassify(now);  // carry 2e: answers age out even when no new one arrives
  }
}

// The class and the mapped address from the answers that are still fresh: newer than 2 x stun_refresh_ms (Task 9 carry
// 2e, Task 7 review M-1: a dead server cannot keep a stale kSymmetric alive). The brief's "or the current round" adds
// nothing: once answered, rounds go out every stun_refresh_ms, so an answer to the current round is always fresh.
void Transport::Impl::StunClassify(uint32_t now_ms) {
  const uint64_t ttl = 2ull * config.stun_refresh_ms;
  std::vector<rules::Endpoint> got;  // in server order: got.front() is the first server with a fresh answer
  for (size_t i = 0; i < answers.size(); ++i)
    if (answers[i] && uint64_t(now_ms - answer_ms[i]) <= ttl) got.push_back(*answers[i]);
  const rules::Nat nat = rules::ClassifyNat(got);
  std::optional<rules::Endpoint> mapped;
  if (!got.empty()) mapped = got.front();
  {
    std::lock_guard<std::mutex> lock(stats_mu);
    st.nat = nat;
    st.mapped = mapped;
  }
  if (nat != logged_nat || logged_mapped != mapped) {
    logged_nat = nat;
    logged_mapped = mapped;
    Log("[doa4][NET] STUN: " + std::to_string(got.size()) + " of " + std::to_string(stun_eps.size()) +
        " servers answered recently, NAT " + NatName(nat) + (mapped ? ", mapped " + rules::EndpointText(*mapped) : ", no mapped address"));
  }
}

void Transport::Impl::SendStunRound(uint32_t now_ms) {
  for (size_t i = 0; i < tid.size(); i += 8) {  // a fresh transaction id per round (not secret: it only pairs answers)
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    const uint64_t v = rng ^ (rng >> 29);
    for (size_t k = 0; k < 8 && i + k < tid.size(); ++k) tid[i + k] = uint8_t(v >> (8 * k));
  }
  const std::array<uint8_t, 20> req = rules::StunRequest(tid);
  const std::vector<uint8_t> d(req.begin(), req.end());
  for (const auto& ep : stun_eps)
    if (ep) RawSend(SockOf(*ep), d);  // STUN bypasses the simulator: it is not peer traffic
  stun_sent = true;
  stun_last_ms = now_ms;
}

bool Transport::Impl::HandleStun(const rules::Endpoint& from, std::span<const uint8_t> d) {
  size_t idx = stun_eps.size();
  for (size_t i = 0; i < stun_eps.size(); ++i)
    if (stun_eps[i] && *stun_eps[i] == from) {
      idx = i;
      break;
    }
  if (idx == stun_eps.size()) return false;
  rules::Endpoint mapped;
  if (!rules::StunMapped(d, tid, &mapped)) return false;
  stun_answered = true;
  const uint32_t now = NowMs();
  answers[idx] = mapped;
  answer_ms[idx] = now;
  {
    std::lock_guard<std::mutex> lock(stats_mu);
    ++st.stun_answers;
  }
  StunClassify(now);
  return true;
}

void Transport::Impl::Close() {
  if (sock != INVALID_SOCKET) {
    closesocket(sock);
    sock = INVALID_SOCKET;
  }
  if (sock_event != WSA_INVALID_EVENT) {
    WSACloseEvent(sock_event);
    sock_event = WSA_INVALID_EVENT;
  }
  if (timer) {
    CloseHandle(timer);
    timer = nullptr;
  }
  if (wsa) {
    WSACleanup();
    wsa = false;
  }
}

// =====================================================================================================
// Transport
// =====================================================================================================
Transport::Transport() : impl_(std::make_unique<Impl>()) {
  impl_->owner = this;
  impl_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  impl_->arrived = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Transport::~Transport() {
  Stop();
  if (impl_->wake) CloseHandle(impl_->wake);
  if (impl_->arrived) CloseHandle(impl_->arrived);
}

bool Transport::Start(const Config& config, Log log, std::string* error) {
  Impl& m = *impl_;
  auto fail = [&](const std::string& why) {
    m.Close();
    if (error) *error = why;
    return false;
  };
  if (m.started) {  // never through fail(): that would close the socket of the running transport
    if (error) *error = "the transport was already started (a Transport starts once)";
    return false;
  }
  if (!m.wake || !m.arrived) return fail("could not create the transport's events");
  m.config = config;
  std::sort(m.config.sim_drop.begin(), m.config.sim_drop.end());  // DrainOutbound binary-searches it
  {
    std::lock_guard<std::mutex> lock(m.log_mu);
    m.log = std::move(log);
  }
  m.sim_on = false;
  if (!config.sim.empty()) {
    if (!sim::Parse(config.sim, &m.sim)) return fail("bad DOA4_NP_SIM \"" + config.sim + "\" (expected delay_ms,jitter_ms,loss_pct)");
    m.sim_on = true;
  }
  WSADATA w;
  if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return fail("WSAStartup failed");
  m.wsa = true;

  // ---- the socket (DOA2U sock_open) ----
  std::vector<uint16_t> ports;
  if (config.port == 0) {
    ports.push_back(0);
  } else {
    ports.push_back(config.port);
    if (!config.exact_port) {
      // the next ports of the join code's range first, so the code stays short; then any free port
      for (uint32_t p = uint32_t(config.port) + 1; p < uint32_t(rules::kPortBase) + rules::kPorts; ++p) ports.push_back(uint16_t(p));
      ports.push_back(0);
    }
  }
  int last_error = 0;
  for (uint16_t p : ports) {
    m.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m.sock == INVALID_SOCKET) return fail("socket() failed (WSA error " + std::to_string(WSAGetLastError()) + ")");
    BOOL on = TRUE;
    setsockopt(m.sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(config.loopback ? INADDR_LOOPBACK : INADDR_ANY);
    a.sin_port = htons(p);
    if (bind(m.sock, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) break;
    last_error = WSAGetLastError();
    closesocket(m.sock);
    m.sock = INVALID_SOCKET;
  }
  const char* where = config.loopback ? "127.0.0.1" : "every interface";
  if (m.sock == INVALID_SOCKET) {
    if (config.exact_port)
      return fail("UDP port " + std::to_string(config.port) + " on " + where + " is not available (exact port; WSA error " +
                  std::to_string(last_error) + ")");
    return fail("no UDP port could be bound on " + std::string(where) + " (preferred " + std::to_string(config.port) +
                "; WSA error " + std::to_string(last_error) + ")");
  }
  {
    sockaddr_in a{};
    int l = sizeof a;
    m.port = getsockname(m.sock, reinterpret_cast<sockaddr*>(&a), &l) == 0 ? ntohs(a.sin_port) : config.port;
  }
  // an ICMP "port unreachable" from a peer that is gone must not fail every later recvfrom with WSAECONNRESET
  DWORD b = FALSE, ret = 0;
  WSAIoctl(m.sock, SIO_UDP_CONNRESET, &b, sizeof b, nullptr, 0, &ret, nullptr, nullptr);
  int big = kSocketBuffer;
  setsockopt(m.sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof big);
  setsockopt(m.sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&big), sizeof big);
  if (!config.loopback && config.lan_broadcast) {
    BOOL on = TRUE;
    setsockopt(m.sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
  }
  m.sock_event = WSACreateEvent();
  if (m.sock_event == WSA_INVALID_EVENT || WSAEventSelect(m.sock, m.sock_event, FD_READ) != 0)  // also makes it non-blocking
    return fail("WSAEventSelect failed (WSA error " + std::to_string(WSAGetLastError()) + ")");

  // ---- STUN: resolve on a helper thread ----
  m.stun_on = config.stun && !config.stun_servers.empty();
  if (m.stun_on) {
    m.resolve = std::make_shared<Resolve>();
    try {
      std::thread(ResolveServers, m.resolve, config.stun_servers).detach();
    } catch (...) {
      m.stun_on = false;
    }
  }
  m.rng = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (uint64_t(GetCurrentProcessId()) << 32) ^
          reinterpret_cast<uintptr_t>(&m);
  m.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!m.timer) m.Log("[doa4][NET] transport: no high-resolution timer (error " + std::to_string(GetLastError()) +
                      "); ticking on the 5 ms timeout");
  m.t0 = std::chrono::steady_clock::now();
  m.stop = false;
  try {
    m.thread = std::thread([&m] { m.Run(); });
  } catch (...) {
    return fail("could not start the transport thread");
  }
  m.started = m.running = true;
  return true;
}

void Transport::Stop() {
  Impl& m = *impl_;
  if (!m.running) return;
  m.stop = true;
  SetEvent(m.wake);
  if (m.thread.joinable()) m.thread.join();
  m.io_tid = 0;  // a later thread that reuses the id is not the I/O thread
  m.Close();
  m.running = false;
  std::lock_guard<std::mutex> lock(m.log_mu);  // waits for a WARN line in progress on another thread
  m.log = nullptr;                              // review M-3: the sink is never called once Stop returns
}

uint16_t Transport::port() const { return impl_->port.load(); }

void Transport::SendTo(const rules::Endpoint& to, std::vector<uint8_t> datagram) {
  Impl& m = *impl_;
  if (!m.OnIoThread()) {
    m.WrongThread("SendTo");
    return;
  }
  if (datagram.empty()) return;
  m.SendOut(SockOf(to), std::move(datagram));
}

void Transport::Broadcast(uint16_t port, std::vector<uint8_t> datagram) {
  Impl& m = *impl_;
  if (!m.OnIoThread()) {
    m.WrongThread("Broadcast");
    return;
  }
  if (m.config.loopback || !m.config.lan_broadcast || datagram.empty()) return;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  a.sin_port = htons(port);
  m.SendOut(a, std::move(datagram));
}

void Transport::SetPeer(const rules::Endpoint& peer, uint32_t token) {
  std::lock_guard<std::mutex> lock(impl_->peer_mu);
  impl_->has_peer = true;
  impl_->peer = peer;
  impl_->token = token;
}

void Transport::ClearPeer() {
  std::lock_guard<std::mutex> lock(impl_->peer_mu);
  impl_->has_peer = false;
}

bool Transport::PushGekko(std::vector<uint8_t> payload) {
  Impl& m = *impl_;
  if (!m.out.Push(std::move(payload))) {
    ++m.queue_full;
    return false;
  }
  SetEvent(m.wake);  // the packet leaves at once, not at the next 5 ms tick
  return true;
}

bool Transport::PopGekko(std::vector<uint8_t>* payload) { return impl_->in.Pop(payload); }

void* Transport::gekko_arrived() const { return impl_->arrived; }

Stats Transport::stats() const {
  Stats s;
  {
    std::lock_guard<std::mutex> lock(impl_->stats_mu);
    s = impl_->st;
  }
  s.queue_full = impl_->queue_full.load();
  s.wrong_thread = impl_->wrong_thread.load();
  return s;
}

}  // namespace doa4::np::net

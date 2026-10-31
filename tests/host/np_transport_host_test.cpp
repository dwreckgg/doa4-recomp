// Host test for src/net/np_spsc.h and src/net/np_transport (one UDP socket on its own I/O thread). Built and run by
// run_np_transport_test.bat. Every socket here, the transports' and the test's own, binds 127.0.0.1: nothing listens
// on another interface (no firewall prompt) and nothing reaches the internet (the STUN servers are fakes on loopback).
// np_upnp is not exercised (the router is the user's). "np_transport_host_test.exe -v" prints each group before it
// runs. A watchdog thread ends the run with a FAIL line after 180 s, so a transport that never stops cannot hang it.
//
// Timing checks (Stop <= 200 ms, the STUN cadence 500/300 ms +- 50 ms, the simulator's >= 40 ms) read the wall clock;
// the runner prints them as info lines so a reader can see how close each was.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../../src/net/np_rules.h"
#include "../../src/net/np_spsc.h"
#include "../../src/net/np_transport.h"
#include "../../src/net/np_wire.h"

using namespace doa4::np;
using Bytes = std::vector<uint8_t>;

static int g_checks = 0;
static int g_failures = 0;
static bool g_verbose = false;

#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::fflush(stdout);                                        \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

// A check that prints the observed values when it fails (the arguments after the condition are a printf).
#define CHECKF(cond, ...)                                           \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                     \
      std::printf("\n");                                            \
      std::fflush(stdout);                                          \
      ++g_failures;                                                 \
    }                                                               \
  } while (0)

static void Group(const char* name) {
  if (g_verbose) {
    std::printf("group: %s\n", name);
    std::fflush(stdout);
  }
}

using Clock = std::chrono::steady_clock;
static int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}
static void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static rules::Endpoint Loop(uint16_t port) {
  rules::Endpoint e;
  e.ip = {127, 0, 0, 1};
  e.port = port;
  return e;
}

// ---- the log sink (the transport calls it from its I/O thread, and from a thread that breaks the rule) ----
struct LogSink {
  std::mutex mu;
  std::vector<std::string> lines;
  net::Transport::Log Fn() {
    return [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(mu);
      lines.push_back(line);
    };
  }
  int CountPrefix(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mu);
    int n = 0;
    for (const std::string& l : lines) n += l.rfind(prefix, 0) == 0 ? 1 : 0;
    return n;
  }
  int CountExact(const std::string& text) {
    std::lock_guard<std::mutex> lock(mu);
    int n = 0;
    for (const std::string& l : lines) n += l == text ? 1 : 0;
    return n;
  }
  bool WaitPrefix(const std::string& prefix, int ms) {
    for (int i = 0; i < ms; i += 5) {
      if (CountPrefix(prefix) > 0) return true;
      SleepMs(5);
    }
    return CountPrefix(prefix) > 0;
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
    DWORD b = FALSE, ret = 0;  // an ICMP port-unreachable must not fail later receives
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &b, sizeof b, nullptr, 0, &ret, nullptr, nullptr);
    int big = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof big);
  }
  ~Udp() {
    if (s != INVALID_SOCKET) closesocket(s);
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
  // One datagram within ms milliseconds; false when none came.
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

// True when 127.0.0.1:port can be bound right now (the socket is closed again at once).
static bool PortFree(uint16_t port) {
  Udp u(port);
  return u.ok();
}

static net::Config LoopCfg(uint16_t port = 0) {
  net::Config c;
  c.port = port;
  c.loopback = true;
  c.stun = false;
  c.lan_broadcast = false;
  return c;
}

static Bytes Payload(uint32_t seq, size_t len) {  // seq little-endian, then a pattern derived from seq
  Bytes p(std::max<size_t>(len, 4));
  for (int i = 0; i < 4; ++i) p[size_t(i)] = uint8_t(seq >> (8 * i));
  for (size_t i = 4; i < p.size(); ++i) p[i] = uint8_t(seq * 31 + i);
  return p;
}
static uint32_t SeqOf(const Bytes& p) {
  if (p.size() < 4) return 0xFFFFFFFFu;
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// Pushes on the game-thread side, retrying while the outbound ring is full (the I/O thread drains it every loop).
static bool PushRetry(net::Transport& t, Bytes p, int max_ms = 2000) {
  const int64_t until = NowUs() + int64_t(max_ms) * 1000;
  while (!t.PushGekko(p)) {
    if (NowUs() > until) return false;
    SleepMs(1);
  }
  return true;
}

// =====================================================================================================
// Spsc
// =====================================================================================================

static void TestSpscOneThread() {
  Group("spsc, one thread");
  Spsc<int, 4> q;
  int v = -7;
  CHECK(!q.Pop(&v) && v == -7);  // empty: false, *item untouched
  for (int lap = 0; lap < 5; ++lap) {  // five laps of the ring: the indices wrap around the slots
    for (int i = 0; i < 4; ++i) CHECKF(q.Push(lap * 10 + i), "lap %d: push %d refused", lap, i);
    int extra = 99;
    CHECKF(!q.Push(std::move(extra)) && extra == 99, "lap %d: a fifth push was accepted (or moved from)", lap);
    for (int i = 0; i < 4; ++i) {
      int got = -1;
      CHECKF(q.Pop(&got) && got == lap * 10 + i, "lap %d: pop %d gave %d", lap, i, got);
    }
    CHECK(!q.Pop(&v));
  }
  // Partly full, interleaved: order kept across the wrap.
  Spsc<int, 4> r;
  int next_in = 0, next_out = 0, bad = 0;
  for (int step = 0; step < 100; ++step) {
    if (step % 3 != 2) {
      int item = next_in;
      if (r.Push(std::move(item))) ++next_in;
    }
    int got;
    if (step % 2 == 0 && r.Pop(&got)) bad += got == next_out++ ? 0 : 1;
  }
  int got;
  while (r.Pop(&got)) bad += got == next_out++ ? 0 : 1;
  CHECKF(bad == 0 && next_out == next_in, "interleaved: %d out of order, %d in, %d out", bad, next_in, next_out);
  // Move-only payloads move through intact (the production element is a byte vector).
  Spsc<Bytes, 2> b;
  CHECK(b.Push(Bytes{1, 2, 3}));
  Bytes o;
  CHECK(b.Pop(&o) && o == Bytes({1, 2, 3}));
  CHECK((Spsc<Bytes, 1024>::kCapacity == 1024));
}

// One producer thread, one consumer thread, `total` sequence numbers: every one arrives, once, in order.
template <size_t N>
static void SpscTwoThreads(const char* label, uint64_t total) {
  auto q = std::make_unique<Spsc<uint64_t, N>>();
  std::atomic<bool> go{false}, quit{false};  // quit: the consumer gave up, so a producer spinning on a full ring ends
  std::thread producer([&] {
    while (!go.load()) {
    }
    for (uint64_t i = 1; i <= total; ++i)
      while (!q->Push(uint64_t(i)))
        if (quit.load()) return;
  });
  uint64_t expected = 1, out_of_order = 0, first_bad_at = 0, first_bad_val = 0;
  const int64_t deadline = NowUs() + 20'000'000;  // 20 s: a lost item must not hang the run
  go.store(true);
  while (expected <= total && NowUs() < deadline) {
    uint64_t v;
    if (!q->Pop(&v)) continue;
    if (v != expected) {
      if (!out_of_order) {
        first_bad_at = expected;
        first_bad_val = v;
      }
      ++out_of_order;
      if (v > expected) expected = v;  // resynchronise so one race counts once
    }
    ++expected;
  }
  const bool finished = expected > total;
  if (!finished) std::printf("info: %s: the consumer stopped at %llu of %llu (deadline)\n", label, (unsigned long long)expected,
                             (unsigned long long)total);
  quit = true;
  producer.join();  // never detached: the thread uses q and this frame's locals
  CHECKF(finished, "%s: only %llu of %llu items arrived in 20 s", label, (unsigned long long)(expected - 1), (unsigned long long)total);
  CHECKF(out_of_order == 0, "%s: %llu items out of order; first: expected %llu, got %llu", label, (unsigned long long)out_of_order,
         (unsigned long long)first_bad_at, (unsigned long long)first_bad_val);
  uint64_t rest;
  CHECK(!q->Pop(&rest));  // nothing extra
}

static void TestSpscTwoThreads() {
  Group("spsc, two threads, 1,000,000 sequence numbers");
  SpscTwoThreads<1024>("spsc N=1024", 1'000'000);
  SpscTwoThreads<8>("spsc N=8", 1'000'000);
  // The production element: byte vectors carrying their sequence number.
  auto q = std::make_unique<Spsc<Bytes, 1024>>();
  constexpr uint32_t kN = 200'000;
  std::atomic<bool> quit{false};
  std::thread producer([&] {
    for (uint32_t i = 0; i < kN; ++i) {
      Bytes p = Payload(i, 4 + i % 13);
      while (!q->Push(std::move(p)))
        if (quit.load()) return;
    }
  });
  uint32_t next = 0, bad = 0;
  const int64_t deadline = NowUs() + 20'000'000;
  while (next < kN && NowUs() < deadline) {
    Bytes p;
    if (!q->Pop(&p)) continue;
    if (p != Payload(next, 4 + next % 13)) ++bad;
    ++next;
  }
  quit = true;
  producer.join();
  CHECKF(next == kN && bad == 0, "byte vectors: %u of %u arrived, %u wrong", next, kN, bad);
}

// =====================================================================================================
// Transport: delivery, filters and garbage
// =====================================================================================================

static constexpr uint32_t kToken = 0x5EED1234u;

// Everything a transport's callbacks reach (the log sinks, on_packet's state) is declared BEFORE the transports, so it
// outlives their I/O threads: members and locals are destroyed in reverse order, and ~Transport (Stop) joins the
// thread. Every group below keeps that order (a sink destroyed first was written into by an I/O thread still starting:
// a heap corruption in probe round 1).
struct Pair {
  LogSink la, lb;
  std::atomic<int> b_packets{0};  // on_packet calls on B
  std::mutex mu;
  std::vector<std::pair<rules::Endpoint, wire::Type>> b_seen;
  net::Transport a, b;
  bool Start(const net::Config& ca, const net::Config& cb) {
    b.on_packet = [this](const rules::Endpoint& from, const wire::Packet& p) {
      std::lock_guard<std::mutex> lock(mu);
      b_seen.emplace_back(from, p.type);
      ++b_packets;
    };
    std::string ea, eb;
    const bool oka = a.Start(ca, la.Fn(), &ea);
    const bool okb = b.Start(cb, lb.Fn(), &eb);
    if (!oka) std::printf("info: A failed to start: %s\n", ea.c_str());
    if (!okb) std::printf("info: B failed to start: %s\n", eb.c_str());
    return oka && okb;
  }
};

static uint32_t SumDropped(const net::Stats& s) {
  uint32_t n = 0;
  for (uint32_t d : s.dropped) n += d;
  return n;
}

static void TestDeliveryAndFilters() {
  Group("transport, delivery A -> B, the kGekko filter and garbage");
  Pair p;
  CHECK(p.Start(LoopCfg(), LoopCfg()));
  const uint16_t pa = p.a.port(), pb = p.b.port();
  CHECKF(pa != 0 && pb != 0 && pa != pb, "ports A %u B %u", pa, pb);
  // The started line, once, with the port, logged by the I/O thread once it runs.
  CHECK(p.la.WaitPrefix("[doa4][NET] transport started on port", 1000));
  CHECK(p.lb.WaitPrefix("[doa4][NET] transport started on port", 1000));
  CHECKF(p.la.CountExact("[doa4][NET] transport started on port " + std::to_string(pa)) == 1, "A's started line (port %u)", pa);
  CHECKF(p.lb.CountExact("[doa4][NET] transport started on port " + std::to_string(pb)) == 1, "B's started line (port %u)", pb);
  p.a.SetPeer(Loop(pb), kToken);
  p.b.SetPeer(Loop(pa), kToken);

  // gekko_arrived: not set before anything arrived, set by the first payload.
  HANDLE arrived = static_cast<HANDLE>(p.b.gekko_arrived());
  CHECK(arrived != nullptr);
  CHECK(WaitForSingleObject(arrived, 0) == WAIT_TIMEOUT);
  CHECK(p.a.PushGekko(Payload(0, 20)));
  CHECKF(WaitForSingleObject(arrived, 1000) == WAIT_OBJECT_0, "gekko_arrived was not set within 1 s of the first payload");
  Bytes got;
  CHECK(p.b.PopGekko(&got) && got == Payload(0, 20));
  CHECK(!p.b.PopGekko(&got));

  // 999 more, popped while they are pushed: all arrive, in order, intact.
  uint32_t next = 1, wrong = 0;
  int waits_signalled = 0;
  for (uint32_t i = 1; i < 1000; ++i) {
    CHECKF(PushRetry(p.a, Payload(i, 4 + i % 60)), "push %u refused for 2 s", i);
    while (p.b.PopGekko(&got)) wrong += got == Payload(next, 4 + next % 60) ? 0 : 1, ++next;
  }
  const int64_t until = NowUs() + 3'000'000;
  while (next < 1000 && NowUs() < until) {
    if (WaitForSingleObject(arrived, 50) == WAIT_OBJECT_0) ++waits_signalled;
    while (p.b.PopGekko(&got)) wrong += got == Payload(next, 4 + next % 60) ? 0 : 1, ++next;
  }
  CHECKF(next == 1000 && wrong == 0, "1,000 payloads: %u arrived, %u wrong or out of order", next, wrong);
  std::printf("info: delivery: 1,000 payloads in order; gekko_arrived waited on %d times in the tail\n", waits_signalled);

  // ---- garbage, sent to B from a third socket and from A ----
  Udp third;
  CHECK(third.ok());
  uint32_t want[size_t(wire::Drop::kCount)] = {};
  const Bytes body = {1, 2, 3, 4, 5, 6, 7, 8};
  auto send3 = [&](const Bytes& d, wire::Drop why) {
    third.SendTo(pb, d);
    ++want[size_t(why)];
  };
  // A well-formed kGekko with A's token, but from the third socket: kSource.
  for (int i = 0; i < 25; ++i) send3(wire::Encode(wire::Type::kGekko, kToken, Payload(5000 + i, 12)), wire::Drop::kSource);
  // ... and with another token from the third socket: still kSource (the source is checked first).
  for (int i = 0; i < 5; ++i) send3(wire::Encode(wire::Type::kGekko, kToken ^ 1, body), wire::Drop::kSource);
  // Each header refusal of wire::Decode, at its edge.
  for (size_t n : {0, 1, 11}) send3(Bytes(n, 0x44), wire::Drop::kShort);
  send3(Bytes(1401, 0x44), wire::Drop::kTooLarge);
  send3(Bytes(3000, 0x44), wire::Drop::kTooLarge);  // longer than the receive buffer too
  {
    Bytes d = wire::Encode(wire::Type::kGekko, kToken, body);
    d[0] ^= 0xFF;
    send3(d, wire::Drop::kMagic);
    d = wire::Encode(wire::Type::kGekko, kToken, body);
    d[4] = wire::kProto + 1;
    send3(d, wire::Drop::kProto);
    d = wire::Encode(wire::Type::kGekko, kToken, body);
    d[5] = 12;
    send3(d, wire::Drop::kType);
    d = wire::Encode(wire::Type::kGekko, kToken, body);
    d.push_back(0);
    send3(d, wire::Drop::kLength);
  }
  // Random bytes of random lengths (0..1500): the reason is wire::Decode's own.
  std::mt19937 rng(20261030);
  int random_refused = 0, random_n = 0;
  for (int i = 0; i < 400; ++i) {
    Bytes d(rng() % 1501);
    for (uint8_t& c : d) c = uint8_t(rng());
    if (i % 50 == 0 && d.size() >= 4) {  // some with the real magic, so later header fields are reached
      d[0] = 'D', d[1] = 'O', d[2] = 'A', d[3] = '4';
    }
    wire::Packet pk;
    const wire::Drop why = wire::Decode(d, &pk);
    random_refused += why != wire::Drop::kNone ? 1 : 0;
    ++random_n;
    if (why == wire::Drop::kNone) continue;  // (not expected: counted by the precondition below)
    send3(d, why);
    if (i % 40 == 39) SleepMs(2);  // paced: a full socket buffer would lose datagrams, not count them
  }
  CHECKF(random_refused == random_n, "fixture: %d of %d random datagrams are refused by wire::Decode", random_refused, random_n);
  // A packet of another type is not the transport's to judge: it reaches on_packet, whatever its source or token.
  third.SendTo(pb, wire::Encode(wire::Type::kPing, 0, Bytes{9, 9, 9, 9}));
  // From A with a wrong token: kToken.
  p.a.SetPeer(Loop(pb), kToken ^ 0x80000000u);
  for (uint32_t i = 0; i < 7; ++i) CHECK(PushRetry(p.a, Payload(6000 + i, 16)));
  want[size_t(wire::Drop::kToken)] += 7;
  SleepMs(100);  // the wrong-token payloads leave A before its token is put back
  p.a.SetPeer(Loop(pb), kToken);
  // ... and then 100 good ones: PopGekko yields exactly these, nothing else.
  for (uint32_t i = 1000; i < 1100; ++i) CHECK(PushRetry(p.a, Payload(i, 4 + i % 60)));
  const int64_t until2 = NowUs() + 2'000'000;
  std::vector<uint32_t> after;
  while (after.size() < 100 && NowUs() < until2) {
    WaitForSingleObject(arrived, 20);
    while (p.b.PopGekko(&got)) after.push_back(got == Payload(SeqOf(got), 4 + SeqOf(got) % 60) ? SeqOf(got) : 0xFFFFFFFFu);
  }
  SleepMs(200);  // anything late would show here
  while (p.b.PopGekko(&got)) after.push_back(SeqOf(got));
  bool exact = after.size() == 100;
  for (size_t i = 0; exact && i < after.size(); ++i) exact = after[i] == 1000 + i;
  CHECKF(exact, "after the garbage PopGekko gave %zu payloads (first %u, last %u); expected exactly 1000..1099 in order",
         after.size(), after.empty() ? 0 : after.front(), after.empty() ? 0 : after.back());

  // The counters equal the numbers sent, reason by reason.
  const net::Stats sb = p.b.stats();
  uint32_t want_total = 0;
  for (size_t r = 0; r < size_t(wire::Drop::kCount); ++r) {
    want_total += want[r];
    CHECKF(sb.dropped[r] == want[r], "B dropped[%s] = %u, sent %u", wire::DropName(wire::Drop(r)), sb.dropped[r], want[r]);
  }
  CHECKF(SumDropped(sb) == want_total, "B dropped %u in all, %u garbage datagrams sent", SumDropped(sb), want_total);
  std::printf("info: garbage: %u datagrams dropped by B (kSource %u, kToken %u, kShort %u, kTooLarge %u, random %d)\n", want_total,
              want[size_t(wire::Drop::kSource)], want[size_t(wire::Drop::kToken)], want[size_t(wire::Drop::kShort)],
              want[size_t(wire::Drop::kTooLarge)], random_n);
  CHECK(sb.queue_full == 0 && sb.wrong_thread == 0);
  {
    std::lock_guard<std::mutex> lock(p.mu);
    CHECKF(p.b_seen.size() == 1, "on_packet on B ran %zu times (expected once: the kPing)", p.b_seen.size());
    if (p.b_seen.size() == 1) CHECK(p.b_seen[0].first == Loop(third.port) && p.b_seen[0].second == wire::Type::kPing);
  }
  const net::Stats sa = p.a.stats();
  CHECKF(SumDropped(sa) == 0 && sa.queue_full == 0, "A dropped %u, queue_full %u (A received nothing)", SumDropped(sa), sa.queue_full);
  p.a.Stop();
  p.b.Stop();
}

// =====================================================================================================
// Ports
// =====================================================================================================

// Review M-5: two copies of this suite at once would race for the same P. The ports group (the only one on fixed ports;
// every other group binds port 0) holds a named mutex of the user's session while it runs, so a second copy waits.
struct PortsLock {
  HANDLE h = CreateMutexW(nullptr, FALSE, L"Local\\doa4_np_transport_test_ports");
  bool held = false;
  PortsLock() {
    const DWORD w = h ? WaitForSingleObject(h, 120000) : WAIT_FAILED;
    held = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
  }
  ~PortsLock() {
    if (held) ReleaseMutex(h);
    if (h) CloseHandle(h);
  }
};

static void TestPorts() {
  Group("transport, ports: preferred, next, exact");
  PortsLock lock;
  CHECKF(lock.held, "the ports mutex was not taken within 120 s");
  // P comes from INSIDE the range with its lower neighbour free too (P - 1, P, P + 1 all free), so a fallback that
  // wrongly restarted at kPortBase (DOA2U goes on from P + 1, netplay.c:323-327) lands on P - 1, not P + 1 (T2.2).
  // 7215 stays out of the choice: the any-port case below uses it.
  uint16_t P = 0;
  for (uint16_t c = rules::kPortBase + 1; c + 1 < rules::kPortBase + rules::kPorts - 1; ++c)
    if (PortFree(uint16_t(c - 1)) && PortFree(c) && PortFree(uint16_t(c + 1))) {
      P = c;
      break;
    }
  CHECKF(P != 0, "no three free ports in a row in 7200-7214 on 127.0.0.1");
  if (!P) return;
  std::printf("info: ports: P = %u\n", P);
  LogSink l1, l2, l3;
  net::Transport t1, t2, t3;
  std::string e1, e2, e3;
  CHECK(t1.Start(LoopCfg(P), l1.Fn(), &e1));
  CHECKF(t1.port() == P, "the first transport preferring %u got %u", P, t1.port());
  CHECK(t2.Start(LoopCfg(P), l2.Fn(), &e2));
  CHECKF(t2.port() == P + 1, "the second transport preferring %u got %u (expected %u)", P, t2.port(), P + 1);
  net::Config ex = LoopCfg(P);
  ex.exact_port = true;
  CHECK(!t3.Start(ex, l3.Fn(), &e3));
  CHECKF(e3.find(std::to_string(P)) != std::string::npos, "the exact-port error does not name %u: \"%s\"", P, e3.c_str());
  CHECK(t3.port() == 0);
  SleepMs(50);
  CHECK(l3.CountPrefix("[doa4][NET] transport started") == 0);  // a failed Start starts no thread
  CHECK(l1.WaitPrefix("[doa4][NET] transport started", 1000));
  CHECK(l1.CountExact("[doa4][NET] transport started on port " + std::to_string(P)) == 1);
  CHECK(l2.WaitPrefix("[doa4][NET] transport started", 1000));
  CHECK(l2.CountExact("[doa4][NET] transport started on port " + std::to_string(P + 1)) == 1);
  // exact_port on a free port takes exactly it (the other side of the rule np_pair relies on).
  t2.Stop();
  LogSink l4;
  net::Transport t4;
  std::string e4;
  net::Config ex2 = LoopCfg(uint16_t(P + 1));
  ex2.exact_port = true;
  CHECKF(t4.Start(ex2, l4.Fn(), &e4) && t4.port() == P + 1, "exact %u on a free port: got %u (%s)", P + 1, t4.port(), e4.c_str());
  // Past the range: the last port of the range taken, a transport preferring it gets any free port.
  if (PortFree(rules::kPortBase + rules::kPorts - 1)) {
    const uint16_t last = rules::kPortBase + rules::kPorts - 1;
    LogSink l5, l6;
    net::Transport t5, t6;
    std::string e5, e6;
    CHECK(t5.Start(LoopCfg(last), l5.Fn(), &e5) && t5.port() == last);
    CHECK(t6.Start(LoopCfg(last), l6.Fn(), &e6));
    CHECKF(t6.port() != 0 && (t6.port() < rules::kPortBase || t6.port() > last), "preferring %u (taken) gave %u", last, t6.port());
  } else {
    std::printf("info: ports: 7215 is in use; the any-port fallback was not checked\n");
  }
  // A second Start of the same object fails (a Transport starts once).
  std::string again;
  CHECK(!t1.Start(LoopCfg(0), l1.Fn(), &again) && !again.empty());
  CHECK(t1.port() == P);
}

// =====================================================================================================
// Queue full, broadcast, wrong thread
// =====================================================================================================

static void TestQueueFull() {
  Group("transport, inbound ring full");
  Pair p;
  CHECK(p.Start(LoopCfg(), LoopCfg()));
  p.a.SetPeer(Loop(p.b.port()), kToken);
  p.b.SetPeer(Loop(p.a.port()), kToken);
  constexpr uint32_t kN = 5000;
  for (uint32_t i = 0; i < kN; ++i) {
    CHECKF(PushRetry(p.a, Payload(i, 8)), "push %u refused for 2 s", i);
    if (i % 200 == 199) SleepMs(5);  // paced: the socket buffer must not be what loses them
  }
  // Wait until B has seen all of them (dropped + queued), then pop.
  uint32_t qf = 0, last = 0xFFFFFFFFu;
  for (int i = 0; i < 300; ++i) {
    qf = p.b.stats().queue_full;
    if (qf + net::Transport::kGekkoQueue >= kN && qf == last) break;
    last = qf;
    SleepMs(10);
  }
  SleepMs(50);
  std::vector<uint32_t> popped;
  Bytes got;
  while (p.b.PopGekko(&got)) popped.push_back(SeqOf(got));
  const net::Stats sb = p.b.stats();
  std::printf("info: queue full: B queue_full %u, popped %zu\n", sb.queue_full, popped.size());
  CHECK(sb.queue_full > 0);
  CHECKF(sb.queue_full + popped.size() == kN, "queue_full %u + popped %zu != %u", sb.queue_full, popped.size(), kN);
  CHECKF(popped.size() == net::Transport::kGekkoQueue, "popped %zu (the ring holds %zu)", popped.size(), net::Transport::kGekkoQueue);
  bool first_ones = popped.size() > 0;  // the ring keeps the first that arrived: 0, 1, 2, ...
  for (size_t i = 0; first_ones && i < popped.size(); ++i) first_ones = popped[i] == i;
  CHECK(first_ones);
  CHECK(p.a.stats().queue_full == 0);  // A's ring never refused (PushRetry waited)
}

static void TestBroadcastLoopback() {
  Group("transport, Broadcast with loopback is a no-op");
  Udp r;
  CHECK(r.ok());
  LogSink l;
  // Counted BEFORE the call: counted after it, the receiver can see the datagram before the counter moves.
  std::atomic<int> want_bcast{0}, want_send{0}, did_bcast{0}, did_send{0};
  net::Transport t;
  t.on_tick = [&](uint32_t) {
    if (want_bcast.exchange(0)) {
      ++did_bcast;
      t.Broadcast(r.port, Bytes{0xB0, 0xB1, 0xB2});
    }
    if (want_send.exchange(0)) {
      ++did_send;
      t.SendTo(Loop(r.port), Bytes{0x50, 0x51});
    }
  };
  net::Config c = LoopCfg();
  c.lan_broadcast = true;  // asked for, but loopback turns it off
  std::string e;
  CHECK(t.Start(c, l.Fn(), &e));
  want_bcast = 1;
  Bytes got;
  const bool any = r.Recv(300, &got);
  CHECKF(did_bcast == 1, "on_tick called Broadcast %d times (expected 1: the check must run the call)", did_bcast.load());
  CHECKF(!any, "the socket on 127.0.0.1:%u received %zu bytes from a loopback Broadcast", r.port, got.size());
  // Positive control: the same socket does receive what on_tick sends it directly.
  want_send = 1;
  CHECKF(r.Recv(1000, &got) && got == Bytes({0x50, 0x51}), "the control SendTo from on_tick did not arrive");
  CHECK(did_send == 1);
  CHECK(t.stats().wrong_thread == 0);
  t.Stop();
}

static void TestWrongThread() {
  Group("transport, socket calls off the I/O thread");
  Udp r;
  CHECK(r.ok());
  LogSink l;
  std::atomic<int> want_send{0}, did_send{0};
  net::Transport t;
  t.on_tick = [&](uint32_t) {
    if (want_send.exchange(0)) {
      ++did_send;  // before the call (see TestBroadcastLoopback)
      t.SendTo(Loop(r.port), Bytes{0x70, 0x71});
    }
  };
  std::string e;
  CHECK(t.Start(LoopCfg(), l.Fn(), &e));
  CHECK(l.WaitPrefix("[doa4][NET] transport started", 1000));
  t.SendTo(Loop(r.port), Bytes{0x61, 0x62});  // the test's main thread: refused
  CHECKF(t.stats().wrong_thread == 1, "wrong_thread %u after one SendTo off the I/O thread", t.stats().wrong_thread);
  CHECKF(l.CountPrefix("WARN transport:") == 1, "%d WARN transport: lines (expected 1)", l.CountPrefix("WARN transport:"));
  Bytes got;
  CHECKF(!r.Recv(300, &got), "the refused SendTo sent %zu bytes", got.size());
  // Positive control: the same SendTo from on_tick (the I/O thread) arrives and is not counted.
  want_send = 1;
  CHECKF(r.Recv(1000, &got) && got == Bytes({0x70, 0x71}), "SendTo from on_tick did not arrive");
  CHECK(did_send == 1 && t.stats().wrong_thread == 1);
  t.Broadcast(r.port, Bytes{1});  // Broadcast is an I/O-thread call too
  CHECK(t.stats().wrong_thread == 2 && l.CountPrefix("WARN transport:") == 2);
  t.Stop();
}

// =====================================================================================================
// STUN with fake servers on loopback
// =====================================================================================================

// A STUN binding success for transaction `tid` (12 bytes) carrying `e` as its XOR-MAPPED-ADDRESS.
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
  rules::Endpoint answer;  // the XOR-MAPPED-ADDRESS it reports
  int ignore_first = 0;    // requests left unanswered at the start
  bool silent = false;     // never answers
  std::atomic<bool> stop{false};
  std::mutex mu;
  std::vector<int64_t> req_us;          // when each binding request arrived
  std::array<uint8_t, 12> last_tid{};   // the transaction id of the last request
  int answers = 0;                      // answers sent
  std::thread th;
  void Begin() {
    th = std::thread([this] {
      while (!stop.load()) {
        Bytes d;
        uint16_t from = 0;
        if (!sock.Recv(5, &d, &from)) continue;
        const int64_t t = NowUs();
        if (d.size() != 20 || d[0] != 0 || d[1] != 1 || d[4] != 0x21 || d[5] != 0x12 || d[6] != 0xA4 || d[7] != 0x42) continue;
        int n;
        {
          std::lock_guard<std::mutex> lock(mu);
          req_us.push_back(t);
          std::copy(d.begin() + 8, d.begin() + 20, last_tid.begin());
          n = int(req_us.size());
        }
        if (silent || n <= ignore_first) continue;
        const Bytes r = StunSuccess(d.data() + 8, answer);
        if (sock.SendTo(from, r) == int(r.size())) {
          std::lock_guard<std::mutex> lock(mu);
          ++answers;
        }
      }
    });
  }
  void End() {
    stop = true;
    if (th.joinable()) th.join();
  }
  std::vector<int64_t> Requests() {
    std::lock_guard<std::mutex> lock(mu);
    return req_us;
  }
  int Answers() {
    std::lock_guard<std::mutex> lock(mu);
    return answers;
  }
  std::array<uint8_t, 12> LastTid() {
    std::lock_guard<std::mutex> lock(mu);
    return last_tid;
  }
  ~FakeStun() { End(); }
};

static rules::Endpoint Pub(uint16_t port) {
  rules::Endpoint e;
  e.ip = {203, 0, 113, 7};
  e.port = port;
  return e;
}

struct StunRun {
  net::Stats at_class;  // stats when the class was reached (or at the 2 s limit)
  int64_t class_ms = -1;
  net::Stats end;
  int answers_sent = 0;
  std::vector<int64_t> req[2];
};

// Starts a transport with the two fakes as its STUN servers, waits up to 2 s for `want`, keeps running until
// `run_ms` after Start, then silences the fakes, lets their last answers land, and stops.
static StunRun RunStun(FakeStun& f1, FakeStun& f2, rules::Nat want, int run_ms, LogSink& log) {
  StunRun out;
  f1.Begin();
  f2.Begin();
  net::Config c = LoopCfg();
  c.stun = true;
  c.stun_servers = {"127.0.0.1:" + std::to_string(f1.sock.port), "127.0.0.1:" + std::to_string(f2.sock.port)};
  c.stun_refresh_ms = 300;
  net::Transport t;
  std::string e;
  const int64_t t0 = NowUs();
  CHECKF(t.Start(c, log.Fn(), &e), "start: %s", e.c_str());
  const net::Stats first = t.stats();
  CHECK(first.nat == rules::Nat::kNoAnswer && !first.mapped && first.stun_answers == 0);  // before any answer
  while (NowUs() - t0 < 2'000'000) {
    const net::Stats s = t.stats();
    if (s.nat == want && s.mapped) {
      out.class_ms = (NowUs() - t0) / 1000;
      break;
    }
    SleepMs(5);
  }
  out.at_class = t.stats();
  while (NowUs() - t0 < int64_t(run_ms) * 1000) SleepMs(10);
  f1.End();
  f2.End();
  SleepMs(100);  // answers in flight land
  out.end = t.stats();
  out.answers_sent = f1.Answers() + f2.Answers();
  t.Stop();
  out.req[0] = f1.Requests();
  out.req[1] = f2.Requests();
  return out;
}

// Every interval between consecutive requests in [from, to) is lo..hi ms; prints them.
static bool Intervals(const std::vector<int64_t>& r, size_t from, size_t to, int lo, int hi, const char* label) {
  bool ok = to <= r.size() && from + 1 < to;
  std::string list;
  for (size_t i = from + 1; i < std::min(to, r.size()); ++i) {
    const int ms = int((r[i] - r[i - 1]) / 1000);
    list += " " + std::to_string(ms);
    ok = ok && ms >= lo && ms <= hi;
  }
  std::printf("info: %s: intervals (ms)%s\n", label, list.c_str());
  return ok;
}

static void TestStun() {
  Group("transport, STUN against two fake servers on loopback");
  {  // equal answers: kCone, mapped, every request answered and counted, refresh every 300 ms
    FakeStun f1, f2;
    f1.answer = f2.answer = Pub(51234);
    LogSink l;
    StunRun r = RunStun(f1, f2, rules::Nat::kCone, 1700, l);
    CHECKF(r.class_ms >= 0, "kCone with a mapped address not reached within 2 s (nat %d)", int(r.at_class.nat));
    CHECK(r.at_class.mapped && *r.at_class.mapped == Pub(51234));
    CHECKF(r.end.stun_answers == uint32_t(r.answers_sent) && r.answers_sent > 0, "stun_answers %u, the fakes sent %d",
           r.end.stun_answers, r.answers_sent);
    CHECK(r.end.nat == rules::Nat::kCone);
    // R20: answered at once, so every request after the first is a refresh, 300 +- 50 ms apart (>= 4 in 1.7 s).
    CHECKF(r.req[0].size() >= 5, "server 1 saw %zu requests in 1.7 s", r.req[0].size());
    CHECK(Intervals(r.req[0], 0, r.req[0].size(), 250, 350, "cone, server 1 (refresh 300)"));
    CHECK(Intervals(r.req[1], 0, r.req[1].size(), 250, 350, "cone, server 2 (refresh 300)"));
    std::printf("info: STUN cone: class after %lld ms, %u answers\n", (long long)r.class_ms, r.end.stun_answers);
  }
  {  // different mapped ports: kSymmetric
    FakeStun f1, f2;
    f1.answer = Pub(51234);
    f2.answer = Pub(51235);
    LogSink l;
    StunRun r = RunStun(f1, f2, rules::Nat::kSymmetric, 600, l);
    CHECKF(r.class_ms >= 0, "kSymmetric not reached within 2 s (nat %d)", int(r.at_class.nat));
    CHECKF(r.at_class.mapped && *r.at_class.mapped == Pub(51234), "mapped is not the first server's answer");
    CHECK(r.end.stun_answers == uint32_t(r.answers_sent));
  }
  {  // the second server silent: kOneAnswer, mapped from the first; the silent one is still asked at the refresh rate
    FakeStun f1, f2;
    f1.answer = Pub(51234);
    f2.silent = true;
    LogSink l;
    StunRun r = RunStun(f1, f2, rules::Nat::kOneAnswer, 1300, l);
    CHECKF(r.class_ms >= 0, "kOneAnswer not reached within 2 s (nat %d)", int(r.at_class.nat));
    CHECK(r.end.nat == rules::Nat::kOneAnswer && r.end.mapped && *r.end.mapped == Pub(51234));
    CHECK(r.end.stun_answers == uint32_t(r.answers_sent) && f2.Answers() == 0);
    CHECK(Intervals(r.req[1], 0, r.req[1].size(), 250, 350, "one silent, server 2"));
  }
  {  // the first two requests unanswered: they repeat every 500 ms, then every 300 ms once answered
    FakeStun f1, f2;
    f1.answer = f2.answer = Pub(40000);
    f1.ignore_first = f2.ignore_first = 2;
    LogSink l;
    StunRun r = RunStun(f1, f2, rules::Nat::kCone, 2600, l);
    CHECKF(r.class_ms >= 900, "kCone after %lld ms (expected after the third request, ~1000 ms)", (long long)r.class_ms);
    CHECKF(r.req[0].size() >= 6, "server 1 saw %zu requests", r.req[0].size());
    CHECK(Intervals(r.req[0], 0, 3, 450, 550, "retry, server 1 (500 until answered)"));
    CHECK(Intervals(r.req[0], 2, r.req[0].size(), 250, 350, "retry, server 1 (300 once answered)"));
    CHECK(r.end.stun_answers == uint32_t(r.answers_sent));
  }
  {  // no server answers at all: kNoAnswer, and requests every 500 ms
    FakeStun f1, f2;
    f1.silent = f2.silent = true;
    LogSink l;
    StunRun r = RunStun(f1, f2, rules::Nat::kCone, 1300, l);
    CHECK(r.class_ms < 0 && r.end.nat == rules::Nat::kNoAnswer && !r.end.mapped && r.end.stun_answers == 0);
    CHECK(Intervals(r.req[0], 0, r.req[0].size(), 450, 550, "none answer, server 1"));
  }
}

// =====================================================================================================
// The simulator
// =====================================================================================================

static void TestSimOracle() {
  Group("simulator, the seeded decision function");
  net::sim::Params p;
  CHECK(net::sim::Parse("40,0,25", &p) && p.delay_ms == 40 && p.jitter_ms == 0 && p.loss_pct == 25);
  CHECK(net::sim::Parse("60,15,2", &p) && p.delay_ms == 60 && p.jitter_ms == 15 && p.loss_pct == 2);
  CHECK(net::sim::Parse("0,0,100", &p) && p.loss_pct == 100);
  for (const char* bad : {"", "40,0", "40,0,25,1", "40,0,101", "a,0,0", "-1,0,0", "40,,25", " 40,0,25", "40,0,25 ", "10001,0,0"}) {
    net::sim::Params q;
    q.delay_ms = 7;
    CHECKF(!net::sim::Parse(bad, &q) && q.delay_ms == 7, "Parse accepted \"%s\"", bad);
  }
  // Loss rate over 100,000 draws: 25% +- 1%; delays exactly 40 with no jitter; pure (the same draw twice agrees).
  net::sim::Parse("40,0,25", &p);
  int drops = 0, bad_delay = 0, impure = 0;
  for (uint64_t n = 0; n < 100000; ++n) {
    const net::sim::Decision d = net::sim::Decide(p, 7, n);
    drops += d.drop ? 1 : 0;
    bad_delay += (!d.drop && d.delay_ms != 40) ? 1 : 0;
    const net::sim::Decision again = net::sim::Decide(p, 7, n);
    impure += (again.drop != d.drop || again.delay_ms != d.delay_ms) ? 1 : 0;
  }
  std::printf("info: simulator: %d of 100000 dropped at 25%%\n", drops);
  CHECKF(drops >= 24000 && drops <= 26000, "%d of 100,000 dropped (expected 25%% +- 1%%)", drops);
  CHECK(bad_delay == 0 && impure == 0);
  // Another seed draws another sequence.
  int differ = 0;
  for (uint64_t n = 0; n < 1000; ++n) differ += net::sim::Decide(p, 7, n).drop != net::sim::Decide(p, 8, n).drop ? 1 : 0;
  CHECK(differ > 100);
  // Jitter: 60 +- 15, both edges reached, mean near 60.
  net::sim::Parse("60,15,0", &p);
  uint32_t lo = 1000, hi = 0;
  double sum = 0;
  int jitter_drops = 0;
  for (uint64_t n = 0; n < 100000; ++n) {
    const net::sim::Decision d = net::sim::Decide(p, 3, n);
    jitter_drops += d.drop ? 1 : 0;
    lo = std::min(lo, d.delay_ms);
    hi = std::max(hi, d.delay_ms);
    sum += d.delay_ms;
  }
  CHECKF(lo == 45 && hi == 75 && sum / 100000 > 59.5 && sum / 100000 < 60.5, "jitter 60,15: delays %u..%u, mean %.2f", lo, hi,
         sum / 100000);
  CHECK(jitter_drops == 0);  // loss 0: nothing dropped
  // A bad DOA4_NP_SIM refuses the start (a test switch with a typo must not silently run without the simulator).
  LogSink l;
  net::Transport t;
  net::Config c = LoopCfg();
  c.sim = "40;0;25";
  std::string e;
  CHECKF(!t.Start(c, l.Fn(), &e) && e.find("40;0;25") != std::string::npos, "a bad sim text started (error \"%s\")", e.c_str());
}

static void TestSimTransport() {
  Group("simulator, 2,000 payloads through sim 40,0,25 seed 7");
  Pair p;
  net::Config ca = LoopCfg();
  ca.sim = "40,0,25";
  ca.sim_seed = 7;
  CHECK(p.Start(ca, LoopCfg()));
  p.a.SetPeer(Loop(p.b.port()), kToken);
  p.b.SetPeer(Loop(p.a.port()), kToken);
  net::sim::Params sp;
  net::sim::Parse("40,0,25", &sp);
  constexpr uint32_t kN = 2000;
  std::set<uint32_t> kept;  // the oracle: draw n is the n-th datagram A sends, i.e. payload n
  for (uint32_t n = 0; n < kN; ++n)
    if (!net::sim::Decide(sp, 7, n).drop) kept.insert(n);
  std::vector<int64_t> pushed_us(kN, 0);
  std::set<uint32_t> delivered;
  int64_t min_lag_us = INT64_MAX;
  uint32_t dup = 0, unknown = 0;
  auto drain = [&] {
    Bytes got;
    while (p.b.PopGekko(&got)) {
      const int64_t now = NowUs();
      const uint32_t s = SeqOf(got);
      if (s >= kN || got != Payload(s, 16)) {
        ++unknown;
        continue;
      }
      if (!delivered.insert(s).second) ++dup;
      min_lag_us = std::min(min_lag_us, now - pushed_us[s]);
    }
  };
  for (uint32_t i = 0; i < kN; ++i) {
    pushed_us[i] = NowUs();
    CHECKF(PushRetry(p.a, Payload(i, 16)), "push %u refused for 2 s", i);
    if (i % 100 == 99) SleepMs(2);
    drain();
  }
  const int64_t until = NowUs() + 2'000'000;
  while (delivered.size() < kept.size() && NowUs() < until) {
    WaitForSingleObject(static_cast<HANDLE>(p.b.gekko_arrived()), 20);
    drain();
  }
  SleepMs(150);
  drain();
  const net::Stats sa = p.a.stats();
  std::printf("info: simulator: oracle keeps %zu of %u; delivered %zu; A sim_dropped %u sim_delayed %u; min lag %.1f ms\n", kept.size(),
              kN, delivered.size(), sa.sim_dropped, sa.sim_delayed, min_lag_us / 1000.0);
  CHECKF(delivered == kept, "delivered %zu payloads, the oracle keeps %zu (sets differ)", delivered.size(), kept.size());
  CHECK(dup == 0 && unknown == 0);
  CHECKF(sa.sim_dropped == kN - kept.size(), "sim_dropped %u, oracle drops %zu", sa.sim_dropped, kN - kept.size());
  CHECKF(sa.sim_delayed == delivered.size(), "sim_delayed %u, delivered %zu", sa.sim_delayed, delivered.size());
  CHECKF(min_lag_us >= 40'000, "a payload arrived %.2f ms after it was pushed (expected >= 40)", min_lag_us / 1000.0);
  const net::Stats sb = p.b.stats();
  CHECK(sb.sim_dropped == 0 && sb.sim_delayed == 0 && SumDropped(sb) == 0);  // B runs no simulator
}

// The deterministic drop option (Config::sim_drop, DOA4_NP_SIM_DROP; research item 5): the parser, then exactly the
// listed kGekko datagrams are dropped, counted and logged with their GekkoNet packet type, and a dropped one takes no
// draw of the random simulator.
static void TestSimDrop() {
  Group("simulator, deterministic drops (sim_drop)");
  std::vector<uint32_t> ks = {99};
  CHECK(net::sim::ParseDrop("", &ks) && ks.empty());
  CHECK(net::sim::ParseDrop("5", &ks) && ks == std::vector<uint32_t>({5}));
  CHECK(net::sim::ParseDrop("7,3,7,0", &ks) && ks == std::vector<uint32_t>({0, 3, 7}));
  CHECK(net::sim::ParseDrop("999999999", &ks) && ks == std::vector<uint32_t>({999999999}));
  std::string sixty_four;
  for (int i = 0; i < 64; ++i) sixty_four += (i ? "," : "") + std::to_string(i);
  const std::string sixty_five = sixty_four + ",64";
  CHECK(net::sim::ParseDrop(sixty_four, &ks) && ks.size() == 64);
  for (const char* bad : {"1,,2", "a", "1,2,", ",1", " 1", "1 ", "-1", "1000000000"}) {
    std::vector<uint32_t> keep = {42};
    CHECKF(!net::sim::ParseDrop(bad, &keep) && keep == std::vector<uint32_t>({42}), "ParseDrop accepted \"%s\"", bad);
  }
  {
    std::vector<uint32_t> keep = {42};
    CHECK(!net::sim::ParseDrop(sixty_five, &keep) && keep.size() == 1);
  }
  // Drops only: payloads 3 and 10 of 20 never arrive, nothing else is missing; the log names each with its type byte.
  {
    Pair p;
    net::Config ca = LoopCfg();
    ca.sim_drop = {10, 3};  // unsorted on purpose: Start sorts it
    CHECK(p.Start(ca, LoopCfg()));
    p.a.SetPeer(Loop(p.b.port()), kToken);
    p.b.SetPeer(Loop(p.a.port()), kToken);
    std::set<uint32_t> got;
    for (uint32_t i = 0; i < 20; ++i) CHECK(PushRetry(p.a, Payload(i, 12)));
    const int64_t until = NowUs() + 1'000'000;
    while (got.size() < 18 && NowUs() < until) {
      Bytes b;
      while (p.b.PopGekko(&b)) got.insert(SeqOf(b));
      SleepMs(2);
    }
    SleepMs(100);
    Bytes b;
    while (p.b.PopGekko(&b)) got.insert(SeqOf(b));
    std::set<uint32_t> want;
    for (uint32_t i = 0; i < 20; ++i)
      if (i != 3 && i != 10) want.insert(i);
    CHECKF(got == want, "delivered %zu payloads; expected all of 0..19 but 3 and 10", got.size());
    CHECK(p.a.stats().sim_drop_hits == 2);
    CHECK(p.la.CountExact("[doa4][NET] sim_drop: kGekko datagram 3 dropped (GekkoNet packet type 3)") == 1);
    CHECK(p.la.CountExact("[doa4][NET] sim_drop: kGekko datagram 10 dropped (GekkoNet packet type 10)") == 1);
    CHECK(p.a.stats().sim_dropped == 0 && p.a.stats().sim_delayed == 0);  // the random simulator is off
  }
  // With the random simulator on: draw d is taken only by the datagrams sim_drop let through.
  {
    Pair p;
    net::Config ca = LoopCfg();
    ca.sim = "5,0,50";
    ca.sim_seed = 7;
    ca.sim_drop = {0, 5};
    CHECK(p.Start(ca, LoopCfg()));
    p.a.SetPeer(Loop(p.b.port()), kToken);
    p.b.SetPeer(Loop(p.a.port()), kToken);
    net::sim::Params sp;
    net::sim::Parse("5,0,50", &sp);
    std::set<uint32_t> want;
    uint64_t draw = 0;
    for (uint32_t i = 0; i < 40; ++i) {
      if (i == 0 || i == 5) continue;
      if (!net::sim::Decide(sp, 7, draw++).drop) want.insert(i);
    }
    for (uint32_t i = 0; i < 40; ++i) CHECK(PushRetry(p.a, Payload(i, 12)));
    SleepMs(300);
    std::set<uint32_t> got;
    Bytes b;
    while (p.b.PopGekko(&b)) got.insert(SeqOf(b));
    CHECKF(got == want, "with sim 5,0,50: delivered %zu, expected %zu (draws skip the dropped datagrams)", got.size(), want.size());
    const net::Stats sa = p.a.stats();
    CHECKF(sa.sim_drop_hits == 2 && sa.sim_dropped + sa.sim_delayed == 38, "hits %u, sim dropped %u delayed %u", sa.sim_drop_hits,
           sa.sim_dropped, sa.sim_delayed);
  }
}

// Review I-3(a): a binding success is taken only from a configured STUN server. A second socket sends a well-formed
// success with the CURRENT transaction id (copied from the fake's last request; the refresh is 60 s, so it stays
// current); it must change nothing and count as kMagic. Positive control: the same bytes from the server's own socket
// are taken (so the rogue datagram was a valid answer in everything but its source).
static void TestStunSource() {
  Group("transport, STUN answers only from a configured server");
  FakeStun f1, f2;
  f1.answer = f2.answer = Pub(51234);
  f1.Begin();
  f2.Begin();
  LogSink l;
  net::Transport t;
  net::Config c = LoopCfg();
  c.stun = true;
  c.stun_servers = {"127.0.0.1:" + std::to_string(f1.sock.port), "127.0.0.1:" + std::to_string(f2.sock.port)};
  c.stun_refresh_ms = 60000;
  std::string e;
  CHECKF(t.Start(c, l.Fn(), &e), "start: %s", e.c_str());
  for (int i = 0; i < 400 && t.stats().nat != rules::Nat::kCone; ++i) SleepMs(5);
  SleepMs(50);
  f1.End();  // its socket is now the test's (no more answers), and the transaction id stays the current one
  f2.End();
  const net::Stats before = t.stats();
  CHECK(before.nat == rules::Nat::kCone && before.mapped && *before.mapped == Pub(51234) && before.stun_answers == 2);
  const std::array<uint8_t, 12> tid = f1.LastTid();
  CHECK(tid == f2.LastTid());  // one round, one transaction id
  const Bytes rogue_answer = StunSuccess(tid.data(), Pub(40404));
  Udp rogue;
  rogue.SendTo(t.port(), rogue_answer);
  SleepMs(150);
  const net::Stats after = t.stats();
  CHECKF(after.stun_answers == before.stun_answers, "an answer from a non-server socket was counted (%u -> %u)", before.stun_answers,
         after.stun_answers);
  CHECKF(after.nat == rules::Nat::kCone && after.mapped && *after.mapped == Pub(51234), "the rogue answer changed nat %d / mapped",
         int(after.nat));
  CHECKF(after.dropped[size_t(wire::Drop::kMagic)] == before.dropped[size_t(wire::Drop::kMagic)] + 1, "kMagic %u -> %u",
         before.dropped[size_t(wire::Drop::kMagic)], after.dropped[size_t(wire::Drop::kMagic)]);
  f1.sock.SendTo(t.port(), rogue_answer);  // the positive control: from server 1's own address
  SleepMs(150);
  const net::Stats ctl = t.stats();
  CHECKF(ctl.stun_answers == before.stun_answers + 1 && ctl.nat == rules::Nat::kSymmetric, "control from the server: answers %u, nat %d",
         ctl.stun_answers, int(ctl.nat));
  CHECK(ctl.mapped && *ctl.mapped == Pub(40404));  // server 1 is the first configured: its answer is `mapped`
  t.Stop();
}

// Review I-3(b): the transport takes its port exclusively (SO_EXCLUSIVEADDRUSE). Without it Windows lets a later
// socket that sets SO_REUSEADDR bind the same address and port, and that socket then receives the transport's datagrams.
static SOCKET ReuseBind(uint16_t port, int* err) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  BOOL on = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    *err = WSAGetLastError();
    closesocket(s);
    return INVALID_SOCKET;
  }
  *err = 0;
  return s;
}

static void TestExclusiveBind() {
  Group("transport, the port is taken exclusively");
  {
    LogSink l;
    net::Transport t;
    std::string e;
    CHECK(t.Start(LoopCfg(0), l.Fn(), &e));
    int err = 0;
    SOCKET hijack = ReuseBind(t.port(), &err);
    CHECKF(hijack == INVALID_SOCKET, "a SO_REUSEADDR socket bound the transport's 127.0.0.1:%u", t.port());
    std::printf("info: exclusive: a reusing bind after the transport failed with %d\n", err);
    if (hijack != INVALID_SOCKET) closesocket(hijack);
    t.Stop();
  }
  {  // the other order: a reusing socket holds the port, an exact-port transport refuses it
    int err = 0;
    SOCKET holder = ReuseBind(0, &err);
    CHECK(holder != INVALID_SOCKET);
    sockaddr_in a{};
    int l = sizeof a;
    getsockname(holder, reinterpret_cast<sockaddr*>(&a), &l);
    const uint16_t q = ntohs(a.sin_port);
    LogSink ls;
    net::Transport t;
    net::Config c = LoopCfg(q);
    c.exact_port = true;
    std::string e;
    CHECKF(!t.Start(c, ls.Fn(), &e), "an exact-port transport took 127.0.0.1:%u from a socket that holds it", q);
    std::printf("info: exclusive: exact %u under a reusing holder: \"%s\"\n", q, e.c_str());
    closesocket(holder);
  }
}

// Review I-3(c): PushGekko refused on a full outbound ring counts queue_full. The I/O thread is held inside on_tick
// (bounded wait) so it cannot drain: 1,024 pushes fit, the 1,025th is refused and counted.
static void TestOutboundQueueFull() {
  Group("transport, outbound ring full");
  LogSink l;
  HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr), release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::atomic<int> hold{0};
  net::Transport t;
  t.on_tick = [&](uint32_t) {
    if (hold.exchange(0)) {
      SetEvent(entered);
      WaitForSingleObject(release, 5000);
    }
  };
  std::string e;
  CHECK(t.Start(LoopCfg(), l.Fn(), &e));
  hold = 1;
  CHECK(WaitForSingleObject(entered, 2000) == WAIT_OBJECT_0);
  int accepted = 0;
  for (uint32_t i = 0; i < net::Transport::kGekkoQueue; ++i) accepted += t.PushGekko(Payload(i, 8)) ? 1 : 0;
  const bool refused = !t.PushGekko(Payload(9999, 8));
  CHECKF(accepted == int(net::Transport::kGekkoQueue) && refused, "accepted %d of %zu, 1,025th refused %d", accepted,
         net::Transport::kGekkoQueue, int(refused));
  CHECKF(t.stats().queue_full == 1, "queue_full %u after one refused PushGekko", t.stats().queue_full);
  SetEvent(release);
  t.Stop();
  CloseHandle(entered);
  CloseHandle(release);
}

// Review M-3: the log sink is never called once Stop has returned (a misuse after Stop is counted, not logged).
static void TestNoLogAfterStop() {
  Group("transport, no log call after Stop");
  Udp r;
  LogSink l;
  net::Transport t;
  std::string e;
  CHECK(t.Start(LoopCfg(), l.Fn(), &e));
  CHECK(l.WaitPrefix("[doa4][NET] transport started", 1000));
  t.SendTo(Loop(r.port), Bytes{1});  // control: before Stop the misuse logs its WARN line
  CHECK(l.CountPrefix("WARN transport:") == 1);
  t.Stop();
  size_t lines;
  {
    std::lock_guard<std::mutex> lock(l.mu);
    lines = l.lines.size();
  }
  t.SendTo(Loop(r.port), Bytes{2});
  t.Broadcast(r.port, Bytes{3});
  size_t lines_after;
  {
    std::lock_guard<std::mutex> lock(l.mu);
    lines_after = l.lines.size();
  }
  CHECKF(lines_after == lines, "the sink got %zu lines after Stop returned", lines_after - lines);
  CHECK(t.stats().wrong_thread == 3);  // still counted
}

// Wi-Fi media streaming mode (research item 1; DOA2U recomp by Parker Allan, MIT). Off: nothing loads wlanapi.dll and no
// line is logged. On: Start and Stop succeed and one line gives the number of interfaces set (0 on a PC without Wi-Fi;
// the mode itself is checked on a Wi-Fi PC at plan 1b's real-world gate). The WLAN handle is closed by Stop, which
// releases the mode, so the test leaves nothing set.
static void TestWifi() {
  Group("transport, Wi-Fi media streaming mode");
  const bool loaded_before = GetModuleHandleW(L"wlanapi.dll") != nullptr;
  CHECKF(!loaded_before, "fixture: wlanapi.dll was already loaded, so the off case cannot be told apart");
  {
    LogSink l;
    net::Transport t;
    std::string e;
    CHECK(t.Start(LoopCfg(), l.Fn(), &e));
    CHECK(l.WaitPrefix("[doa4][NET] transport started", 1000));
    SleepMs(100);
    CHECK(l.CountPrefix("[doa4][NET] Wi-Fi") == 0);
    CHECK(t.stats().wifi_interfaces == -1);
    t.Stop();
    CHECKF(GetModuleHandleW(L"wlanapi.dll") == nullptr, "flag off, but wlanapi.dll was loaded");
  }
  {
    LogSink l;
    net::Transport t;
    net::Config c = LoopCfg();
    c.wifi_streaming = true;
    std::string e;
    CHECK(t.Start(c, l.Fn(), &e));
    CHECK(l.WaitPrefix("[doa4][NET] Wi-Fi media streaming mode", 3000));
    CHECK(l.CountPrefix("[doa4][NET] Wi-Fi") == 1);
    const int32_t n = t.stats().wifi_interfaces;
    {
      std::lock_guard<std::mutex> lock(l.mu);
      for (const std::string& line : l.lines)
        if (line.rfind("[doa4][NET] Wi-Fi", 0) == 0) std::printf("info: %s\n", line.c_str());
    }
    CHECKF(n >= 0, "wifi_interfaces %d with the flag on", n);
    CHECK(l.CountExact("[doa4][NET] Wi-Fi media streaming mode (background scans off) on " + std::to_string(n) + " interface(s)") == 1);
    CHECK(GetModuleHandleW(L"wlanapi.dll") != nullptr);
    const int64_t s0 = NowUs();
    t.Stop();
    std::printf("info: Wi-Fi: Stop took %.3f ms\n", (NowUs() - s0) / 1000.0);
    CHECKF(NowUs() - s0 <= 200'000, "Stop with the WLAN handle took %.1f ms", (NowUs() - s0) / 1000.0);
  }
}

// =====================================================================================================
// Stop
// =====================================================================================================

static void TestStop() {
  Group("transport, Stop");
  net::Transport idle;
  idle.Stop();  // never started: nothing to do
  LogSink l;
  std::atomic<int> ticks{0};
  net::Transport t;
  std::string e;
  t.on_tick = [&](uint32_t) { ++ticks; };
  CHECK(t.Start(LoopCfg(), l.Fn(), &e));
  const uint16_t port = t.port();
  CHECK(l.WaitPrefix("[doa4][NET] transport started", 1000));
  // on_tick every 5 ms: about 100 in 500 ms. FLOOR 80: a tick on the default 15.6 ms timer resolution (a plain 5 ms
  // wait timeout in this process) gives about 32.
  const int t_before = ticks.load();
  const int64_t w0 = NowUs();
  SleepMs(500);
  const int in_window = ticks.load() - t_before;
  const double window_ms = (NowUs() - w0) / 1000.0;
  std::printf("info: on_tick ran %d times in %.1f ms\n", in_window, window_ms);
  CHECKF(in_window >= 80 && in_window <= 105 * window_ms / 500, "on_tick ran %d times in %.1f ms (expected ~one per 5 ms)", in_window,
         window_ms);
  const int64_t s0 = NowUs();
  t.Stop();
  const int64_t stop_us = NowUs() - s0;
  std::printf("info: Stop took %.3f ms\n", stop_us / 1000.0);
  CHECKF(stop_us <= 200'000, "Stop took %.1f ms (expected <= 200)", stop_us / 1000.0);
  const int after = ticks.load();
  SleepMs(30);
  CHECK(ticks.load() == after);  // the thread is gone
  Udp again(port);
  CHECKF(again.ok(), "127.0.0.1:%u could not be bound after Stop (error %d)", port, WSAGetLastError());
  t.Stop();  // twice: harmless
}

// =====================================================================================================

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a run that dies still shows how far it got (probe round 1 lost a crash)
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(180));
    std::printf("FAIL watchdog: the run exceeded 180 s (%d checks, %d failures so far)\n", g_checks, g_failures);
    std::fflush(stdout);
    std::_Exit(3);
  }).detach();
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
  TestSpscOneThread();
  TestSpscTwoThreads();
  TestDeliveryAndFilters();
  TestPorts();
  TestQueueFull();
  TestBroadcastLoopback();
  TestWrongThread();
  TestStun();
  TestSimOracle();
  TestSimTransport();
  TestSimDrop();
  TestStunSource();
  TestExclusiveBind();
  TestOutboundQueueFull();
  TestNoLogAfterStop();
  TestStop();
  TestWifi();  // last: once wlanapi.dll is loaded it stays loaded
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  std::fflush(stdout);
  return g_failures ? 1 : 0;
}

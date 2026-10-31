// Host test for src/net/np_live (the online session core on the game thread) over the real GekkoNet (np_gekko) and the
// executor (np_exec). No SDK, no socket, no file. Built and run by run_np_live_test.bat; "-v" prints each group.
//
// The world: two live::Sessions, A (slot 0) and B (slot 1), each driven frame by frame by its own Windows FIBER on the
// test's one thread, over a FAKE clock shared by both. A side runs until it blocks in its Clock (wait_packet or
// sleep_precise); when no side can run, the scheduler moves the fake time to the earliest wake-up (a wait's deadline,
// a packet's release, a late side's start). So a 61 s wait costs microseconds of real time, and every run is
// deterministic except for GekkoNet's handshake (below).
//
// The fake clock (contract, one sentence each):
//  - now_us() returns the time and then adds 1 us (so every measured bucket around a call is > 0);
//  - wait_packet(ms) returns at once if a released packet waits for this side, else when one is released or after
//    ms ms of fake time, whichever is sooner; it records (time, ms, banner_waiting()) where time = the value now_us()
//    last returned to this side;
//  - sleep_precise(us) advances this side by us (it is never woken by a packet) and records us.
// The network (contract): every payload is delivered whole, once, in order, released at its send time, except that a
// HOLD on side X's inbound keeps every payload sent to X from the moment it is armed (on the peer's frame, before the
// peer sends the held input) until `begin + duration`, where begin = the time of X's first wait in Frame(hold frame).
// A real link loses, reorders and duplicates: that is the transport's tests (Task 7), not this file's.
//
// GekkoNet's handshake is paced by the steady clock (SyncRequest / SyncResponse at most every 200 ms, backend.cpp:
// 364-388, NUM_TO_SYNC 4, backend.h:260), which a fake clock cannot advance. So while both sides run and either has
// not yet received an Inputs payload, each step of fake time is also slept in real time (fake <= real, so the
// handshake costs at most its real duration in fake time; budget 3 s of real sleep per world; printed per world).
//
// Brief deviations, each with its trace in the group (see the report): B is held too when A is held (lockstep: B
// lacks A's input for frame 103), and a PeerLeft delivered after A's leave reaches a B that may already have advanced
// the frames A sent before leaving (200 and 201 with delay 2).

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../src/net/np_exec.h"
#include "../../src/net/np_gekko.h"
#include "../../src/net/np_live.h"
#include "../../src/net/np_rules.h"

using namespace doa4::np;

static int g_checks = 0;
static int g_failures = 0;
static bool g_verbose = false;

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

static void Group(const char* name) {
  if (g_verbose) std::printf("group: %s\n", name);
}

using Bytes = std::vector<uint8_t>;
constexpr uint64_t kInf = UINT64_MAX;
constexpr int kDelay = 2;
constexpr int kNoFrame = -1000;
constexpr uint64_t kMs = 1000;
constexpr uint64_t kSec = 1000 * kMs;

// ---- real-time watchdog: one thread for the whole run; each world sets its own deadline ----
static std::atomic<long long> g_deadline_ms{0};  // 0 = none
static std::atomic<bool> g_done{false};
static const char* g_world_label = "";
static long long RealMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the world ----
struct WaitRec { uint64_t t; uint32_t ms; bool banner; int32_t frame; };
struct EvRec { live::Event ev; int32_t frame; uint64_t at; int32_t slack; };
struct Pkt { uint64_t release; Bytes bytes; };

struct World;

struct Actor {
  World* w = nullptr;
  int s = 0;
  gekko::Session gk;
  exec::Executor ex;
  std::unique_ptr<live::Session> live;
  void* fiber = nullptr;
  enum St { kNotStarted, kRunnable, kWaitPacket, kSleep, kDone } st = kNotStarted;
  uint64_t start_at = 0;
  uint64_t wake = 0;
  int32_t frames = 300;  // Frame calls to make (0 .. frames-1) unless an end comes first
  int32_t cur = -1;      // the frame of the Frame call in progress
  uint64_t last_now = 0; // the value now_us() last returned to this side
  // the fake game state, advanced from the staged records identically on both sides
  uint64_t state = 0xCBF29CE484222325ull;
  int32_t last_frame = -1;
  int bad_frame = kNoFrame;
  uint32_t cs_handed[2] = {0, 0};  // [0] the checksum handed for frame 150's Save (Frame(151)), [1] unused
  std::vector<std::pair<int32_t, live::FrameResult>> results;
  std::vector<uint64_t> call_t;    // the fake time at each Frame call (= the first reading inside it)
  std::vector<int> waits_in_call;  // wait_packet calls made inside each Frame call
  std::vector<int32_t> slack_after;      // remote_slack() after each Frame call
  std::vector<int32_t> last_remote_after;  // gekko last_remote_input_frame() after each Frame call
  std::vector<std::pair<int32_t, FrameInput>> taps;
  std::vector<WaitRec> waits;
  std::vector<EvRec> events;
  std::vector<uint32_t> sleeps;
  std::function<void(Actor&, int32_t)> before_frame;  // test hook, inside the fiber, before Frame(f)
  std::function<void(Actor&)> on_end;                 // test hook, inside the fiber, after an end was returned
  std::function<float()> fa;                          // hooks.frames_ahead (empty: GekkoNet's)
  bool got_inputs = false;  // received an Inputs payload (the handshake is over for this side)
  int sent = 0;
  Bytes first_inputs, first_sync_request;  // the first such payload this side sent (layout print)
  Actor() : ex(2, [this](int32_t f, const FrameInput& in) { taps.emplace_back(f, in); }) {}
};

struct Hold { bool on = false; bool armed = false; int32_t at_frame = 0; uint64_t dur = 0; uint64_t begin = kInf; uint64_t end = kInf; };

struct World {
  uint64_t t = 1'000'000;  // fake us (not 0: a reading of 0 must not look like "never")
  std::deque<Pkt> q[2];    // q[s]: what waits for side s
  Hold hold[2];            // hold[s]: side s's inbound
  Actor a[2];
  live::Limits limits;
  bool zero_magic_from[2] = {false, false};  // rewrite this side's SyncRequest rng (its session magic) to 0
  int rewrites = 0;
  long long pace_budget_ms = 3000;
  long long paced_ms = 0;
  uint64_t handshake_done_t = 0;  // fake time when both sides had received an Inputs payload
  bool deadlock = false, timed_out = false;
  const char* label = "";
};

static World* g_world = nullptr;
static void* g_main_fiber = nullptr;

static Pad LocalPad(int side, int index) {
  Pad p;
  p.buttons = uint16_t(0x1000 + 16 * side + (index & 0xF));
  p.lt = uint8_t(index);
  p.rt = uint8_t(side + 1);
  p.lx = int16_t(index * 3 + side);
  p.ly = int16_t(-index * 5 - side);
  p.rx = int16_t(index);
  p.ry = int16_t(side);
  return p;
}
static uint64_t Mix(uint64_t h, uint8_t b) { return (h ^ b) * 0x100000001b3ull; }

static uint32_t Entering(const Actor& A) {
  uint32_t ce = exec::FoldChecksum(A.last_frame, std::vector<uint64_t>{A.state}, std::vector<uint64_t>{uint64_t(A.last_frame) * 3});
  if (A.last_frame == A.bad_frame) ce ^= 1u;  // the state is perturbed for the checksum only: one wrong frame
  return ce;
}

// Blocks the calling fiber (an actor) and returns to the scheduler.
static void Block(Actor& A) { SwitchToFiber(g_main_fiber); }

static bool Released(const World& w, int s) { return !w.q[s].empty() && w.q[s].front().release <= w.t; }

static void WINAPI ActorMain(void* p) {
  Actor& A = *static_cast<Actor*>(p);
  for (int32_t f = 0; f < A.frames; ++f) {
    A.cur = f;
    if (A.before_frame) A.before_frame(A, f);
    const uint32_t ce = Entering(A);
    if (A.last_frame == 150) A.cs_handed[0] = ce;
    const size_t w0 = A.waits.size();
    A.call_t.push_back(A.w->t);
    live::FrameResult r = A.live->Frame(f, ce, LocalPad(A.s, f));
    A.waits_in_call.push_back(int(A.waits.size() - w0));
    A.slack_after.push_back(A.live->remote_slack());
    A.last_remote_after.push_back(A.gk.last_remote_input_frame());
    A.results.emplace_back(f, r);
    if (!r.ok) {
      if (A.on_end) A.on_end(A);
      break;
    }
    uint64_t h = A.state;
    h = Mix(h, r.input.connected);
    for (int pl = 0; pl < 2; ++pl) {
      uint8_t g[12];
      PadToGuest(r.input.pad[size_t(pl)], g);
      for (uint8_t b : g) h = Mix(h, b);
    }
    A.state = h;
    A.last_frame = f;
  }
  A.st = Actor::kDone;
  SwitchToFiber(g_main_fiber);
}

// Builds both sides (gekko session, executor, live session over the fake clock). Call after setting the actors' fields.
static void Setup(World& w) {
  for (int s = 0; s < 2; ++s) {
    Actor& A = w.a[s];
    A.w = &w;
    A.s = s;
    gekko::Config cfg;
    cfg.num_players = 2;
    cfg.local_slot = s;
    cfg.delay = kDelay;
    const bool ok = A.gk.Create(
        cfg,
        [&w, s](std::span<const uint8_t> payload) {
          Actor& me = w.a[s];
          Bytes b(payload.begin(), payload.end());
          ++me.sent;
          if (!b.empty() && b[0] == 1 && me.first_inputs.empty()) me.first_inputs = b;
          if (!b.empty() && b[0] == 4 && me.first_sync_request.empty()) me.first_sync_request = b;
          // SyncRequest / SyncResponse: header (type 4 / 5, magic u16), variant index 2 (SyncMsg), rng_data u16 = the
          // sender's session magic (net.h:42-45, 59-61, 99-109; backend.cpp:184-207). The peer learns the magic from both
          // (OnSyncRequest and OnSyncResponse, backend.cpp:920, 960), so both are rewritten.
          if (w.zero_magic_from[s] && b.size() == 6 && (b[0] == 4 || b[0] == 5) && b[3] == 2) {
            b[4] = b[5] = 0;
            ++w.rewrites;
          }
          const int to = 1 - s;
          const Hold& h = w.hold[to];
          uint64_t rel = w.t;
          if (h.armed && (h.end == kInf || w.t < h.end)) rel = h.end;
          w.q[to].push_back({rel, std::move(b)});
        },
        [&w, s](Bytes* out) {
          if (!Released(w, s)) return false;
          *out = std::move(w.q[s].front().bytes);
          w.q[s].pop_front();
          if (!out->empty() && (*out)[0] == 1) w.a[s].got_inputs = true;
          return true;
        });
    CHECKF(ok, "%s: gekko Create failed on side %d", w.label, s);
    live::Clock clock;
    clock.now_us = [&w, s]() {
      w.a[s].last_now = w.t;
      return w.t++;
    };
    clock.wait_packet = [&w, s](uint32_t ms) {
      Actor& me = w.a[s];
      me.waits.push_back({me.last_now, ms, me.live->banner_waiting(), me.cur});
      Hold& h = w.hold[s];
      if (h.on && h.begin == kInf && me.cur >= h.at_frame) {  // the hold's begin: this side's first wait in Frame(at)
        h.begin = me.last_now;
        h.end = w.t + h.dur;
        for (Pkt& p : w.q[s])
          if (p.release == kInf) p.release = h.end;
      }
      if (Released(w, s)) return;
      me.st = Actor::kWaitPacket;
      me.wake = w.t + uint64_t(ms) * kMs;
      Block(me);
    };
    clock.sleep_precise = [&w, s](uint32_t us) {
      Actor& me = w.a[s];
      me.sleeps.push_back(us);
      me.st = Actor::kSleep;
      me.wake = w.t + us;
      Block(me);
    };
    live::Hooks hooks;
    hooks.on_event = [&w, s](live::Event e, int32_t frame, uint64_t at) {
      Actor& me = w.a[s];
      me.events.push_back({e, frame, at, me.live->remote_slack()});
    };
    hooks.frames_ahead = A.fa;
    A.live = std::make_unique<live::Session>(&A.gk, &A.ex, clock, w.limits, hooks);
  }
}

// Runs both sides until both are done; real_limit_ms of wall clock at most.
static void Run(World& w, long long real_limit_ms = 30'000) {
  g_world = &w;
  g_world_label = w.label;
  const long long t_real0 = RealMs();
  g_deadline_ms = t_real0 + real_limit_ms + 15'000;  // the watchdog thread's backstop (a Frame call that never returns)
  for (int s = 0; s < 2; ++s) w.a[s].fiber = CreateFiber(1 << 22, ActorMain, &w.a[s]);
  int rr = 0;
  for (;;) {
    for (int s = 0; s < 2; ++s) {
      Actor& A = w.a[s];
      if (A.st == Actor::kNotStarted && w.t >= A.start_at) A.st = Actor::kRunnable;
      if (A.st == Actor::kWaitPacket && (Released(w, s) || w.t >= A.wake)) A.st = Actor::kRunnable;
      if (A.st == Actor::kSleep && w.t >= A.wake) A.st = Actor::kRunnable;
    }
    if (!w.handshake_done_t && w.a[0].got_inputs && w.a[1].got_inputs) w.handshake_done_t = w.t;
    int pick = -1;
    for (int k = 0; k < 2; ++k)
      if (w.a[(rr + k) % 2].st == Actor::kRunnable) {
        pick = (rr + k) % 2;
        break;
      }
    if (pick >= 0) {
      rr = pick + 1;
      SwitchToFiber(w.a[pick].fiber);
      continue;
    }
    if (w.a[0].st == Actor::kDone && w.a[1].st == Actor::kDone) break;
    uint64_t T = kInf;
    for (int s = 0; s < 2; ++s) {
      const Actor& A = w.a[s];
      if (A.st == Actor::kNotStarted) T = std::min(T, A.start_at);
      if (A.st == Actor::kWaitPacket || A.st == Actor::kSleep) T = std::min(T, A.wake);
      if (A.st == Actor::kWaitPacket && !w.q[s].empty() && w.q[s].front().release != kInf) T = std::min(T, w.q[s].front().release);
    }
    if (T == kInf) {
      w.deadlock = true;
      break;
    }
    auto running = [](const Actor& A) { return A.st != Actor::kNotStarted && A.st != Actor::kDone; };
    if (running(w.a[0]) && running(w.a[1]) && !(w.a[0].got_inputs && w.a[1].got_inputs) && w.paced_ms < w.pace_budget_ms && T > w.t) {
      const long long ms = (long long)((T - w.t + kMs - 1) / kMs);
      const long long r0 = RealMs();
      Sleep(DWORD(ms));
      w.paced_ms += RealMs() - r0;
    }
    if (T > w.t) w.t = T;
    if (RealMs() - t_real0 > real_limit_ms) {
      w.timed_out = true;
      break;
    }
  }
  for (int s = 0; s < 2; ++s) {
    DeleteFiber(w.a[s].fiber);
    w.a[s].fiber = nullptr;
  }
  g_deadline_ms = 0;
  CHECKF(!w.deadlock, "%s: deadlock (no side can run and nothing is due) at fake %llu us", w.label, (unsigned long long)w.t);
  CHECKF(!w.timed_out, "%s: ran past %lld ms of real time", w.label, real_limit_ms);
  std::printf("info: %s: fake end %.3f s, handshake done at %.3f s, real pacing %lld ms, real %lld ms, frames A %zu B %zu\n", w.label,
              double(w.t - 1'000'000) / 1e6, w.handshake_done_t ? double(w.handshake_done_t - 1'000'000) / 1e6 : -1.0, w.paced_ms,
              RealMs() - t_real0, w.a[0].results.size(), w.a[1].results.size());
  // Every wait the session asked for was a slice of 1..16 ms (no busy loop, no zero wait).
  for (int s = 0; s < 2; ++s) {
    int bad = 0;
    for (const WaitRec& r : w.a[s].waits)
      if (r.ms < 1 || r.ms > 16) ++bad;
    CHECKF(bad == 0, "%s side %d: %d waits outside 1..16 ms", w.label, s, bad);
  }
}

static int OkCount(const Actor& A) {
  int n = 0;
  for (const auto& r : A.results) n += r.second.ok ? 1 : 0;
  return n;
}
static const live::FrameResult* Last(const Actor& A) { return A.results.empty() ? nullptr : &A.results.back().second; }
static int CountEv(const Actor& A, live::Event e, int32_t frame = INT32_MIN) {
  int n = 0;
  for (const EvRec& r : A.events)
    if (r.ev == e && (frame == INT32_MIN || r.frame == frame)) ++n;
  return n;
}
static const EvRec* FindEv(const Actor& A, live::Event e) {
  for (const EvRec& r : A.events)
    if (r.ev == e) return &r;
  return nullptr;
}
static const char* EndName(live::End e) {
  switch (e) {
    case live::End::kNone: return "none";
    case live::End::kLeft: return "left";
    case live::End::kPeerLeft: return "peer-left";
    case live::End::kLost: return "lost";
    case live::End::kDesync: return "desync";
    case live::End::kFrame0Timeout: return "frame0-timeout";
    case live::End::kInternal: return "internal";
  }
  return "?";
}
// The end a side returned (kNone if it never ended), and the frame of the call that returned it.
static live::End EndOf(const Actor& A) { return Last(A) && !Last(A)->ok ? Last(A)->end : live::End::kNone; }

// Arms a hold of `dur` on side `to`'s inbound: from the peer's Frame(at - kDelay) (before it sends its input for frame
// `at`) until the begin (side to's first wait in Frame(at)) + dur.
static void ArmHold(World& w, int to, int32_t at, uint64_t dur) {
  w.hold[to].on = true;
  w.hold[to].at_frame = at;
  w.hold[to].dur = dur;
  Actor& peer = w.a[1 - to];
  auto prev = peer.before_frame;
  peer.before_frame = [&w, to, at, prev](Actor& me, int32_t f) {
    if (prev) prev(me, f);
    if (f == at - kDelay) w.hold[to].armed = true;
  };
}

// ---------------------------------------------------------------------------------------------------------------
// 300 frames in lockstep: identical records, the taps agree, no end, the buckets, one add per Frame call.
static void TestLockstep() {
  Group("lockstep, 300 frames");
  World w;
  w.label = "lockstep";
  Setup(w);
  Run(w);
  for (int s = 0; s < 2; ++s) {
    const Actor& A = w.a[s];
    CHECKF(OkCount(A) == 300 && A.results.size() == 300, "side %d: %d of %zu calls ok", s, OkCount(A), A.results.size());
    CHECKF(EndOf(A) == live::End::kNone && CountEv(A, live::Event::kEnd) == 0, "side %d: end %s", s, EndName(EndOf(A)));
    int net0 = 0, wait_bad = 0, waited = 0;
    for (size_t i = 0; i < A.results.size(); ++i) {
      const live::FrameResult& r = A.results[i].second;
      if (r.timing.net_us == 0) ++net0;
      if (A.waits_in_call[i] == 0 && r.timing.wait_us != 0) ++wait_bad;  // inputs already there: no wait time
      if (A.waits_in_call[i] > 0 && r.timing.wait_us == 0) ++wait_bad;   // and a wait is measured when there was one
      if (A.waits_in_call[i] > 0) ++waited;
    }
    CHECKF(net0 == 0, "side %d: %d frames with net_us 0", s, net0);
    CHECKF(wait_bad == 0, "side %d: %d frames whose wait_us disagrees with their wait calls", s, wait_bad);
    std::printf("info: lockstep side %d: %d of 300 calls waited\n", s, waited);
    // Both kinds of frame occur, so the wait_us check above is not vacuous on either side of it.
    CHECKF(waited > 0 && waited < 300, "side %d: %d of 300 calls waited", s, waited);
    // One add per Frame call: GekkoNet would drop a second add for a frame (input.cpp:50-65), so only the count shows it.
    CHECKF(A.gk.local_inputs_added() == uint32_t(A.results.size()), "side %d: %u adds for %zu Frame calls", s,
           A.gk.local_inputs_added(), A.results.size());
    CHECKF(A.ex.saves_filled() >= 299, "side %d: %u saves filled", s, A.ex.saves_filled());
    // Holds are reported only at frame 0 (the handshake); none later on an instant link.
    int late = 0;
    for (const EvRec& e : A.events)
      if (e.frame != 0) ++late;
    CHECKF(late == 0, "side %d: %d events after frame 0", s, late);
    // The staged records equal the taps, frame by frame.
    bool same = A.taps.size() == A.results.size();
    for (size_t i = 0; same && i < A.taps.size(); ++i)
      same = A.taps[i].first == int32_t(i) && A.taps[i].second == A.results[i].second.input;
    CHECKF(same, "side %d: the staged records differ from the executor's taps (%zu taps)", s, A.taps.size());
  }
  size_t wrong = 0, unequal = 0;
  for (size_t f = 0; f < 300 && f < w.a[0].results.size() && f < w.a[1].results.size(); ++f) {
    const FrameInput& x = w.a[0].results[f].second.input;
    const FrameInput& y = w.a[1].results[f].second.input;
    if (!(x == y)) ++unequal;
    FrameInput want;
    want.connected = 0x03;
    if (f >= size_t(kDelay)) {
      want.pad[0] = LocalPad(0, int(f) - kDelay);
      want.pad[1] = LocalPad(1, int(f) - kDelay);
    }
    if (!(x == want)) ++wrong;
  }
  CHECKF(unequal == 0 && wrong == 0, "%zu frames differ between the sides, %zu from the pattern", unequal, wrong);
  // Remote slack (carry 2b): >= 0 throughout, and the peek is consistent with GekkoNet: frame f is never advanced
  // before the peek has seen the peer's input for f.
  for (int s = 0; s < 2; ++s) {
    const Actor& A = w.a[s];
    int neg = 0, early = 0, mx = 0;
    for (size_t i = 0; i < A.results.size(); ++i) {
      if (A.slack_after[i] < 0) ++neg;
      if (A.results[i].second.ok && A.last_remote_after[i] < A.results[i].first) ++early;
      mx = std::max(mx, int(A.slack_after[i]));
    }
    CHECKF(neg == 0 && early == 0, "side %d: %d negative slack readings, %d frames advanced before the peek saw them", s, neg, early);
    std::printf("info: lockstep side %d: remote slack max %d, last remote input frame %d\n", s, mx,
                A.last_remote_after.empty() ? -2 : A.last_remote_after.back());
  }
  auto hex = [](const Bytes& b) {
    std::string out;
    char buf[4];
    for (size_t i = 0; i < b.size() && i < 12; ++i) {
      std::snprintf(buf, sizeof buf, "%02X ", b[i]);
      out += buf;
    }
    return out;
  };
  std::printf("info: first Inputs payload of A: %zu bytes: %s\n", w.a[0].first_inputs.size(), hex(w.a[0].first_inputs).c_str());
  std::printf("info: first SyncRequest of A: %zu bytes: %s\n", w.a[0].first_sync_request.size(), hex(w.a[0].first_sync_request).c_str());
  // The layout the peek and the magic-0 rewrite rely on: Inputs = 01, magic, index 0, start frame 0 (the first payload
  // carries frames from 0, input.cpp), count >= 1; SyncRequest = 6 bytes: 04, magic 0, index 2, rng.
  const Bytes& in = w.a[0].first_inputs;
  CHECKF(in.size() >= 10 && in[0] == 1 && in[3] == 0, "Inputs layout: %s", hex(in).c_str());
  const Bytes& sr = w.a[0].first_sync_request;
  CHECKF(sr.size() == 6 && sr[0] == 4 && sr[1] == 0 && sr[2] == 0 && sr[3] == 2, "SyncRequest layout: %s", hex(sr).c_str());
}

// B's packets held 50 ms at A's frame 50: A's frame 50 waits >= 50 ms, below hold_report_ms: no event, no hold.
static void TestShortWait() {
  Group("short wait, 50 ms at frame 50");
  World w;
  w.label = "short wait";
  w.a[0].frames = w.a[1].frames = 80;
  ArmHold(w, 0, 50, 50 * kMs);
  Setup(w);
  Run(w);
  const Actor& A = w.a[0];
  CHECKF(w.hold[0].begin != kInf && A.results.size() == 80 && OkCount(A) == 80, "hold begun %d, A %d of %zu ok",
         w.hold[0].begin != kInf, OkCount(A), A.results.size());
  if (A.results.size() < 80) return;
  const live::FrameResult& r = A.results[50].second;
  // Deviation from the brief's "wait_us >= 50,000", traced: the hold spans 50 ms of fake time from the first wait, but a
  // wait is cut into slices and every wake-up runs one Update between two clock readings, which the clock charges 1 us
  // to net_us. So wait_us = 50,000 - (wake-ups) + (the release's own reading), and wait_us + net_us covers the hold.
  const int n50 = A.waits_in_call[50];
  std::printf("info: short wait: frame 50: wait_us %llu, net_us %llu, %d waits\n", (unsigned long long)r.timing.wait_us,
              (unsigned long long)r.timing.net_us, n50);
  CHECKF(r.ok && r.timing.wait_us + r.timing.net_us >= 50'000 && r.timing.wait_us + n50 >= 50'000 && n50 > 0,
         "frame 50: ok %d wait_us %llu net_us %llu, %d waits", r.ok, (unsigned long long)r.timing.wait_us,
         (unsigned long long)r.timing.net_us, n50);
  CHECKF(r.holds == A.results[49].second.holds && r.hold_us == 0, "frame 50: holds %u (before %u), hold_us %llu", r.holds,
         A.results[49].second.holds, (unsigned long long)r.hold_us);
  CHECKF(CountEv(A, live::Event::kHoldBegin, 50) == 0 && CountEv(A, live::Event::kBannerWaiting) == 0, "events at frame 50");
  // Positive control on the fixture: the hold began in Frame(50) and the wait there was the hold's.
  CHECKF(w.hold[0].begin >= A.call_t[50] && (A.results.size() < 52 || w.hold[0].begin < A.call_t[51]), "hold began outside Frame(50)");
}

// B's packets held 1.5 s at A's frame 100.
static void TestHold() {
  Group("hold, 1.5 s at frame 100");
  World w;
  w.label = "hold";
  w.a[0].frames = w.a[1].frames = 130;
  ArmHold(w, 0, 100, 1500 * kMs);
  Setup(w);
  Run(w);
  const Actor& A = w.a[0];
  const Actor& B = w.a[1];
  const uint64_t begin = w.hold[0].begin;
  CHECKF(begin != kInf && A.results.size() == 130 && OkCount(A) == 130 && OkCount(B) == 130, "hold begun %d; A %d B %d ok",
         begin != kInf, OkCount(A), OkCount(B));
  if (A.results.size() < 130 || begin == kInf) return;
  const live::FrameResult& r = A.results[100].second;
  CHECKF(r.ok && r.holds == A.results[99].second.holds + 1, "frame 100: ok %d holds %u (before %u)", r.ok, r.holds,
         A.results[99].second.holds);
  CHECKF(r.hold_us >= 1'500'000, "frame 100: hold_us %llu", (unsigned long long)r.hold_us);
  // As in the short wait (deviation, traced there): each wake-up charges 1 us to net_us.
  std::printf("info: hold: frame 100: wait_us %llu, net_us %llu, %d waits\n", (unsigned long long)r.timing.wait_us,
              (unsigned long long)r.timing.net_us, A.waits_in_call[100]);
  CHECKF(r.timing.wait_us + r.timing.net_us >= 1'500'000 && r.timing.wait_us + A.waits_in_call[100] >= 1'500'000,
         "frame 100: wait_us %llu net_us %llu, %d waits", (unsigned long long)r.timing.wait_us, (unsigned long long)r.timing.net_us,
         A.waits_in_call[100]);
  // Events on A at frame 100, in order: kHoldBegin(begin), kBannerWaiting(begin + 1 s .. + 1 s 1 ms), kHoldEnd(release).
  std::vector<EvRec> ev;
  for (const EvRec& e : A.events)
    if (e.frame == 100) ev.push_back(e);
  CHECKF(ev.size() == 3, "A: %zu events at frame 100", ev.size());
  if (ev.size() == 3) {
    CHECKF(ev[0].ev == live::Event::kHoldBegin && ev[0].at == begin, "first: event %d at %llu (begin %llu)", int(ev[0].ev),
           (unsigned long long)ev[0].at, (unsigned long long)begin);
    CHECKF(ev[1].ev == live::Event::kBannerWaiting && ev[1].at >= begin + 1'000'000 && ev[1].at < begin + 1'001'000,
           "second: event %d at begin + %lld us", int(ev[1].ev), (long long)(ev[1].at - begin));
    CHECKF(ev[2].ev == live::Event::kHoldEnd && ev[2].at >= w.hold[0].end && ev[2].at < w.hold[0].end + 1000,
           "third: event %d at %llu (release %llu)", int(ev[2].ev), (unsigned long long)ev[2].at, (unsigned long long)w.hold[0].end);
    // Remote slack is 0 at the start of a hold (carry 2b).
    CHECKF(ev[0].slack == 0, "remote slack %d at kHoldBegin", ev[0].slack);
  }
  // banner_waiting(): false before begin + 1 s, true from there to the end of the hold, false after.
  int early_true = 0, late_false = 0, after_true = 0, n_before = 0, n_in = 0, n_after = 0;
  for (const WaitRec& wr : A.waits) {
    if (wr.frame == 100 && wr.t < begin + 1'000'000) n_before++, early_true += wr.banner;
    else if (wr.frame == 100) n_in++, late_false += !wr.banner;
    else if (wr.frame > 100) n_after++, after_true += wr.banner;
  }
  CHECKF(early_true == 0 && late_false == 0 && after_true == 0 && n_before > 0 && n_in > 0,
         "banner samples: %d true early (of %d), %d false inside (of %d), %d true after (of %d)", early_true, n_before, late_false, n_in,
         after_true, n_after);
  CHECKF(!w.a[0].live->banner_waiting(), "banner still up after the run");
  // No busy wait: over the 1.5 s hold at most ceil(1500 / 16) + 4 = 98 waits, none of 0 ms (Run checks 1..16).
  int n100 = A.waits_in_call[100];
  CHECKF(n100 <= 98 && n100 > 0, "A: %d waits in Frame(100)", n100);
  // B (deviation from the brief's "B is unaffected and has no event", traced): in lockstep B can run only to frame 102
  // (A added its input for 102 in Frame(100) before the hold); B's Frame(103) needs A's input for 103, which A adds in
  // Frame(101), after the release. So B holds too, at frame 103, for about as long, and reports it.
  std::printf("info: hold: B's events:");
  for (const EvRec& e : B.events) std::printf(" (%d f%d +%lld us)", int(e.ev), e.frame, (long long)(e.at - begin));
  std::printf("\n");
  int b_bad = 0;
  for (const EvRec& e : B.events)
    if (e.frame != 0 && e.frame != 100 + kDelay + 1) ++b_bad;
  CHECKF(b_bad == 0 && CountEv(B, live::Event::kEnd) == 0, "B: %d events at frames other than 0 and 103, %d ends", b_bad,
         CountEv(B, live::Event::kEnd));
}

// B's packets held 10.5 s at frame 100: A ends kLost at begin + 10 s; held 9.95 s: A survives (both sides of the line).
static void TestLost() {
  Group("lost, 10.5 s and 9.95 s at frame 100");
  {
    World w;
    w.label = "lost 10.5 s";
    w.a[0].frames = w.a[1].frames = 130;
    ArmHold(w, 0, 100, 10'500 * kMs);
    Setup(w);
    Run(w);
    const Actor& A = w.a[0];
    const uint64_t begin = w.hold[0].begin;
    const live::FrameResult* r = Last(A);
    CHECKF(r && !r->ok && r->end == live::End::kLost && r->end_frame == 100 && A.results.back().first == 100,
           "A: end %s at frame %d", r ? EndName(r->end) : "-", r ? r->end_frame : -1);
    const EvRec* e = FindEv(A, live::Event::kEnd);
    CHECKF(e && begin != kInf && e->frame == 100 && e->at >= begin + 10'000'000 && e->at < begin + 10'001'000,
           "A: kEnd at begin + %lld us", e && begin != kInf ? (long long)(e->at - begin) : -1LL);
    CHECKF(OkCount(A) == 100, "A: %d frames ok", OkCount(A));
    CHECKF(!A.live->banner_waiting(), "the banner stays up after the end");
    std::printf("info: lost: A's why \"%s\"; B ended %s at frame %d\n", r ? r->why.c_str() : "", EndName(EndOf(w.a[1])),
                Last(w.a[1]) ? Last(w.a[1])->end_frame : -1);
  }
  {
    World w;
    w.label = "held 9.95 s";
    w.a[0].frames = w.a[1].frames = 130;
    ArmHold(w, 0, 100, 9'950 * kMs);
    Setup(w);
    Run(w);
    const Actor& A = w.a[0];
    CHECKF(OkCount(A) == 130 && OkCount(w.a[1]) == 130 && CountEv(A, live::Event::kEnd) == 0, "A %d B %d ok; A end %s", OkCount(A),
           OkCount(w.a[1]), EndName(EndOf(A)));
    CHECKF(A.results.size() > 100 && A.results[100].second.hold_us >= 9'950'000, "the hold at 100 lasted %llu us",
           A.results.size() > 100 ? (unsigned long long)A.results[100].second.hold_us : 0ULL);
  }
}

// Frame 0: B starts 59 s late -> A succeeds; 61 s late -> A ends kFrame0Timeout with kEnd at 60 s.
static void TestFrame0() {
  Group("frame 0, B late by 59 s and 61 s");
  {
    World w;
    w.label = "frame 0, 59 s";
    w.a[0].frames = w.a[1].frames = 30;
    w.a[1].start_at = w.t + 59 * kSec;
    Setup(w);
    Run(w);
    CHECKF(OkCount(w.a[0]) == 30 && OkCount(w.a[1]) == 30, "A %d B %d ok; A end %s", OkCount(w.a[0]), OkCount(w.a[1]),
           EndName(EndOf(w.a[0])));
    // Fixture: the handshake did finish before 60 s of A's frame 0 (else the line below is not the one tested).
    CHECKF(w.handshake_done_t && w.handshake_done_t < w.a[0].call_t[0] + 60 * kSec, "handshake at %.3f s after A's Frame(0)",
           w.handshake_done_t ? double(w.handshake_done_t - w.a[0].call_t[0]) / 1e6 : -1.0);
  }
  {
    World w;
    w.label = "frame 0, 61 s";
    w.a[0].frames = w.a[1].frames = 30;
    w.a[1].start_at = w.t + 61 * kSec;
    Setup(w);
    Run(w);
    const Actor& A = w.a[0];
    const live::FrameResult* r = Last(A);
    CHECKF(r && !r->ok && r->end == live::End::kFrame0Timeout && r->end_frame == 0 && A.results.size() == 1, "A: end %s at %d",
           r ? EndName(r->end) : "-", r ? r->end_frame : -1);
    const EvRec* e = FindEv(A, live::Event::kEnd);
    const uint64_t f0 = A.call_t.empty() ? 0 : A.call_t[0];
    CHECKF(e && e->at >= f0 + 60'000'000 && e->at < f0 + 60'001'000, "A: kEnd at first Frame(0) + %lld us",
           e ? (long long)(e->at - f0) : -1LL);
  }
}

// B's checksum perturbed for frame 150: both end kDesync naming frame 150 and both checksums.
static void TestDesync() {
  Group("desync at frame 150");
  World w;
  w.label = "desync";
  w.a[0].frames = w.a[1].frames = 200;
  w.a[1].bad_frame = 150;
  Setup(w);
  Run(w);
  const uint32_t ca = w.a[0].cs_handed[0], cb = w.a[1].cs_handed[0];
  CHECKF(ca != 0 && cb != 0 && (ca ^ cb) == 1u, "handed checksums for 150: A %08X B %08X", ca, cb);
  for (int s = 0; s < 2; ++s) {
    const live::FrameResult* r = Last(w.a[s]);
    char want[96];
    std::snprintf(want, sizeof want, "desync at frame 150 (this side %08X, other side %08X)", s ? cb : ca, s ? ca : cb);
    CHECKF(r && !r->ok && r->end == live::End::kDesync && r->why == want, "side %d: end %s why \"%s\" (want \"%s\")", s,
           r ? EndName(r->end) : "-", r ? r->why.c_str() : "", want);
    CHECKF(r && r->end_frame >= 151 && r->end_frame <= 160, "side %d: ended in Frame(%d)", s, r ? r->end_frame : -1);
    CHECKF(CountEv(w.a[s], live::Event::kEnd) == 1, "side %d: %d kEnd events", s, CountEv(w.a[s], live::Event::kEnd));
  }
}

// Leave: A Leave(200) -> A kLeft at 200; B told when A has left (as the lobby would) -> kPeerLeft at 200.
static void TestLeave() {
  Group("leave at frame 200");
  {
    World w;
    w.label = "leave, peer told after A left";
    w.a[0].frames = w.a[1].frames = 260;
    w.a[0].before_frame = [](Actor& me, int32_t f) {
      if (f == 190) me.live->Leave(200);
    };
    w.a[0].on_end = [&w](Actor&) { w.a[1].live->PeerLeft(200, wire::LeaveReason::kLeft); };
    Setup(w);
    Run(w);
    const Actor& A = w.a[0];
    const Actor& B = w.a[1];
    const live::FrameResult* ra = Last(A);
    CHECKF(ra && !ra->ok && ra->end == live::End::kLeft && ra->end_frame == 200 && A.results.back().first == 200 && OkCount(A) == 200,
           "A: end %s at %d in Frame(%d), %d ok", ra ? EndName(ra->end) : "-", ra ? ra->end_frame : -1,
           A.results.empty() ? -1 : A.results.back().first, OkCount(A));
    // A's Frame(200) did not touch GekkoNet: 200 adds (frames 0..199).
    CHECKF(A.gk.local_inputs_added() == 200, "A: %u adds", A.gk.local_inputs_added());
    const live::FrameResult* rb = Last(B);
    CHECKF(rb && !rb->ok && rb->end == live::End::kPeerLeft && rb->end_frame == 200, "B: end %s at %d", rb ? EndName(rb->end) : "-",
           rb ? rb->end_frame : -1);
    // Deviation from the brief's "never advances past 200", traced: A added its inputs for 200 and 201 in Frame(198) and
    // Frame(199), so B may have advanced 200 and 201 before the lobby's message arrived; it can never advance 202.
    std::printf("info: leave: B advanced through frame %d, its last call was Frame(%d)\n", OkCount(B) - 1,
                B.results.empty() ? -1 : B.results.back().first);
    CHECKF(OkCount(B) <= 202 && OkCount(B) >= 200, "B: %d frames advanced", OkCount(B));
  }
  {
    World w;
    w.label = "leave, peer told early";
    w.a[0].frames = w.a[1].frames = 260;
    w.a[0].before_frame = [](Actor& me, int32_t f) {
      if (f == 150) me.live->Leave(200);
    };
    w.a[1].before_frame = [](Actor& me, int32_t f) {
      if (f == 150) me.live->PeerLeft(200, wire::LeaveReason::kLeft);
    };
    Setup(w);
    Run(w);
    const Actor& B = w.a[1];
    const live::FrameResult* rb = Last(B);
    CHECKF(rb && rb->end == live::End::kPeerLeft && rb->end_frame == 200 && B.results.back().first == 200 && OkCount(B) == 200,
           "B: end %s at %d in Frame(%d), %d ok", rb ? EndName(rb->end) : "-", rb ? rb->end_frame : -1,
           B.results.empty() ? -1 : B.results.back().first, OkCount(B));
    CHECKF(B.gk.local_inputs_added() == 200, "B: %u adds", B.gk.local_inputs_added());
    CHECKF(rb && rb->why.find("left") != std::string::npos, "B's why \"%s\"", rb ? rb->why.c_str() : "");
  }
}

// A Load (a GekkoNet event list fed through the executor) -> kInternal; a later call returns the same end without
// touching GekkoNet or the executor (carry 2a).
static void TestLoad() {
  Group("load -> internal");
  World w;
  w.label = "load";
  w.a[0].frames = w.a[1].frames = 40;
  w.a[0].before_frame = [](Actor& me, int32_t f) {
    if (f == 20) {
      exec::Event load;
      load.kind = exec::Event::kLoad;
      load.frame = 10;
      me.ex.Run(std::vector<exec::Event>{load}, 0);  // the executor now refuses everything ("load in lockstep")
    }
  };
  uint32_t adds = 0, filled = 0;
  live::FrameResult again;
  w.a[0].on_end = [&](Actor& me) {
    adds = me.gk.local_inputs_added();
    filled = me.ex.saves_filled();
    again = me.live->Frame(21, 0, Pad{});
  };
  Setup(w);
  Run(w);
  const Actor& A = w.a[0];
  const live::FrameResult* r = Last(A);
  CHECKF(r && !r->ok && r->end == live::End::kInternal && r->end_frame == 20 && r->why == "load in lockstep", "A: end %s at %d why \"%s\"",
         r ? EndName(r->end) : "-", r ? r->end_frame : -1, r ? r->why.c_str() : "");
  CHECKF(again.end == live::End::kInternal && again.why == "load in lockstep" && again.end_frame == 20, "again: end %s at %d why \"%s\"",
         EndName(again.end), again.end_frame, again.why.c_str());
  CHECKF(A.gk.local_inputs_added() == adds && adds == 20 && A.ex.saves_filled() == filled, "adds %u -> %u, saves %u -> %u", adds,
         A.gk.local_inputs_added(), filled, A.ex.saves_filled());
  CHECKF(CountEv(A, live::Event::kEnd) == 1, "A: %d kEnd events (the repeated call must not raise another)", CountEv(A, live::Event::kEnd));
}

// The pacer: frames_ahead injected; sleep_precise(PacerExtraUs(fa)) after every Advance when > 0.
static void PacerCase(float fa_a, uint32_t want_us) {
  World w;
  char label[64];
  std::snprintf(label, sizeof label, "pacer %.2f", fa_a);
  w.label = label;
  w.a[0].frames = w.a[1].frames = 60;
  w.a[0].fa = [fa_a] { return fa_a; };
  w.a[1].fa = [] { return -2.0f; };
  Setup(w);
  Run(w);
  const Actor& A = w.a[0];
  CHECKF(OkCount(A) == 60 && OkCount(w.a[1]) == 60, "%s: A %d B %d ok", label, OkCount(A), OkCount(w.a[1]));
  CHECKF(w.a[1].sleeps.empty(), "%s: B slept %zu times on -2.0", label, w.a[1].sleeps.size());
  CHECKF(rules::PacerExtraUs(fa_a) == int(want_us), "%s: PacerExtraUs %d (pin %u)", label, rules::PacerExtraUs(fa_a), want_us);
  CHECKF(A.live->frames_ahead() == fa_a, "%s: frames_ahead() %.3f", label, A.live->frames_ahead());
  if (want_us == 0) {
    CHECKF(A.sleeps.empty(), "%s: A slept %zu times", label, A.sleeps.size());
    int nz = 0;
    for (const auto& r : A.results) nz += r.second.timing.pacer_us != 0;
    CHECKF(nz == 0, "%s: %d frames with pacer_us", label, nz);
    return;
  }
  int wrong = 0, bad_bucket = 0;
  for (uint32_t us : A.sleeps) wrong += us != want_us;
  for (const auto& r : A.results)
    if (r.second.timing.pacer_us < want_us || r.second.timing.pacer_us > want_us + 4) ++bad_bucket;
  CHECKF(A.sleeps.size() == 60 && wrong == 0, "%s: %zu sleeps, %d not %u", label, A.sleeps.size(), wrong, want_us);
  CHECKF(bad_bucket == 0, "%s: %d frames with pacer_us outside %u..%u", label, bad_bucket, want_us, want_us + 4);
}
static void TestPacer() {
  Group("pacer");
  PacerCase(2.0f, 556);
  PacerCase(8.0f, 1000);
  PacerCase(0.99f, 0);
}

// Teardown (carry 2a): Close() drops the pending Save, then destroys the gekko session; idempotent.
static void TestClose() {
  Group("close");
  World w;
  w.label = "close";
  w.a[0].frames = w.a[1].frames = 20;
  Setup(w);
  Run(w);
  Actor& A = w.a[0];
  CHECKF(OkCount(A) == 20, "A: %d ok", OkCount(A));
  const uint32_t filled = A.ex.saves_filled();
  A.live->Close();
  A.live->Close();  // twice
  // The Save of frame 19 was pending: a Run now would have written it into the destroyed session's storage.
  exec::Step st = A.ex.Run({}, 0x55);
  CHECKF(st.end == exec::End::kNone && A.ex.saves_filled() == filled, "saves filled %u -> %u after Close", filled, A.ex.saves_filled());
  CHECK(A.gk.Update().empty() && A.gk.FramesAhead() == 0.f);  // destroyed: the wrapper's no-session answers
  const uint32_t adds = A.gk.local_inputs_added();
  live::FrameResult r = A.live->Frame(20, 0, Pad{});
  CHECKF(r.end == live::End::kInternal && r.why == "session closed" && !r.ok, "Frame after Close: end %s why \"%s\"", EndName(r.end),
         r.why.c_str());
  CHECK(A.gk.local_inputs_added() == adds);
}

// The single-producer debug check (carry 2c): a Frame call from a second thread ends the session without touching GekkoNet.
static void TestThread() {
  Group("second thread");
  World w;
  w.label = "thread";
  w.a[0].frames = w.a[1].frames = 10;
  Setup(w);
  Run(w);
  Actor& A = w.a[0];
  const uint32_t adds = A.gk.local_inputs_added();
  live::FrameResult r;
  std::thread th([&] { r = A.live->Frame(10, Entering(A), Pad{}); });
  th.join();
  CHECKF(r.end == live::End::kInternal && r.why == "Frame called from a second thread" && !r.ok, "end %s why \"%s\"", EndName(r.end),
         r.why.c_str());
  CHECK(A.gk.local_inputs_added() == adds && adds == 10);
}

// Remote slack grows when the peer runs ahead (carry 2b): A pauses 100 ms of fake time before each of its frames 60..69,
// so B runs on until it lacks A's input. Derivation (delay 2): A has added its inputs up to f + 1 when it pauses before
// Frame(f); B advances through f + 1 and blocks in Frame(f + 2) having added its inputs up to f + 4; after A advances f,
// the frame it is about to advance is f + 1, so the slack is (f + 4) - (f + 1) = 3.
static void TestSlackAhead() {
  Group("remote slack, peer ahead");
  World w;
  w.label = "slack";
  w.a[0].frames = w.a[1].frames = 90;
  w.a[0].before_frame = [&w](Actor& me, int32_t f) {
    if (f >= 60 && f < 70) {
      me.st = Actor::kSleep;
      me.wake = w.t + 100 * kMs;
      Block(me);
    }
  };
  Setup(w);
  Run(w);
  const Actor& A = w.a[0];
  CHECKF(OkCount(A) == 90, "A: %d ok", OkCount(A));
  if (A.slack_after.size() < 90) return;
  int at3 = 0;
  for (int f = 60; f < 70; ++f) at3 += A.slack_after[size_t(f)] == 3;
  std::printf("info: slack: A after frames 58..71:");
  for (int f = 58; f < 72; ++f) std::printf(" %d", A.slack_after[size_t(f)]);
  std::printf("\n");
  CHECKF(at3 == 10, "A: %d of frames 60..69 read slack 3", at3);
}

// Magic 0 (carry 2d): GekkoNet's SendSyncResponse refuses magic 0 (backend.cpp:193), so a session whose magic is 0
// never connects. The test rewrites A's SyncRequest rng (its magic as B learns it) to 0: neither side connects, and both
// end kFrame0Timeout at 60 s - the frame-0 limit is the guard. The lockstep group is the control (no rewrite: connects).
static void TestMagicZero() {
  Group("magic 0");
  World w;
  w.label = "magic 0";
  w.a[0].frames = w.a[1].frames = 10;
  w.zero_magic_from[0] = true;
  Setup(w);
  Run(w);
  CHECKF(w.rewrites > 0, "no SyncRequest was rewritten");
  CHECKF(w.paced_ms >= 2000, "only %lld ms of real time for the handshake (a normal one finishes in about 1 s)", w.paced_ms);
  for (int s = 0; s < 2; ++s) {
    const live::FrameResult* r = Last(w.a[s]);
    CHECKF(r && r->end == live::End::kFrame0Timeout && w.a[s].results.size() == 1, "side %d: end %s after %zu calls", s,
           r ? EndName(r->end) : "-", w.a[s].results.size());
  }
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
  g_main_fiber = ConvertThreadToFiber(nullptr);
  std::thread watchdog([] {
    while (!g_done.load()) {
      const long long d = g_deadline_ms.load();
      if (d && RealMs() > d) {
        std::printf("FAIL watchdog: %s did not finish in time: run ended\n", g_world_label);
        std::_Exit(3);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  TestLockstep();
  TestShortWait();
  TestHold();
  TestLost();
  TestFrame0();
  TestDesync();
  TestLeave();
  TestLoad();
  TestPacer();
  TestClose();
  TestThread();
  TestSlackAhead();
  TestMagicZero();
  g_done = true;
  watchdog.join();
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

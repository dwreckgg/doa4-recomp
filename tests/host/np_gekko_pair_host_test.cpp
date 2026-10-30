// Host test: GekkoNet as vendored in third_party/GekkoNet (the DOA2U netplay tree, commit 6c35c7b), driven the way
// the netplay session executor drives it. Two GekkoGameSessions run in one process, joined by an in-memory
// adapter: lockstep (input_prediction_window 0), 2 players, 16-byte inputs, 8-byte states, limited_saving off,
// desync detection on, local input delay 2 on both, disconnect timeout off.
//
// What it pins (every line cites the vendored source it was read from; the numbers are not copied from a plan):
//  - the order of the events: every update yields nothing or one Advance followed by its Save, never a Load;
//  - one local input per advanced frame: gekko_add_local_input files the input at the session's current frame
//    plus the delay and accepts only the next sequential frame (input.cpp:50-65, sync.cpp:28-35), so a second
//    add before the frame advances is dropped, and the first add also fills frames 0..delay-1 with empty input;
//  - desync detection: the checksum of frame (current - 1) is read from the Save entry at the START of an
//    update (game_session.cpp:365-385), so the game must write it before that update; the latency of the
//    DesyncDetected event is measured and printed.
//
// The fake transport (contract, one sentence): a perfect link between exactly two endpoints; each datagram is
// delivered once, whole and in order, to the other side's next receive_data call that runs at or after its due
// tick, as a malloc'd GekkoNetResult whose addr is the SENDER's address as the receiver registered it.
// A real link loses, reorders and delays; the delayed run covers delay only, loss is the executor's tests'.
//
// Built and run by run_np_gekko_test.bat. "np_gekko_pair_host_test.exe -v" prints each group before it runs.
// The test opens no socket and writes no file. It sleeps 1 ms per tick until both sessions have started, because
// the library's hand-shake is paced by the steady clock (SYNC_MSG_DELAY 200 ms, backend.cpp:375).

#include "gekkonet.h"
#include "net.h"  // include/private/net.h: Gekko::SessionHealth, only to recognise health datagrams on the wire

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Bytes = std::vector<uint8_t>;

static int g_checks = 0;
static int g_failures = 0;
static bool g_verbose = false;
static const char* g_group = "";

// Every check prints the observed values when it fails (the arguments after the condition are a printf).
#define CHECKF(cond, ...)                                                                \
  do {                                                                                   \
    ++g_checks;                                                                          \
    if (!(cond)) {                                                                       \
      std::printf("FAIL %s:%d [%s]: %s -- ", __FILE__, __LINE__, g_group, #cond);        \
      std::printf(__VA_ARGS__);                                                          \
      std::printf("\n");                                                                 \
      ++g_failures;                                                                      \
    }                                                                                    \
  } while (0)

static void Group(const char* name) {
  g_group = name;
  if (g_verbose) {
    std::printf("group: %s\n", name);
    std::fflush(stdout);
  }
}

// ---- the configuration the executor uses (the brief's values, pinned here) ----
constexpr int kPlayers = 2;
constexpr int kInputSize = 16;
constexpr int kStateSize = 8;
constexpr int kDelay = 2;
constexpr int kFrames = 300;
constexpr int kMaxTicks = 2000;
constexpr int kBadFrame = 150;
constexpr uint32_t kGood = 0xC0DE0000u;
constexpr uint32_t kBad = 0xBAD00000u;
constexpr int kHost = 0;    // side A, handle 0, the host
constexpr int kJoiner = 1;  // side B, handle 1, the joiner
static_assert(kPlayers * kInputSize == 32);
// include/private/net.h:27-39: the packet type is the first byte of every datagram (an enum : u8, serialised first).
static_assert(Gekko::SessionHealth == 6);

// ---- the fake transport ----
struct Datagram {
  int due_tick;
  int from;
  Bytes bytes;
};
struct FakeNet {
  std::deque<Datagram> q[2];  // q[s] holds what is waiting for side s
  int tick = 0;
  int delay_ticks = 0;
  int sent[2] = {0, 0};
  int health_sent[2] = {0, 0};
  int bad_dest = 0;
  int delivered = 0;  // results handed to GekkoNet
  int frees = 0;      // free_data calls (GekkoNet frees addr.data, data and the result: three per result)
  void Reset(int delay) {
    q[0].clear();
    q[1].clear();
    tick = 0;
    delay_ticks = delay;
    sent[0] = sent[1] = 0;
    health_sent[0] = health_sent[1] = 0;
    bad_dest = 0;
    delivered = 0;
    frees = 0;
  }
};
static FakeNet g_net;
static const uint8_t kAddr[2] = {'A', 'B'};  // the address each side registers for its peer's end of the link

template <int Side>
static void SendData(GekkoNetAddress* addr, const char* data, int length) {
  constexpr int other = 1 - Side;
  if (!addr || addr->size != 1 || *static_cast<const uint8_t*>(addr->data) != kAddr[other]) ++g_net.bad_dest;
  Datagram d;
  d.due_tick = g_net.tick + g_net.delay_ticks;
  d.from = Side;
  d.bytes.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + length);
  ++g_net.sent[Side];
  if (length >= 1 && d.bytes[0] == uint8_t(Gekko::SessionHealth)) ++g_net.health_sent[Side];
  g_net.q[other].push_back(std::move(d));
}

template <int Side>
static GekkoNetResult** ReceiveData(int* length) {
  static std::vector<GekkoNetResult*> out;  // the array stays the fake's; the results are GekkoNet's to free
  out.clear();
  auto& q = g_net.q[Side];
  while (!q.empty() && q.front().due_tick <= g_net.tick) {
    Datagram& d = q.front();
    auto* r = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    r->addr.size = 1;
    r->addr.data = std::malloc(1);
    *static_cast<uint8_t*>(r->addr.data) = kAddr[d.from];
    r->data_len = unsigned(d.bytes.size());
    r->data = std::malloc(d.bytes.size());
    std::memcpy(r->data, d.bytes.data(), d.bytes.size());
    out.push_back(r);
    ++g_net.delivered;
    q.pop_front();
  }
  *length = int(out.size());
  return out.data();
}

static void FreeData(void* p) {
  ++g_net.frees;
  std::free(p);
}

static GekkoNetAdapter g_adapter[2] = {{SendData<0>, ReceiveData<0>, FreeData}, {SendData<1>, ReceiveData<1>, FreeData}};

// ---- one run of the pair ----
struct Scenario {
  const char* name;
  int delay_ticks;   // one-way delay of the fake link, in ticks
  bool second_add;   // host adds a second input at A == 100, right after its normal add
  int bad_frame;     // -1, or the frame whose checksum side B writes wrong (kBad | frame)
  bool late_fill;    // side B writes the right checksum of bad_frame only after the NEXT update (the order control)
};

struct GEvent {
  int type;  // GekkoAdvanceEvent, GekkoSaveEvent, GekkoLoadEvent
  int frame;
  unsigned input_len;
  bool rolling_back;
  bool running_ahead;
  Bytes inputs;
};
struct Update {
  int tick;
  std::vector<GEvent> ev;
};
struct SEvent {  // a session event
  int type;
  int update_no;  // this side's update count when it was raised (1-based)
  int tick;
  int frame;      // DesyncDetected only
  unsigned local_cs, remote_cs;
  int handle;
};
struct SaveSlot {
  bool valid = false;
  int frame = 0;
  unsigned* checksum = nullptr;
  unsigned* state_len = nullptr;
  unsigned char* state = nullptr;
};
struct Side {
  GekkoSession* s = nullptr;
  int local = -1;
  int advances = 0;  // A: the number of Advances this side has seen
  bool need_add = true;
  bool started = false;
  int start_tick = -1;
  bool second_done = false;
  int adds = 0;  // gekko_add_local_input calls made
  SaveSlot pend;  // the Save of the previous Advance, to be filled before the next update
  SaveSlot late;  // the same, held back by the order control
  std::vector<Update> updates;
  std::vector<SEvent> sev;
  std::vector<int> adv_frames;
  std::vector<Bytes> adv_inputs;
};
struct Run {
  Scenario sc;
  Side s[2];
  int tick = 0;
  int ticks_used = 0;
  int write_tick = -1;     // the tick at which side B's wrong (or late) checksum was written
  size_t write_updates[2] = {0, 0};   // each side's update count at that moment
  size_t write_desyncs[2] = {0, 0};   // each side's DesyncDetected count at that moment
};

static void WriteSave(int side, const SaveSlot& sl, uint32_t checksum) {
  *sl.checksum = checksum;
  *sl.state_len = kStateSize;
  for (int i = 0; i < kStateSize; ++i) sl.state[i] = uint8_t((sl.frame >> (8 * (i & 1))) + 31 * i + 7 * side);
}

// The only place the test calls gekko_add_local_input, so the counter is the number of calls really made.
static void AddInputTo(Side& S, uint8_t* input) {
  gekko_add_local_input(S.s, S.local, input);
  ++S.adds;
}

static void AddLocal(Run& r, int side) {
  Side& S = r.s[side];
  uint8_t in[kInputSize] = {};
  in[0] = uint8_t(side);
  in[1] = uint8_t(S.advances & 0xFF);
  AddInputTo(S, in);
  if (r.sc.second_add && side == kHost && S.advances == 100 && !S.second_done) {
    uint8_t again[kInputSize] = {};
    again[0] = 0;
    again[1] = 0xEE;
    AddInputTo(S, again);  // same frame, no Advance in between
    S.second_done = true;
  }
}

static int CountDesyncs(const Side& S) {
  int n = 0;
  for (const SEvent& e : S.sev)
    if (e.type == GekkoDesyncDetected) ++n;
  return n;
}

static void StepSide(Run& r, int side) {
  Side& S = r.s[side];
  const Scenario& sc = r.sc;
  // The Save of the previous Advance is filled BEFORE the next update: GekkoNet reads the checksum at the
  // start of that update (game_session.cpp:365-385).
  if (S.pend.valid) {
    const bool bad_here = side == kJoiner && S.pend.frame == sc.bad_frame;
    if (bad_here && sc.late_fill) {
      S.late = S.pend;  // the order control writes it after the update instead
    } else {
      if (bad_here) {
        r.write_tick = r.tick;
        for (int i = 0; i < 2; ++i) {
          r.write_updates[i] = r.s[i].updates.size();
          r.write_desyncs[i] = size_t(CountDesyncs(r.s[i]));
        }
      }
      WriteSave(side, S.pend, (bad_here ? kBad : kGood) | uint32_t(S.pend.frame));
    }
    S.pend.valid = false;
  }
  if (S.need_add) {
    AddLocal(r, side);
    S.need_add = false;
  }
  int count = 0;
  GekkoGameEvent** ev = gekko_update_session(S.s, &count);
  Update u;
  u.tick = r.tick;
  bool advanced = false;
  SaveSlot last_save;
  for (int i = 0; i < count; ++i) {
    GekkoGameEvent* e = ev[i];
    GEvent g{};
    g.type = int(e->type);
    if (e->type == GekkoAdvanceEvent) {
      g.frame = e->data.adv.frame;
      g.input_len = e->data.adv.input_len;
      g.rolling_back = e->data.adv.rolling_back;
      g.running_ahead = e->data.adv.running_ahead;
      g.inputs.assign(e->data.adv.inputs, e->data.adv.inputs + e->data.adv.input_len);
      S.adv_frames.push_back(g.frame);
      S.adv_inputs.push_back(g.inputs);
      ++S.advances;
      advanced = true;
    } else if (e->type == GekkoSaveEvent) {
      g.frame = e->data.save.frame;
      last_save.valid = true;
      last_save.frame = e->data.save.frame;
      last_save.checksum = e->data.save.checksum;
      last_save.state_len = e->data.save.state_len;
      last_save.state = e->data.save.state;
    } else if (e->type == GekkoLoadEvent) {
      g.frame = e->data.load.frame;
    }
    u.ev.push_back(std::move(g));
  }
  S.updates.push_back(std::move(u));
  const int update_no = int(S.updates.size());

  int scount = 0;
  GekkoSessionEvent** se = gekko_session_events(S.s, &scount);
  for (int i = 0; i < scount; ++i) {
    SEvent x{};
    x.type = int(se[i]->type);
    x.update_no = update_no;
    x.tick = r.tick;
    if (se[i]->type == GekkoDesyncDetected) {
      x.frame = se[i]->data.desynced.frame;
      x.local_cs = se[i]->data.desynced.local_checksum;
      x.remote_cs = se[i]->data.desynced.remote_checksum;
      x.handle = se[i]->data.desynced.remote_handle;
    }
    if (se[i]->type == GekkoSessionStarted && !S.started) {
      S.started = true;
      S.start_tick = r.tick;
    }
    S.sev.push_back(x);
  }

  if (S.late.valid) {  // the order control: the right value, one update too late
    r.write_tick = r.tick;
    for (int i = 0; i < 2; ++i) {
      r.write_updates[i] = r.s[i].updates.size();
      r.write_desyncs[i] = size_t(CountDesyncs(r.s[i]));
    }
    WriteSave(side, S.late, kGood | uint32_t(S.late.frame));
    S.late.valid = false;
  }
  if (advanced) {
    if (last_save.valid) S.pend = last_save;
    S.need_add = false;
    AddLocal(r, side);  // again after every update that yielded an Advance
  }
}

static bool Setup(Run& r) {
  GekkoNetAddress addr[2] = {{const_cast<uint8_t*>(&kAddr[0]), 1}, {const_cast<uint8_t*>(&kAddr[1]), 1}};
  for (int side = 0; side < 2; ++side) {
    Side& S = r.s[side];
    if (!gekko_create(&S.s, GekkoGameSession)) return false;
    GekkoConfig cfg{};
    cfg.num_players = kPlayers;
    cfg.max_spectators = 0;
    cfg.input_prediction_window = 0;
    cfg.spectator_delay = 0;
    cfg.input_size = kInputSize;
    cfg.state_size = kStateSize;
    cfg.limited_saving = false;
    cfg.desync_detection = true;
    cfg.check_distance = 0;
    gekko_start(S.s, &cfg);
    gekko_net_adapter_set(S.s, &g_adapter[side]);
    // Actors in the same order on both sides: handle 0 = host (local on A), handle 1 = joiner (local on B).
    int h0, h1;
    if (side == kHost) {
      h0 = gekko_add_actor(S.s, GekkoLocalPlayer, nullptr);
      h1 = gekko_add_actor(S.s, GekkoRemotePlayer, &addr[kJoiner]);
    } else {
      h0 = gekko_add_actor(S.s, GekkoRemotePlayer, &addr[kHost]);
      h1 = gekko_add_actor(S.s, GekkoLocalPlayer, nullptr);
    }
    CHECKF(h0 == 0 && h1 == 1, "side %d: handles %d, %d (expected 0, 1)", side, h0, h1);
    S.local = side == kHost ? h0 : h1;
    gekko_set_local_delay(S.s, S.local, kDelay);
    gekko_set_disconnect_timeout(S.s, 0);
  }
  return true;
}

static void RunPair(Run& r) {
  g_net.Reset(r.sc.delay_ticks);
  if (!Setup(r)) {
    CHECKF(false, "gekko_create failed");
    return;
  }
  for (r.tick = 0; r.tick < kMaxTicks; ++r.tick) {
    g_net.tick = r.tick;
    StepSide(r, 0);
    StepSide(r, 1);
    const bool both_started = r.s[0].started && r.s[1].started;
    if (both_started && std::min(r.s[0].advances, r.s[1].advances) >= kFrames) {
      ++r.tick;
      break;
    }
    if (!both_started) std::this_thread::sleep_for(std::chrono::milliseconds(1));  // the hand-shake runs on the clock
  }
  r.ticks_used = r.tick;
  for (int side = 0; side < 2; ++side) gekko_destroy(&r.s[side].s);
}

// ---- helpers for the checks ----
static std::string ShapeOf(const Update& u) {
  std::string s = "[";
  for (size_t i = 0; i < u.ev.size(); ++i) {
    const char* t = u.ev[i].type == GekkoAdvanceEvent ? "Advance" : u.ev[i].type == GekkoSaveEvent ? "Save" : "Load";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s%s %d", i ? ", " : "", t, u.ev[i].frame);
    s += buf;
  }
  return s + "]";
}

// The shape the brief states: nothing, or exactly [Advance f, Save f].
static bool ShapeOkAfterStartSave(const Update& u);

static bool BriefShape(const Update& u) {
  if (u.ev.empty()) return true;
  return u.ev.size() == 2 && u.ev[0].type == GekkoAdvanceEvent && u.ev[1].type == GekkoSaveEvent &&
         u.ev[0].frame == u.ev[1].frame;
}

// A byte of a buffer, or -1 past its end: failure messages must not index out of range.
static int ByteAt(const Bytes& b, size_t i) { return i < b.size() ? int(b[i]) : -1; }

// The rule as amended: the first update of the started session may begin with exactly one Save of frame -1
// (the library's start-up save, game_session.cpp:619-623); what follows it is nothing or [Advance f, Save f].
static bool ShapeOkAfterStartSave(const Update& u) {
  if (u.ev.empty() || u.ev[0].type != GekkoSaveEvent || u.ev[0].frame != -1) return false;
  Update rest;
  rest.tick = u.tick;
  rest.ev.assign(u.ev.begin() + 1, u.ev.end());
  return BriefShape(rest);
}

// What a Save-less, Load-less, one-Advance update looks like in general (the claims that hold on every run).
static void CheckOrder(const Run& r, const char* label) {
  for (int side = 0; side < 2; ++side) {
    const Side& S = r.s[side];
    int loads = 0, multi = 0, rolling = 0, ahead = 0, bad_len = 0;
    for (const Update& u : S.updates) {
      int adv = 0;
      for (const GEvent& e : u.ev) {
        if (e.type == GekkoLoadEvent) ++loads;
        if (e.type == GekkoAdvanceEvent) {
          ++adv;
          if (e.rolling_back) ++rolling;
          if (e.running_ahead) ++ahead;
          if (e.input_len != unsigned(kPlayers * kInputSize)) ++bad_len;
        }
      }
      if (adv > 1) ++multi;
    }
    CHECKF(loads == 0, "%s side %d: %d Load events", label, side, loads);
    CHECKF(multi == 0, "%s side %d: %d updates with two Advances", label, side, multi);
    CHECKF(rolling == 0 && ahead == 0, "%s side %d: rolling_back %d, running_ahead %d", label, side, rolling, ahead);
    CHECKF(bad_len == 0, "%s side %d: %d Advances whose input_len is not 32", label, side, bad_len);
    CHECKF(S.started, "%s side %d: SessionStarted never raised within %d ticks", label, side, kMaxTicks);
    CHECKF(S.advances >= kFrames, "%s side %d: %d Advances in %d ticks (expected >= %d)", label, side, S.advances,
           r.ticks_used, kFrames);
    // The Advance frames run 0, 1, 2, ... with no gap and no repeat.
    int first_bad = -1;
    for (size_t i = 0; i < S.adv_frames.size(); ++i)
      if (S.adv_frames[i] != int(i)) {
        first_bad = int(i);
        break;
      }
    CHECKF(first_bad < 0, "%s side %d: Advance #%d carries frame %d", label, side, first_bad,
           first_bad < 0 ? 0 : S.adv_frames[size_t(first_bad)]);
  }
  // The same 32 bytes on both sides, equal to the pattern: frames 0 and 1 are zeros (the first add fills
  // 0..delay-1 with empty input, input.cpp:52-56); frame f >= 2 carries the add made with A = f - 2.
  const size_t common = std::min(r.s[0].adv_inputs.size(), r.s[1].adv_inputs.size());
  CHECKF(common >= size_t(kFrames), "%s: %zu common frames", label, common);
  int unequal = 0, wrong = 0;
  int first_unequal = -1, first_wrong = -1;
  Bytes first_wrong_got, first_wrong_want;
  for (size_t f = 0; f < common; ++f) {
    const Bytes& a = r.s[0].adv_inputs[f];
    const Bytes& b = r.s[1].adv_inputs[f];
    if (a != b) {
      if (!unequal) first_unequal = int(f);
      ++unequal;
    }
    Bytes want(kPlayers * kInputSize, 0);
    if (f >= size_t(kDelay)) {
      want[0] = 0;
      want[1] = uint8_t((f - kDelay) & 0xFF);
      want[kInputSize] = 1;
      want[kInputSize + 1] = uint8_t((f - kDelay) & 0xFF);
    }
    if (a != want) {
      if (!wrong) {
        first_wrong = int(f);
        first_wrong_got = a;
        first_wrong_want = want;
      }
      ++wrong;
    }
  }
  CHECKF(unequal == 0, "%s: %d frames differ between the sides, first at frame %d", label, unequal, first_unequal);
  CHECKF(wrong == 0,
         "%s: %d frames differ from the pattern, first at frame %d: host slot got {%d,%d} want {%d,%d}, joiner slot got "
         "{%d,%d} want {%d,%d}",
         label, wrong, first_wrong, ByteAt(first_wrong_got, 0), ByteAt(first_wrong_got, 1), ByteAt(first_wrong_want, 0),
         ByteAt(first_wrong_want, 1), ByteAt(first_wrong_got, kInputSize), ByteAt(first_wrong_got, kInputSize + 1),
         ByteAt(first_wrong_want, kInputSize), ByteAt(first_wrong_want, kInputSize + 1));
  // The fake's own contract: every send was addressed to the registered peer, and GekkoNet freed what it was given.
  CHECKF(g_net.bad_dest == 0, "%s: %d datagrams were sent to an address other than the peer's", label, g_net.bad_dest);
  CHECKF(g_net.frees == 3 * g_net.delivered, "%s: %d results delivered, %d free_data calls (expected 3 per result)", label,
         g_net.delivered, g_net.frees);
  CHECKF(g_net.delivered > 0, "%s: nothing was delivered", label);
}

static void InfoShapes(const Run& r, const char* label) {
  for (int side = 0; side < 2; ++side) {
    std::map<std::string, int> shapes;  // by event types only, with the frame numbers dropped
    std::string first_odd;
    for (const Update& u : r.s[side].updates) {
      std::string k = "[";
      for (size_t i = 0; i < u.ev.size(); ++i)
        k += std::string(i ? ", " : "") +
             (u.ev[i].type == GekkoAdvanceEvent ? "Advance" : u.ev[i].type == GekkoSaveEvent ? "Save" : "Load");
      k += "]";
      ++shapes[k];
      if (first_odd.empty() && !BriefShape(u)) first_odd = ShapeOf(u) + " at update " + std::to_string(&u - r.s[side].updates.data() + 1);
    }
    std::printf("info: %s side %d: started at tick %d, %d ticks, updates by shape:", label, side, r.s[side].start_tick,
                r.ticks_used);
    for (auto& kv : shapes) std::printf("  %s x%d", kv.first.c_str(), kv.second);
    std::printf("%s%s\n", first_odd.empty() ? "" : "; first shape outside the brief's: ", first_odd.c_str());
  }
}

// ---- the groups ----
static void TestCleanRun() {
  Group("clean run: order, inputs, one add per frame");
  Run r;
  r.sc = {"clean", 0, true, -1, false};
  RunPair(r);
  InfoShapes(r, "clean");
  CheckOrder(r, "clean");
  // Every update yields nothing or [Advance f, Save f], except that the first update that saves frame -1 (and only
  // that one) begins with that Save: the library's start-up save, game_session.cpp:619-623 (_last_saved_frame starts
  // at -2). The brief had stated the rule without the exception; the ruling amended it.
  for (int side = 0; side < 2; ++side) {
    int odd = 0;
    std::string first;
    bool start_used = false;
    for (size_t i = 0; i < r.s[side].updates.size(); ++i) {
      const Update& u = r.s[side].updates[i];
      bool has_m1 = false;
      for (const GEvent& e : u.ev)
        if (e.type == GekkoSaveEvent && e.frame == -1) has_m1 = true;
      bool ok = BriefShape(u);
      if (!ok && has_m1 && !start_used && ShapeOkAfterStartSave(u)) ok = true;
      if (has_m1) start_used = true;
      if (!ok) {
        if (!odd) first = "update " + std::to_string(i + 1) + " (tick " + std::to_string(u.tick) + ") yielded " + ShapeOf(u);
        ++odd;
      }
    }
    CHECKF(odd == 0, "side %d: %d updates break the rule; first: %s", side, odd, first.c_str());
  }
  // The shape observed: exactly one update per side is outside the brief's shape; it is the update that raised
  // SessionStarted, it starts with Save -1 and carries either nothing more or [Advance 0, Save 0] (when the other
  // side's inputs for frame 0 had already arrived); every other update is [] or [Advance f, Save f].
  // GekkoNet never reads the checksum of frame -1 (the health check starts at frame 0, game_session.cpp:372-374),
  // and this harness leaves that Save unfilled, so the clean run below also shows it is not needed.
  for (int side = 0; side < 2; ++side) {
    const Side& S = r.s[side];
    int with_m1 = 0;
    size_t at = 0;
    for (size_t i = 0; i < S.updates.size(); ++i)
      for (const GEvent& e : S.updates[i].ev)
        if (e.type == GekkoSaveEvent && e.frame == -1) {
          ++with_m1;
          at = i;
        }
    CHECKF(with_m1 == 1, "side %d: %d updates saved frame -1 (expected exactly 1)", side, with_m1);
    int started_update = -1;
    for (const SEvent& e : S.sev)
      if (e.type == GekkoSessionStarted) started_update = e.update_no;
    CHECKF(int(at) + 1 == started_update, "side %d: Save -1 came in update %zu, SessionStarted in update %d", side, at + 1,
           started_update);
    const Update empty_update{};  // the reads below must not raise when the run produced no update at all
    const Update& u = at < S.updates.size() ? S.updates[at] : empty_update;
    const bool alone = u.ev.size() == 1;
    const bool with_zero = u.ev.size() == 3 && u.ev[1].type == GekkoAdvanceEvent && u.ev[1].frame == 0 &&
                           u.ev[2].type == GekkoSaveEvent && u.ev[2].frame == 0;
    CHECKF(!u.ev.empty() && u.ev[0].type == GekkoSaveEvent && u.ev[0].frame == -1 && (alone || with_zero),
           "side %d: the first started update yielded %s (expected [Save -1] or [Save -1, Advance 0, Save 0])", side,
           ShapeOf(u).c_str());
    int odd_elsewhere = 0;
    for (size_t i = 0; i < S.updates.size(); ++i)
      if (i != at && !BriefShape(S.updates[i])) ++odd_elsewhere;
    CHECKF(odd_elsewhere == 0, "side %d: %d updates other than the first started one are outside [] / [Advance f, Save f]",
           side, odd_elsewhere);
  }
  // The second add at A == 100 was dropped: frame 102 still carries {0, 100} on both sides, and the add
  // really was made (the fixture's precondition).
  CHECKF(r.s[kHost].second_done, "the second add was never made (host advanced %d)", r.s[kHost].advances);
  // One add before the first update and one after every Advance, so advances + 1 per side; the host made one more.
  CHECKF(r.s[kHost].adds == r.s[kHost].advances + 2 && r.s[kJoiner].adds == r.s[kJoiner].advances + 1,
         "gekko_add_local_input calls: host %d for %d Advances (expected +2), joiner %d for %d Advances (expected +1)",
         r.s[kHost].adds, r.s[kHost].advances, r.s[kJoiner].adds, r.s[kJoiner].advances);
  for (int side = 0; side < 2; ++side) {
    const Bytes& f102 = r.s[side].adv_inputs.size() > 102 ? r.s[side].adv_inputs[102] : Bytes(32, 0xFF);
    CHECKF(ByteAt(f102, 0) == 0 && ByteAt(f102, 1) == 100,
           "side %d: frame 102 host slot is {%d,%d}, expected {0,100} (0xEE would be an accepted second add)", side,
           ByteAt(f102, 0), ByteAt(f102, 1));
  }
  CHECKF(CountDesyncs(r.s[0]) == 0 && CountDesyncs(r.s[1]) == 0, "clean run raised %d + %d DesyncDetected", CountDesyncs(r.s[0]),
         CountDesyncs(r.s[1]));
  // Not vacuous: health datagrams (the desync detector's traffic) really crossed the link, about one per frame.
  std::printf("info: clean: health datagrams sent A %d, B %d; datagrams sent A %d, B %d\n", g_net.health_sent[0],
              g_net.health_sent[1], g_net.sent[0], g_net.sent[1]);
  CHECKF(g_net.health_sent[0] >= kFrames - 10 && g_net.health_sent[1] >= kFrames - 10,
         "health datagrams sent: A %d, B %d (expected about one per frame, >= %d)", g_net.health_sent[0], g_net.health_sent[1],
         kFrames - 10);
}

static void ExpectDesync(const Run& r, int side, int frame, uint32_t local, uint32_t remote, int handle, const char* label) {
  int n = CountDesyncs(r.s[side]);
  CHECKF(n == 1, "%s side %d: %d DesyncDetected events (expected exactly 1)", label, side, n);
  for (const SEvent& e : r.s[side].sev)
    if (e.type == GekkoDesyncDetected) {
      CHECKF(e.frame == frame && e.local_cs == local && e.remote_cs == remote && e.handle == handle,
             "%s side %d: DesyncDetected frame %d local %08X remote %08X handle %d; expected frame %d local %08X remote %08X handle %d",
             label, side, e.frame, e.local_cs, e.remote_cs, e.handle, frame, local, remote, handle);
      break;
    }
}

static void TestDesync() {
  Group("desync run: B's checksum of frame 150 is wrong");
  Run r;
  r.sc = {"desync", 0, false, kBadFrame, false};
  RunPair(r);
  CheckOrder(r, "desync");
  CHECKF(r.write_tick >= 0, "side B never wrote the wrong checksum");
  ExpectDesync(r, kHost, kBadFrame, kGood | kBadFrame, kBad | kBadFrame, kJoiner, "desync");
  ExpectDesync(r, kJoiner, kBadFrame, kBad | kBadFrame, kGood | kBadFrame, kHost, "desync");
  // None before the wrong write; within 5 updates after it, on each side.
  int worst = 0;
  for (int side = 0; side < 2; ++side) {
    CHECKF(r.write_desyncs[side] == 0, "side %d had raised %zu DesyncDetected before the wrong write", side, r.write_desyncs[side]);
    for (const SEvent& e : r.s[side].sev)
      if (e.type == GekkoDesyncDetected) {
        const int after = e.update_no - int(r.write_updates[side]);
        worst = std::max(worst, after);
        std::printf("info: desync latency: side %c saw it on its update #%d after B's wrong write (tick %d, write tick %d)\n",
                    side ? 'B' : 'A', after, e.tick, r.write_tick);
        CHECKF(after >= 1 && after <= 5, "side %d: DesyncDetected %d updates after the wrong write (expected 1..5)", side, after);
      }
  }
  std::printf("info: desync latency: worst %d updates (limit 5)\n", worst);
}

static void TestOrderControl() {
  Group("order control: B fills frame 150's checksum one update late");
  // GekkoNet reads the checksum of frame (current - 1) at the start of an update; B writes the RIGHT value, but
  // after the update that follows the Save. The slot still holds frame 148's checksum (two states are kept:
  // storage.cpp:12, frame % 2), so that stale value is what B sends, and both sides report a desync.
  Run r;
  r.sc = {"late", 0, false, kBadFrame, true};
  RunPair(r);
  CheckOrder(r, "late");
  CHECKF(r.write_tick >= 0, "side B never wrote the late checksum");
  ExpectDesync(r, kHost, kBadFrame, kGood | kBadFrame, kGood | (kBadFrame - 2), kJoiner, "late");
  ExpectDesync(r, kJoiner, kBadFrame, kGood | (kBadFrame - 2), kGood | kBadFrame, kHost, "late");
}

static void TestDelayedLink() {
  Group("delayed link: three ticks each way");
  Run r;
  r.sc = {"delayed", 3, false, -1, false};
  RunPair(r);
  InfoShapes(r, "delayed");
  CheckOrder(r, "delayed");
  CHECKF(CountDesyncs(r.s[0]) == 0 && CountDesyncs(r.s[1]) == 0, "delayed run raised %d + %d DesyncDetected", CountDesyncs(r.s[0]),
         CountDesyncs(r.s[1]));
  // Not vacuous: the updates that wait for the other side's input really happened.
  for (int side = 0; side < 2; ++side) {
    int stalls = 0;
    bool seen_advance = false;
    for (const Update& u : r.s[side].updates) {
      if (!u.ev.empty()) seen_advance = true;
      else if (seen_advance) ++stalls;
    }
    CHECKF(stalls >= 100, "side %d: only %d updates after the first Advance yielded nothing", side, stalls);
  }
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
  TestCleanRun();
  TestDesync();
  TestOrderControl();
  TestDelayedLink();
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

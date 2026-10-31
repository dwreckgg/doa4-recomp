// Host test: two real GekkoNet sessions (np_gekko) over two real transports (np_transport) on 127.0.0.1, each side on its
// own thread, paced by wall clock at 16.7 ms per frame (a high-resolution timer), the way the game will run them. Built
// and run by run_np_link_test.bat. Nothing reaches the internet; no UPnP, no STUN.
//
// What it measures (Task 7 fix round, the INPUT_RETRY_INTERVAL ruling): the wall-clock stall after ONE kGekko datagram
// is lost while the sides are held. Both transports simulate a 40 ms one-way delay (Config::sim "40,0,0"; GekkoNet's
// ping reads ~83 ms) with an input delay of 2 frames (33 ms), so every frame waits a few ms for the peer's input on both
// sides (the "held waits" line). Side A drops 8 kGekko datagrams, 61 apart from 150 (Config::sim_drop; the spacing
// rotates the GekkoNet packet types); each drop is logged with its type, and its stall is the longest gap between two
// advances of either side that starts in the 300 ms after the drop. A control run without drops gives the steady gap.
// Measured 2026-10-31 (vendored GekkoNet, INPUT_RETRY_INTERVAL 200): steady longest gap 17.4 ms; a dropped Inputs
// datagram costs one frame (33 ms), not GekkoNet's 200 ms pure re-send (backend.cpp:1440-1445), because the next frame's
// new local input misses the per-peer cache and its message is rebuilt from last_acked_frame + 1 (backend.cpp:1458-1459),
// so it carries the lost input again; with an input delay >= 1 the sender always gets that next frame before its peer runs
// out of input. The 200 ms re-send decides only when a side adds no input for that long (d + 1 losses in a row, or a
// stalled game thread). Ruling: < 150 ms -> no GekkoNet patch; the bound below pins "one or two frames, not a re-send".
// Watchdogs from the first run: a thread ends the run with a FAIL line after 120 s or a 1 GB working set; each side gives
// up on a frame after 5 s.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/net/np_exec.h"
#include "../../src/net/np_gekko.h"
#include "../../src/net/np_transport.h"

using namespace doa4::np;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

static int g_checks = 0;
static int g_failures = 0;

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

static int64_t Us(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count(); }
static int64_t NowUs() { return Us(Clock::now()); }

static size_t WorkingSet() {
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof pmc;
  return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? size_t(pmc.WorkingSetSize) : 0;
}

struct TimedLog {  // the transport's log, each line with the time it was written
  std::mutex mu;
  std::vector<std::pair<int64_t, std::string>> lines;
  net::Transport::Log Fn() {
    return [this](const std::string& l) {
      std::lock_guard<std::mutex> lock(mu);
      lines.emplace_back(NowUs(), l);
    };
  }
};

constexpr uint32_t kToken = 0x51DEC0DEu;
constexpr int64_t kFrameUs = 16667;

struct Side {
  TimedLog log;
  net::Transport transport;
  gekko::Session session;
  exec::Executor ex{2, nullptr};
  std::vector<int64_t> advance_us;  // wall clock of every Advance
  int held_waits = 0;               // 1 ms waits for a packet inside a frame (the side was held)
  float ping_ms = 0;                // GekkoNet's average ping at the end of the run
  int send_refused = 0;
  // Targeted loss (this side's own thread, inside Send): the first Inputs payload (GekkoNet PacketType Inputs = 1,
  // net.h:28) at or after frame next_drop_frame is not handed to the transport, as if the network lost it.
  int drops_left = 0, next_drop_frame = 0, drop_every = 0;
  std::vector<int64_t> drop_us;
  bool gave_up = false;
  std::string why;
};

static Pad PadOf(int side, int frame) {
  Pad p;
  p.buttons = uint16_t(0x100 * (side + 1) + (frame & 0xFF));
  p.lx = int16_t(frame * 7 - side);
  return p;
}

// One side's game loop: per frame, the executor gets the checksum entering the frame, one local input is added, then
// Update until the frame advances (waiting on gekko_arrived 1 ms at a time), then the thread sleeps to the next 16.7 ms
// boundary. The checksum depends on the frame only, identically on both sides.
static void Loop(Side& S, int side, int frames, std::atomic<bool>& abort) {
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  HANDLE arrived = static_cast<HANDLE>(S.transport.gekko_arrived());
  int32_t last = -1;
  int64_t next_frame = NowUs();
  for (int f = 0; f < frames && !abort.load(); ++f) {
    // Task 11's order (np_exec_host_test StepSide): the checksum entering the frame, one local input, then Update until
    // the frame advances. Frame 0 also waits out the handshake (up to 10 s).
    S.ex.Run({}, exec::FoldChecksum(last, std::vector<uint64_t>{}, std::vector<uint64_t>{}));
    S.session.AddLocalInput(PadOf(side, f));
    const int64_t frame_deadline = NowUs() + (f == 0 ? 10'000'000 : 5'000'000);
    bool advanced = false;
    while (!advanced && !abort.load()) {
      const std::vector<exec::Event> ev = S.session.Update();
      const exec::Step st = S.ex.Run(ev, exec::FoldChecksum(last, std::vector<uint64_t>{}, std::vector<uint64_t>{}));
      if (st.end != exec::End::kNone) {
        S.gave_up = true;
        S.why = "the executor ended: " + st.why;
        break;
      }
      if (st.advanced) {
        advanced = true;
        last = st.frame;
        S.advance_us.push_back(NowUs());
        break;
      }
      if (NowUs() > frame_deadline) {
        S.gave_up = true;
        S.why = "frame " + std::to_string(f) + " did not advance in 5 s";
        break;
      }
      if (WorkingSet() > (size_t(1) << 30)) {
        S.gave_up = true;
        S.why = "working set over 1 GB";
        break;
      }
      if (f > 0) ++S.held_waits;
      WaitForSingleObject(arrived, 1);
    }
    if (S.gave_up) break;
    next_frame += kFrameUs;
    const int64_t wait = next_frame - NowUs();
    if (wait > 0 && timer) {
      LARGE_INTEGER due;
      due.QuadPart = -wait * 10;
      if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, 100);
    } else if (wait < -kFrameUs) {
      next_frame = NowUs();  // late by more than a frame (held): pace from now, no catch-up burst
    }
  }
  if (S.gave_up) abort = true;
  if (timer) CloseHandle(timer);
}

struct DropStall {
  int64_t at_us;
  uint32_t index;
  int type;
  int64_t stall_us;  // the longest gap between two advances of either side in the 1.5 s after the drop
};

struct RunResult {
  bool ok = true;
  std::vector<DropStall> drops;
  int64_t max_gap_us = 0;  // over the whole run after frame 20, both sides
  int advances[2] = {0, 0};
};

static int64_t LongestGap(const std::vector<int64_t>& t, int64_t from, int64_t to) {
  int64_t g = 0;
  for (size_t i = 1; i < t.size(); ++i)
    if (t[i] > from && t[i - 1] < to) g = std::max(g, t[i] - t[i - 1]);
  return g;
}

static RunResult RunPair(const char* label, std::vector<uint32_t> drops, int frames, int input_drops = 0) {
  RunResult out;
  Side a, b;
  Side* sides[2] = {&a, &b};
  for (int s = 0; s < 2; ++s) {
    net::Config c;
    c.port = 0;
    c.loopback = true;
    c.stun = false;
    c.lan_broadcast = false;
    c.sim = "40,0,0";
    c.sim_seed = uint32_t(s + 1);
    if (s == 0) c.sim_drop = drops;
    std::string e;
    if (!sides[s]->transport.Start(c, sides[s]->log.Fn(), &e)) {
      CHECKF(false, "%s: transport %d did not start: %s", label, s, e.c_str());
      out.ok = false;
      return out;
    }
  }
  rules::Endpoint ea, eb;
  ea.ip = eb.ip = {127, 0, 0, 1};
  ea.port = a.transport.port();
  eb.port = b.transport.port();
  a.transport.SetPeer(eb, kToken);
  b.transport.SetPeer(ea, kToken);
  a.drops_left = input_drops;
  a.next_drop_frame = 60;
  a.drop_every = 30;
  for (int s = 0; s < 2; ++s) {
    Side& S = *sides[s];
    gekko::Config gc;
    gc.num_players = 2;
    gc.local_slot = s;
    gc.delay = 2;
    const bool created = S.session.Create(
        gc,
        [&S](std::span<const uint8_t> p) {
          if (S.drops_left > 0 && !p.empty() && p[0] == 1 && int(S.advance_us.size()) >= S.next_drop_frame) {
            S.drop_us.push_back(NowUs());
            --S.drops_left;
            S.next_drop_frame += S.drop_every;
            return;
          }
          if (!S.transport.PushGekko(Bytes(p.begin(), p.end()))) ++S.send_refused;
        },
        [&S](Bytes* out) { return S.transport.PopGekko(out); });
    CHECKF(created, "%s: session %d not created", label, s);
  }
  std::atomic<bool> abort{false};
  std::thread ta([&] { Loop(a, 0, frames, abort); });
  std::thread tb([&] { Loop(b, 1, frames, abort); });
  ta.join();
  tb.join();
  for (int s = 0; s < 2; ++s) sides[s]->ping_ms = sides[s]->session.Stats().ping_ms;
  std::printf("info: %s: GekkoNet ping %.1f|%.1f ms, held waits %d|%d\n", label, a.ping_ms, b.ping_ms, a.held_waits, b.held_waits);
  for (int s = 0; s < 2; ++s) {
    out.advances[s] = int(sides[s]->advance_us.size());
    CHECKF(!sides[s]->gave_up, "%s side %d gave up: %s", label, s, sides[s]->why.c_str());
    CHECKF(out.advances[s] == frames, "%s side %d: %d of %d frames", label, s, out.advances[s], frames);
    CHECKF(sides[s]->send_refused == 0, "%s side %d: %d payloads refused by a full ring", label, s, sides[s]->send_refused);
    if (out.advances[s] > 20) out.max_gap_us = std::max(out.max_gap_us, LongestGap(sides[s]->advance_us, sides[s]->advance_us[20], INT64_MAX));
  }
  {
    std::lock_guard<std::mutex> lock(a.log.mu);
    for (const auto& [t, line] : a.log.lines) {
      unsigned idx = 0;
      int type = -1;
      if (std::sscanf(line.c_str(), "[doa4][NET] sim_drop: kGekko datagram %u dropped (GekkoNet packet type %d)", &idx, &type) == 2) {
        DropStall d{t, idx, type, 0};
        for (int s = 0; s < 2; ++s) d.stall_us = std::max(d.stall_us, LongestGap(sides[s]->advance_us, t, t + 300'000));
        out.drops.push_back(d);
      }
    }
  }
  for (const int64_t t : a.drop_us) {  // targeted Inputs losses: type 1 by construction
    DropStall d{t, 0, 1, 0};
    for (int s = 0; s < 2; ++s) d.stall_us = std::max(d.stall_us, LongestGap(sides[s]->advance_us, t, t + 300'000));
    out.drops.push_back(d);
  }
  CHECKF(int(a.drop_us.size()) == input_drops, "%s: %zu of %d targeted Inputs losses happened", label, a.drop_us.size(), input_drops);
  CHECKF(out.drops.size() == drops.size() + a.drop_us.size(), "%s: %zu drops seen, %zu + %zu made", label, out.drops.size(),
         drops.size(), a.drop_us.size());
  CHECKF(a.transport.stats().sim_drop_hits == drops.size(), "%s: sim_drop_hits %u", label, a.transport.stats().sim_drop_hits);
  a.transport.Stop();
  b.transport.Stop();
  std::printf("info: %s: advances %d|%d, longest gap after frame 20: %.1f ms\n", label, out.advances[0], out.advances[1],
              out.max_gap_us / 1000.0);
  for (const DropStall& d : out.drops)
    std::printf("info: %s: drop of %s (GekkoNet packet type %d): stall %.1f ms\n", label,
                d.index ? ("kGekko datagram " + std::to_string(d.index)).c_str() : "a targeted Inputs payload", d.type,
                d.stall_us / 1000.0);
  return out;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::atomic<bool> done{false};
  std::thread([&done] {
    const int64_t t0 = NowUs();
    while (!done.load()) {
      if (NowUs() - t0 > 120'000'000 || WorkingSet() > (size_t(1) << 30)) {
        std::printf("FAIL watchdog: the link test passed 120 s or 1 GB (working set %zu MB)\n", WorkingSet() >> 20);
        std::_Exit(3);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }).detach();
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
  const RunResult ctl = RunPair("control (no drop)", {}, 150);
  const RunResult dr = RunPair("sim_drop, one kGekko datagram at a time", {150, 211, 272, 333, 394, 455, 516, 577}, 240);
  const RunResult di = RunPair("one Inputs datagram lost at a time (frames 60, 90, 120, 150 of A)", {}, 200, 4);
  int64_t worst_inputs = 0, worst_any = 0;
  int inputs_drops = 0;
  for (const RunResult* r : {&dr, &di})
    for (const DropStall& d : r->drops) {
      worst_any = std::max(worst_any, d.stall_us);
      if (d.type == 1) {
        ++inputs_drops;
        worst_inputs = std::max(worst_inputs, d.stall_us);
      }
    }
  CHECKF(inputs_drops >= 4, "only %d dropped datagrams were Inputs packets", inputs_drops);
  std::printf("info: RESULT steady-state longest gap %.1f ms; worst stall after a dropped Inputs datagram %.1f ms (%d drops), after "
              "any %.1f ms\n",
              ctl.max_gap_us / 1000.0, worst_inputs / 1000.0, inputs_drops, worst_any / 1000.0);
  // The ruling's threshold is 150 ms (GekkoNet's 200 ms re-send would show as >= ~200). The pin is tighter: one or two
  // frames plus scheduling, 100 ms; measured 33.3 ms.
  constexpr int64_t kStallBoundUs = 100'000;
  CHECKF(worst_any < kStallBoundUs, "a dropped kGekko datagram stalled the pair %.1f ms (bound %.0f ms)", worst_any / 1000.0,
         kStallBoundUs / 1000.0);
  CHECKF(ctl.max_gap_us < 50'000, "the control's longest gap was %.1f ms (a frame is 16.7)", ctl.max_gap_us / 1000.0);
  done = true;
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

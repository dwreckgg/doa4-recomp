// DOA4 debug hooks — INSTRUMENTATION ONLY (no behavioural fixes).
//
// Real fixes live in `doa4_app_hooks.cpp`.  This file is for ad-hoc
// diagnostics: AV reporting, suspect-function panic loggers, register-flow
// traces, etc.  Anything in here can be deleted without changing game
// behaviour.

#include <atomic>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <rex/exception_handler.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

uint32_t Tid() {
  auto* ts = rex::runtime::ThreadState::Get();
  return ts ? ts->thread_id() : 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Fiber heartbeat — called from doa4_app_hooks.cpp DoFiberSwap.
// Tracks tick count + last-resumed timestamp per guest fiber save-buffer,
// and a background thread periodically dumps the table so we can see which
// fibers are starving.
namespace {

struct FiberBeat {
  uint64_t ticks = 0;
  uint64_t last_ms = 0;
  uint32_t entry = 0;
  // Last-known guest state (captured at every swap-in and swap-out).
  uint32_t last_lr = 0;    // guest LR
  uint32_t last_r1 = 0;    // guest SP
  uint32_t last_r3 = 0;    // guest arg/return
  uint32_t host_tid = 0;   // host thread that last ran this fiber
  uint64_t swap_in_count = 0;
  uint64_t swap_out_count = 0;
  uint64_t last_swap_in_ms = 0;
  uint64_t last_swap_out_ms = 0;
  bool running = false;    // true between swap-in and swap-out
  // Per-window stats (reset by the 1 Hz Prof sampler when it emits).
  uint64_t win_gap_count = 0;     // # inter-swap-in gaps observed in window
  uint64_t win_gap_total_ms = 0;  // sum of gaps in window
  uint64_t win_gap_max_ms = 0;    // max gap observed in window
  uint32_t win_last_r3_out = 0;   // r3 at most recent swap-out in window
  uint32_t win_distinct_r3 = 0;   // bumped each time r3 at swap-out differs from previous
  uint64_t win_oncpu_max_ms = 0;  // max contiguous on-cpu time (swap_in -> swap_out) in window
  uint64_t win_oncpu_total_ms = 0;
};

// Per host-thread tracking: when DID this thread last swap_out (give up CPU
// to a non-fiber path) and when did its next swap_in occur. The delta
// between any swap_out and the SAME thread's next swap_in is time the host
// thread spent NOT running any fiber: either blocked in a syscall (Wait /
// Sleep / I/O), preempted by Windows scheduler, or running pre-/post-swap
// host code. If this is consistently >5 ms we know the host (not a guest
// CPU hog) is the source of the chop.
struct HostTidBeat {
  uint64_t last_swap_out_ms = 0;
  uint64_t win_off_count = 0;
  uint64_t win_off_total_ms = 0;
  uint64_t win_off_max_ms = 0;
};
std::unordered_map<uint32_t, HostTidBeat> g_host_tid_beats;

std::mutex g_beat_mu;
std::unordered_map<uint32_t, FiberBeat> g_beats;
std::atomic<bool> g_beat_thread_started{false};
// Set once the watchdog has emitted the freeze dump for a particular freeze
// episode; cleared again when forward progress resumes.
std::atomic<bool> g_freeze_dumped{false};

// Last-second snapshot, written by the 1 Hz sampler thread, read by the
// ImGui diagnostic overlay. Plain relaxed atomics — the sampler is the only
// writer; readers tolerate any single-second skew.
std::atomic<uint64_t> g_prof_swaps_per_sec{0};
std::atomic<uint64_t> g_prof_total_swaps{0};
std::atomic<uint64_t> g_prof_dt_ms{0};
std::atomic<uint64_t> g_prof_fiber_count{0};
std::atomic<uint64_t> g_prof_stalled_100{0};
std::atomic<uint64_t> g_prof_stalled_500{0};
std::atomic<uint64_t> g_prof_worst_age_ms{0};
std::atomic<uint32_t> g_prof_worst_buf{0};

uint64_t NowMs() {
#if defined(_WIN32)
  return (uint64_t)GetTickCount64();
#else
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// Audio mix-buffer writer attribution.
//
// On every guest fiber swap-out we hash the audio mix buffer at guest
// address 0x40198580. If the hash has changed since the previous swap-out
// (on ANY host thread), the fiber that just yielded is the one that wrote
// the buffer. This is precise: every guest fiber must yield through
// DoFiberSwap, so the writer cannot escape attribution.
namespace {

constexpr uint32_t kMixBufGuestAddr = 0x40198580;
constexpr size_t   kMixBufBytes = 1536 * sizeof(float);
std::atomic<const float*> g_mix_host{nullptr};
std::atomic<uint64_t>     g_mix_last_hash{0};

struct MixWriterStats {
  uint32_t fiber_buf = 0;
  uint64_t writes = 0;
  uint32_t last_lr = 0;
  uint32_t last_r1 = 0;
  uint32_t host_tid = 0;
};
constexpr size_t kMixWriterSlots = 16;
std::mutex g_mix_mu;
MixWriterStats g_mix_writers[kMixWriterSlots]{};
uint64_t g_mix_total_writes = 0;
uint64_t g_mix_silent_writes = 0;
uint64_t g_mix_last_report_ms = 0;

uint64_t MixFnv1a64(const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
  return h;
}

const float* ResolveMixHost() {
  const float* p = g_mix_host.load(std::memory_order_relaxed);
  if (p) return p;
  if (auto* ks = rex::system::kernel_state()) {
    if (auto* mem = ks->memory()) {
      p = mem->TranslateVirtual<float*>(kMixBufGuestAddr);
      g_mix_host.store(p, std::memory_order_relaxed);
    }
  }
  return p;
}

// Attribute a buffer mutation (if any) to the outgoing fiber. Called from
// doa4_record_fiber_swap_out with g_beat_mu NOT held.
void NoteMixWriter(uint32_t out_buf, uint32_t lr, uint32_t r1, uint32_t host_tid) {
  const float* mix = ResolveMixHost();
  if (!mix) return;

  uint64_t h = MixFnv1a64(mix, kMixBufBytes);
  uint64_t prev = g_mix_last_hash.load(std::memory_order_relaxed);
  if (h == prev) return;  // no change this slice
  // Claim the change. If we lose the CAS, another swap-out on a different
  // host thread already attributed it; we silently move on.
  if (!g_mix_last_hash.compare_exchange_strong(prev, h, std::memory_order_relaxed)) {
    return;
  }

  // Cheap silence test.
  bool silent = true;
  for (size_t i = 0; i < 1536; i += 64) {
    if (mix[i] != 0.0f) { silent = false; break; }
  }

  std::lock_guard<std::mutex> g(g_mix_mu);
  ++g_mix_total_writes;
  if (silent) ++g_mix_silent_writes;

  MixWriterStats* slot = nullptr;
  for (auto& w : g_mix_writers) {
    if (w.fiber_buf == out_buf) { slot = &w; break; }
    if (slot == nullptr && w.fiber_buf == 0 && w.writes == 0) {
      slot = &w;
      slot->fiber_buf = out_buf;
    }
  }
  if (slot) {
    ++slot->writes;
    slot->last_lr = lr;
    slot->last_r1 = r1;
    slot->host_tid = host_tid;
  }

  uint64_t now = NowMs();
  if (now - g_mix_last_report_ms >= 1000) {
    g_mix_last_report_ms = now;
    REXLOG_INFO("[doa4][MixAttr1s] writes={} silent_writes={}",
                g_mix_total_writes, g_mix_silent_writes);
    for (auto& w : g_mix_writers) {
      if (w.writes == 0) continue;
      REXLOG_INFO(
          "[doa4][MixAttr1s]   fiber_buf={:#010x} host_tid={} last_lr={:#010x} "
          "last_r1={:#010x} writes={}",
          w.fiber_buf, w.host_tid, w.last_lr, w.last_r1, w.writes);
    }
    g_mix_total_writes = 0;
    g_mix_silent_writes = 0;
    for (auto& w : g_mix_writers) w = {};
  }
}

}  // namespace

extern "C" void doa4_record_fiber_tick(uint32_t dst_buf, uint32_t entry_pc) {
  std::lock_guard<std::mutex> lk(g_beat_mu);
  auto& b = g_beats[dst_buf];
  b.ticks++;
  b.last_ms = NowMs();
  if (!b.entry) b.entry = entry_pc;
}

extern "C" void doa4_record_fiber_swap_out(uint32_t out_buf, uint32_t lr, uint32_t r1,
                                           uint32_t r3, uint32_t host_tid) {
  if (!out_buf) return;
  // Audio writer attribution -- run BEFORE acquiring g_beat_mu to avoid
  // holding two locks at once.
  NoteMixWriter(out_buf, lr, r1, host_tid);
  uint64_t now = NowMs();
  std::lock_guard<std::mutex> lk(g_beat_mu);
  auto& b = g_beats[out_buf];
  b.last_lr = lr;
  b.last_r1 = r1;
  b.last_r3 = r3;
  b.host_tid = host_tid;
  b.swap_out_count++;
  // On-CPU runtime: how long was this fiber running before it yielded?
  if (b.running && b.last_swap_in_ms != 0) {
    uint64_t oncpu = now - b.last_swap_in_ms;
    if (oncpu > b.win_oncpu_max_ms) b.win_oncpu_max_ms = oncpu;
    b.win_oncpu_total_ms += oncpu;
  }
  b.last_swap_out_ms = now;
  b.running = false;
  // Mark the host-thread as off-cpu for fiber work starting now.
  if (host_tid) {
    g_host_tid_beats[host_tid].last_swap_out_ms = now;
  }
  // Track r3 (likely the wait handle / wait object pointer the fiber is
  // about to block on). If it stays constant, this fiber always blocks on
  // the same kernel object; if it varies, the fiber blocks on different
  // things each cycle.
  if (b.win_last_r3_out != 0 && b.win_last_r3_out != r3) {
    b.win_distinct_r3++;
  }
  b.win_last_r3_out = r3;
}

extern "C" void doa4_record_fiber_swap_in(uint32_t dst_buf, uint32_t lr, uint32_t r1,
                                          uint32_t r3, uint32_t host_tid) {
  if (!dst_buf) return;
  uint64_t now = NowMs();
  std::lock_guard<std::mutex> lk(g_beat_mu);
  auto& b = g_beats[dst_buf];
  // Per host-thread off-cpu gap: time between this thread's last swap_out
  // and this swap_in. Decomposes "why was fiber X off-cpu so long" into
  // (a) another fiber on the same tid ran a long time, vs (b) the host
  // thread was outside the fiber loop entirely (blocked / preempted).
  if (host_tid) {
    auto& ht = g_host_tid_beats[host_tid];
    if (ht.last_swap_out_ms != 0) {
      uint64_t off = now - ht.last_swap_out_ms;
      ht.win_off_count++;
      ht.win_off_total_ms += off;
      if (off > ht.win_off_max_ms) ht.win_off_max_ms = off;
    }
  }
  // Inter-swap-in gap (ms). Skip the very first swap-in for a fiber.
  if (b.last_swap_in_ms != 0) {
    uint64_t gap = now - b.last_swap_in_ms;
    b.win_gap_count++;
    b.win_gap_total_ms += gap;
    if (gap > b.win_gap_max_ms) b.win_gap_max_ms = gap;
  }
  b.last_lr = lr;
  b.last_r1 = r1;
  b.last_r3 = r3;
  b.host_tid = host_tid;
  b.swap_in_count++;
  b.last_swap_in_ms = now;
  b.running = true;
}

extern "C" void doa4_start_fiber_beat_thread() {
  bool expected = false;
  if (!g_beat_thread_started.compare_exchange_strong(expected, true)) return;

  // 1 Hz profile sampler — confirms whether A/V chop correlates with fiber
  // starvation (low swaps/sec, big gaps) vs. wait-timeout (smooth throughput
  // but periodic > 100 ms gaps on individual fibers).
  std::thread([] {
    uint64_t prev_ticks = 0;
    uint64_t prev_ms = NowMs();
    while (true) {
#if defined(_WIN32)
      Sleep(1000);
#else
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
#endif
      uint64_t now = NowMs();
      uint64_t dt = now - prev_ms;
      prev_ms = now;

      uint64_t total = 0;
      size_t fiber_count = 0;
      size_t stalled_100 = 0;  // fibers with no tick in last 100ms
      size_t stalled_500 = 0;  // fibers with no tick in last 500ms
      uint64_t worst_age = 0;
      uint32_t worst_buf = 0;
      // Per-fiber gap snapshots gathered under the lock, dumped after.
      struct FiberSnap {
        uint32_t buf;
        uint64_t in_count;
        uint64_t gap_count;
        uint64_t gap_total_ms;
        uint64_t gap_max_ms;
        uint32_t last_r3_out;
        uint32_t distinct_r3;
        uint32_t last_lr;
        uint32_t host_tid;
        uint64_t oncpu_max_ms;
        uint64_t oncpu_total_ms;
      };
      struct HostTidSnap {
        uint32_t tid;
        uint64_t off_count;
        uint64_t off_total_ms;
        uint64_t off_max_ms;
      };
      std::vector<FiberSnap> snaps;
      std::vector<HostTidSnap> tid_snaps;
      {
        std::lock_guard<std::mutex> lk(g_beat_mu);
        fiber_count = g_beats.size();
        snaps.reserve(fiber_count);
        for (auto& kv : g_beats) {
          total += kv.second.ticks;
          uint64_t age = kv.second.last_ms ? (now - kv.second.last_ms) : 0;
          if (age > 100) ++stalled_100;
          if (age > 500) ++stalled_500;
          if (age > worst_age) { worst_age = age; worst_buf = kv.first; }
          // Snapshot any fiber that swapped in at least ~audio-rate-ish in
          // this window; reset window stats.
          if (kv.second.win_gap_count >= 30) {
            snaps.push_back({kv.first, kv.second.swap_in_count,
                             kv.second.win_gap_count, kv.second.win_gap_total_ms,
                             kv.second.win_gap_max_ms, kv.second.win_last_r3_out,
                             kv.second.win_distinct_r3, kv.second.last_lr,
                             kv.second.host_tid,
                             kv.second.win_oncpu_max_ms,
                             kv.second.win_oncpu_total_ms});
          }
          kv.second.win_gap_count = 0;
          kv.second.win_gap_total_ms = 0;
          kv.second.win_gap_max_ms = 0;
          kv.second.win_distinct_r3 = 0;
          kv.second.win_oncpu_max_ms = 0;
          kv.second.win_oncpu_total_ms = 0;
        }
        tid_snaps.reserve(g_host_tid_beats.size());
        for (auto& kv : g_host_tid_beats) {
          if (kv.second.win_off_count == 0) continue;
          tid_snaps.push_back({kv.first, kv.second.win_off_count,
                               kv.second.win_off_total_ms,
                               kv.second.win_off_max_ms});
          kv.second.win_off_count = 0;
          kv.second.win_off_total_ms = 0;
          kv.second.win_off_max_ms = 0;
        }
      }
      uint64_t delta = total - prev_ticks;
      prev_ticks = total;
      uint64_t rate = dt ? (delta * 1000 / dt) : 0;

      g_prof_swaps_per_sec.store(rate, std::memory_order_relaxed);
      g_prof_total_swaps.store(total, std::memory_order_relaxed);
      g_prof_dt_ms.store(dt, std::memory_order_relaxed);
      g_prof_fiber_count.store(fiber_count, std::memory_order_relaxed);
      g_prof_stalled_100.store(stalled_100, std::memory_order_relaxed);
      g_prof_stalled_500.store(stalled_500, std::memory_order_relaxed);
      g_prof_worst_age_ms.store(worst_age, std::memory_order_relaxed);
      g_prof_worst_buf.store(worst_buf, std::memory_order_relaxed);

      REXLOG_INFO(
          "[doa4][Prof] swaps/s={} dt_ms={} fibers={} stalled>100ms={} >500ms={} "
          "worst_age={}ms worst_buf={:#010x}",
          rate, dt, fiber_count, stalled_100, stalled_500, worst_age, worst_buf);
      for (auto& s : snaps) {
        uint64_t avg = s.gap_count ? (s.gap_total_ms / s.gap_count) : 0;
        REXLOG_INFO(
            "[doa4][ProfFiber] buf={:#010x} host_tid={} in/s={} gap_avg={}ms "
            "gap_max={}ms oncpu_max={}ms oncpu_total={}ms last_lr={:#010x} "
            "last_r3_out={:#010x} distinct_r3={}",
            s.buf, s.host_tid, s.gap_count, avg, s.gap_max_ms,
            s.oncpu_max_ms, s.oncpu_total_ms,
            s.last_lr, s.last_r3_out, s.distinct_r3);
      }
      for (auto& t : tid_snaps) {
        uint64_t avg = t.off_count ? (t.off_total_ms / t.off_count) : 0;
        REXLOG_INFO(
            "[doa4][ProfHostTid] tid={} swaps={} off_avg={}ms off_max={}ms "
            "off_total={}ms",
            t.tid, t.off_count, avg, t.off_max_ms, t.off_total_ms);
      }
    }
  }).detach();

  std::thread([] {
    uint64_t t0 = NowMs();
    uint64_t last_health_log = 0;
    while (true) {
#if defined(_WIN32)
      Sleep(3000);
#else
      std::this_thread::sleep_for(std::chrono::milliseconds(3000));
#endif
      uint64_t now = NowMs();
      size_t fiber_count = 0;
      uint64_t worst_age = 0;
      uint32_t worst_buf = 0;
      uint64_t total_ticks = 0;
      {
        std::lock_guard<std::mutex> lk(g_beat_mu);
        fiber_count = g_beats.size();
        for (auto& kv : g_beats) {
          total_ticks += kv.second.ticks;
          uint64_t age = kv.second.last_ms ? (now - kv.second.last_ms) : (now - t0);
          if (age > worst_age) { worst_age = age; worst_buf = kv.first; }
        }
      }
      // Loud warning only when something looks wedged; quiet heartbeat
      // every 30 s so we can confirm the thread is alive.
      if (worst_age > 500) {
        REXLOG_WARN("[doa4][Beat] STALL fibers={} worst_buf={:#010x} age={}ms ticks_total={}",
                    fiber_count, worst_buf, worst_age, total_ticks);
        // Dump per-fiber state once per freeze episode so we can see what
        // every fiber is blocked on. Cleared when forward progress resumes.
        bool expected = false;
        if (g_freeze_dumped.compare_exchange_strong(expected, true)) {
          std::lock_guard<std::mutex> lk(g_beat_mu);
          REXLOG_WARN("[doa4][Beat] ----- FREEZE DUMP ({} fibers) -----", g_beats.size());
          for (auto& kv : g_beats) {
            const auto& b = kv.second;
            uint64_t age = b.last_ms ? (now - b.last_ms) : (now - t0);
            uint64_t in_age = b.last_swap_in_ms ? (now - b.last_swap_in_ms) : 0;
            uint64_t out_age = b.last_swap_out_ms ? (now - b.last_swap_out_ms) : 0;
            REXLOG_WARN(
                "[doa4][Beat]   buf={:#010x} entry={:#010x} ticks={} age={}ms "
                "state={} host_tid={} last_lr={:#010x} last_r1={:#010x} "
                "last_r3={:#010x} swaps_in={} swaps_out={} since_in={}ms since_out={}ms",
                kv.first, b.entry, b.ticks, age, b.running ? "RUN" : "OUT",
                b.host_tid, b.last_lr, b.last_r1, b.last_r3, b.swap_in_count,
                b.swap_out_count, in_age, out_age);
          }
          REXLOG_WARN("[doa4][Beat] ----- END FREEZE DUMP -----");
        }
      } else if (now - last_health_log >= 30000) {
        REXLOG_INFO("[doa4][Beat] ok fibers={} worst_age={}ms ticks_total={}",
                    fiber_count, worst_age, total_ticks);
        last_health_log = now;
        g_freeze_dumped.store(false, std::memory_order_relaxed);
      } else if (worst_age <= 500) {
        // Quietly arm the next dump once we're healthy again.
        g_freeze_dumped.store(false, std::memory_order_relaxed);
      }
    }
  }).detach();
}

// ---------------------------------------------------------------------------
// PANIC observers — surface guest bugcheck wrappers when they ever fire.
// sub_82793F70: mr r3,r31 ; bl sub_827925F8 ; li r3,14 ; bl KeBugCheck.
// sub_827925F8: TLS-load fn ptr, call indirect, then bl KeBugCheck.
// Neither fires in normal runs — kept armed so we'd see them if hit.
// ---------------------------------------------------------------------------
REX_HOOK_RAW(sub_82793F70) {
  static std::atomic<uint64_t> s_n{0};
  uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed);
  REXLOG_WARN("[doa4][PANIC] sub_82793F70 #{} tid={} lr={:#x} r3={:#x} r4={:#x} r31={:#x}",
              n, Tid(), (uint32_t)ctx.lr, (uint32_t)ctx.r3.u64,
              (uint32_t)ctx.r4.u64, (uint32_t)ctx.r31.u64);
  ctx.r3.s64 = 0;
}

REX_HOOK_RAW(sub_827925F8) {
  static std::atomic<uint64_t> s_n{0};
  uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed);
  REXLOG_WARN("[doa4][PANIC] sub_827925F8 #{} tid={} lr={:#x} r3={:#x} r4={:#x}",
              n, Tid(), (uint32_t)ctx.lr, (uint32_t)ctx.r3.u64, (uint32_t)ctx.r4.u64);
  ctx.r3.s64 = 0;
}

// ---------------------------------------------------------------------------
// AV2 trace: sub_824F31E8 is where the access violation manifests at
// recomp.17.cpp:68570 (lwz r11,4556(r30)).  Root cause is that sub_82793F80
// (KiSwapContext) was returning with a destination thread's r25-r30=0 register
// snapshot grafted onto the C++ caller chain.  FIX 3 in app_hooks neutralises
// the swapper; these traces are left in place to verify behaviour.
// ---------------------------------------------------------------------------
extern "C" {
REX_EXTERN(__imp__sub_824F31E8);
REX_EXTERN(__imp__sub_82792588);
}

REX_HOOK_RAW(sub_824F31E8) {
  static std::atomic<uint64_t> s_n{0};
  uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed);
  bool log = (n < 2) || ((n & 0xFFFF) == 0);
  uint32_t r1_in = (uint32_t)ctx.r1.u64;
  uint32_t r25_in = (uint32_t)ctx.r25.u64;
  uint32_t r26_in = (uint32_t)ctx.r26.u64;
  uint32_t r27_in = (uint32_t)ctx.r27.u64;
  uint32_t r28_in = (uint32_t)ctx.r28.u64;
  uint32_t r29_in = (uint32_t)ctx.r29.u64;
  uint32_t r30_in = (uint32_t)ctx.r30.u64;
  uint32_t r31_in = (uint32_t)ctx.r31.u64;
  uint32_t lr_in = (uint32_t)ctx.lr;
  if (log) {
    REXLOG_DEBUG("[doa4][AV2] sub_824F31E8 ENTER #{} tid={} lr={:#x} r28={:#x} r30={:#x}",
                 n, Tid(), lr_in, r28_in, r30_in);
  }
  __imp__sub_824F31E8(ctx, base);
  if (log) {
    REXLOG_DEBUG("[doa4][AV2] sub_824F31E8 EXIT  #{} tid={} r28={:#x} r30={:#x}",
                 n, Tid(), (uint32_t)ctx.r28.u64, (uint32_t)ctx.r30.u64);
  }
  (void)r25_in; (void)r26_in; (void)r27_in; (void)r29_in; (void)r31_in; (void)r1_in;
}

REX_HOOK_RAW(sub_82792588) {
  static std::atomic<uint64_t> s_n{0};
  uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed);
  uint32_t r25_in = (uint32_t)ctx.r25.u64;
  uint32_t r26_in = (uint32_t)ctx.r26.u64;
  uint32_t r27_in = (uint32_t)ctx.r27.u64;
  uint32_t r28_in = (uint32_t)ctx.r28.u64;
  uint32_t r29_in = (uint32_t)ctx.r29.u64;
  uint32_t r30_in = (uint32_t)ctx.r30.u64;
  // Only the clobber path below ever needs to log. Silence the per-call
  // ENTER trace; FIX D is proven stable.
  __imp__sub_82792588(ctx, base);
  (void)n;
  uint32_t r25_out = (uint32_t)ctx.r25.u64;
  uint32_t r26_out = (uint32_t)ctx.r26.u64;
  uint32_t r27_out = (uint32_t)ctx.r27.u64;
  uint32_t r28_out = (uint32_t)ctx.r28.u64;
  uint32_t r29_out = (uint32_t)ctx.r29.u64;
  uint32_t r30_out = (uint32_t)ctx.r30.u64;
  bool nv_changed = (r25_in != r25_out) || (r26_in != r26_out) ||
                    (r27_in != r27_out) || (r28_in != r28_out) ||
                    (r29_in != r29_out) || (r30_in != r30_out);
  if (nv_changed) {
    REXLOG_ERROR("[doa4][AV2] sub_82792588 NV CLOBBER #{} r25 {:#x}->{:#x} r26 {:#x}->{:#x} r27 {:#x}->{:#x} r28 {:#x}->{:#x} r29 {:#x}->{:#x} r30 {:#x}->{:#x}",
                 n, r25_in, r25_out, r26_in, r26_out, r27_in, r27_out,
                 r28_in, r28_out, r29_in, r29_out, r30_in, r30_out);
  }
}

// ---------------------------------------------------------------------------
// SSM44 trace: sub_82897FB8 is the CompiledShader::SetBytecode setter paired
// with the getter sub_828980E0 (the one whose lwz r3,44(r31) we currently
// bypass-patch via a lazy 16KB allocation when *(self+44)==NULL).
//
//   sub_82897FB8(self=r3, dword_count=r4, src_buffer=r5):
//     assert self != NULL
//     buf = self->vtable[1](self->vtable[0], dword_count*4 + 8)
//     *(self+44) = buf
//     assert buf != NULL
//     assert *buf % 3 == 0 && *buf < 16384
//     memcpy(buf + 8, src, dword_count*4)
//
// If the bypass ever fires for a self pointer, that means this setter was
// NEVER called for that shader.  Log every invocation (entry args + lr +
// returned buf) so we can correlate against bypass triggers.
// ---------------------------------------------------------------------------
extern "C" {
REX_EXTERN(__imp__sub_82897FB8);
REX_EXTERN(__imp__sub_8290D7D0);
REX_EXTERN(__imp__sub_8290DDA0);
REX_EXTERN(__imp__sub_8290FEE8);
REX_EXTERN(__imp__sub_8290FF48);
}

REX_HOOK_RAW(sub_82897FB8) {
  static std::atomic<uint64_t> s_n{0};
  uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed);
  uint32_t self = (uint32_t)ctx.r3.u64;
  uint32_t dwords = (uint32_t)ctx.r4.u64;
  uint32_t src = (uint32_t)ctx.r5.u64;
  uint32_t lr = (uint32_t)ctx.lr;
  __imp__sub_82897FB8(ctx, base);
  uint32_t r3_out = (uint32_t)ctx.r3.u64;
  uint32_t buf44 = self ? rex::memory::load_and_swap<uint32_t>(base + self + 44) : 0;
  REXLOG_INFO("[doa4][SSM44] sub_82897FB8 #{} tid={} self={:#x} dwords={} src={:#x} lr={:#x} -> ret={:#x} *(self+44)={:#x}",
              n, Tid(), self, dwords, src, lr, r3_out, buf44);
}

#define DOA4_SSM_CALLER_HOOK(NAME)                                                   \
  REX_HOOK_RAW(NAME) {                                                               \
    static std::atomic<uint64_t> s_n{0};                                             \
    uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed);                        \
    if (n < 16 || (n & 0xFFF) == 0) {                                                \
      REXLOG_INFO("[doa4][SSM44] " #NAME " ENTER #{} tid={} r3={:#x} r4={:#x} r5={:#x} r6={:#x} lr={:#x}", \
                  n, Tid(), (uint32_t)ctx.r3.u64, (uint32_t)ctx.r4.u64,              \
                  (uint32_t)ctx.r5.u64, (uint32_t)ctx.r6.u64, (uint32_t)ctx.lr);     \
    }                                                                                \
    __imp__##NAME(ctx, base);                                                        \
  }

DOA4_SSM_CALLER_HOOK(sub_8290D7D0)
DOA4_SSM_CALLER_HOOK(sub_8290DDA0)
DOA4_SSM_CALLER_HOOK(sub_8290FEE8)
DOA4_SSM_CALLER_HOOK(sub_8290FF48)

// ---------------------------------------------------------------------------
// Host-side AV handler.  Dumps host PC + GPRs, guest PPC register snapshot,
// and a CaptureStackBackTrace so we can map host_pc → recomp function
// offline via llvm-symbolizer.  Returns false so the process still
// terminates and we get the standard crash dump.
// ---------------------------------------------------------------------------
namespace {

bool DoaAvHandler(rex::arch::Exception* ex, void* /*data*/) {
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation) {
    return false;
  }
  uint64_t pc = ex->pc();
  uint64_t fault = ex->fault_address();

  // Skip faults inside the guest memory mapping entirely. Those are handled
  // by the SDK's MMIO/write-watch path (which sits behind us in the VEH chain
  // because we installed first via static init). Logging them here would
  // produce thousands of bogus "ACCESS VIOLATION" entries per second for
  // perfectly normal GPU dirty-tracking traps.
  //
  // The guest mapping is [virtual_membase, virtual_membase + 0x200000000).
  // Cache the base on first use; before the kernel is up, fall through and
  // log everything (any AV that early is almost certainly a real host bug).
  static std::atomic<uint64_t> s_mapping_base{0};
  uint64_t mapping_base = s_mapping_base.load(std::memory_order_relaxed);
  if (mapping_base == 0) {
    if (auto* ks = rex::system::kernel_state()) {
      if (auto* mem = ks->memory()) {
        mapping_base = reinterpret_cast<uint64_t>(mem->virtual_membase());
        s_mapping_base.store(mapping_base, std::memory_order_relaxed);
      }
    }
  }
  if (mapping_base != 0 && fault >= mapping_base &&
      fault < mapping_base + 0x200000000ull) {
    return false;  // MMIO handler will deal with it.
  }

  // Rate-limit repeated faults at the same (pc, fault) site for everything
  // else. Each unique site logs once; repeats are silently dropped.
  static std::atomic<uint64_t> s_total{0};
  static std::atomic<uint64_t> s_unique{0};
  static constexpr size_t kSeenCap = 256;
  static std::atomic<uint64_t> s_seen[kSeenCap]{};
  uint64_t site = pc ^ (fault << 1);
  bool seen = false;
  for (size_t i = 0; i < kSeenCap; ++i) {
    uint64_t v = s_seen[i].load(std::memory_order_relaxed);
    if (v == site) { seen = true; break; }
    if (v == 0) {
      uint64_t expected = 0;
      if (s_seen[i].compare_exchange_strong(expected, site, std::memory_order_relaxed)) {
        s_unique.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      // Lost race - re-check this slot.
      if (s_seen[i].load(std::memory_order_relaxed) == site) { seen = true; break; }
    }
  }
  uint64_t total = s_total.fetch_add(1, std::memory_order_relaxed);
  if (seen) {
    // Periodic heartbeat so we can confirm the watch loop is still progressing.
    if ((total & 0x3FF) == 0) {
      REXLOG_INFO("[doa4][AV] ...still recovering watches: total={} unique_sites={}",
                  total, s_unique.load(std::memory_order_relaxed));
    }
    return false;
  }

  uint32_t tid = Tid();
  auto* tc = ex->thread_context();
  REXLOG_ERROR("[doa4][AV] ACCESS VIOLATION tid={} host_pc={:#x} fault={:#x} (total={} unique={})",
               tid, pc, fault, total, s_unique.load(std::memory_order_relaxed));
#if defined(_M_X64) || defined(__x86_64__)
  REXLOG_ERROR("[doa4][AV]   rax={:#x} rcx={:#x} rdx={:#x} rbx={:#x}",
               tc->rax, tc->rcx, tc->rdx, tc->rbx);
  REXLOG_ERROR("[doa4][AV]   rsp={:#x} rbp={:#x} rsi={:#x} rdi={:#x}",
               tc->rsp, tc->rbp, tc->rsi, tc->rdi);
  REXLOG_ERROR("[doa4][AV]   r8={:#x} r9={:#x} r10={:#x} r11={:#x}",
               tc->r8, tc->r9, tc->r10, tc->r11);
  REXLOG_ERROR("[doa4][AV]   r12={:#x} r13={:#x} r14={:#x} r15={:#x}",
               tc->r12, tc->r13, tc->r14, tc->r15);
#endif
  auto* ts = rex::runtime::ThreadState::Get();
  if (ts && ts->context()) {
    auto* pctx = ts->context();
    REXLOG_ERROR("[doa4][AV]   guest lr={:#x} r1={:#x} r3={:#x} r4={:#x} r11={:#x} r31={:#x}",
                 (uint32_t)pctx->lr, (uint32_t)pctx->r1.u64,
                 (uint32_t)pctx->r3.u64, (uint32_t)pctx->r4.u64,
                 (uint32_t)pctx->r11.u64, (uint32_t)pctx->r31.u64);
    REXLOG_ERROR("[doa4][AV]   guest r25={:#x} r26={:#x} r27={:#x} r28={:#x} r29={:#x} r30={:#x}",
                 (uint32_t)pctx->r25.u64, (uint32_t)pctx->r26.u64,
                 (uint32_t)pctx->r27.u64, (uint32_t)pctx->r28.u64,
                 (uint32_t)pctx->r29.u64, (uint32_t)pctx->r30.u64);
    REXLOG_ERROR("[doa4][AV]   guest r5={:#x} r6={:#x} r7={:#x} r8={:#x} r9={:#x} r10={:#x} r12={:#x}",
                 (uint32_t)pctx->r5.u64, (uint32_t)pctx->r6.u64,
                 (uint32_t)pctx->r7.u64, (uint32_t)pctx->r8.u64,
                 (uint32_t)pctx->r9.u64, (uint32_t)pctx->r10.u64,
                 (uint32_t)pctx->r12.u64);
  }
#if defined(_WIN32)
  HMODULE main_mod = GetModuleHandleW(nullptr);
  REXLOG_ERROR("[doa4][AV]   main_module_base={:#x}", (uint64_t)main_mod);
  void* frames[16] = {};
  USHORT n = CaptureStackBackTrace(0, 16, frames, nullptr);
  for (USHORT i = 0; i < n; ++i) {
    uint64_t fp = (uint64_t)frames[i];
    uint64_t off = fp - (uint64_t)main_mod;
    REXLOG_ERROR("[doa4][AV]   stack[{}] pc={:#x} off=+{:#x}", i, fp, off);
  }
#endif
  return false;  // let it crash
}

void EnsureAvHandlerInstalled() {
  static std::atomic<bool> s_installed{false};
  bool expected = false;
  if (s_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    rex::arch::ExceptionHandler::Install(DoaAvHandler, nullptr);
    REXLOG_INFO("[doa4][AV] handler installed");
  }
}

struct AvAutoInstall { AvAutoInstall() { EnsureAvHandlerInstalled(); } };
static AvAutoInstall s_av_auto_install;

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// Catch-all VEH for FP / divide / overflow exceptions that the SDK's
// ExceptionHandler (AV + illegal-instruction only) drops on the floor.
// Logs the site, fully masks MXCSR + x87 control word for the current
// thread, then CONTINUEs so the title can keep running.
// ---------------------------------------------------------------------------
LONG CALLBACK DoaFpVeh(PEXCEPTION_POINTERS ex_info) {
  DWORD code = ex_info->ExceptionRecord->ExceptionCode;
  bool is_fp = false;
  const char* name = nullptr;
  switch (code) {
    case EXCEPTION_FLT_INEXACT_RESULT:    name = "FLT_INEXACT";    is_fp = true; break;
    case EXCEPTION_FLT_INVALID_OPERATION: name = "FLT_INVALID";    is_fp = true; break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    name = "FLT_DIV0";       is_fp = true; break;
    case EXCEPTION_FLT_OVERFLOW:          name = "FLT_OVERFLOW";   is_fp = true; break;
    case EXCEPTION_FLT_UNDERFLOW:         name = "FLT_UNDERFLOW";  is_fp = true; break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:  name = "FLT_DENORMAL";   is_fp = true; break;
    case EXCEPTION_FLT_STACK_CHECK:       name = "FLT_STACK";      is_fp = true; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    name = "INT_DIV0";       break;
    case EXCEPTION_INT_OVERFLOW:          name = "INT_OVERFLOW";   break;
    default:
      // Skip codes the SDK or debugger already handles.
      return EXCEPTION_CONTINUE_SEARCH;
  }

  static std::atomic<uint64_t> s_total{0};
  static constexpr size_t kSeenCap = 64;
  static std::atomic<uint64_t> s_seen[kSeenCap]{};
  uint64_t pc = (uint64_t)ex_info->ExceptionRecord->ExceptionAddress;
  bool seen = false;
  for (size_t i = 0; i < kSeenCap; ++i) {
    uint64_t v = s_seen[i].load(std::memory_order_relaxed);
    if (v == pc) { seen = true; break; }
    if (v == 0) {
      uint64_t expected = 0;
      if (s_seen[i].compare_exchange_strong(expected, pc, std::memory_order_relaxed)) {
        break;
      }
      if (s_seen[i].load(std::memory_order_relaxed) == pc) { seen = true; break; }
    }
  }
  uint64_t total = s_total.fetch_add(1, std::memory_order_relaxed);

  if (!seen) {
    HMODULE main_mod = GetModuleHandleW(nullptr);
    uint64_t off = pc - (uint64_t)main_mod;
    uint32_t mxcsr = ex_info->ContextRecord->MxCsr;
    REXLOG_ERROR("[doa4][FP] {} tid={} host_pc={:#x} off=+{:#x} mxcsr={:#x} (#{} unique)",
                 name, Tid(), pc, off, mxcsr, total);
    auto* ts = rex::runtime::ThreadState::Get();
    if (ts && ts->context()) {
      auto* pctx = ts->context();
      REXLOG_ERROR("[doa4][FP]   guest lr={:#x} r1={:#x} r3={:#x} r4={:#x} r31={:#x}",
                   (uint32_t)pctx->lr, (uint32_t)pctx->r1.u64,
                   (uint32_t)pctx->r3.u64, (uint32_t)pctx->r4.u64,
                   (uint32_t)pctx->r31.u64);
    }
    void* frames[12] = {};
    USHORT n = CaptureStackBackTrace(0, 12, frames, nullptr);
    HMODULE mm = GetModuleHandleW(nullptr);
    for (USHORT i = 0; i < n; ++i) {
      uint64_t fp = (uint64_t)frames[i];
      REXLOG_ERROR("[doa4][FP]   stack[{}] pc={:#x} off=+{:#x}", i, fp,
                   fp - (uint64_t)mm);
    }
  }

  if (is_fp) {
    // Clear pending FP status and force all FP exceptions masked on this
    // thread, then resume the faulting instruction. With masks set, the
    // retry produces only the default IEEE result (NaN / inf / inexact)
    // without raising.
    ex_info->ContextRecord->MxCsr = 0x1F80;  // all exceptions masked
    if ((ex_info->ContextRecord->ContextFlags & CONTEXT_FLOATING_POINT) ==
        CONTEXT_FLOATING_POINT) {
      ex_info->ContextRecord->FltSave.ControlWord = 0x027F;  // x87: mask all
      ex_info->ContextRecord->FltSave.StatusWord &= 0xFF00;  // clear exceptions
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

struct FpVehInstaller {
  FpVehInstaller() {
    // FIRST handler: runs before the SDK's VEH so we see FP faults that
    // its switch() drops via CONTINUE_SEARCH.
    AddVectoredExceptionHandler(1, DoaFpVeh);
    REXLOG_INFO("[doa4][FP] VEH installed");
  }
};
static FpVehInstaller s_fp_veh_installer;
#endif  // _WIN32

}  // namespace

// ---------------------------------------------------------------------------
// Snapshot accessor for the ImGui diagnostic overlay (doa4_diag_overlay.cpp).
// All values are produced once per second by the [doa4][Prof] sampler thread
// inside doa4_start_fiber_beat_thread().
// ---------------------------------------------------------------------------
extern "C" {

struct Doa4ProfileSnapshot {
  uint64_t swaps_per_sec;
  uint64_t total_swaps;
  uint64_t sample_dt_ms;
  uint64_t fiber_count;
  uint64_t stalled_over_100ms;
  uint64_t stalled_over_500ms;
  uint64_t worst_age_ms;
  uint32_t worst_buf;
};

void doa4_get_profile_snapshot(Doa4ProfileSnapshot* out) {
  if (!out) return;
  out->swaps_per_sec      = g_prof_swaps_per_sec.load(std::memory_order_relaxed);
  out->total_swaps        = g_prof_total_swaps.load(std::memory_order_relaxed);
  out->sample_dt_ms       = g_prof_dt_ms.load(std::memory_order_relaxed);
  out->fiber_count        = g_prof_fiber_count.load(std::memory_order_relaxed);
  out->stalled_over_100ms = g_prof_stalled_100.load(std::memory_order_relaxed);
  out->stalled_over_500ms = g_prof_stalled_500.load(std::memory_order_relaxed);
  out->worst_age_ms       = g_prof_worst_age_ms.load(std::memory_order_relaxed);
  out->worst_buf          = g_prof_worst_buf.load(std::memory_order_relaxed);
}

}  // extern "C"

// DOA4 netplay phase 0: the job workers (fix "workers").
//
// sub_827921C0 = SignalObjectAndWait(signal r3, wait r4, timeout r5, alertable
// r6) is the top of every job worker's loop (signal own done, wait for the next
// start); only the 25 job workers call it (census logged once per caller). They
// form five per-core chains that run in parallel each frame. Parallel is not
// deterministic: scene jobs on several workers draw from the shared scene RNG
// (MT 0x8357B5C0), and the interleaving of their draws changed which particle
// got which number (a petal-emitter job drew 156 vs 152 times at frame 47).
// Ordering the workers among themselves is not enough either: scene jobs and the
// main thread's update code share state (the stage-light timers, the scene RNG
// again), and a worker running beside the main thread made a Tag fight diverge
// now and then (frames 4026, 5923, 7421 in different runs; one in three runs).
// Modes (DOA4_NP_SERIAL overrides; the fix's default in a session is 3):
//   0  parallel, as the game runs them;
//   1  mutually exclusive: a worker holds one process-wide lock from its wait's
//      return to its next signal (the order is still the OS's);
//   2  one at a time in a fixed order: by thread id, which is creation order and
//      round-robin over the five chains, so it agrees with each chain's own
//      order; a token, reset by the frame hook, passes from worker to worker as
//      each one signals. Every worker runs exactly once per frame (traced), so
//      the token never waits for one that does not come; if it ever did, the
//      waiter gives up after 2 s and says so;
//   3  mode 2, and the workers never run beside the main thread: after the
//      update half sets the pool's start events the main thread waits until
//      the workers have gone as far as they can before the pool's mid event,
//      and after it sets the mid event it waits until all are done.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/xthread.h>

#include "generated/default/dead_or_alive_4_init.h"

#include "np_det_internal.h"
#include "np_session.h"

extern "C" {
REX_EXTERN(__imp__sub_827921C0);
REX_EXTERN(__imp__sub_82792140);
REX_EXTERN(__imp__sub_82791190);
}

namespace doa4::np::det {
namespace {

constexpr int kWorkerCount = 25;
std::mutex g_mu;          // mode 2: the token
std::condition_variable g_cv;
std::mutex g_serial_mu;   // mode 1: held through a worker's work section
thread_local bool t_held = false;   // mode 1
thread_local int t_rank = -1;       // mode 2
std::atomic<int> g_mode{-1};        // -1: not decided yet
std::vector<uint32_t> g_tids;       // sorted worker thread ids
int g_token = 0;
uint32_t g_timeouts = 0;
int g_ranked = 0;                   // workers that have their rank
int g_mid_waiter = -1;              // rank of the worker blocked on the pool's mid event
uint32_t g_main_timeouts = 0;
constexpr uint32_t kPool = 0x8341FAA0;  // job pool: start events +20..+36, done +40.., mid +60

int Mode() {
  int v = g_mode.load(std::memory_order_relaxed);
  if (v < 0) {
    const char* e = std::getenv("DOA4_NP_SERIAL");
    v = e && *e ? std::atoi(e) : (On(kWorkers) ? 3 : 0);
    g_mode.store(v);
    LogLine(std::string("job workers: ") +
            (v == 3   ? "one at a time in a fixed order, never beside the main thread"
             : v == 2 ? "one at a time in a fixed order"
             : v == 1 ? "mutually exclusive"
                      : "parallel") +
            (e && *e ? " (DOA4_NP_SERIAL)" : ""));
  }
  return Stopped() ? 0 : v;
}

uint32_t Be32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}

// Mode 3, main thread, right after it set the pool's last start event (until
// the token holder is blocked on the mid event, or all are done) or the mid
// event (until all are done): the workers' work never overlaps the main
// thread's own update code.
void MainWaitForWorkers(bool until_done) {
  std::unique_lock lk(g_mu);
  if (g_ranked < kWorkerCount) return;
  auto ready = [until_done] {
    return Stopped() || g_token >= kWorkerCount || (!until_done && g_mid_waiter >= 0 && g_mid_waiter == g_token);
  };
  if (!g_cv.wait_for(lk, std::chrono::seconds(2), ready) && g_main_timeouts++ < 20) {
    LogLine(fmt::format("job workers: frame {}: the main thread waited 2 s for the workers ({}, token {}, "
                        "mid waiter {})",
                        Frame(), until_done ? "all" : "up to the mid event", g_token, g_mid_waiter));
  }
}

}  // namespace

void WorkersFrame() {
  std::lock_guard lk(g_mu);
  g_token = 0;
}

void WorkersStop() {
  {
    std::lock_guard lk(g_mu);
    g_token = 0;
  }
  g_cv.notify_all();  // waiters see Stopped() and run free
}

}  // namespace doa4::np::det

using namespace doa4::np::det;

REX_HOOK_RAW(sub_827921C0) {
  if (!doa4::np::Active()) {
    __imp__sub_827921C0(ctx, base);
    return;
  }
  rex::system::XThread* thread = rex::system::XThread::GetCurrentThread();
  const bool main = thread && thread->main_thread();
  const uint32_t tid = thread ? thread->thread_id() : 0;
  {
    static std::mutex mu;
    static std::vector<uint32_t> seen;
    const uint32_t lr = uint32_t(ctx.lr);
    std::lock_guard lk(mu);
    if (std::find(seen.begin(), seen.end(), lr) == seen.end() && seen.size() < 64) {
      seen.push_back(lr);
      doa4::np::LogLine(fmt::format("SignalObjectAndWait caller lr {:08X} thread {} start {:08X}{} (frame {})",
                                    lr, tid, ThreadStart(), main ? " (main)" : "", Frame()));
    }
  }
  const int mode = main ? 0 : Mode();
  if (t_held) {  // mode 1 (or a mode that ended): give the lock back
    t_held = false;
    g_serial_mu.unlock();
  }
  if (mode >= 2) {
    std::lock_guard lk(g_mu);
    if (t_rank < 0) {
      if (std::find(g_tids.begin(), g_tids.end(), tid) == g_tids.end()) {
        g_tids.push_back(tid);
        std::sort(g_tids.begin(), g_tids.end());
        if (int(g_tids.size()) == kWorkerCount) {
          doa4::np::LogLine("job workers: all 25 registered at frame " + std::to_string(Frame()) +
                            "; the fixed order applies from their next wake");
        }
      }
    } else if (g_token == t_rank) {
      ++g_token;  // end of this worker's work section: the next one may run
      g_cv.notify_all();
    }
  }
  __imp__sub_827921C0(ctx, base);
  TraceRead(kRdWorker, tid);
  if (mode == 1) {
    g_serial_mu.lock();
    t_held = true;
  } else if (mode >= 2) {
    std::unique_lock lk(g_mu);
    if (t_rank < 0 && int(g_tids.size()) == kWorkerCount) {
      t_rank = int(std::find(g_tids.begin(), g_tids.end(), tid) - g_tids.begin());
      ++g_ranked;
    }
    if (t_rank >= 0) {
      const int rank = t_rank;
      if (!g_cv.wait_for(lk, std::chrono::milliseconds(2000), [rank] { return g_token == rank || Stopped(); })) {
        if (g_timeouts++ < 20) {
          doa4::np::LogLine(fmt::format("job workers: frame {}: rank {} (thread {}) waited 2 s for the "
                                        "token at {}; running anyway",
                                        Frame(), rank, tid, g_token));
        }
        g_token = rank;
      }
    }
  }
}

// sub_82792140 = SetEvent(r3). The main loop's update half sets the job pool's
// five start events (call site 0x824F48AC) and later its mid event (0x824F491C);
// in mode 3 the main thread then waits for the workers (MainWaitForWorkers).
REX_HOOK_RAW(sub_82792140) {
  const uint32_t handle = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82792140(ctx, base);
  if (!doa4::np::Active() || (lr != 0x824F48ACu && lr != 0x824F491Cu) || Mode() != 3 || !IsMainThread()) return;
  if (lr == 0x824F48ACu && handle == Be32(base, kPool + 36)) {
    MainWaitForWorkers(false);
  } else if (lr == 0x824F491Cu) {
    MainWaitForWorkers(true);
  }
}

// sub_82791190 = WaitForSingleObject(r3, timeout r4): a worker that waits for the
// pool's mid event (set by the main thread midway through its update half) is
// marked, so the main thread's first wait in mode 3 knows the workers have gone
// as far as they can before the mid event.
REX_HOOK_RAW(sub_82791190) {
  if (!doa4::np::Active() || t_rank < 0 || ctx.r3.u32 != Be32(base, kPool + 60)) {
    __imp__sub_82791190(ctx, base);
    return;
  }
  {
    std::lock_guard lk(g_mu);
    g_mid_waiter = t_rank;
  }
  g_cv.notify_all();
  __imp__sub_82791190(ctx, base);
  std::lock_guard lk(g_mu);
  if (g_mid_waiter == t_rank) g_mid_waiter = -1;
}

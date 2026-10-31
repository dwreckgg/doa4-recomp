// Diagnostic ImGui overlay for DOA4. Combines:
//   * Fiber scheduler snapshot from the [doa4][Prof] 1Hz sampler in
//     doa4_debug_hooks.cpp (via doa4_get_profile_snapshot).
//   * SDK perf counters (rex::perf) which are flipped per-frame by the
//     profiler; we accumulate snapshot deltas in OnDraw to compute per-second
//     rates with proper wall-clock timing.
//
// Pure consumer — no game-side behavior changes.

#include "doa4_diag_overlay.h"

#include <imgui.h>

#include <chrono>
#include <cinttypes>

#include <rex/perf/counter.h>

#include "net/np_session.h"

extern "C" {

struct Doa4ProfileSnapshot {
  unsigned long long swaps_per_sec;
  unsigned long long total_swaps;
  unsigned long long sample_dt_ms;
  unsigned long long fiber_count;
  unsigned long long stalled_over_100ms;
  unsigned long long stalled_over_500ms;
  unsigned long long worst_age_ms;
  unsigned int       worst_buf;
};

void doa4_get_profile_snapshot(Doa4ProfileSnapshot* out);

}  // extern "C"

namespace doa4 {

namespace {

using clock = std::chrono::steady_clock;

// Wall-clock window state for SDK perf-counter rate computation.
// Snapshots are per-frame and zeroed by ResetFrameCounters(); we sum them
// here and divide by elapsed wall time once per second.
struct RateWindow {
  clock::time_point start = clock::now();
  long long accum_draws = 0;
  long long accum_xma = 0;
  long long accum_dispatch = 0;
  long long accum_irq = 0;
  long long accum_verts = 0;
  long long accum_stalls = 0;
};

RateWindow g_win;

}  // namespace

DiagOverlayDialog::DiagOverlayDialog(rex::ui::ImGuiDrawer* drawer)
    : rex::ui::ImGuiDialog(drawer) {}

void DiagOverlayDialog::OnDraw(ImGuiIO& io) {
  (void)io;

  // ---- Fiber sampler snapshot (already 1 Hz, just read) ----
  Doa4ProfileSnapshot s{};
  doa4_get_profile_snapshot(&s);

  if (s.total_swaps != last_seen_total_) {
    last_seen_total_ = s.total_swaps;
    swaps_history_[history_idx_] = static_cast<float>(s.swaps_per_sec);
    draws_history_[history_idx_] = rate_draws_;
    xma_history_[history_idx_]   = rate_xma_;
    history_idx_ = (history_idx_ + 1) % kHistory;
  }

  // ---- Accumulate per-frame SDK snapshots into a 1s window ----
  using namespace rex::perf;
  g_win.accum_draws    += GetSnapshotCounter(CounterId::kDrawCalls);
  g_win.accum_xma      += GetSnapshotCounter(CounterId::kXmaFramesDecoded);
  g_win.accum_dispatch += GetSnapshotCounter(CounterId::kFunctionsDispatched);
  g_win.accum_irq      += GetSnapshotCounter(CounterId::kInterruptDispatches);
  g_win.accum_verts    += GetSnapshotCounter(CounterId::kVerticesProcessed);
  g_win.accum_stalls   += GetSnapshotCounter(CounterId::kCommandBufferStalls);

  auto now = clock::now();
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - g_win.start).count();
  if (elapsed_ms >= 1000) {
    double secs = elapsed_ms / 1000.0;
    rate_draws_    = static_cast<float>(g_win.accum_draws    / secs);
    rate_xma_      = static_cast<float>(g_win.accum_xma      / secs);
    rate_dispatch_ = static_cast<float>(g_win.accum_dispatch / secs);
    rate_irq_      = static_cast<float>(g_win.accum_irq      / secs);
    g_win = RateWindow{};
    g_win.start = now;
  }

  // ---- Draw window ----
  ImGui::SetNextWindowPos(ImVec2(10, 300), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(340, 400), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.6f);
  if (ImGui::Begin("DOA4 Diagnostics##doa4diag", nullptr,
                   ImGuiWindowFlags_NoCollapse)) {

    if (ImGui::CollapsingHeader("Fiber scheduler", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Text("Swaps/sec    : %llu", s.swaps_per_sec);
      ImGui::Text("Total swaps  : %llu", s.total_swaps);
      ImGui::Text("Active fibers: %llu", s.fiber_count);
      ImGui::Text("Stalled >100ms: %llu   >500ms: %llu",
                  s.stalled_over_100ms, s.stalled_over_500ms);
      ImGui::Text("Worst age    : %llu ms (buf %#010x)",
                  s.worst_age_ms, s.worst_buf);
      char overlay[32];
      std::snprintf(overlay, sizeof(overlay), "%llu /s", s.swaps_per_sec);
      ImGui::PlotLines("##swapsHist", swaps_history_, kHistory, history_idx_,
                       overlay, 0.0f, 1000.0f, ImVec2(320, 60));
      if (s.stalled_over_500ms > 0) {
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                           "WARNING: %llu fibers stalled >500ms",
                           s.stalled_over_500ms);
      }
    }

    if (ImGui::CollapsingHeader("GPU", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Text("Draws/sec  : %.0f", rate_draws_);
      ImGui::Text("Stalls (1s): %lld", static_cast<long long>(g_win.accum_stalls));
      ImGui::Text("Verts/sec  : %.0f",
                  elapsed_ms ? (g_win.accum_verts * 1000.0 / elapsed_ms) : 0.0);
      char overlay[32];
      std::snprintf(overlay, sizeof(overlay), "%.0f /s", rate_draws_);
      ImGui::PlotLines("##drawHist", draws_history_, kHistory, history_idx_,
                       overlay, 0.0f, 0.0f, ImVec2(320, 50));
    }

    if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Text("XMA frames/sec : %.0f", rate_xma_);
      ImGui::Text("Buffer queue   : %lld",
                  static_cast<long long>(GetSnapshotCounter(CounterId::kBufferQueueDepth)));
      ImGui::Text("Frame latency  : %.2f ms",
                  GetSnapshotCounter(CounterId::kAudioFrameLatencyUs) / 1000.0);
      char overlay[32];
      std::snprintf(overlay, sizeof(overlay), "%.0f /s", rate_xma_);
      ImGui::PlotLines("##xmaHist", xma_history_, kHistory, history_idx_,
                       overlay, 0.0f, 0.0f, ImVec2(320, 50));
    }

    if (ImGui::CollapsingHeader("Dispatch / Threading")) {
      ImGui::Text("Functions dispatched/sec : %.0f", rate_dispatch_);
      ImGui::Text("Interrupt dispatches/sec : %.0f", rate_irq_);
      ImGui::Text("Active threads : %lld",
                  static_cast<long long>(GetSnapshotCounter(CounterId::kActiveThreads)));
      ImGui::Text("APC queue      : %lld",
                  static_cast<long long>(GetSnapshotCounter(CounterId::kApcQueueDepth)));
      ImGui::Text("CS contentions : %lld",
                  static_cast<long long>(GetSnapshotCounter(CounterId::kCriticalRegionContentions)));
    }

    if (ImGui::CollapsingHeader("Caches")) {
      auto tex_h = GetSnapshotCounter(CounterId::kTextureCacheHits);
      auto tex_m = GetSnapshotCounter(CounterId::kTextureCacheMisses);
      auto pip_h = GetSnapshotCounter(CounterId::kPipelineCacheHits);
      auto pip_m = GetSnapshotCounter(CounterId::kPipelineCacheMisses);
      ImGui::Text("Texture  hit/total : %lld / %lld", (long long)tex_h,
                  (long long)(tex_h + tex_m));
      ImGui::Text("Pipeline hit/total : %lld / %lld", (long long)pip_h,
                  (long long)(pip_h + pip_m));
    }

    // Online session timings (the frame hook's buckets of the last frame; Task 14). Offline: no section.
    if (doa4::np::Online() && ImGui::CollapsingHeader("Netplay", ImGuiTreeNodeFlags_DefaultOpen)) {
      const doa4::np::NetTiming t = doa4::np::LastTiming();
      ImGui::Text("Frame          : %d", static_cast<int>(t.frame));
      ImGui::Text("Delay          : %d frames", t.delay);
      ImGui::Text("Ping           : %.1f ms", t.ping_ms);
      ImGui::Text("Frames ahead   : %.2f", t.frames_ahead);
      ImGui::Text("Net            : %.0f us", t.net_us);
      ImGui::Text("Wait           : %.0f us", t.wait_us);
      ImGui::Text("Pacer          : %.0f us", t.pacer_us);
      ImGui::Text("Hook           : %.0f us", t.hook_us);
      ImGui::Text("Checksum       : %.0f us", t.hash_us);
      ImGui::Text("Holds          : %u", static_cast<unsigned>(t.holds));
    }
  }
  ImGui::End();
}

}  // namespace doa4

// Full-quality copies of the game's own snapshots. See doa4_photo_watch.h.

#include "doa4_photo_watch.h"

#include <format>
#include <utility>

namespace doa4::photo {

void SnapshotWatcher::OnSwap(uint32_t state) {
  const uint32_t n = ++swaps_;
  if (n % kDriftLogEvery == 0) {
    // The capture waits for "GPU swaps >= n": that only picks the right frame
    // while both counts start together. A gap that grows shows up here.
    hooks_.log(std::format("photo hook: guest swaps {}, gpu swaps {}", n, hooks_.gpu_swaps()));
  }
  if (hooks_.debug && state != last_state_) {
    hooks_.log(std::format("photo hook: snapshot state {} -> {} at guest swap {} (gpu swaps {})",
                           last_state_, state, n, hooks_.gpu_swaps()));
  }
  last_state_ = state;

  if (debug_next_ != 0 && n == debug_next_) {
    debug_next_ = 0;
    Image frame;
    if (WaitAndCapture(n, &frame)) hooks_.submit(std::move(frame), Source::kDebug, "n1");
  }

  if (!edge_.OnSwap(state)) return;
  const bool copy = hooks_.auto_enabled();
  if (!copy && !hooks_.debug) return;
  Image frame;
  if (!WaitAndCapture(n, &frame)) return;
  if (hooks_.debug) {
    hooks_.submit(copy ? frame : std::move(frame), Source::kDebug, "n");
    debug_next_ = n + 1;
  }
  if (copy) hooks_.submit(std::move(frame), Source::kSnapshot, "");
}

bool SnapshotWatcher::WaitAndCapture(uint32_t n, Image* image) {
  const uint64_t start = hooks_.now_ms();
  while (hooks_.gpu_swaps() < n) {
    if (hooks_.now_ms() - start > kWaitLimitMs) {
      hooks_.log(std::format("photo hook: timed out waiting for the GPU to reach swap {} "
                             "(gpu swaps {}); no copy of this snapshot",
                             n, hooks_.gpu_swaps()));
      return false;
    }
    hooks_.sleep();
  }
  if (!hooks_.capture(image)) {
    hooks_.log(std::format("photo hook: capture of swap {} failed", n));
    return false;
  }
  return true;
}

}  // namespace doa4::photo

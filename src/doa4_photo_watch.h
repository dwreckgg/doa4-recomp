// Full-quality copies of the game's own snapshots: what runs on the guest
// thread after each guest swap (the sub_8275AEE0 hook in
// doa4_photo_hook.cpp). No SDK headers: the app hands it the GPU's swap count,
// the frame capture and the service, so a host test drives it.
//
// At the swap where the game's snapshot state turns from 1 to 2 (the frame
// that shows the finished 30-frame photo), the guest thread is held until the
// GPU has processed that swap, so the captured frame is that one and not the
// next; then the frame goes to the service. The game is already in its slowed
// snapshot moment, so the short hold is not felt.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "doa4_photo.h"
#include "doa4_photo_service.h"

namespace doa4::photo {

class SnapshotWatcher {
 public:
  struct Hooks {
    std::function<uint32_t()> gpu_swaps;  // swaps the GPU thread has processed
    std::function<bool(Image*)> capture;  // the frame the GPU last produced
    std::function<bool()> auto_enabled;   // doa4_photo_auto
    std::function<void(Image, Source, std::string)> submit;  // to the service
    std::function<void(const std::string&)> log;
    std::function<void()> sleep;          // about 1 ms
    std::function<uint64_t()> now_ms;     // a steady clock
    bool debug = false;                   // DOA4_PHOTO_DEBUG=1
  };
  static constexpr uint64_t kWaitLimitMs = 500;
  static constexpr uint32_t kDriftLogEvery = 600;

  explicit SnapshotWatcher(Hooks hooks) : hooks_(std::move(hooks)) {}

  // After guest swap number n (1, 2, ...): `state` is the snapshot state read
  // just before that swap was issued.
  void OnSwap(uint32_t state);

  uint32_t swaps() const { return swaps_; }

 private:
  // Holds the calling thread until the GPU has processed swap n (or the wait
  // limit passes), then captures. False on timeout or capture failure.
  bool WaitAndCapture(uint32_t n, Image* image);

  Hooks hooks_;
  SnapshotEdge edge_;
  uint32_t swaps_ = 0;
  uint32_t last_state_ = 0;
  uint32_t debug_next_ = 0;  // swap whose frame the debug mode also saves (n + 1)
};

}  // namespace doa4::photo

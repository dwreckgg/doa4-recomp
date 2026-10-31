// DOA4 netplay phase 1: the event-list executor (spec 8), pure (no SDK, no GekkoNet, no Windows).
// Host-tested by tests/host/np_exec_host_test.cpp.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include "np_core.h"

namespace doa4::np::exec {

inline constexpr unsigned kPlayerInputBytes = 16;  // 12 guest XINPUT_GAMEPAD bytes + 4 reserved (zero), R3
void EncodePlayerInput(const Pad& pad, uint8_t out[kPlayerInputBytes]);
bool DecodePlayerInput(const uint8_t in[kPlayerInputBytes], Pad* out);  // false if a reserved byte is not zero

enum class Role : uint8_t { kFighter1 = 0, kFighter2 = 1, kFighter3 = 2, kFighter4 = 3, kSpectator = 255 };  // spec 8

struct SaveToken { int32_t frame = 0; uint32_t checksum = 0; };  // spec 8: phase 3 swaps the token for a snapshot handle

struct Event {  // converted from GekkoNet's game events (np_gekko)
  enum Kind : uint8_t { kLoad, kAdvance, kSave } kind = kAdvance;
  int32_t frame = 0;
  std::vector<uint8_t> inputs;   // kAdvance: num_players * kPlayerInputBytes
  uint32_t* checksum = nullptr;  // kSave: GekkoNet's slots, written by the NEXT call (deferred save)
  uint32_t* state_len = nullptr;
  uint8_t* state = nullptr;      // 8 bytes: the SaveToken
};

using ConfirmedTap = std::function<void(int32_t frame, const FrameInput& input)>;  // spec 8

enum class End : uint8_t { kNone, kInternal };
struct Step {
  bool advanced = false;  // an Advance arrived in this call
  int32_t frame = -1;
  FrameInput input;       // the record to stage for `frame`
  End end = End::kNone;
  std::string why;        // for kInternal
};

class Executor {
 public:
  // num_players 2 in phase 1; slots 0..num_players-1 are connected for the whole session (R3).
  explicit Executor(int num_players, ConfirmedTap tap);
  // One update's events. `checksum_entering` = the checksum of the state entering the frame about to run,
  // i.e. the state after the last advanced frame; it is written into the pending Save (if any) BEFORE these events
  // are looked at. Callers call Run({}, checksum_entering) BEFORE the first Update of each frame (an empty list
  // fills the pending Save and advances nothing): GekkoNet reads Save(f-1)'s checksum at the START of the update
  // for f (UpdateSession calls SendSessionHealthCheck before the Advance, game_session.cpp:203 and 365-385), so a
  // fill after that update comes one update late. Rules: Load -> kInternal ("load in lockstep"); an Advance whose
  // frame is not the next one, or a second Advance in one call -> kInternal; a Save for a frame not just advanced
  // (or a second Save for it) -> kInternal. The one exception: GekkoNet's first update of a started session begins
  // with a Save of frame -1 (game_session.cpp:11, 619-623; never read, :372-374); it is accepted and ignored as the
  // first event of the first call that holds any event, once, before any Advance. A refused list is refused whole:
  // nothing of it is staged or tapped. Once an end is returned every later call returns it.
  Step Run(std::span<const Event> events, uint32_t checksum_entering);
  // Lifetime: a pending Save holds the pointers GekkoNet handed out, into the state storage of its session; they are
  // valid only until that session is destroyed (Session::Destroy, or a new Create on the same object), and a Run after
  // that writes freed memory. The owner calls DropPending() BEFORE destroying the session (or destroys the Executor
  // first). It forgets the pending Save without writing it; the next Run writes nothing for it.
  void DropPending();
  uint32_t saves_filled() const;  // pending Saves filled so far ("checksums handed to GekkoNet", Task 12)
  int32_t next_frame() const;

 private:
  struct Pending {
    bool valid = false;
    int32_t frame = 0;
    uint32_t* checksum = nullptr;
    uint32_t* state_len = nullptr;
    uint8_t* state = nullptr;
  };
  int num_players_;
  ConfirmedTap tap_;
  int32_t next_ = 0;
  uint32_t filled_ = 0;
  bool start_save_seen_ = false;
  Pending pending_;
  Step ended_;
};

// The checksum GekkoNet compares: FNV-1a 64 over (frame as 4 little-endian bytes, every digest-row u64 as 8
// little-endian bytes, every sweep-row u64 likewise), folded to 32 bits as (h ^ (h >> 32)). R19.
uint32_t FoldChecksum(int32_t frame, std::span<const uint64_t> digest_row, std::span<const uint64_t> sweep_row);

}  // namespace doa4::np::exec

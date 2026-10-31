#include "np_exec.h"

#include <cstring>
#include <utility>

namespace doa4::np::exec {

static_assert(sizeof(SaveToken) == 8, "the token fills the 8 state bytes GekkoNet is configured with");

void EncodePlayerInput(const Pad& pad, uint8_t out[kPlayerInputBytes]) {
  PadToGuest(pad, out);
  std::memset(out + 12, 0, kPlayerInputBytes - 12);
}

bool DecodePlayerInput(const uint8_t in[kPlayerInputBytes], Pad* out) {
  for (unsigned i = 12; i < kPlayerInputBytes; ++i)
    if (in[i] != 0) return false;
  *out = PadFromGuest(in);
  return true;
}

Executor::Executor(int num_players, ConfirmedTap tap)
    : num_players_(num_players < 1 ? 1 : (num_players > kUsers ? kUsers : num_players)), tap_(std::move(tap)) {}

uint32_t Executor::saves_filled() const { return filled_; }
int32_t Executor::next_frame() const { return next_; }

namespace {
// The state GekkoNet keeps for a frame: the token, little-endian in both fields (8 bytes).
void EncodeToken(const SaveToken& t, uint8_t out[8]) {
  for (int i = 0; i < 4; ++i) {
    out[i] = uint8_t(uint32_t(t.frame) >> (8 * i));
    out[4 + i] = uint8_t(t.checksum >> (8 * i));
  }
}
}  // namespace

Step Executor::Run(std::span<const Event> events, uint32_t checksum_entering) {
  if (ended_.end != End::kNone) return ended_;
  // The deferred save: the slots of the Save handed out by an earlier call are written first, before any event of
  // this call is looked at (GekkoNet reads the checksum at the start of its next update).
  if (pending_.valid) {
    if (pending_.checksum) *pending_.checksum = checksum_entering;
    if (pending_.state_len) *pending_.state_len = sizeof(SaveToken);
    if (pending_.state) EncodeToken(SaveToken{pending_.frame, checksum_entering}, pending_.state);
    pending_.valid = false;
    ++filled_;
  }

  auto fail = [&](const char* why) {
    Step refused;
    refused.end = End::kInternal;
    refused.why = why;
    ended_ = refused;
    return refused;
  };

  // Look at the whole list before staging any of it: a refused list stages and taps nothing.
  const Event* advance = nullptr;
  const Event* save = nullptr;
  bool start_save = false;
  FrameInput record;
  for (size_t i = 0; i < events.size(); ++i) {
    const Event& e = events[i];
    switch (e.kind) {
      case Event::kLoad:
        return fail("load in lockstep");
      case Event::kAdvance: {
        if (advance) return fail("two advances in one update");
        if (e.frame != next_) return fail("advance out of order");
        if (e.inputs.size() != size_t(num_players_) * kPlayerInputBytes) return fail("advance input length");
        for (int p = 0; p < num_players_; ++p) {
          if (!DecodePlayerInput(e.inputs.data() + size_t(p) * kPlayerInputBytes, &record.pad[size_t(p)]))
            return fail("reserved input byte not zero");
          record.connected |= uint8_t(1u << p);  // R3: every slot of the session is connected, whatever it sent
        }
        advance = &e;
        break;
      }
      case Event::kSave: {
        if (i == 0 && e.frame == -1 && !start_save_seen_ && next_ == 0) {
          start_save = true;  // the library's start-up save: accepted, ignored, never read
          break;
        }
        if (!advance || e.frame != advance->frame) return fail("save for a frame not just advanced");
        if (save) return fail("two saves for one advance");
        save = &e;
        break;
      }
      default:
        return fail("unknown event kind");
    }
  }

  Step step;
  if (start_save) start_save_seen_ = true;
  if (advance) {
    step.advanced = true;
    step.frame = advance->frame;
    step.input = record;
    ++next_;
    if (tap_) tap_(step.frame, record);
  }
  if (save) {
    pending_.valid = true;
    pending_.frame = save->frame;
    pending_.checksum = save->checksum;
    pending_.state_len = save->state_len;
    pending_.state = save->state;
  }
  return step;
}

uint32_t FoldChecksum(int32_t frame, std::span<const uint64_t> digest_row, std::span<const uint64_t> sweep_row) {
  uint64_t h = 0xcbf29ce484222325ull;
  auto byte = [&](uint8_t b) { h = (h ^ b) * 0x100000001b3ull; };
  const uint32_t f = uint32_t(frame);
  for (int i = 0; i < 4; ++i) byte(uint8_t(f >> (8 * i)));
  for (uint64_t v : digest_row)
    for (int i = 0; i < 8; ++i) byte(uint8_t(v >> (8 * i)));
  for (uint64_t v : sweep_row)
    for (int i = 0; i < 8; ++i) byte(uint8_t(v >> (8 * i)));
  return uint32_t(h ^ (h >> 32));
}

}  // namespace doa4::np::exec

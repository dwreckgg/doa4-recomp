// src/net/np_upnp.h - map the host's UDP port on the router (DOA2U net_upnp.c), background thread.
// Requests run in order on one background thread (started when a request arrives), so the caller never waits on router
// traffic; Get() reads the last result under a mutex. Uses miniupnpc (third_party). While a leased mapping is held the
// thread stays and renews it at RenewDelayMs(lease) after it was made (Task 9 carry 2f); it ends when no request is
// left and nothing leased is held (after Unmap, or a failure, or a permanent mapping).
// Never exercised by the host tests: the router is the user's (plan 1b's real-world gate is its first run); only the
// pure schedule below is host-tested.
#pragma once
#include <cstdint>
#include <string>

namespace doa4::np::upnp {
enum class State : uint8_t { kIdle, kWorking, kOk, kFailed };
struct Result { State state = State::kIdle; std::string external_ip, lan_ip, message; };

inline constexpr uint32_t kLeaseSeconds = 3600;
// When a lease of lease_s seconds is renewed, in ms after it was made: at half its length, so one failed renewal
// still leaves time for the next attempt before it runs out; 0 (a permanent mapping, UPnP error 725's fallback) = never.
constexpr uint32_t RenewDelayMs(uint32_t lease_s) {
  return lease_s == 0 ? 0u : uint32_t((uint64_t(lease_s) * 1000u + 1u) / 2u);
}

void MapAsync(uint16_t port);  // lease kLeaseSeconds; removes an earlier mapping first
Result Get();
void Unmap();                  // background
void Shutdown(uint32_t max_ms);  // at exit: unmap, waiting at most max_ms (2000: the shutdown watchdog is 5 s)
}  // namespace doa4::np::upnp

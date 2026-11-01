// src/net/np_upnp.h - map the host's UDP port on the router (DOA2U net_upnp.c), background thread.
// Requests run in order on one background thread (started when a request arrives), so the caller never waits on router
// traffic; Get() reads the last result under a mutex. Uses miniupnpc (third_party). While a leased mapping is held the
// thread stays and renews it at RenewDelayMs(lease) after it was made (Task 9 carry 2f); it ends when no request is
// left and nothing leased is held (after Unmap, or a give-up, or a failure, or a permanent mapping).
// Renewal (final review R2-I1): AddPortMapping again with the same mapping (port, LAN address, description, lease) -
// never a delete first: a mapping is deleted only by Unmap (or when a map of ANOTHER port replaces it). A renewal that
// fails is tried again every kRetryMs, the last attempt at the lease's end (with a fresh router session after a failure);
// one that still fails there gives up: the mapping is counted gone, Result::state kFailed, Result::message says so and
// Result::lost counts it (the lobby turns it into an event line).
// Host-tested by tests/host/np_upnp_host_test.cpp through the router seam below (a fake router, short timings); the
// real router is the user's.
#pragma once
#include <cstdint>
#include <string>

namespace doa4::np::upnp {
enum class State : uint8_t { kIdle, kWorking, kOk, kFailed };
struct Result {
  State state = State::kIdle;
  std::string external_ip, lan_ip, message;
  uint32_t lost = 0;  // how many leased mappings ran out unrenewed in this process (each one: message says so)
};

inline constexpr uint32_t kLeaseSeconds = 3600;
inline constexpr uint32_t kRetryMs = 60000;  // a failed renewal is tried again after this (R2-I1)
// When a lease of lease_s seconds is renewed, in ms after it was made: at half its length, so a failed renewal still
// leaves time for the retries before it runs out; 0 (a permanent mapping, UPnP error 725's fallback) = never.
constexpr uint32_t RenewDelayMs(uint32_t lease_s) {
  return lease_s == 0 ? 0u : uint32_t((uint64_t(lease_s) * 1000u + 1u) / 2u);
}

void MapAsync(uint16_t port);  // lease kLeaseSeconds; a mapping of ANOTHER port held by this process is removed first
Result Get();
void Unmap();                  // background
void Shutdown(uint32_t max_ms);  // at exit: unmap, waiting at most max_ms (2000: the shutdown watchdog is 5 s)

// ---- the router seam (host tests) ----
namespace detail {
enum class Igd : uint8_t { kNoDevice, kNotIgd, kDown, kConnected, kPrivate };  // miniupnpc's UPNP_GetValidIGD verdicts
struct Router {
  virtual ~Router() = default;
  // Finds the gateway and opens a session on it (held until Forget): its verdict, this PC's LAN address and the
  // external address ("" when unknown).
  virtual Igd Discover(std::string* lan, std::string* external) = 0;
  // UPNP_AddPortMapping(port -> lan:port, UDP, the description, lease seconds as text, "0" = permanent): 0 = success,
  // else the UPnP error code (725 = only permanent leases).
  virtual int Add(uint16_t port, const std::string& lan, const char* lease) = 0;
  virtual void Delete(uint16_t port) = 0;  // UPNP_DeletePortMapping
  virtual void Forget() = 0;               // drops the session (FreeUPNPUrls)
};
struct Timing {
  uint32_t renew_ms = RenewDelayMs(kLeaseSeconds);  // after a mapping is made or renewed
  uint32_t retry_ms = kRetryMs;
  uint32_t lease_ms = kLeaseSeconds * 1000u;
};
// Only while no worker runs (between tests); nullptr = the miniupnpc router.
void SetRouterForTest(Router* router);
void SetTimingForTest(const Timing& timing);
}  // namespace detail
}  // namespace doa4::np::upnp

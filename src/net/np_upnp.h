// src/net/np_upnp.h - map the host's UDP port on the router (DOA2U net_upnp.c), background thread.
// Requests run in order on one background thread (started when a request arrives, ended when none is left), so the
// caller never waits on router traffic; Get() reads the last result under a mutex. Uses miniupnpc (third_party).
// Never exercised by the host tests: the router is the user's (plan 1b's real-world gate is its first run).
#pragma once
#include <cstdint>
#include <string>

namespace doa4::np::upnp {
enum class State : uint8_t { kIdle, kWorking, kOk, kFailed };
struct Result { State state = State::kIdle; std::string external_ip, lan_ip, message; };
void MapAsync(uint16_t port);  // lease 3600 s; removes an earlier mapping first
Result Get();
void Unmap();                  // background
void Shutdown(uint32_t max_ms);  // at exit: unmap, waiting at most max_ms (2000: the shutdown watchdog is 5 s)
}  // namespace doa4::np::upnp

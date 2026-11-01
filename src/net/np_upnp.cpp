// src/net/np_upnp.cpp - see np_upnp.h. Ported from DOA2U net_upnp.c (doa2u-release 6c35c7b, doa2u/src/online/), with:
// a lease of 3600 s instead of a permanent one (a crashed game leaves at most an hour-long mapping; a router that only
// takes permanent leases, UPnP error 725, gets one, as miniupnpc's upnpcommands.h lists); the job queue as one worker
// thread instead of one thread per request; Shutdown capped at max_ms (the 5 s shutdown watchdog, #0321); the IGD
// verdict kept in its own variable (DOA2U compared it after AddPortMapping had overwritten it); the renewal re-adds the
// same mapping and retries (final review R2-I1). The router calls go through detail::Router (the host test's fake);
// built with DOA4_UPNP_NO_MINIUPNPC (the host test) there is no miniupnpc router.
#include "np_upnp.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#ifndef DOA4_UPNP_NO_MINIUPNPC
#include "miniupnpc.h"
#include "upnpcommands.h"
#include "upnperrors.h"
#endif

namespace doa4::np::upnp {
namespace {

using Steady = std::chrono::steady_clock;
constexpr const char* kPermanent = "0";
constexpr int kOnlyPermanentLeases = 725;  // UPnP error OnlyPermanentLeasesSupported (upnpcommands.h)

#ifndef DOA4_UPNP_NO_MINIUPNPC
constexpr const char* kDescription = "DOA4 PC online play";

class MiniupnpcRouter final : public detail::Router {
 public:
  detail::Igd Discover(std::string* lan, std::string* external) override {
    Forget();
    int err = 0;
    UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &err);
    if (!devlist) return detail::Igd::kNoDevice;
    char l[64] = {0}, wan[64] = {0}, ext[64] = {0};
    const int igd = UPNP_GetValidIGD(devlist, &urls_, &data_, l, sizeof l, wan, sizeof wan);
    freeUPNPDevlist(devlist);
    *lan = l;
    if (igd != UPNP_CONNECTED_IGD && igd != UPNP_PRIVATEIP_IGD) {
      if (igd != UPNP_NO_IGD) FreeUPNPUrls(&urls_);  // set for every non-zero answer (miniupnpc.h)
      return igd == UPNP_NO_IGD ? detail::Igd::kNotIgd : detail::Igd::kDown;
    }
    have_ = true;
    if (UPNP_GetExternalIPAddress(urls_.controlURL, data_.first.servicetype, ext) != UPNPCOMMAND_SUCCESS) ext[0] = 0;
    if (!ext[0] && wan[0]) std::snprintf(ext, sizeof ext, "%s", wan);
    *external = ext;
    return igd == UPNP_PRIVATEIP_IGD ? detail::Igd::kPrivate : detail::Igd::kConnected;
  }
  int Add(uint16_t port, const std::string& lan, const char* lease) override {
    if (!have_) return -1;
    const std::string p = std::to_string(port);
    const int r = UPNP_AddPortMapping(urls_.controlURL, data_.first.servicetype, p.c_str(), p.c_str(), lan.c_str(), kDescription,
                                      "UDP", nullptr, lease);
    return r == UPNPCOMMAND_SUCCESS ? 0 : r;
  }
  void Delete(uint16_t port) override {
    if (!have_) return;
    const std::string p = std::to_string(port);
    UPNP_DeletePortMapping(urls_.controlURL, data_.first.servicetype, p.c_str(), "UDP", nullptr);
  }
  void Forget() override {
    if (have_) FreeUPNPUrls(&urls_);
    have_ = false;
  }

 private:
  UPNPUrls urls_{};
  IGDdatas data_{};
  bool have_ = false;
};
std::string ErrorText(int r) {
  const char* s = r > 0 ? strupnperror(r) : nullptr;
  return std::to_string(r) + ": " + (s ? s : "?");
}
#else
std::string ErrorText(int r) { return std::to_string(r); }
#endif

// Shared between the callers and the worker. Allocated once and never freed: a worker still waiting on a slow router
// when the process exits must not find this destroyed by static destructors.
struct Shared {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<uint16_t> jobs;  // 0 = unmap, else map that port
  bool worker_running = false;  // a worker thread exists; it ends when no job is left
  Result res;
  detail::Router* router = nullptr;  // the test's; else the miniupnpc one
  detail::Timing timing;
  // ---- worker only (one worker at a time: worker_running is set and cleared under mu) ----
  bool have_session = false;  // Discover succeeded and no Forget since
  std::string lan;
  bool mapped = false;
  bool leased = false;  // mapped with kLeaseSeconds (not the permanent fallback): renewed by the worker
  bool retrying = false;  // a renewal failed: the next attempt is at retry_due
  Steady::time_point mapped_at{}, lease_end{}, retry_due{};
  uint16_t mapped_port = 0;
};

Shared& S() {
  static Shared* s = new Shared;
  return *s;
}

detail::Router& R(Shared& s) {
#ifndef DOA4_UPNP_NO_MINIUPNPC
  static MiniupnpcRouter* real = new MiniupnpcRouter;
  if (!s.router) return *real;
#endif
  return *s.router;
}

void SetResult(State state, const std::string* ext, const std::string* lan, const std::string& msg, bool lost = false) {
  Shared& s = S();
  std::lock_guard<std::mutex> lock(s.mu);
  s.res.state = state;
  if (ext) s.res.external_ip = *ext;
  if (lan) s.res.lan_ip = *lan;
  s.res.message = msg;
  if (lost) ++s.res.lost;
}

void Forget(Shared& s) {
  R(s).Forget();
  s.have_session = false;
}

// Unmap (or a map of another port): the only place a mapping is deleted.
void DeleteMapping(Shared& s) {
  if (s.mapped && s.have_session) R(s).Delete(s.mapped_port);
  s.mapped = false;
  s.leased = false;
  s.retrying = false;
  Forget(s);
}

// A session on the gateway: "" or the failure message (the result is set by the caller).
std::string OpenSession(Shared& s, detail::Igd* igd, std::string* ext) {
  std::string lan;
  *igd = R(s).Discover(&lan, ext);
  s.lan = lan;
  switch (*igd) {
    case detail::Igd::kNoDevice:
      return "No UPnP router found. Forward the UDP port on your router by hand, or play over Tailscale/ZeroTier.";
    case detail::Igd::kNotIgd:
      R(s).Forget();
      return "A UPnP device answered but it is not an internet gateway. Forward the UDP port by hand, or play over "
             "Tailscale/ZeroTier.";
    case detail::Igd::kDown:
      R(s).Forget();
      return "The router's internet connection is down (UPnP).";
    default:
      s.have_session = true;
      return "";
  }
}

void DoMap(Shared& s, uint16_t want) {
  if (s.mapped && s.mapped_port != want) DeleteMapping(s);  // another port's mapping: replaced (not a renewal)
  detail::Igd igd = detail::Igd::kNoDevice;
  std::string ext;
  if (const std::string why = OpenSession(s, &igd, &ext); !why.empty()) {
    s.mapped = s.leased = s.retrying = false;
    SetResult(State::kFailed, nullptr, s.lan.empty() ? nullptr : &s.lan, why);
    return;
  }
  const std::string lease = std::to_string(kLeaseSeconds);
  int r = R(s).Add(want, s.lan, lease.c_str());  // never a delete first: an existing mapping of ours is simply refreshed
  const bool leased = r != kOnlyPermanentLeases;
  if (r == kOnlyPermanentLeases) r = R(s).Add(want, s.lan, kPermanent);
  const std::string port = std::to_string(want);
  if (r != 0) {
    s.mapped = s.leased = s.retrying = false;
    SetResult(State::kFailed, ext.empty() ? nullptr : &ext, &s.lan,
              "The router refused the port mapping (" + ErrorText(r) + "). Forward UDP " + port +
                  " by hand, or play over Tailscale/ZeroTier.");
    return;
  }
  s.mapped = true;
  s.leased = leased;
  s.retrying = false;
  s.mapped_at = Steady::now();
  s.lease_end = s.mapped_at + std::chrono::milliseconds(s.timing.lease_ms);
  s.mapped_port = want;
  if (igd == detail::Igd::kPrivate || ext.empty()) {
    SetResult(State::kOk, ext.empty() ? nullptr : &ext, &s.lan,
              "UDP " + port + " opened on the router, but it sits behind another router (no public address). Use the LAN "
                              "code, or Tailscale/ZeroTier.");
  } else {
    SetResult(State::kOk, &ext, &s.lan, "UDP " + port + " opened on the router via UPnP.");
  }
}

// R2-I1: the same mapping added again (no delete). A failure is retried every retry_ms, the last attempt at the lease's
// end; a failure at or after the lease's end gives up.
void Renew(Shared& s) {
  const std::string lease = std::to_string(kLeaseSeconds);
  int r = s.have_session ? R(s).Add(s.mapped_port, s.lan, lease.c_str()) : -1;
  if (r != 0) {  // the session may be stale (a router restart): a fresh one, then the same add
    Forget(s);
    detail::Igd igd;
    std::string ext;
    if (OpenSession(s, &igd, &ext).empty()) r = R(s).Add(s.mapped_port, s.lan, lease.c_str());
  }
  const auto now = Steady::now();
  if (r == 0) {
    s.retrying = false;
    s.mapped_at = now;
    s.lease_end = now + std::chrono::milliseconds(s.timing.lease_ms);
    return;
  }
  const std::string port = std::to_string(s.mapped_port);
  if (now >= s.lease_end) {
    s.mapped = s.leased = s.retrying = false;
    Forget(s);
    SetResult(State::kFailed, nullptr, nullptr,
              "The router's mapping of UDP " + port + " ran out and could not be renewed (" + ErrorText(r) +
                  "). Forward UDP " + port + " by hand, or play over Tailscale/ZeroTier.",
              /*lost=*/true);
    return;
  }
  s.retrying = true;
  s.retry_due = std::min(now + std::chrono::milliseconds(s.timing.retry_ms), s.lease_end);
}

void Worker() {
  WSADATA w;
  const bool wsa = WSAStartup(MAKEWORD(2, 2), &w) == 0;
  Shared& s = S();
  for (;;) {
    int job = -1;  // -1 = renew, 0 = unmap, else map that port
    {
      std::unique_lock<std::mutex> lock(s.mu);
      bool end = false;
      for (;;) {
        if (!s.jobs.empty()) {
          job = s.jobs.front();
          s.jobs.pop_front();
          break;
        }
        if (!(s.mapped && s.leased)) {  // nothing to renew: the worker ends
          s.worker_running = false;
          s.cv.notify_all();
          end = true;
          break;
        }
        // carry 2f: renew the lease before it runs out (a job arriving first is served first; Enqueue notifies)
        const auto due = s.retrying ? s.retry_due : s.mapped_at + std::chrono::milliseconds(s.timing.renew_ms);
        if (Steady::now() >= due) break;
        s.cv.wait_until(lock, due);
      }
      if (end) break;
    }
    if (job == 0) {
      DeleteMapping(s);
      std::lock_guard<std::mutex> lock(s.mu);
      const uint32_t lost = s.res.lost;
      s.res = Result{};
      s.res.lost = lost;
    } else if (job > 0) {
      DoMap(s, uint16_t(job));
    } else {
      Renew(s);
    }
  }
  if (wsa) WSACleanup();
}

void Enqueue(uint16_t job) {
  Shared& s = S();
  std::lock_guard<std::mutex> lock(s.mu);
  s.jobs.push_back(job);
  s.cv.notify_all();  // a worker waiting for a lease renewal takes the job now
  if (s.worker_running) return;
  try {
    std::thread(Worker).detach();
    s.worker_running = true;
  } catch (...) {
    s.jobs.pop_back();
    if (job != 0) {
      s.res.state = State::kFailed;
      s.res.message = "Could not start the UPnP thread.";
    }
  }
}

}  // namespace

void MapAsync(uint16_t port) {
  if (port == 0) return;
  SetResult(State::kWorking, nullptr, nullptr, "Asking the router to open the port (UPnP)...");
  Enqueue(port);
}

Result Get() {
  Shared& s = S();
  std::lock_guard<std::mutex> lock(s.mu);
  return s.res;
}

void Unmap() {
  Shared& s = S();
  {
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.res.state == State::kIdle && !s.worker_running) return;  // nothing mapped, nothing in flight
  }
  Enqueue(0);
}

void Shutdown(uint32_t max_ms) {
  Unmap();
  Shared& s = S();
  std::unique_lock<std::mutex> lock(s.mu);
  s.cv.wait_for(lock, std::chrono::milliseconds(max_ms), [&s] { return !s.worker_running; });
}

namespace detail {
void SetRouterForTest(Router* router) {
  std::lock_guard<std::mutex> lock(S().mu);
  S().router = router;
}
void SetTimingForTest(const Timing& timing) {
  std::lock_guard<std::mutex> lock(S().mu);
  S().timing = timing;
}
}  // namespace detail

}  // namespace doa4::np::upnp

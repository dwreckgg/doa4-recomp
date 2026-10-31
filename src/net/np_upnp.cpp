// src/net/np_upnp.cpp - see np_upnp.h. Ported from DOA2U net_upnp.c (doa2u-release 6c35c7b, doa2u/src/online/), with:
// a lease of 3600 s instead of a permanent one (a crashed game leaves at most an hour-long mapping; a router that only
// takes permanent leases, UPnP error 725, gets one, as miniupnpc's upnpcommands.h lists); the job queue as one worker
// thread instead of one thread per request; Shutdown capped at max_ms (the 5 s shutdown watchdog, #0321); the IGD
// verdict kept in its own variable (DOA2U compared it after AddPortMapping had overwritten it).
#include "np_upnp.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "miniupnpc.h"
#include "upnpcommands.h"
#include "upnperrors.h"

namespace doa4::np::upnp {
namespace {

constexpr const char* kLease = "3600";
constexpr const char* kPermanent = "0";
constexpr int kOnlyPermanentLeases = 725;  // UPnP error OnlyPermanentLeasesSupported (upnpcommands.h)
constexpr const char* kDescription = "DOA4 PC online play";

struct Job {
  bool unmap = false;
  uint16_t port = 0;
};

// Shared between the callers and the worker. Allocated once and never freed: a worker still waiting on a slow router
// when the process exits must not find this destroyed by static destructors.
struct Shared {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Job> jobs;
  bool worker_running = false;  // a worker thread exists; it ends when no job is left
  Result res;
  // ---- worker only (one worker at a time: worker_running is set and cleared under mu) ----
  UPNPUrls urls{};
  IGDdatas data{};
  bool have_urls = false;
  bool mapped = false;
  uint16_t mapped_port = 0;
};

Shared& S() {
  static Shared* s = new Shared;
  return *s;
}

void SetResult(State state, const char* ext, const char* lan, const std::string& msg) {
  Shared& s = S();
  std::lock_guard<std::mutex> lock(s.mu);
  s.res.state = state;
  if (ext) s.res.external_ip = ext;
  if (lan) s.res.lan_ip = lan;
  s.res.message = msg;
}

void DeleteMapping(Shared& s) {
  if (s.mapped && s.have_urls) {
    const std::string port = std::to_string(s.mapped_port);
    UPNP_DeletePortMapping(s.urls.controlURL, s.data.first.servicetype, port.c_str(), "UDP", nullptr);
  }
  s.mapped = false;
  if (s.have_urls) {
    FreeUPNPUrls(&s.urls);
    s.have_urls = false;
  }
}

void DoMap(Shared& s, uint16_t want) {
  DeleteMapping(s);
  int err = 0;
  UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &err);
  if (!devlist) {
    SetResult(State::kFailed, nullptr, nullptr,
              "No UPnP router found. Forward the UDP port on your router by hand, or play over Tailscale/ZeroTier.");
    return;
  }
  char lan[64] = {0}, wan[64] = {0}, ext[64] = {0};
  const int igd = UPNP_GetValidIGD(devlist, &s.urls, &s.data, lan, sizeof lan, wan, sizeof wan);
  freeUPNPDevlist(devlist);
  if (igd != UPNP_CONNECTED_IGD && igd != UPNP_PRIVATEIP_IGD) {
    if (igd != UPNP_NO_IGD) FreeUPNPUrls(&s.urls);  // set for every non-zero answer (miniupnpc.h)
    SetResult(State::kFailed, nullptr, lan[0] ? lan : nullptr,
              igd == UPNP_NO_IGD ? "A UPnP device answered but it is not an internet gateway. Forward the UDP port by hand, or play "
                                   "over Tailscale/ZeroTier."
                                 : "The router's internet connection is down (UPnP).");
    return;
  }
  s.have_urls = true;
  if (UPNP_GetExternalIPAddress(s.urls.controlURL, s.data.first.servicetype, ext) != UPNPCOMMAND_SUCCESS) ext[0] = 0;
  if (!ext[0] && wan[0]) std::snprintf(ext, sizeof ext, "%s", wan);
  const std::string port = std::to_string(want);
  int r = UPNP_AddPortMapping(s.urls.controlURL, s.data.first.servicetype, port.c_str(), port.c_str(), lan, kDescription, "UDP",
                              nullptr, kLease);
  if (r == kOnlyPermanentLeases)
    r = UPNP_AddPortMapping(s.urls.controlURL, s.data.first.servicetype, port.c_str(), port.c_str(), lan, kDescription, "UDP",
                            nullptr, kPermanent);
  if (r != UPNPCOMMAND_SUCCESS) {
    char msg[200];
    std::snprintf(msg, sizeof msg,
                  "The router refused the port mapping (%d: %s). Forward UDP %s by hand, or play over Tailscale/ZeroTier.", r,
                  strupnperror(r) ? strupnperror(r) : "?", port.c_str());
    SetResult(State::kFailed, ext[0] ? ext : nullptr, lan, msg);
    return;
  }
  s.mapped = true;
  s.mapped_port = want;
  char msg[200];
  if (igd == UPNP_PRIVATEIP_IGD || !ext[0]) {
    std::snprintf(msg, sizeof msg,
                  "UDP %s opened on the router, but it sits behind another router (no public address). Use the LAN code, or "
                  "Tailscale/ZeroTier.",
                  port.c_str());
    SetResult(State::kOk, ext[0] ? ext : nullptr, lan, msg);
  } else {
    std::snprintf(msg, sizeof msg, "UDP %s opened on the router via UPnP.", port.c_str());
    SetResult(State::kOk, ext, lan, msg);
  }
}

void Worker() {
  WSADATA w;
  const bool wsa = WSAStartup(MAKEWORD(2, 2), &w) == 0;
  Shared& s = S();
  for (;;) {
    Job j;
    {
      std::lock_guard<std::mutex> lock(s.mu);
      if (s.jobs.empty()) {
        s.worker_running = false;
        s.cv.notify_all();
        break;
      }
      j = s.jobs.front();
      s.jobs.pop_front();
    }
    if (j.unmap) {
      DeleteMapping(s);
      SetResult(State::kIdle, "", "", "");
    } else {
      DoMap(s, j.port);
    }
  }
  if (wsa) WSACleanup();
}

void Enqueue(const Job& j) {
  Shared& s = S();
  std::lock_guard<std::mutex> lock(s.mu);
  s.jobs.push_back(j);
  if (s.worker_running) return;
  try {
    std::thread(Worker).detach();
    s.worker_running = true;
  } catch (...) {
    s.jobs.pop_back();
    if (!j.unmap) {
      s.res.state = State::kFailed;
      s.res.message = "Could not start the UPnP thread.";
    }
  }
}

}  // namespace

void MapAsync(uint16_t port) {
  SetResult(State::kWorking, "", "", "Asking the router to open the port (UPnP)...");
  Enqueue(Job{false, port});
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
  Enqueue(Job{true, 0});
}

void Shutdown(uint32_t max_ms) {
  Unmap();
  Shared& s = S();
  std::unique_lock<std::mutex> lock(s.mu);
  s.cv.wait_for(lock, std::chrono::milliseconds(max_ms), [&s] { return !s.worker_running; });
}

}  // namespace doa4::np::upnp

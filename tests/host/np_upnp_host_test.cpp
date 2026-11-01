// Host test for src/net/np_upnp (final review R2-I1: the lease renewal). np_upnp.cpp is built with
// DOA4_UPNP_NO_MINIUPNPC and drives a fake router (detail::Router) with short timings (detail::Timing), so nothing
// reaches the network. Built and run by run_np_upnp_test.bat. Real time, bounded waits.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/net/np_upnp.h"

using namespace doa4::np;
using Clock = std::chrono::steady_clock;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)
#define CHECKF(cond, ...)                                           \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                     \
      std::printf("\n");                                            \
      ++g_failures;                                                 \
    }                                                               \
  } while (0)

static int64_t Ms(Clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}
template <class F>
static bool WaitFor(F f, int ms) {
  const auto t0 = Clock::now();
  while (Ms(t0) < ms) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return f();
}

// A router: every call recorded with its time; Add answers from a script (then `add_default`).
struct FakeRouter final : upnp::detail::Router {
  struct AddCall { uint16_t port; std::string lan, lease; int64_t t; };
  std::mutex mu;
  Clock::time_point t0 = Clock::now();
  std::vector<AddCall> adds;
  std::vector<uint16_t> deletes;
  int discovers = 0, forgets = 0;
  std::deque<int> add_script;
  int add_default = 0;
  upnp::detail::Igd Discover(std::string* lan, std::string* ext) override {
    std::lock_guard<std::mutex> l(mu);
    ++discovers;
    *lan = "192.168.1.20";
    *ext = "203.0.113.9";
    return upnp::detail::Igd::kConnected;
  }
  int Add(uint16_t port, const std::string& lan, const char* lease) override {
    std::lock_guard<std::mutex> l(mu);
    adds.push_back({port, lan, lease, Ms(t0)});
    if (!add_script.empty()) {
      const int r = add_script.front();
      add_script.pop_front();
      return r;
    }
    return add_default;
  }
  void Delete(uint16_t port) override {
    std::lock_guard<std::mutex> l(mu);
    deletes.push_back(port);
  }
  void Forget() override {
    std::lock_guard<std::mutex> l(mu);
    ++forgets;
  }
  size_t AddCount() {
    std::lock_guard<std::mutex> l(mu);
    return adds.size();
  }
  size_t DeleteCount() {
    std::lock_guard<std::mutex> l(mu);
    return deletes.size();
  }
};

static upnp::detail::Timing Fast() {
  upnp::detail::Timing t;
  t.renew_ms = 150;
  t.retry_ms = 60;
  t.lease_ms = 400;
  return t;
}

// Every test: a fresh fake, no worker running (Shutdown unmaps and waits for the worker to end).
static void Reset(FakeRouter* r) {
  upnp::Shutdown(2000);
  upnp::detail::SetRouterForTest(r);
  upnp::detail::SetTimingForTest(Fast());
}

static void TestRenewal() {
  std::printf("group: the renewal adds the same mapping again and never deletes it\n");
  FakeRouter r;
  Reset(&r);
  upnp::MapAsync(7200);
  CHECK(WaitFor([&] { return upnp::Get().state == upnp::State::kOk; }, 1000));
  CHECK(WaitFor([&] { return r.AddCount() >= 3; }, 1000));  // the map and two renewals
  upnp::Unmap();
  CHECK(WaitFor([&] { return r.DeleteCount() == 1; }, 1000));
  std::lock_guard<std::mutex> l(r.mu);
  bool same = true;
  for (const auto& a : r.adds) same = same && a.port == 7200 && a.lan == "192.168.1.20" && a.lease == "3600";
  CHECK(same);
  CHECKF(r.adds.size() >= 3 && r.adds[1].t - r.adds[0].t >= 130 && r.adds[2].t - r.adds[1].t >= 130,
         "renewals at %lld / %lld ms after the map", r.adds.size() > 1 ? (long long)(r.adds[1].t - r.adds[0].t) : -1LL,
         r.adds.size() > 2 ? (long long)(r.adds[2].t - r.adds[0].t) : -1LL);
  CHECKF(r.discovers == 1, "%d router sessions opened (a successful renewal keeps the session)", r.discovers);
  // the only delete is Unmap's, after every add
  CHECK(r.deletes.size() == 1 && r.deletes[0] == 7200);
}

static void TestRenewalGivesUp() {
  std::printf("group: a failed renewal is retried until the lease ends, then gives up with lost + message\n");
  FakeRouter r;
  Reset(&r);
  const uint32_t lost0 = upnp::Get().lost;
  upnp::MapAsync(7200);
  CHECK(WaitFor([&] { return upnp::Get().state == upnp::State::kOk; }, 1000));
  const auto t_map = Clock::now();
  {
    std::lock_guard<std::mutex> l(r.mu);
    r.add_default = 501;  // every renewal fails from here on (501: Action Failed)
  }
  CHECK(WaitFor([&] { return upnp::Get().state == upnp::State::kFailed; }, 2000));
  const int64_t gave_up_ms = Ms(t_map);
  const upnp::Result res = upnp::Get();
  CHECKF(res.lost == lost0 + 1, "lost %u -> %u", lost0, res.lost);
  CHECKF(res.message.find("UDP 7200 ran out and could not be renewed") != std::string::npos, "message '%s'", res.message.c_str());
  CHECKF(gave_up_ms >= 380 && gave_up_ms < 900, "gave up %lld ms after the map (lease 400 ms)", (long long)gave_up_ms);
  {
    std::lock_guard<std::mutex> l(r.mu);
    // attempts: at 150, then every 60 ms, the last at the lease's end (400): 150 210 270 330 390 400 -> 6 attempts,
    // each the held session's add plus a fresh session's add
    const size_t attempts = (r.adds.size() - 1) / 2;
    CHECKF(attempts >= 5 && attempts <= 7, "%zu renewal attempts (%zu adds)", attempts, r.adds.size());
    CHECKF(r.deletes.empty(), "%zu deletes during the renewals", r.deletes.size());
    bool spaced = true;  // every retry at least ~retry_ms after the previous attempt (the last may be the lease's end)
    for (size_t i = 3; i + 2 < r.adds.size(); i += 2) spaced = spaced && r.adds[i + 2].t - r.adds[i].t >= 5;
    CHECK(spaced);
  }
  // given up: the worker has ended, nothing more is tried
  const size_t n = r.AddCount();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK(r.AddCount() == n);
  CHECK(upnp::Get().lost == lost0 + 1);
}

static void TestRenewalRecovers() {
  std::printf("group: a renewal that fails once and then works keeps the mapping\n");
  FakeRouter r;
  Reset(&r);
  const uint32_t lost0 = upnp::Get().lost;
  upnp::MapAsync(7201);
  CHECK(WaitFor([&] { return upnp::Get().state == upnp::State::kOk; }, 1000));
  {
    std::lock_guard<std::mutex> l(r.mu);
    r.add_script = {501, 501};  // the first renewal attempt: both of its adds fail
  }
  CHECK(WaitFor([&] { return r.AddCount() >= 5; }, 1500));  // map, failed attempt (2), retry, the next renewal
  std::this_thread::sleep_for(std::chrono::milliseconds(500));  // past the first lease's end
  const upnp::Result res = upnp::Get();
  CHECKF(res.state == upnp::State::kOk && res.lost == lost0, "state %d lost %u", int(res.state), res.lost);
  CHECK(r.DeleteCount() == 0);
  upnp::Shutdown(2000);
  CHECK(r.DeleteCount() == 1);
}

static void TestOtherPortAndPermanent() {
  std::printf("group: a map of another port replaces the old mapping; a permanent mapping is never renewed\n");
  FakeRouter r;
  Reset(&r);
  upnp::MapAsync(7200);
  CHECK(WaitFor([&] { return upnp::Get().state == upnp::State::kOk; }, 1000));
  upnp::MapAsync(7202);
  CHECK(WaitFor([&] { return r.DeleteCount() == 1; }, 1000));
  {
    std::lock_guard<std::mutex> l(r.mu);
    CHECK(r.deletes[0] == 7200);
  }
  FakeRouter p;
  Reset(&p);
  {
    std::lock_guard<std::mutex> l(p.mu);
    p.add_script = {725};  // only permanent leases
  }
  upnp::MapAsync(7203);
  CHECK(WaitFor([&] { return upnp::Get().state == upnp::State::kOk; }, 1000));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  std::lock_guard<std::mutex> l(p.mu);
  CHECKF(p.adds.size() == 2 && p.adds[1].lease == "0", "%zu adds", p.adds.size());
}

int main() {
  const auto t0 = Clock::now();
  TestRenewal();
  TestRenewalGivesUp();
  TestRenewalRecovers();
  TestOtherPortAndPermanent();
  FakeRouter last;
  Reset(&last);
  std::printf("info: run time %lld ms\n", (long long)Ms(t0));
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  std::fflush(stdout);
  return g_failures ? 1 : 0;
}

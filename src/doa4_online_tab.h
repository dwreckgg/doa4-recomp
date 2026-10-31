// The Online tab of the Esc menu (netplay phase 1, Task 14): draws what doa4_online_view decides and calls the app's
// lobby callbacks. Ported in layout from DOA2U doa2u_ui.cpp 893-1014: status, event, your code + Copy, join field +
// Join + Host + Last code, reach out, Start / Leave / Disconnect, input delay, the F9 box, version and build id, "Open
// session logs". No decision lives here that the view could make (the view is host-tested).

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "doa4_online_view.h"

namespace doa4 {

// The app's side of the tab (MenuHost::online). Every callback runs on the UI thread. Empty `state` = no netplay.
struct OnlineHost {
  std::function<online_view::In()> state;                    // a snapshot of the lobby / session, the settings
  std::function<void(const std::string& code)> join;         // creates the lobby when none exists, then joins
  std::function<void()> host;                                // creates the lobby when none exists, then hosts
  std::function<void(const std::string& code)> reach_out;
  std::function<void()> disconnect;
  std::function<void()> start_session;
  std::function<void()> leave_session;
  std::function<void(bool automatic, int frames)> set_delay;
  std::function<void()> shown_changed;                       // the F9 setting changed (the overlay follows it)
};

class OnlineTab {
 public:
  // `save_then_set(name, value, quote)`: the menu's SaveThenSet (saves to the toml first, then sets the cvar; false =
  // failed, the menu's status line says why). `open_logs`: closes the menu and opens <user data>/netplay/sessions.
  void Draw(const OnlineHost& host, const online_view::In& in, const online_view::View& view,
            const std::function<bool(std::string_view, std::string_view, bool)>& save_then_set,
            const std::function<void()>& open_logs);

 private:
  char join_[64] = {};
  char reach_[64] = {};
  bool join_seeded_ = false;  // the join field starts from the last code once
};

}  // namespace doa4

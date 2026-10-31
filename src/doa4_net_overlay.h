// The online banner ("Waiting for the other player...", the end reasons) and the F9 ping / input-delay box
// (netplay phase 1, Task 14). An ImGuiDialog that exists only while one of them is shown: while any ImGui dialog
// exists the presenter paints from the UI thread (doa4_photo_notice.h), so offline play never pays for it.
// The app creates / destroys it (UpdateNetOverlay); what it shows is decided by doa4_online_view.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

namespace doa4 {

class NetOverlay : public rex::ui::ImGuiDialog {
 public:
  struct Content {
    std::string banner;                  // "" = none
    std::vector<std::string> stats;      // the F9 box lines; empty = no box
  };
  // `poll` runs on the UI thread every frame the overlay draws.
  NetOverlay(rex::ui::ImGuiDrawer* drawer, std::function<Content()> poll);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::function<Content()> poll_;
};

}  // namespace doa4

// Diagnostic ImGui overlay for DOA4 — fiber scheduler health, audio, etc.
// Installed from DeadOrAlive4App::OnCreateDialogs.

#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

namespace doa4 {

class DiagOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit DiagOverlayDialog(rex::ui::ImGuiDrawer* drawer);
  ~DiagOverlayDialog() = default;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  static constexpr int kHistory = 120;  // ~2 minutes at 1 Hz sample rate
  float swaps_history_[kHistory] = {};
  float draws_history_[kHistory] = {};
  float xma_history_[kHistory] = {};
  int   history_idx_ = 0;
  unsigned long long last_seen_total_ = 0;

  // Per-second rates of SDK perf counters, updated every ~1s in OnDraw.
  float rate_draws_ = 0;
  float rate_xma_ = 0;
  float rate_dispatch_ = 0;
  float rate_irq_ = 0;
};

}  // namespace doa4

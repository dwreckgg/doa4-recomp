// In-game menu, opened and closed with Escape (bind_doa4_menu).
//
// Tabs: Video (display mode, render scale), Audio (speakers), Controls (per-slot controller
// remap, keyboard binds), Photo, Extras (unlocks, game tweaks), Online (hosting and joining a friend) and Game (where
// the game files are). Every setting is
// applied to its cvar and saved to dead_or_alive_4.toml with doa4::SaveTomlKey. The game keeps running while
// the menu is open; its input is blocked (DeadOrAlive4App::OnBlocksGuestInput),
// so the game sees an untouched controller and keyboard.
//
// Owned by DeadOrAlive4App: created when opened, destroyed when closed, so the
// presenter goes back to direct GPU-thread presents as soon as it closes.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include <rex/input/input.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window_listener.h>

#include "doa4_extras.h"
#include "doa4_guest.h"
#include "doa4_online_tab.h"
#include "doa4_online_view.h"
#include "doa4_photo_service.h"
#include "doa4_tweaks.h"

namespace rex::input {
class InputSystem;
}
namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace doa4 {

struct MenuHost {
  rex::ui::Window* window = nullptr;
  std::filesystem::path config_path;  // <exe dir>/dead_or_alive_4.toml
  int running_scale = 1;              // resolution_scale the game started with
  // resolution_scale saved for the next start; owned by the app so it survives
  // the menu being closed and reopened. Changed only after a successful save.
  int* saved_scale = nullptr;
  // Audio tab: audio_speakers as the game started with (the audio driver reads
  // it once, when it opens), and as last saved for the next start (owned by the
  // app like saved_scale; changed only after a successful save).
  std::string running_speakers = "auto";
  std::string* saved_speakers = nullptr;
  // The runtime's input system, or null (before the runtime exists).
  std::function<rex::input::InputSystem*()> input;
  // Runs `fn` on the UI thread after the current frame: window changes and
  // closing the menu must not happen while the menu is drawing.
  std::function<void(std::function<void()> fn)> defer;
  // Destroys the menu (called through `defer`).
  std::function<void()> close_menu;
  // Switches the running game's anti-aliasing to graphics::kAntiAliasing[index]
  // (the SDK reads swap_post_effect only at start).
  std::function<void(int index)> set_post_effect;
  // Photo tab: the photo service's last result, and the folder photos go to
  // when doa4_photo_dir is empty.
  std::function<photo::Result()> photo_result;
  std::filesystem::path photo_default_dir;

  // Extras tab. `guest` is null until the XEX is loaded into guest memory.
  const GuestBytes* guest = nullptr;
  tweaks::TweakPatcher* patcher = nullptr;  // owned by the app
  tweaks::ProtectFn protect;
  std::filesystem::path save_dir;  // the ups.dat save package folder, or empty
  // Where the one-time save backup goes: outside the folder the game lists
  // its saves from.
  std::filesystem::path backup_root;
  ExtrasState* extras = nullptr;   // owned by the app: survives closing the menu
  // Netplay (Task 12 fix round I3): asked before an Extras action (an unlock, a tweak toggle); true = refused (in an
  // online session: *status says why, a log line is written). Empty: never refused.
  std::function<bool(std::string_view action, std::string* status)> refuse;
  // Netplay (Task 14): true while this process runs an online session or its lobby is starting one
  // (online_view::SessionLocks). Four controls read their disabled state from online_view::LockedInSession with it
  // (Extras, the Video and Audio "Restart now", the Game tab's "Change..."), and the refusal behind them reads the same
  // answer. Empty: never locked.
  std::function<bool()> session_locked;
  OnlineHost online;  // the Online tab's callbacks; empty `state` = the tab says it is unavailable
  // Game tab: the folder the game started from, whether --game_data_root
  // chose it, and the action that closes the menu and opens the setup screen
  // to choose another (used from the next start).
  std::filesystem::path game_files;
  bool game_files_from_command_line = false;
  std::filesystem::path user_data;  // saves and the shader cache (shown only)
  std::function<void()> change_game_files;
};

class MenuDialog : public rex::ui::ImGuiDialog, public rex::ui::WindowInputListener {
 public:
  // Input listener z-order: above the ImGui drawer (64), so a key or click
  // that answers "press a key" never reaches ImGui, the binds or the game.
  static constexpr size_t kInputZOrder = 65;

  MenuDialog(rex::ui::ImGuiDrawer* drawer, MenuHost host);
  ~MenuDialog() override;

  // Logs the Online tab's state and the session locks if they changed (the same call OnDraw makes every frame). The
  // app calls it from a timer when DOA4_MENU_OPEN=1: a minimized window never paints, so OnDraw alone would not log.
  void PollNetState();

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;

 private:
  enum class Listen { kNone, kPad, kKey };

  void DrawVideoTab();
  void DrawAudioTab();
  void DrawControlsTab();
  void DrawPadSection();
  void DrawKeyboardSection();
  void DrawExtrasTab();
  void DrawPhotoTab();
  void DrawOnlineTab();
  void DrawGameTab();
  bool SessionLocked() const;
  bool Locked(online_view::Control control) const;  // LockedInSession(control, SessionLocked())
  // Logs the Online tab's state and the locks once per change (every frame the menu draws, whichever tab is shown).
  void LogNetState(const online_view::In& in, const online_view::View& view);
  // Saves the tweak's cvar; on success sets it and, for a live tweak, writes
  // or reverts its bytes. Failures go to status_.
  void SetTweak(size_t tweak, bool on);
  // Saves `name` first; only on success sets the cvar. Failures go to
  // status_ and leave the running game as it was. Returns whether it saved.
  bool SaveThenSet(std::string_view name, std::string_view value, bool quote);
  void StartPadListen(size_t row, const rex::input::X_INPUT_GAMEPAD* pad);
  void PollPadListen(const rex::input::X_INPUT_GAMEPAD& pad);
  void AssignPad(size_t target, size_t source);
  void AssignKey(size_t row, const std::string& key_name);
  // `control` is the Restart now button's own (kVideoRestart / kAudioRestart): refused in an online session.
  void Restart(online_view::Control control);
  // Sets the cvar and saves it; `quote` writes the value as a TOML string.
  // Failures go to status_.
  void SetAndSave(std::string_view name, std::string_view value, bool quote);

  MenuHost host_;
  int slot_ = 0;  // controller slot shown in the Controls tab
  Listen listen_ = Listen::kNone;
  size_t listen_row_ = 0;
  uint32_t pad_held_ = 0;  // PadControl bits held since pad listening started
  bool pad_connected_ = false;  // last non-busy GetRawState answer for slot_
  double next_save_poll_ = 0.0;  // ImGui time of the next "has the game saved?" check
  std::string status_;     // last error, shown in red
  OnlineTab online_tab_;
  std::string logged_tab_line_;  // the last "[doa4][NET] tab: ..." line written
  bool logged_locks_ = false;    // the "menu: locked in session" line was written
};

}  // namespace doa4

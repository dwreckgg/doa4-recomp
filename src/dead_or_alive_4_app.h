// dead_or_alive_4 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window_win.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xthread.h>

#include "doa4_config.h"
#include "doa4_diag_overlay.h"
#include "doa4_extras.h"
#include "doa4_gamefiles.h"
#include "doa4_graphics.h"
#include "doa4_lowmem.h"
#include "doa4_net_overlay.h"
#include "doa4_online_view.h"
#include "doa4_photo_hook.h"
#include "doa4_photo_notice.h"
#include "doa4_photo_service.h"
#include "doa4_photo_watch.h"
#include "doa4_photo_win.h"
#include "doa4_menu.h"
#include "doa4_setup_dialog.h"
#include "doa4_tweaks.h"
#include "doa4_unlocks.h"
#include "doa4_updater.h"
#include "doa4_utf8.h"
#include "net/np_files.h"
#include "net/np_handoff.h"
#include "net/np_online.h"
#include "net/np_restart.h"
#include "net/np_session.h"
#include "net/np_transport.h"
#include "net/np_upnp.h"

REXCVAR_DECLARE(bool, log_noisy);

extern "C" void doa4_arm_shutdown_watchdog();       // doa4_app_hooks.cpp
extern "C" void doa4_wait_for_previous_instance();  // doa4_app_hooks.cpp

class DeadOrAlive4App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DeadOrAlive4App>(new DeadOrAlive4App(ctx, "dead_or_alive_4",
        PPCImageConfig));
  }

  // Where the game files are: --game_data_root, else the folder the
  // first-run setup saved (doa4_game_files), else the setup screen, which
  // resumes start-up once the user has chosen. The SDK calls this after the
  // window, the ImGui drawer and the toml exist, before the runtime. See
  // notes/specs/2026-10-28-first-run-setup-design.md.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& sdk_defaults, std::function<void(rex::PathConfig)> resume) override {
    const rex::PathConfig defaults = WithUserData(sdk_defaults);
    if (launch_refused_) {  // fix round I1: a session start that failed after a host setting was applied
      REXLOG_WARN("[doa4][NET] the runtime is not built: this process quits (the offline relaunch runs instead)");
      app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); });
      return std::nullopt;
    }
    const std::filesystem::path saved =
        doa4::PathFromUtf8(rex::cvar::Query<std::string>("doa4_game_files"));
    const doa4::gamefiles::StartChoice choice =
        doa4::gamefiles::ChooseStartPath(defaults.game_data_root, saved, doa4::gamefiles::HasXex,
                                         doa4::gamefiles::PathFitsCodePage);
    if (!choice.path.empty()) {
      game_files_ = choice.path;
      game_files_from_command_line_ = choice.from_command_line;
      REXLOG_INFO("[doa4][SETUP] game files: {} ({})", doa4::Utf8(choice.path),
                  choice.from_command_line ? "--game_data_root" : "saved by the setup");
      rex::PathConfig paths = defaults;
      paths.game_data_root = choice.path;
      return paths;
    }
    REXLOG_INFO("[doa4][SETUP] setup screen: {}",
                choice.reason.empty() ? std::string("no game files configured") : choice.reason);
    if (!drawer_) return defaults;  // no window to ask in: the SDK reports the missing path
    OpenSetup(doa4::SetupHost::Mode::kFirstRun, choice.reason,
              [this, defaults, resume](const std::filesystem::path& folder) {
                std::string error = SaveGameFiles(folder);
                game_files_ = folder;
                rex::PathConfig paths = defaults;
                paths.game_data_root = folder;
                app_context().CallInUIThreadDeferred([this, paths, resume] {
                  CloseSetup();
                  resume(paths);
                });
                return error;
              });
    return std::nullopt;
  }

  // The save and shader-cache folder: --user_data_root, else doa4_user_data,
  // else the SDK's default (Documents\dead_or_alive_4). The SDK reads its own
  // user_data_root before the toml is loaded, so a saved folder can only be
  // applied here; the app's own readers use UserData(), never the base
  // class's user_data_root(), which keeps the pre-hook value.
  rex::PathConfig WithUserData(const rex::PathConfig& sdk) {
    rex::PathConfig paths = sdk;
    const doa4::gamefiles::UserDataChoice choice = doa4::gamefiles::ChooseUserData(
        sdk.user_data_root, !rex::cvar::Query<std::string>("user_data_root").empty(),
        doa4::PathFromUtf8(rex::cvar::Query<std::string>("doa4_user_data")));
    if (!choice.note.empty()) REXLOG_WARN("[doa4][SETUP] {}", choice.note);
    if (choice.source == doa4::gamefiles::UserDataSource::kSaved) {
      paths.user_data_root = choice.path;
      if (rex::cvar::Query<std::string>("cache_path").empty()) {
        paths.cache_root = choice.path / "cache";  // the SDK's rule, re-applied
      }
    }
    user_data_ = paths.user_data_root;
    static constexpr const char* kSource[] = {"--user_data_root", "doa4_user_data", "default"};
    REXLOG_INFO("[doa4][SETUP] user data: {} ({}), cache {}", doa4::Utf8(user_data_),
                kSource[int(choice.source)], doa4::Utf8(paths.cache_root));
    if (session_) RedirectUserData(&paths);
    return paths;
  }

  // Netplay (Task 12): a process started for an online session runs the guest on <U>/netplay/session, a folder
  // cleared and filled with the host's save. Only the RUNTIME is redirected: user_data_ keeps <U> (the pre-redirect
  // root), which every app-side reader of UserData() reads (photos, the menu, the save backups), and the session's
  // own folders are built from it. The cache stays the player's (<U>/cache or cache_path). A folder that cannot be
  // cleared or written leaves the session off: the process starts offline on <U>.
  void RedirectUserData(rex::PathConfig* paths) {
    namespace restart = doa4::np::restart;
    const std::filesystem::path session = restart::SessionUserData(user_data_);
    std::string error;
    if (!restart::ClearFolder(session, &error) || !restart::WriteSaveFiles(session, session_->files, &error)) {
      REXLOG_WARN("[doa4][NET] WARN session: the session folder {} cannot be prepared ({}): relaunching offline",
                  doa4::Utf8(session), error);
      session_.reset();
      AbortIntoOfflineRelaunch("the session folder cannot be prepared");
      return;
    }
    size_t bytes = 0;
    for (const doa4::np::handoff::File& f : session_->files) bytes += f.data.size();
    paths->user_data_root = session;
    session_user_data_ = session;
    const std::string line =
        fmt::format("[doa4][NET] session user data: {} (save from the host: {} files, {} bytes), cache {}",
                    doa4::Utf8Generic(session), session_->files.size(), bytes, doa4::Utf8Generic(paths->cache_root));
    doa4::np::EarlyLog(line);
  }

  const std::filesystem::path& UserData() const {
    return user_data_.empty() ? user_data_root() : user_data_;
  }

  // Esc menu, Game tab: choose another location (used from the next start).
  void ChangeGameFiles() {
    if (setup_) return;
    REXLOG_INFO("[doa4][SETUP] change screen opened");
    OpenSetup(doa4::SetupHost::Mode::kChange, "", [this](const std::filesystem::path& folder) {
      return SaveGameFiles(folder);
    });
  }

  void CloseSetup() {
    setup_open_.store(false, std::memory_order_release);
    setup_.reset();
  }

  void OpenSetup(doa4::SetupHost::Mode mode, std::string reason,
                 std::function<std::string(const std::filesystem::path&)> done) {
    doa4::SetupHost host;
    host.mode = mode;
    auto* win32 = dynamic_cast<rex::ui::Win32Window*>(window());
    host.hwnd = win32 ? win32->hwnd() : nullptr;
    host.reason = std::move(reason);
    host.current = game_files_;
    host.defer = [this](std::function<void()> fn) {
      app_context().CallInUIThreadDeferred(std::move(fn));
    };
    host.post = [this](std::function<void()> fn) { app_context().CallInUIThread(std::move(fn)); };
    host.done = std::move(done);
    host.close = [this, mode] {
      if (mode == doa4::SetupHost::Mode::kFirstRun) {
        REXLOG_INFO("[doa4][SETUP] quit from the setup screen");
        app_context().QuitFromUIThread();
      } else {
        REXLOG_INFO("[doa4][SETUP] change screen closed");
        CloseSetup();
      }
    };
    host.log = [](const std::string& line) { REXLOG_INFO("[doa4][SETUP] {}", line); };
    setup_open_.store(true, std::memory_order_release);
    setup_ = std::make_unique<doa4::SetupDialog>(drawer_, std::move(host));
  }

  // Saves the chosen folder as doa4_game_files (the file, then the cvar the
  // Game tab reads). Returns "" or the failure, which is also logged; after a
  // first-run failure the game still starts with the folder, and the setup
  // screen opens again next time. Saved with forward slashes: the SDK's F4
  // "Save to config" writes strings unescaped, and a backslash there would
  // make the loader drop the whole file.
  std::string SaveGameFiles(const std::filesystem::path& folder) {
    const std::string text = doa4::Utf8Generic(folder);
    const std::string error =
        doa4::SaveTomlKey(ConfigPath(), "doa4_game_files", doa4::TomlString(text));
    if (error.empty()) {
      rex::cvar::SetFlagByName("doa4_game_files", text);
      REXLOG_INFO("[doa4][SETUP] saved doa4_game_files = {}", text);
    } else {
      REXLOG_WARN("[doa4][SETUP] could not save doa4_game_files ({}); the setup screen will "
                  "open again next time",
                  error);
    }
    return error;
  }

  std::filesystem::path ConfigPath() {
    return rex::filesystem::GetExecutableFolder() / (GetName() + ".toml");
  }

  // Override virtual hooks for customization:
  void OnPostInitLogging() override {
    // The dot-product NaN redirect is compile-time, and the build id hashes bytes, not features: this line says
    // whether this exe has it (netplay phase 1, Task 1; the gate reads it from every run's game.log).
    REXLOG_INFO("[doa4][LIFE] dot products: canonical NaN {}", doa4::np::DotProductSelfTest());
    // The netplay release identity (CMakeLists.txt: DOA4_RELEASE_*; "dev" for local builds). A [LIFE] line, so an
    // offline run still has no [NET]/[NP] line (gate S9); plan 1b reads it.
    REXLOG_INFO("[doa4][LIFE] version {} ({} releases of {})", DOA4_RELEASE_TAG, DOA4_RELEASE_CHANNEL, DOA4_RELEASE_REPO);
    // Task 10 review I4 (carry 2k): how this process was asked to show its window (a relaunch passes it on).
    {
      STARTUPINFOW si{};
      si.cb = sizeof si;
      GetStartupInfoW(&si);
      REXLOG_INFO("[doa4][LIFE] started show_window={}", (si.dwFlags & STARTF_USESHOWWINDOW) ? int(si.wShowWindow) : -1);
    }
    NetplayAtStart();
  }

  // Netplay (Task 12), before the runtime and the game's first thread: the test dir, the hand-off of a session process
  // (read, deleted, the variable cleared, its build checked, the host's guest settings applied and read back), the
  // switch gate (R22), a replay's recording rules (R21) and the pids.txt line.
  void NetplayAtStart() {
    if (const char* t = std::getenv("DOA4_NP_TEST_DIR"); t && *t) test_dir_ = doa4::PathFromUtf8(t);
    bool had_handoff = false;
    if (const wchar_t* h = _wgetenv(L"DOA4_NP_HANDOFF"); h && *h) {
      had_handoff = true;
      {  // carry 2b: the config file as it was before any host setting is applied
        std::error_code ec;
        std::ifstream f(ConfigPath(), std::ios::binary);
        if (f) toml_snapshot_ = std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        toml_had_ = bool(f) || std::filesystem::exists(ConfigPath(), ec);
      }
      const std::filesystem::path path(h);
      SetEnvironmentVariableW(L"DOA4_NP_HANDOFF", nullptr);  // the offline relaunch at the end must not inherit it
      _wputenv(L"DOA4_NP_HANDOFF=");
      ReadHandoff(path);
    }
    doa4::np::SetSwitchContext(session_.has_value(), !test_dir_.empty());
    doa4::np::PrepareReplayRules();
    if (!test_dir_.empty()) {
      const char* kind = (session_ || launch_refused_) ? "session" : (had_handoff || AfterSession()) ? "offline" : "lobby";
      std::error_code ec;
      std::filesystem::create_directories(test_dir_, ec);
      std::ofstream out(test_dir_ / "pids.txt", std::ios::binary | std::ios::app);
      out << "pid " << GetCurrentProcessId() << ' ' << kind << ' '
          << std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch()).count()
          << '\n';
    }
    // fix round M2: like DOA4_WAIT_PID, the marker is for this process only (its own relaunches start clean)
    SetEnvironmentVariableW(L"DOA4_NP_AFTER_SESSION", nullptr);
    _wputenv(L"DOA4_NP_AFTER_SESSION=");
  }

  // Fix round I1/I2: a session start that fails once a host-imposed value was applied (or once the redirect is known
  // wrong) never continues in this process: the offline relaunch (a clean process, DOA4_WAIT_PID) starts now, and this
  // process never builds the runtime (OnFinalizePaths) or never launches the module (OnPreLaunchModule).
  void AbortIntoOfflineRelaunch(const std::string& why) {
    launch_refused_ = true;
    guest_touched_ = true;
    std::string e;
    if (!doa4::np::restart::Relaunch(nullptr, true, &e)) {
      REXLOG_ERROR("[doa4][NET] offline relaunch failed: {}", e);
    }
    REXLOG_WARN("[doa4][NET] session start aborted ({}): offline relaunch started, this process quits", why);
  }

  static bool AfterSession() {
    const char* a = std::getenv("DOA4_NP_AFTER_SESSION");
    return a && a[0] == '1';
  }

  // The hand-off of a session process: read and delete the file (one restart per hand-off), unpack, check the build,
  // apply the host's guest settings. Anything wrong: this process starts offline (the peer gives up after 60 s).
  void ReadHandoff(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    {
      HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        uint8_t buf[65536];
        DWORD n = 0;
        while (::ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) bytes.insert(bytes.end(), buf, buf + n);
        CloseHandle(h);
      }
      DeleteFileW(path.c_str());
    }
    if (bytes.empty()) {
      doa4::np::EarlyLog(fmt::format("[doa4][NET] hand-off {} unreadable: starting offline", doa4::Utf8Generic(path)));
      return;
    }
    doa4::np::handoff::Handoff h;
    if (const std::string err = doa4::np::handoff::Unpack(bytes, &h); !err.empty()) {
      doa4::np::EarlyLog(fmt::format("[doa4][NET] hand-off {} unreadable ({}): starting offline",
                                     doa4::Utf8Generic(path), err));
      return;
    }
    const uint64_t own = doa4::np::BuildIdValue();
    if (h.build != own || own == 0) {
      doa4::np::EarlyLog(fmt::format("[doa4][NET] WARN session: the hand-off's build {:016x} is not this exe's {:016x}: "
                                     "starting offline", h.build, own));
      return;
    }
    // carry 2b: the SDK never calls rex::cvar::FinalizeInit (sdk-np1 src/core/cvar.cpp: defined, no caller), so the
    // init-only cvars (rexcrt_heap_*) can still be set here, before the runtime exists; ApplyGuestSettings validates
    // the whole text before it sets anything.
    std::string why;
    bool touched = false;
    if (!doa4::np::ApplyGuestText(h.guest_settings, &why, &touched)) {
      if (!touched) {  // refused before any set: this process is still the player's own, it starts offline
        doa4::np::EarlyLog("[doa4][NET] WARN session: the host's guest settings were refused (" + why +
                           "): the session does not start; starting offline");
        return;
      }
      doa4::np::EarlyLog("[doa4][NET] WARN session: the host's guest settings did not take (" + why +
                         ") after some were set: relaunching offline");
      AbortIntoOfflineRelaunch("guest settings");
      return;
    }
    guest_touched_ = true;
    doa4::np::EarlyLog(fmt::format("[doa4][NET] hand-off read: {}, delay {}, {} save files; the session starts",
                                   h.role == doa4::np::handoff::Role::kHost ? "host" : "joiner", int(h.delay),
                                   h.files.size()));
    session_ = std::move(h);
  }
  void OnPreSetup(rex::RuntimeConfig& config) override {
    (void)config;
    doa4_wait_for_previous_instance();  // after "Restart now" in the menu
    // Plan 1b Task 4: the game files Check for Updates renamed away (<name>.old) are no longer in use once the instance
    // that ran from them has exited; one still in use (another instance) stays for the next start.
    doa4::updater::DeleteOldFiles(doa4::updater::ExeDir());
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  void OnPostSetup() override {
    REXLOG_INFO("[doa4][LIFE] OnPostSetup: runtime ready");
    // Test switch: open the Game tab's change screen once the game runs (with
    // DOA4_SETUP_TEST choosing the folder), as "Change..." would.
    if (const char* change = std::getenv("DOA4_SETUP_CHANGE_TEST"); change && change[0] == '1') {
      app_context().CallInUIThreadDeferred([this] { ChangeGameFiles(); });
    }
    // NOTE: log_noisy intentionally left OFF. It was force-enabled during boot
    // bring-up to trace kernel imports; that trace is hot-path spam now and is
    // a no-op at the relwithdebinfo log level (info) anyway.
    MountDiscRoot();
    MountCacheDevice();
    StartThreadSnapshotter();
    StartTestLobbyWatch();  // netplay: DOA4_NP_TEST_DIR only (np_ctl.on creates the lobby)

    // Photo/snapshot capture reads the rendered frame back from GUEST memory.
    // rexglue's GPU defaults readback_resolve="none", so render-to-texture
    // resolve results never get copied from the host GPU into guest RAM — the
    // in-match photo buffer stays zeroed and saved photos come out solid black.
    // Enable readback so the captured frame reaches CPU memory.
    //
    // Mode = "some": copy a resolve result to guest RAM only ON DEMAND (when the
    // CPU/texture cache actually reads that region). "fast" (copy EVERY frame)
    // also fixes the black photo but continuously overwrites the save-slot
    // preview surfaces with the live capture, so every saved slot's thumbnail
    // showed the most recent photo. "some" copies only when the save path reads
    // the captured frame, which keeps photos working without that overwrite —
    // and stays cheaper than "full" (per-resolve sync stall that hurts gameplay).
    if (rex::cvar::SetFlagByName("readback_resolve", "some")) {
      REXLOG_INFO("[doa4][GPU] readback_resolve=some (enables photo/snapshot "
                  "framebuffer capture)");
    } else {
      REXLOG_WARN("[doa4][GPU] failed to set readback_resolve cvar");
    }
  }
  void OnPostLoadXexImage() override {
    REXLOG_INFO("[doa4][LIFE] OnPostLoadXexImage: XEX mapped into guest memory");
    DumpGuestStrings();
    // Before any guest code allocates: keeps fighter data out of low addresses,
    // where the game's relocation test skips tables (Ryu Hayabusa / Brad Wong crash).
    doa4::ReserveLowGuestMemory(runtime() ? runtime()->memory() : nullptr);
    SetUpExtras();  // before any guest code runs: the tweaks patch tables read at boot
    SetUpSnapshotWatcher();  // before the first guest swap, so the swap counts start together
    StartNetplaySession();   // DOA4_NP only: record / replay / hash from the first frame
  }

  // Netplay phase 0 (src/net/np_session.h): inert unless DOA4_NP is set; phase 1 (Task 12): the online session of a
  // process started from a hand-off.
  void StartNetplaySession() {
    doa4::np::Host host;
    host.capture = [this](doa4::photo::Image* image) { return CaptureFrame(image); };
    host.quit = [this] {
      app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); });
    };
    // The folder the game was loaded from (the one MountDiscRoot mounts as the disc), else the one the
    // setup chose: the session hashes its default.xex into the recording's meta.
    if (auto* rt = runtime()) host.game_data_root = std::filesystem::absolute(rt->game_data_root());
    if (host.game_data_root.empty()) host.game_data_root = game_files_;
    host.guest = guest_ready_ ? &guest_ : nullptr;  // the unlock of a session, and of a replay of one (R21)
    if (!session_) {
      doa4::np::Start(std::move(host));
      return;
    }
    StartOnlineSession(std::move(host));
  }

  void StartOnlineSession(doa4::np::Host host) {
    namespace restart = doa4::np::restart;
    // The redirect proof (#0282): the RUNTIME's user data (the root the content manager mounts), not the app's variable.
    rex::Runtime* rt = runtime();
    const std::filesystem::path runtime_ud = rt ? rt->user_data_root() : std::filesystem::path();
    if (!restart::RedirectHolds(runtime_ud, session_user_data_)) {
      REXLOG_WARN("[doa4][NET] WARN session: user data is {}, not the session folder {}: relaunching offline",
                  doa4::Utf8Generic(runtime_ud), doa4::Utf8Generic(session_user_data_));
      AbortIntoOfflineRelaunch("the user-data redirect did not take");
      return;
    }
    REXLOG_INFO("[doa4][NET] redirect verified: the runtime's user data is {}", doa4::Utf8Generic(runtime_ud));
    const bool is_host = session_->role == doa4::np::handoff::Role::kHost;
    const std::filesystem::path dir = restart::SessionDir(user_data_, test_dir_, is_host, std::time(nullptr));
    std::string error;
    if (!restart::ClearFolder(dir, &error)) {
      REXLOG_WARN("[doa4][NET] WARN session: the session folder {} cannot be prepared ({}): restarting offline",
                  doa4::Utf8Generic(dir), error);
      RelaunchOffline();
      return;
    }
    // the save an offline replay of this session starts from (the session's user data without the cache)
    {
      std::error_code ec;
      std::filesystem::create_directories(dir / "ud_start", ec);
      for (const auto& e : std::filesystem::directory_iterator(session_user_data_, ec)) {
        if (e.path().filename() == "cache") continue;
        std::filesystem::copy(e.path(), dir / "ud_start" / e.path().filename(),
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                              ec);
      }
    }
    host.online = *session_;
    host.session_dir = dir;
    host.test_dir = test_dir_;
    host.release = DOA4_RELEASE_TAG;
    host.read_local_pad = [this](doa4::np::Pad* pad) { return ReadLocalPad(pad); };
    host.restart = [this](bool with_handoff) {
      app_context().CallInUIThreadDeferred([this, with_handoff] {
        std::string e;
        const std::filesystem::path h = restart::HandoffPath(user_data_);
        if (!restart::Relaunch(with_handoff ? &h : nullptr, !with_handoff, &e)) {
          REXLOG_ERROR("[doa4][NET] relaunch failed: {}", e);
          if (with_handoff) {  // fix round M5: no hand-off is left for a later start to pick up
            std::error_code ec;
            std::filesystem::remove(h, ec);
          }
        }
        app_context().QuitFromUIThread();
      });
    };
    // the hold banner and the end banner (Task 14): posted to the UI thread, which creates / destroys the overlay
    host.overlay_changed = [this] {
      app_context().CallInUIThreadDeferred([this] { UpdateNetOverlay(); });
    };
    host.send_leave = [this](int32_t frame, doa4::np::wire::LeaveReason reason) {
      if (lobby_) lobby_->SendLeave(frame, reason);
    };
    host.transport = [this]() -> doa4::np::net::Transport* { return lobby_ ? &lobby_->transport() : nullptr; };
    host.session_refused = [this] { return lobby_ && lobby_->status().session_refused; };
    host.quit_session = [this](int32_t frame) { return lobby_ ? lobby_->QuitSession(frame, 150) : 0; };
    host.stop_transport = [this] {
      if (lobby_) lobby_->transport().Stop();
    };
    const doa4::np::handoff::Handoff h = *session_;
    doa4::np::Start(std::move(host));
    // the session lobby: exactly the hand-off's port, Wi-Fi media streaming on (session processes only)
    doa4::np::net::Config cfg = NetConfig();
    cfg.wifi_streaming = true;
    lobby_ = std::make_unique<doa4::np::online::Lobby>();
    doa4::np::online::Providers p = LobbyProviders(/*session=*/true);
    if (!lobby_->StartInSession(std::move(p), cfg, h,
                                [](int32_t frame, doa4::np::wire::LeaveReason reason) { doa4::np::PeerLeft(frame, reason); },
                                &error)) {
      doa4::np::LogLine("WARN session: could not open UDP port " + std::to_string(h.local_port) + " (" + error + ")");
      REXLOG_WARN("[doa4][NET] banner: could not open UDP port {}", h.local_port);
      doa4::np::Abort("could not open UDP port " + std::to_string(h.local_port));
    }
  }

  // InputSystem::GetState(0): what XamInputGetState returns for user 0 (the session's local pad without a test script).
  bool ReadLocalPad(doa4::np::Pad* pad) {
    if (menu_open_.load(std::memory_order_acquire) || setup_open_.load(std::memory_order_acquire)) return false;
    rex::Runtime* rt = runtime();
    auto* input = rt ? dynamic_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
    if (!input) return false;
    rex::input::X_INPUT_STATE state{};
    if (input->GetState(0, &state) != 0) return false;
    *pad = doa4::np::PadFromGuest(reinterpret_cast<const uint8_t*>(&state.gamepad));
    return true;
  }

  void RelaunchOffline() {
    app_context().CallInUIThreadDeferred([this] {
      std::string e;
      if (!doa4::np::restart::Relaunch(nullptr, true, &e)) REXLOG_ERROR("[doa4][NET] relaunch failed: {}", e);
      app_context().QuitFromUIThread();
    });
  }

  // net::Config from the settings cvars and, with a test dir only, the harness switches (R11).
  doa4::np::net::Config NetConfig() const {
    namespace net = doa4::np::net;
    net::Config cfg;
    cfg.port = uint16_t(rex::cvar::Query<uint32_t>("netplay_port"));
    cfg.upnp = rex::cvar::Query<bool>("netplay_upnp");
    if (!test_dir_.empty()) {
      auto env = [](const char* n) {
        const char* v = std::getenv(n);
        return std::string(v ? v : "");
      };
      if (env("DOA4_NP_BIND") == "loopback") {
        cfg.loopback = true;
        cfg.lan_broadcast = false;
      }
      if (env("DOA4_NP_STUN") == "off") cfg.stun = false;
      if (env("DOA4_NP_UPNP") == "off") cfg.upnp = false;
      cfg.sim = env("DOA4_NP_SIM");
      if (const std::string d = env("DOA4_NP_SIM_DROP"); !d.empty()) {
        if (!net::sim::ParseDrop(d, &cfg.sim_drop)) REXLOG_WARN("[doa4][NET] DOA4_NP_SIM_DROP '{}' ignored", d);
      }
    }
    if (cfg.loopback) cfg.upnp = false;  // carry 2g: no router mapping for a loopback socket
    return cfg;
  }

  // The lobby's providers (Task 9's interface). In a session process the lobby and transport log into np_log (a
  // transport WARN line fails the run).
  doa4::np::online::Providers LobbyProviders(bool session) {
    namespace online = doa4::np::online;
    namespace restart = doa4::np::restart;
    online::Providers p;
    p.build = [] { return doa4::np::BuildIdValue(); };
    p.release = DOA4_RELEASE_TAG;
    const std::filesystem::path files_dir = game_files_;
    p.files = [files_dir] { return doa4::np::files::Text(doa4::np::files::Fingerprint(files_dir)); };
    p.guest_settings = [] {
      return doa4::np::handoff::CaptureGuestSettings(
          [](const std::string& name) { return rex::cvar::Query<std::string>(name); });
    };
    const std::filesystem::path real = user_data_;
    p.save_files = [real](std::string* error) { return SaveFilesOf(real, error); };
    p.handoff_path = restart::HandoffPath(user_data_);
    p.restart_into_session = [this] {
      app_context().CallInUIThreadDeferred([this] {
        std::string e;
        const std::filesystem::path h = doa4::np::restart::HandoffPath(user_data_);
        if (doa4::np::restart::TestNoRestart(!test_dir_.empty(), std::getenv("DOA4_NP_TEST_NO_RESTART"))) {
          // Task 15 carry f (gate S8d): the harness's lobby-only peer. It said yes but stays a lobby, so the other
          // game's session Hello reaches a lobby; no hand-off is left behind for a later start.
          REXLOG_INFO("[doa4][NET] test: DOA4_NP_TEST_NO_RESTART=1: not restarting into the session (staying a lobby)");
          std::error_code ec;
          std::filesystem::remove(h, ec);
          return;
        }
        if (!doa4::np::restart::Relaunch(&h, false, &e)) {
          REXLOG_ERROR("[doa4][NET] relaunch into the session failed: {}", e);
          std::error_code ec;
          std::filesystem::remove(h, ec);  // fix round M5
        }
        app_context().QuitFromUIThread();
      });
    };
    p.events_path = user_data_ / "netplay" / "events.txt";
    p.test_dir = test_dir_;
    if (!test_dir_.empty()) {
      for (const char* n : {"DOA4_NP_SEED", "DOA4_NP_DELAY", "DOA4_NP_TEST_BUILD_SALT", "DOA4_NP_TEST_FILES_SALT"}) {
        if (const char* v = std::getenv(n); v && *v) p.test[n] = v;
      }
    }
    if (session) {
      p.log = [](const std::string& line) { doa4::np::LogLine(line); };
    } else {
      p.log = [](const std::string& line) { REXLOG_INFO("{}", line); };
    }
    return p;
  }

  // The host's save: every file below <U>/B13EBABEBABEBABE/544307D1/, named from <U> with '/' separators.
  static std::vector<doa4::np::handoff::File> SaveFilesOf(const std::filesystem::path& real, std::string* error) {
    std::vector<doa4::np::handoff::File> out;
    const std::filesystem::path root = real / "B13EBABEBABEBABE" / "544307D1";
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
      *error = "no save folder " + doa4::Utf8Generic(root);
      return out;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      doa4::np::handoff::File f;
      f.name = std::string(doa4::np::handoff::kSavePrefix) +
               doa4::Utf8Generic(std::filesystem::relative(it->path(), root, ec));
      std::ifstream in(it->path(), std::ios::binary);
      f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      out.push_back(std::move(f));
    }
    if (ec) *error = "cannot list " + doa4::Utf8Generic(root);
    return out;
  }

  // With a test dir (DOA4_NP_TEST_DIR), in a process that is not a session: the harness drives the lobby through
  // np_ctl.on. status.txt says "offline" at once (np_pair waits for it); the lobby is created when the first request
  // file arrives (spec 4: no socket before that), and its own I/O thread polls from then on.
  void StartTestLobbyWatch() {
    if (test_dir_.empty() || session_) return;
    {
      const std::string text = "state=offline\nport=0\npeer_port=0\nping=0.0\ndelay=" +
                               std::to_string(rex::cvar::Query<int32_t>("netplay_delay")) +
                               "\ncan_start=0\nevent=\nrefusal=none\n";
      const std::filesystem::path tmp = test_dir_ / "status.tmp", dst = test_dir_ / "status.txt";
      {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
      }
      MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    lobby_watch_ = std::thread([this] {
      using namespace std::chrono_literals;
      while (!lobby_watch_stop_.load(std::memory_order_acquire)) {
        std::error_code ec;
        if (std::filesystem::exists(test_dir_ / "np_ctl.on", ec)) {
          app_context().CallInUIThreadDeferred([this] { EnsureLobby(); });
          return;
        }
        std::this_thread::sleep_for(100ms);
      }
    });
  }

  // The Online tab (Task 14) or the first np_ctl.on: the lobby, created once.
  void EnsureLobby() {
    if (lobby_ || session_) return;
    lobby_ = std::make_unique<doa4::np::online::Lobby>();
    std::string error;
    if (!lobby_->Start(LobbyProviders(false), NetConfig(), &error)) {
      REXLOG_WARN("[doa4][NET] the lobby could not start: {}", error);
    } else {
      REXLOG_INFO("[doa4][NET] lobby started on port {}", lobby_->status().port);
      // the player's delay preference (with a test dir the harness's DOA4_NP_DELAY, read by the lobby itself, stays in charge)
      if (test_dir_.empty()) {
        lobby_->SetDelay(rex::cvar::Query<bool>("netplay_delay_auto"), rex::cvar::Query<int32_t>("netplay_delay"));
      }
    }
    UpdateNetOverlay();
  }

  // ---- the Online tab, the banner and the F9 box (Task 14) ----
  // This process runs an online session: the one predicate behind the menu locks, their refusals and the tab.
  bool InOnlineSession() const { return session_.has_value() && !launch_refused_; }
  // The menu's locks and their refusals: in a session, or the lobby is starting one (online_view::SessionLocks).
  bool SessionLocked() { return doa4::online_view::SessionLocks(OnlineViewIn(false)); }

  doa4::online_view::In OnlineViewIn(bool tab_shown) {
    doa4::online_view::In in;
    if (lobby_) {
      in.lobby = lobby_->status();
      in.lobby_open = true;
    }
    in.in_session = InOnlineSession();
    if (in.in_session) {
      const doa4::np::NetTiming t = doa4::np::LastTiming();
      in.frame = t.frame;
      in.delay = t.delay;
      in.ping_ms = t.ping_ms;
      in.frames_ahead = t.frames_ahead;
    }
    in.release = DOA4_RELEASE_TAG;
    // Only when the Online tab draws or a lobby / session exists (spec 4: an offline game that never opened the tab
    // hashes nothing); once, on the UI thread. net_build_hashes_ counts the computations (the log line below).
    if (net_build_id_ == 0 && doa4::online_view::NeedsBuildId(tab_shown, lobby_ != nullptr, in.in_session)) {
      net_build_id_ = doa4::np::BuildIdValue();
      REXLOG_INFO("[doa4][NET] build id hashed for the Online tab/lobby (computation {})", ++net_build_hashes_);
    }
    in.build = net_build_id_;
    in.show_stats = rex::cvar::Query<bool>("netplay_show_stats");
    return in;
  }

  doa4::NetOverlay::Content NetOverlayContent() {
    namespace ov = doa4::online_view;
    const ov::In in = OnlineViewIn(false);
    ov::Ending ending = ov::Ending::kNone;
    if (in.in_session) {
      switch (doa4::np::LastResult().end) {
        case doa4::np::live::End::kLeft: ending = ov::Ending::kLeft; break;
        case doa4::np::live::End::kPeerLeft: {  // review I2: the text follows the reason
          const doa4::np::live::FrameResult r = doa4::np::LastResult();
          ending = ov::PeerLeftEnding(r.peer_reason, r.end_frame, doa4::np::SessionRefused());
          break;
        }
        case doa4::np::live::End::kLost: ending = ov::Ending::kLost; break;
        case doa4::np::live::End::kDesync: ending = ov::Ending::kDesync; break;
        case doa4::np::live::End::kFrame0Timeout: ending = ov::Ending::kFrame0Timeout; break;
        case doa4::np::live::End::kInternal: ending = ov::Ending::kInternal; break;
        case doa4::np::live::End::kNone: break;
      }
    }
    doa4::NetOverlay::Content c;
    c.banner = ov::BannerText(in.in_session && doa4::np::BannerWaiting(), ending);
    c.stats = ov::Describe(in).stats_lines;
    return c;
  }

  // The overlay exists only while the banner is up or the F9 box can show (it is on and there is a lobby or a session):
  // while any ImGui dialog exists the presenter paints from the UI thread. UI thread only.
  void UpdateNetOverlay() {
    if (net_closing_.load(std::memory_order_acquire) || !drawer_) return;
    const doa4::online_view::In in = OnlineViewIn(false);
    bool want = in.show_stats && (in.in_session || in.lobby_open);
    if (!want && in.in_session) {
      want = !NetOverlayContent().banner.empty();
    }
    // the banner text is logged here, not in the overlay's OnDraw: a minimized window never paints (the harness's games)
    const std::string banner = in.in_session ? NetOverlayContent().banner : std::string();
    if (banner != net_logged_banner_) {
      net_logged_banner_ = banner;
      if (!banner.empty()) REXLOG_INFO("[doa4][NET] overlay: banner \"{}\"", banner);
    }
    if (want && !net_overlay_) {
      net_overlay_ = std::make_unique<doa4::NetOverlay>(drawer_, [this] { return NetOverlayContent(); });
      REXLOG_INFO("[doa4][NET] overlay: shown");
    } else if (!want && net_overlay_) {
      net_overlay_.reset();
      REXLOG_INFO("[doa4][NET] overlay: hidden");
    }
  }

  // The F9 box's lines, logged once per change as "[doa4][NET] stats: Ping 17 ms | Delay 2 frames (33 ms)" (the test
  // hook behind the harness's check that the in-session ping is real; UI thread).
  void LogNetStatsLine() {
    const doa4::online_view::View v = doa4::online_view::Describe(OnlineViewIn(false));
    std::string line;
    for (const std::string& s : v.stats_lines) line += (line.empty() ? "" : " | ") + s;
    if (line == net_logged_stats_) return;
    net_logged_stats_ = line;
    if (!line.empty()) REXLOG_INFO("[doa4][NET] stats: {}", line);
  }

  // F9: the ping and input-delay box (in memory only; the Online tab checkbox saves the setting).
  void ToggleNetStats() {
    const bool now = !rex::cvar::Query<bool>("netplay_show_stats");
    rex::cvar::SetFlagByName("netplay_show_stats", now ? "true" : "false");
    REXLOG_INFO("[doa4][NET] F9: ping and delay box {}", now ? "on" : "off");
    UpdateNetOverlay();
  }
  void OnPreLaunchModule() override {
    if (launch_refused_) {  // fix round I2: the guest never runs on the wrong user data (the module is not launched)
      REXLOG_WARN("[doa4][NET] module launch refused: the session start was aborted; this process ends now");
      if (auto l = ::rex::GetLogger()) l->flush();
      TerminateProcess(GetCurrentProcess(), 0);
    }
    REXLOG_INFO("[doa4][LIFE] OnPreLaunchModule: about to spawn main guest thread");
  }
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    REXLOG_INFO("[doa4][LIFE] OnPostLaunchModule: main thread tid={} created (suspended)",
                thread ? thread->thread_id() : 0u);
  }
  void OnGuestThreadExit(rex::system::XThread* thread) override {
    REXLOG_INFO("[doa4][LIFE] OnGuestThreadExit: main thread tid={} exited",
                thread ? thread->thread_id() : 0u);
  }
  void OnShutdown() override {
    doa4_arm_shutdown_watchdog();
    // first of all (fix round M5): the menu timer posts to the UI thread and reads the lobby; <= 25 ms to join, before
    // the lobby stop and the UPnP removal spend the 5 s watchdog's budget
    menu_tick_stop_.store(true, std::memory_order_release);
    if (menu_tick_.joinable()) menu_tick_.join();
    if (session_ || guest_touched_) CheckConfigUnchanged();  // carry 2b, while np_log is still open
    // first: the session's writer captures frames through the presenter; np::Shutdown stops the session's transport
    // before anything else (Stop is bounded by 200 ms, Task 7)
    doa4::np::Shutdown();
    // np::Shutdown returns at once without a session (a lobby process): the lobby is stopped here, whenever one exists.
    // Budget under the 5 s watchdog (#0321): shot drain (1.9 s for 36 shots) + transport 200 ms + lobby 200 ms + UPnP 2 s.
    lobby_watch_stop_.store(true, std::memory_order_release);
    if (lobby_watch_.joinable()) lobby_watch_.join();
    if (lobby_) {
      lobby_->Stop();  // idempotent: in a session process its transport is already stopped
      if (NetConfig().upnp) doa4::np::upnp::Shutdown(2000);  // Task 7's contract: the router mapping is removed
    }
    rex::ui::UnregisterBind("bind_doa4_menu");
    rex::ui::UnregisterBind("bind_doa4_photo");
    rex::ui::UnregisterBind("bind_doa4_netstats");
    net_closing_.store(true, std::memory_order_release);
    photo_closing_.store(true, std::memory_order_release);
    doa4::photo::InstallSnapshotWatcher(nullptr);  // the watcher itself lives until exit
    photo_stop_.store(true, std::memory_order_release);
    if (photo_test_thread_.joinable()) photo_test_thread_.join();
    if (photo_) {
      const size_t dropped = photo_->Stop();
      if (dropped) REXLOG_WARN("[doa4][PHOTO] {} queued photo(s) dropped at shutdown", dropped);
    }
    if (setup_) {
      // Before the base app destroys the ImGui drawer; also cancels a running
      // extraction and waits for its clean-up.
      REXLOG_INFO("[doa4][SETUP] setup screen closed at shutdown");
      CloseSetup();
    }
    photo_notice_.reset();  // before the base app destroys the ImGui drawer
    net_overlay_.reset();   // likewise
    menu_.reset();  // before the base app destroys the ImGui drawer
    rex::ui::UnregisterBind("bind_doa4_diag");
    diag_overlay_.reset();  // before the base app destroys the ImGui drawer
    snap_stop_.store(true, std::memory_order_release);
    if (snap_thread_.joinable()) snap_thread_.join();
    extras_stop_.store(true, std::memory_order_release);
    gfx_stop_.store(true, std::memory_order_release);
    if (gfx_test_thread_.joinable()) gfx_test_thread_.join();
    if (extras_thread_.joinable()) extras_thread_.join();
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}

  // The diagnostics overlay is off until F6 (bind_doa4_diag) or
  // DOA4_DIAG_OVERLAY=1 turns it on: while any ImGui dialog exists the
  // presenter paints from the UI thread (every guest frame waits for a UI
  // repaint, and the UI repaints continuously), instead of presenting each
  // frame straight from the GPU thread.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (!drawer) return;
    rex::ui::RegisterBind("bind_doa4_diag", "F6", "Toggle DOA4 diagnostics overlay",
                          [this, drawer] { ToggleDiagOverlay(drawer); });
    const char* env = std::getenv("DOA4_DIAG_OVERLAY");
    if (env && env[0] == '1') ToggleDiagOverlay(drawer);
    rex::ui::RegisterBind("bind_doa4_netstats", "F9", "Show the online ping and input delay",
                          [this] { ToggleNetStats(); });

    // Escape opens the menu, so it cannot also be the keyboard's Start (the
    // SDK default for keyboard-as-controller); applies whether or not
    // mnk_mode is on, so turning keyboard play on later cannot clash.
    if (rex::cvar::Query<std::string>("keybind_start") == "Escape" &&
        rex::cvar::SetFlagByName("keybind_start", "Return")) {
      REXLOG_INFO("[doa4][UI] keybind_start Escape -> Return (Escape opens the menu)");
    }
    drawer_ = drawer;
    running_scale_ = rex::cvar::Query<int32_t>("resolution_scale");
    saved_scale_ = running_scale_;
    running_speakers_ = rex::cvar::Query<std::string>("audio_speakers");
    saved_speakers_ = running_speakers_;
    REXLOG_INFO("[doa4][UI] resolution_scale {} fullscreen {} swap_post_effect {} "
                "anisotropic_override {}",
                running_scale_, rex::cvar::Query<bool>("fullscreen"),
                rex::cvar::Query<std::string>("swap_post_effect"),
                rex::cvar::Query<int32_t>("anisotropic_override"));
    rex::ui::RegisterBind("bind_doa4_menu", "Escape", "Toggle DOA4 menu",
                          [this] { ToggleMenu(); });
    const char* menu_env = std::getenv("DOA4_MENU_OPEN");  // for tests
    if (menu_env && menu_env[0] == '1') {
      ToggleMenu();
      // test hook: the tab and lock log lines come from a timer (a minimized window never paints the menu)
      menu_tick_ = std::thread([this] {
        using namespace std::chrono_literals;
        while (!menu_tick_stop_.load(std::memory_order_acquire)) {
          app_context().CallInUIThreadDeferred([this] {
            if (menu_) menu_->PollNetState();
            LogNetStatsLine();
          });
          // 250 ms in 25 ms slices: OnShutdown joins this thread first and must not wait a whole period (M5)
          for (int i = 0; i < 10 && !menu_tick_stop_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(25ms);
          }
        }
      });
    }
    UpdateNetOverlay();  // a session process whose netplay_show_stats is on shows the box from the start
    StartGfxTest();
    SetUpPhotos();
  }

  // Full-quality photos: the service, F12, the notice. See
  // notes/specs/2026-10-28-photo-mode-design.md.
  void SetUpPhotos() {
    doa4::photo::Service::Hooks hooks;
    hooks.capture = [this](doa4::photo::Image* image) { return CaptureFrame(image); };
    hooks.monitor = [this] {
      auto* win32 = dynamic_cast<rex::ui::Win32Window*>(window());
      return doa4::photo::MonitorSize(win32 ? win32->hwnd() : nullptr);
    };
    hooks.settings = [this] { return PhotoSettings(); };
    hooks.now = [] {
      const std::time_t now = std::time(nullptr);
      std::tm local{};
      localtime_s(&local, &now);
      return local;
    };
    hooks.on_result = [this](const doa4::photo::Result& result) {
      if (result.error) {
        REXLOG_WARN("[doa4][PHOTO] {}", result.text);
      } else {
        REXLOG_INFO("[doa4][PHOTO] {} ({})", result.text,
                    doa4::photo::PathText(result.path.parent_path()));
      }
      app_context().CallInUIThreadDeferred([this, result] { ShowPhotoNotice(result); });
    };
    photo_ = std::make_unique<doa4::photo::Service>(std::move(hooks));
    rex::ui::RegisterBind("bind_doa4_photo", "F12", "Save a full-quality photo",
                          [this] { TakePhoto(); });
    StartPhotoTest();
  }

  // Full-quality copies of in-game snapshots: the guest-swap hook
  // (doa4_photo_hook.cpp) calls the watcher after every guest swap. Installed
  // before the guest runs so its swap count matches the GPU's from the start.
  void SetUpSnapshotWatcher() {
    doa4::photo::SnapshotWatcher::Hooks hooks;
    hooks.gpu_swaps = [this]() -> uint32_t {
      rex::Runtime* rt = runtime();
      auto* graphics = rt ? dynamic_cast<rex::graphics::GraphicsSystem*>(rt->graphics_system())
                          : nullptr;
      rex::graphics::CommandProcessor* processor =
          graphics ? graphics->command_processor() : nullptr;
      return processor ? processor->swap_count() : 0;
    };
    hooks.capture = [this](doa4::photo::Image* image) { return CaptureFrame(image); };
    hooks.auto_enabled = [] { return rex::cvar::Query<bool>("doa4_photo_auto"); };
    hooks.submit = [this](doa4::photo::Image image, doa4::photo::Source source,
                          std::string suffix) {
      if (photo_) photo_->SubmitCaptured(std::move(image), source, std::move(suffix));
    };
    hooks.log = [](const std::string& line) { REXLOG_INFO("[doa4][PHOTO] {}", line); };
    hooks.sleep = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
    hooks.now_ms = [] {
      return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count());
    };
    const char* debug = std::getenv("DOA4_PHOTO_DEBUG");
    hooks.debug = debug && debug[0] == '1';
    if (hooks.debug) REXLOG_INFO("[doa4][PHOTO] snapshot debug on (DOA4_PHOTO_DEBUG=1)");
    photo_watcher_ = std::make_unique<doa4::photo::SnapshotWatcher>(std::move(hooks));
    doa4::photo::InstallSnapshotWatcher(photo_watcher_.get());
  }

  // F12: the current frame, saved by the worker.
  void TakePhoto() {
    if (photo_) photo_->RequestHotkeyPhoto();
  }

  // The game's frame after the render scale and FXAA, before the window
  // downscale and any overlay. Callable from any thread.
  bool CaptureFrame(doa4::photo::Image* image) {
    rex::Runtime* rt = runtime();
    auto* graphics = rt ? dynamic_cast<rex::graphics::GraphicsSystem*>(rt->graphics_system())
                        : nullptr;
    rex::ui::Presenter* presenter = graphics ? graphics->presenter() : nullptr;
    rex::ui::RawImage raw;
    if (!presenter || !presenter->CaptureGuestOutput(raw)) return false;
    if (raw.stride != size_t(raw.width) * 4 ||
        raw.data.size() != size_t(raw.width) * raw.height * 4) {
      return false;
    }
    image->width = raw.width;
    image->height = raw.height;
    image->rgbx = std::move(raw.data);
    return true;
  }

  doa4::photo::Settings PhotoSettings() const {
    doa4::photo::Settings settings;
    settings.format = doa4::photo::Format(doa4::photo::ChoiceIndex(
        doa4::photo::kFormats, rex::cvar::Query<std::string>("doa4_photo_format")));
    const std::string dir = rex::cvar::Query<std::string>("doa4_photo_dir");
    settings.dir = dir.empty() ? doa4::photo::DefaultPhotoDir()
                               : std::filesystem::path(
                                     std::u8string(dir.begin(), dir.end()));
    if (settings.dir.empty()) settings.dir = UserData() / "photos";
    return settings;
  }

  void ShowPhotoNotice(const doa4::photo::Result& result) {
    if (photo_closing_.load(std::memory_order_acquire) || !drawer_) return;
    photo_notice_.reset();
    const uint64_t serial = result.serial;
    photo_notice_ = std::make_unique<doa4::PhotoNotice>(
        drawer_, result.text, result.error, [this, serial] {
          app_context().CallInUIThreadDeferred([this, serial] {
            // Only the notice this callback belongs to: a newer one may have
            // replaced it.
            if (photo_notice_serial_ == serial) photo_notice_.reset();
          });
        });
    photo_notice_serial_ = serial;
  }

  // DOA4_PHOTO_TEST=hotkey: take a photo on the UI thread through TakePhoto,
  // as F12 does, 20, 40, 60 and 80 s after start (the first seconds of attract
  // mode are black logo screens). For runtime checks only.
  void StartPhotoTest() {
    const char* env = std::getenv("DOA4_PHOTO_TEST");
    if (!env || std::string_view(env) != "hotkey") return;
    REXLOG_INFO("[doa4][PHOTO] test: hotkey photos at 20, 40, 60 and 80 s");
    photo_test_thread_ = std::thread([this] {
      using namespace std::chrono_literals;
      for (int shot = 0; shot < 4; ++shot) {
        for (int i = 0; i < 200 && !photo_stop_.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(100ms);
        }
        if (photo_stop_.load(std::memory_order_acquire)) return;
        app_context().CallInUIThreadDeferred([this] { TakePhoto(); });
      }
    });
  }

  // Live anti-aliasing switch, for the menu and DOA4_GFX_TEST: the SDK reads
  // swap_post_effect only at start, so hand the new value to the GPU thread
  // (CommandProcessor::SetDesiredSwapPostEffect; applied at the next swap).
  void SetPostEffect(int index) {
    using Effect = rex::graphics::CommandProcessor::SwapPostEffect;
    static constexpr Effect kEffects[] = {Effect::kNone, Effect::kFxaa, Effect::kFxaaExtreme};
    if (index < 0 || index >= int(doa4::graphics::kAntiAliasing.size())) return;
    const char* value = doa4::graphics::kAntiAliasing[index].value;
    rex::Runtime* rt = runtime();
    auto* graphics = rt ? dynamic_cast<rex::graphics::GraphicsSystem*>(rt->graphics_system())
                        : nullptr;
    rex::graphics::CommandProcessor* processor = graphics ? graphics->command_processor() : nullptr;
    if (!processor) {
      REXLOG_WARN("[doa4][GFX] post effect -> {}: no graphics system; applies at the next start",
                  value);
      return;
    }
    processor->SetDesiredSwapPostEffect(kEffects[index]);
    REXLOG_INFO("[doa4][GFX] post effect -> {}", value);
  }

  // DOA4_GFX_TEST=cycle: 20 s after start, switch FXAA, FXAA Extreme, Off two
  // seconds apart on the UI thread, through SetPostEffect as the menu does.
  // For runtime checks only.
  void StartGfxTest() {
    const char* env = std::getenv("DOA4_GFX_TEST");
    if (!env || std::string_view(env) != "cycle") return;
    REXLOG_INFO("[doa4][GFX] test cycle on");
    gfx_test_thread_ = std::thread([this] {
      using namespace std::chrono_literals;
      auto nap = [this](int tenths) {
        for (int i = 0; i < tenths && !gfx_stop_.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(100ms);
        }
        return !gfx_stop_.load(std::memory_order_acquire);
      };
      if (!nap(200)) return;
      for (int index : {1, 2, 0}) {
        app_context().CallInUIThreadDeferred([this, index] { SetPostEffect(index); });
        if (!nap(20)) return;
      }
    });
  }

  void ToggleMenu() {
    if (setup_) return;  // the setup screen is up: nothing else to show yet
    if (menu_) {
      CloseMenu();
      return;
    }
    if (!drawer_ || !window()) return;
    doa4::MenuHost host;
    host.window = window();
    host.config_path = rex::filesystem::GetExecutableFolder() / (GetName() + ".toml");
    host.running_scale = running_scale_;
    host.saved_scale = &saved_scale_;
    host.running_speakers = running_speakers_;
    host.saved_speakers = &saved_speakers_;
    host.input = [this]() -> rex::input::InputSystem* {
      rex::Runtime* rt = runtime();
      return rt ? dynamic_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
    };
    host.defer = [this](std::function<void()> fn) {
      app_context().CallInUIThreadDeferred(std::move(fn));
    };
    host.close_menu = [this] { CloseMenu(); };
    host.set_post_effect = [this](int index) { SetPostEffect(index); };
    host.photo_result = [this] { return photo_ ? photo_->last_result() : doa4::photo::Result{}; };
    host.photo_default_dir = doa4::photo::DefaultPhotoDir();
    if (host.photo_default_dir.empty()) host.photo_default_dir = UserData() / "photos";
    host.guest = guest_ready_ ? &guest_ : nullptr;
    host.patcher = &patcher_;
    host.protect = protect_;
    host.save_dir = SaveFolder();
    host.backup_root = BackupFolder();
    host.extras = &extras_;
    host.refuse = [this](std::string_view action, std::string* status) {
      // the same predicate as the Extras controls' disabled state (online_view::LockedInSession)
      return doa4::online_view::Refuse(doa4::online_view::Control::kExtras, SessionLocked(), action, status,
                                       [](const std::string& line) { REXLOG_INFO("{}", line); });
    };
    host.session_locked = [this] { return SessionLocked(); };
    host.online.state = [this](bool tab_shown) { return OnlineViewIn(tab_shown); };
    host.online.host = [this] {
      EnsureLobby();
      if (lobby_) lobby_->Host(0);
    };
    host.online.join = [this](const std::string& code) {
      EnsureLobby();
      if (lobby_) lobby_->Join(code);
    };
    host.online.reach_out = [this](const std::string& code) {
      if (lobby_) lobby_->ReachOut(code);
    };
    host.online.disconnect = [this] {
      if (lobby_) lobby_->Disconnect();
    };
    host.online.start_session = [this] {
      if (lobby_) lobby_->StartSession();
    };
    host.online.leave_session = [] { doa4::np::RequestLeave(); };
    host.online.set_delay = [this](bool automatic, int frames) {
      if (lobby_) lobby_->SetDelay(automatic, frames);
    };
    host.online.shown_changed = [this] { UpdateNetOverlay(); };
    host.game_files = game_files_;
    host.game_files_from_command_line = game_files_from_command_line_;
    host.user_data = UserData();
    host.change_game_files = [this] { ChangeGameFiles(); };
    menu_ = std::make_unique<doa4::MenuDialog>(drawer_, std::move(host));
    menu_open_.store(true, std::memory_order_release);
    REXLOG_INFO("[doa4][UI] menu opened");
  }

  void CloseMenu() {
    if (!menu_) return;
    menu_open_.store(false, std::memory_order_release);
    menu_.reset();
    REXLOG_INFO("[doa4][UI] menu closed");
  }

  // The game sees an untouched controller and keyboard while the menu is open
  // (the button pressed to remap something must not also act in the game).
  // Called from the guest's input polls: reads only the atomic.
  bool OnBlocksGuestInput() override {
    return menu_open_.load(std::memory_order_acquire) ||
           setup_open_.load(std::memory_order_acquire);
  }

  void ToggleDiagOverlay(rex::ui::ImGuiDrawer* drawer) {
    if (diag_overlay_) {
      diag_overlay_.reset();
      REXLOG_INFO("[doa4][UI] DiagOverlayDialog closed");
    } else {
      diag_overlay_ = std::make_unique<doa4::DiagOverlayDialog>(drawer);
      REXLOG_INFO("[doa4][UI] DiagOverlayDialog attached");
    }
  }

  // Carry 2b: a session applies the host's guest settings to the cvars; nothing may save them into the player's config
  // (the settings overlay's "Save to config", a menu save). The file must be byte-identical after the session: if it is
  // not, the snapshot taken at OnPostInitLogging is written back and a WARN line goes to np_log (the harness fails it).
  void CheckConfigUnchanged() {
    std::error_code ec;
    std::string now;
    bool has = false;
    {
      std::ifstream f(ConfigPath(), std::ios::binary);
      if (f) {
        has = true;
        now.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      }
    }
    if (has == toml_had_ && now == toml_snapshot_) return;
    if (toml_had_) {  // fix round M3: a temp file, then a rename over the config
      const std::filesystem::path tmp = ConfigPath().wstring() + L".np_restore";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << toml_snapshot_;
      }
      MoveFileExW(tmp.c_str(), ConfigPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } else {
      std::filesystem::remove(ConfigPath(), ec);
    }
    doa4::np::LogLine("WARN session: the config file " + doa4::Utf8Generic(ConfigPath()) +
                      " changed during the session (host-imposed settings must never be saved): restored");
  }

 private:
  // Netplay (Task 12)
  std::filesystem::path test_dir_;                                // DOA4_NP_TEST_DIR, or empty
  std::optional<doa4::np::handoff::Handoff> session_;             // this process was started for a session
  std::filesystem::path session_user_data_;                       // <U>/netplay/session once the runtime is redirected
  std::unique_ptr<doa4::np::online::Lobby> lobby_;                // the lobby, or the session lobby
  std::thread lobby_watch_;                                       // test dir: waits for the first np_ctl.on
  std::atomic<bool> lobby_watch_stop_{false};
  std::string toml_snapshot_;
  bool toml_had_ = false;
  bool guest_touched_ = false;   // a host-imposed setting was applied in this process (fix round I1)
  bool launch_refused_ = false;  // the session start was aborted: no runtime / no module launch (I1, I2)

  // First-run setup (doa4_setup_dialog.h): the screen while it is open, and
  // the folder the game started from.
  std::unique_ptr<doa4::SetupDialog> setup_;
  std::atomic<bool> setup_open_{false};  // read by the guest's input polls
  std::filesystem::path game_files_;
  std::filesystem::path user_data_;  // the resolved save folder (WithUserData)
  bool game_files_from_command_line_ = false;
  std::unique_ptr<doa4::DiagOverlayDialog> diag_overlay_;
  std::unique_ptr<doa4::NetOverlay> net_overlay_;  // the banner / F9 box; exists only while one is shown
  std::atomic<bool> net_closing_{false};           // set at shutdown: no new overlay
  std::string net_logged_banner_;                  // the last banner text logged (UI thread)
  std::string net_logged_stats_;                   // the last "stats:" line logged (UI thread)
  uint32_t net_build_hashes_ = 0;                  // how many times the build id was hashed (UI thread)
  std::thread menu_tick_;                          // DOA4_MENU_OPEN=1 only: polls the menu's net state
  std::atomic<bool> menu_tick_stop_{false};
  uint64_t net_build_id_ = 0;                      // np::BuildIdValue(), hashed on first use (UI thread)
  std::unique_ptr<doa4::MenuDialog> menu_;
  std::atomic<bool> menu_open_{false};
  rex::ui::ImGuiDrawer* drawer_ = nullptr;
  int running_scale_ = 1;
  std::string running_speakers_ = "auto";  // audio_speakers at start (read once by the driver)
  std::string saved_speakers_ = "auto";     // what the menu last saved for the next start
  int saved_scale_ = 1;  // what the menu last saved for the next start
  // Extras tab (doa4_unlocks.h, doa4_tweaks.h): set up at OnPostLoadXexImage.
  doa4::GuestBytes guest_;
  bool guest_ready_ = false;
  doa4::tweaks::TweakPatcher patcher_;
  doa4::tweaks::ProtectFn protect_;
  doa4::ExtrasState extras_;
  std::atomic<bool> extras_stop_{false};
  std::thread extras_thread_;  // DOA4_EXTRAS_TEST only
  std::atomic<bool> gfx_stop_{false};
  std::thread gfx_test_thread_;  // DOA4_GFX_TEST only
  std::unique_ptr<doa4::photo::Service> photo_;
  std::unique_ptr<doa4::PhotoNotice> photo_notice_;
  uint64_t photo_notice_serial_ = 0;
  std::atomic<bool> photo_closing_{false};  // set at shutdown: no new notices
  std::atomic<bool> photo_stop_{false};
  std::thread photo_test_thread_;  // DOA4_PHOTO_TEST only
  std::unique_ptr<doa4::photo::SnapshotWatcher> photo_watcher_;  // used by the swap hook
  std::atomic<bool> snap_stop_{false};
  std::thread snap_thread_;

  // Guest memory access for the Extras tab, the game tweaks the config turns
  // on (applied before any guest code runs, as Xenia applies patches), and the
  // DOA4_EXTRAS_TEST watcher.
  void SetUpExtras() {
    rex::Runtime* rt = runtime();
    rex::memory::Memory* memory = rt ? rt->memory() : nullptr;
    if (!memory) {
      REXLOG_WARN("[doa4][EXTRAS] no guest memory; Extras tab disabled");
      return;
    }
    guest_.read = [memory](uint32_t address) {
      return *memory->TranslateVirtual<uint8_t*>(address);
    };
    guest_.write = [memory](uint32_t address, uint8_t value) {
      *memory->TranslateVirtual<uint8_t*>(address) = value;
    };
    protect_ = [memory](uint32_t address, uint32_t size, bool writable) {
      rex::memory::BaseHeap* heap = memory->LookupHeap(address);
      const uint32_t access =
          writable ? (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)
                   : rex::memory::kMemoryProtectRead;
      return heap && heap->Protect(address, size, access);
    };
    patcher_.CaptureOriginals(guest_);
    guest_ready_ = true;
    const auto& tweaks = doa4::tweaks::AllTweaks();
    for (size_t i = 0; i < tweaks.size(); ++i) {
      if (!rex::cvar::Query<bool>(tweaks[i].cvar)) continue;
      std::string error = patcher_.Apply(guest_, i, true, protect_);
      const bool in_memory = doa4::tweaks::TweakPatcher::IsOn(guest_, i);
      if (error.empty() && in_memory) {
        REXLOG_INFO("[doa4][EXTRAS] {} applied at load", tweaks[i].cvar);
      } else {
        REXLOG_ERROR("[doa4][EXTRAS] {} at load: {} (bytes in memory: {})", tweaks[i].cvar,
                     error.empty() ? "no error" : error, in_memory);
      }
    }
    StartExtrasTestWatcher();
  }

  // <user_data_root>/<XUID>/544307D1/00000001/ups.dat, or empty before the
  // kernel has a profile.
  std::filesystem::path SaveFolder() const {
    auto* ks = rex::system::kernel_state();
    const uint64_t xuid = ks && ks->user_profile() ? ks->user_profile()->xuid() : 0;
    return doa4::SaveDir(UserData(), xuid);
  }

  // <user_data_root>/doa4_save_backups: outside the folder the game lists its
  // saves from (it would see a backup as a second save package).
  std::filesystem::path BackupFolder() const {
    return UserData().empty() ? std::filesystem::path()
                                    : UserData() / "doa4_save_backups";
  }

  std::string DumpProfile(uint32_t profile) const {
    std::string out;
    for (uint32_t offset = doa4::unlocks::kFirstStoryClear; offset < 0x404C; ++offset) {
      out += fmt::format("{:02x}", guest_.Read(profile + offset));
    }
    return out;
  }

  // DOA4_EXTRAS_TEST=<anything>: log every change of the profile pointer, slot
  // and FindProfile's answer. =everything: also unlock everything once, with
  // the backup, the first time a profile is found, and log the bytes before and
  // after and when the game rewrites ups.dat. For runtime checks only.
  void StartExtrasTestWatcher() {
    const char* env = std::getenv("DOA4_EXTRAS_TEST");
    if (!env || !env[0]) return;
    const bool unlock_everything = std::string_view(env) == "everything";
    REXLOG_INFO("[doa4][EXTRAS] test watcher on (DOA4_EXTRAS_TEST={})", env);
    extras_thread_ = std::thread([this, unlock_everything] {
      using namespace std::chrono_literals;
      std::string last;
      bool unlocked = false;
      bool saved_logged = false;
      doa4::ExtrasState state;  // its own: extras_ belongs to the UI thread
      while (!extras_stop_.load(std::memory_order_acquire)) {
        const uint32_t pointer = guest_.ReadBe32(doa4::unlocks::kProfilePointer);
        const uint32_t slot = guest_.ReadBe32(doa4::unlocks::kProfileSlot);
        const std::optional<uint32_t> profile = doa4::unlocks::FindProfile(guest_);
        std::string now = fmt::format("profile pointer {:#010x} slot {} -> {}", pointer, slot,
                                      profile ? fmt::format("{:#010x}", *profile) : "none");
        if (now != last) {
          REXLOG_INFO("[doa4][EXTRAS] {}", now);
          last = std::move(now);
        }
        if (unlock_everything && !unlocked && profile) {
          unlocked = true;
          REXLOG_INFO("[doa4][EXTRAS] before {}", DumpProfile(*profile));
          std::string error;
          if (doa4::UnlockWithBackup(guest_, *profile, doa4::unlocks::Unlock::kEverything,
                                     SaveFolder(), BackupFolder(), doa4::BackupStamp(),
                                     &state, &error)) {
            REXLOG_INFO("[doa4][EXTRAS] test unlock: {} backup: {}", state.last_change,
                        state.backup.empty() ? state.backup_note : state.backup.string());
            REXLOG_INFO("[doa4][EXTRAS] after  {}", DumpProfile(*profile));
          } else {
            REXLOG_ERROR("[doa4][EXTRAS] test unlock: {}", error);
          }
        }
        if (unlocked && !saved_logged) {
          doa4::PollSaved(guest_, SaveFolder(), &state);
          if (state.saved) {
            saved_logged = true;
            REXLOG_INFO("[doa4][EXTRAS] the game rewrote ups.dat");
          }
        }
        for (int i = 0; i < 10 && !extras_stop_.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(100ms);
        }
      }
    });
  }

  // DOA4 accesses its data via paths like d:\chrone.afs, D:\Sound\... and
  // d:\dic\... where the disc root *is* the game data directory (NOT a
  // parent containing a DOA4\ subdir). Mount our game_data_root directly at
  // \Device\Cdrom0 and point d: there.
  void MountDiscRoot() {
    auto* rt = runtime();
    if (!rt || !rt->file_system()) return;
    std::filesystem::path game_root = std::filesystem::absolute(rt->game_data_root());
    if (game_root.empty()) {
      REXLOG_WARN("[doa4][DISC] game_data_root empty; leaving d: as-is");
      return;
    }
    const char* disc_mount = "\\Device\\Cdrom0";
    auto disc = std::make_unique<rex::filesystem::HostPathDevice>(disc_mount, game_root, true);
    if (!disc->Initialize() || !rt->file_system()->RegisterDevice(std::move(disc))) {
      REXLOG_WARN("[doa4][DISC] failed to mount {} at {}", game_root.string(), disc_mount);
      return;
    }
    rt->file_system()->UnregisterSymbolicLink("d:");
    rt->file_system()->RegisterSymbolicLink("d:", disc_mount);
    REXLOG_INFO("[doa4][DISC] mounted {} at {}; d: -> {} (d:\\foo == game:\\foo)",
                game_root.string(), disc_mount, disc_mount);
  }

  // DOA4 opens cache:\chrone.afs, cmn.afs, mot.afs, etc. and treats failure
  // as fatal (KeBugCheck(14)). On Xbox 360 these are pre-staged from the
  // game disc into the HDD cache partition; for us the original files live
  // in the game data directory, so just symlink cache: to the same place.
  void MountCacheDevice() {
    auto* rt = runtime();
    if (!rt || !rt->file_system()) return;
    rt->file_system()->RegisterSymbolicLink("cache:", "\\Device\\Harddisk0\\Partition1");
    REXLOG_INFO("[doa4][CACHE] symlinked cache: -> \\Device\\Harddisk0\\Partition1 (game data)");
  }

  static std::string ReadGuestString(uint32_t addr, size_t max_len = 128) {
    if (addr == 0) return "<null>";
    auto* ks = rex::system::kernel_state();
    if (!ks || !ks->memory()) return "<no-base>";
    auto* base = ks->memory()->virtual_membase();
    if (!base) return "<no-base>";
    std::string out;
    out.reserve(max_len);
    for (size_t i = 0; i < max_len; ++i) {
      uint8_t c = base[addr + i];
      if (c == 0) break;
      if (c < 0x20 || c > 0x7e) {
        out.push_back('?');
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
    return out;
  }

  void DumpGuestStrings() {
    // Assertion string-literal triplets passed to sub_82892670 by various
    // getters. lis -32254 -> 0x82020000 base.
    auto base_addr = [](int32_t off) -> uint32_t { return 0x82020000u + (uint32_t)off; };
    struct Entry { const char* tag; uint32_t addr; };
    Entry exact[] = {
        {"828980E0.file(20808)", base_addr(20808)},
        {"828980E0.func(27924)", base_addr(27924)},
        {"828980E0.msg (28760)", base_addr(28760)},
        {"828976D8.file(20808)", base_addr(20808)},
        {"828976D8.func(28180)", base_addr(28180)},
        {"828976D8.msg (27944)", base_addr(27944)},
    };
    for (auto& e : exact) {
      REXLOG_INFO("[doa4][STR] {} @ {:#x} = '{}'", e.tag, e.addr,
                  ReadGuestString(e.addr, 200));
    }
  }

  void StartThreadSnapshotter() {
    REXLOG_INFO("[doa4][SNAP] starting host snapshotter thread");
    snap_thread_ = std::thread([this]() {
      using namespace std::chrono_literals;
      REXLOG_INFO("[doa4][SNAP] host thread alive");
      // Let guest threads spin up + game to reach the SSM shader-compile gate.
      std::this_thread::sleep_for(8000ms);

      auto symbolize = [](uint32_t addr) -> std::string {
        if (!addr) return "0";

        // Build (addr, name) table on first call by parsing the codegen-
        // generated init header for `DECLARE_REX_FUNC(sub_XXXXXXXX)` lines.
        // ~50k entries; one-time cost.
        static std::vector<std::pair<uint32_t, std::string>> syms;
        static std::once_flag init_once;
        std::call_once(init_once, [] {
          std::vector<std::filesystem::path> candidates = {
              "../../../generated/default/dead_or_alive_4_init.h",
              "generated/default/dead_or_alive_4_init.h",
              "E:/Downloads/doa4/rexglue-sdk/deadoralive/generated/default/"
              "dead_or_alive_4_init.h",
          };
          std::filesystem::path found;
          for (auto& c : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(c, ec)) { found = c; break; }
          }
          if (found.empty()) {
            REXLOG_WARN("[doa4][SYM] init.h not found; symbols disabled");
            return;
          }
          std::ifstream in(found);
          std::string line;
          std::regex re(R"(sub_([0-9A-Fa-f]{8}))");
          while (std::getline(in, line)) {
            std::smatch m;
            if (std::regex_search(line, m, re)) {
              uint32_t a = (uint32_t)std::stoul(m[1].str(), nullptr, 16);
              syms.emplace_back(a, "sub_" + m[1].str());
            }
          }
          std::sort(syms.begin(), syms.end());
          syms.erase(std::unique(syms.begin(), syms.end(),
                                 [](auto& a, auto& b) { return a.first == b.first; }),
                     syms.end());
          REXLOG_INFO("[doa4][SYM] loaded {} symbols from {}", syms.size(),
                      found.string());
        });

        if (syms.empty()) return fmt::format("{:#x}", addr);
        // upper_bound -> first > addr; predecessor is largest <= addr.
        auto it = std::upper_bound(syms.begin(), syms.end(), addr,
                                   [](uint32_t v, auto& p) { return v < p.first; });
        if (it == syms.begin()) return fmt::format("{:#x}", addr);
        --it;
        uint32_t off = addr - it->first;
        if (off > 0x10000) return fmt::format("{:#x}", addr);  // probably not in this func
        return fmt::format("{}+{:#x}", it->second, off);
      };

      // Steady state emits one summary line per round (main guest thread's
      // PC + running count) — enough to spot a wedged main thread without the
      // 39-line-per-round flood. The full per-thread dump runs ONCE on round 0
      // to confirm every guest thread spun up, and again on demand when the
      // running-thread count collapses (likely-hang signal).
      // Sleeps end early once OnShutdown sets snap_stop_ (it joins this
      // thread, and a plain 5 s sleep held every window close for up to 5 s).
      auto nap = [this] {
        for (int i = 0; i < 50 && !snap_stop_.load(std::memory_order_acquire); ++i) {
          std::this_thread::sleep_for(100ms);
        }
      };
      size_t prev_running = 0;
      for (int round = 0; round < 240 && !snap_stop_.load(std::memory_order_acquire); ++round) {
        auto* ks = rex::system::kernel_state();
        auto* ot = ks ? ks->object_table() : nullptr;
        if (!ot) { nap(); continue; }
        std::vector<rex::system::object_ref<rex::system::XThread>> threads;
        try { threads = ot->GetObjectsByType<rex::system::XThread>(); }
        catch (...) { nap(); continue; }

        uint32_t main_lr = 0;
        size_t guest_total = 0, guest_running = 0;
        for (auto& t : threads) {
          if (!t || !t->is_guest_thread()) continue;
          ++guest_total;
          if (t->is_running()) ++guest_running;
          if (t->main_thread()) {
            auto* ts = t->thread_state();
            auto* pctx = ts ? ts->context() : nullptr;
            if (pctx) main_lr = (uint32_t)pctx->lr;
          }
        }

        // Full dump only on first round or when running count drops sharply.
        bool collapse = round > 0 && prev_running >= 2 && guest_running * 2 < prev_running;
        if (round == 0 || collapse) {
          REXLOG_INFO("[doa4][SNAP] full dump (round {}{}) guest={} running={}",
                      round, collapse ? ", running collapsed" : "", guest_total, guest_running);
          for (auto& t : threads) {
            if (!t || !t->is_guest_thread()) continue;
            auto* ts = t->thread_state();
            auto* pctx = ts ? ts->context() : nullptr;
            uint32_t lr = pctx ? (uint32_t)pctx->lr : 0u;
            uint32_t r1 = pctx ? (uint32_t)pctx->r1.u64 : 0u;
            REXLOG_INFO("[doa4][SNAP]  tid={:2} {}{} lr={} r1={:#x}",
                        t->thread_id(), t->main_thread() ? "M" : " ",
                        t->is_running() ? "R" : "-", symbolize(lr), r1);
          }
        } else {
          REXLOG_INFO("[doa4][SNAP] guest run={}/{} main_lr={}",
                      guest_running, guest_total, symbolize(main_lr));
        }
        prev_running = guest_running;
        nap();
      }
    });
  }
};

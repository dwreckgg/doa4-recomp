// Full-quality photos: the service that turns a captured frame into files.
//
// No SDK headers: the app hands it the frame capture, the monitor size, the
// settings and a result callback, so a host test drives the whole pipeline.
// One worker thread does the capturing (for the hotkey), cropping, resizing
// and encoding, so the UI and game threads only queue work.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "doa4_photo.h"
#include "doa4_photo_win.h"

namespace doa4::photo {

struct Settings {
  Format format = Format::kPngMonitor;
  Framing framing = Framing::kCrop;  // for copies of in-game snapshots
  std::filesystem::path dir;         // never empty when handed to the service
};

enum class Source {
  kHotkey,    // the whole 16:9 frame
  kSnapshot,  // a copy of an in-game snapshot, framed by Settings::framing
  kDebug,     // DOA4_PHOTO_DEBUG frames: the whole frame, the given suffix
};

// One file to write.
struct Output {
  Image image;
  std::string suffix;  // "" for the hotkey
  FileType type;
};

// The files one captured frame becomes, by source and settings: the framing
// (snapshot copies), then the format (png_monitor fits the image inside the
// monitor without upscaling; *_full keep the captured size).
std::vector<Output> PlanOutputs(const Image& frame, Source source, const std::string& suffix,
                                const Settings& settings, Size monitor);

struct Result {
  uint64_t serial = 0;  // 1 for the first result, +1 per result
  bool error = false;
  std::string text;               // "Saved DOA4_....png" or what went wrong
  std::filesystem::path path;     // the last file written
};

class Service {
 public:
  struct Hooks {
    std::function<bool(Image*)> capture;             // the current frame (any thread)
    std::function<Size()> monitor;                   // the monitor's size
    std::function<Settings()> settings;              // read when a request is made
    std::function<std::tm()> now;                    // local time, for file names
    std::function<void(const Result&)> on_result;    // called on the worker thread
  };

  explicit Service(Hooks hooks);
  ~Service();

  // Captures the current frame on the worker and saves it (F12).
  void RequestHotkeyPhoto();
  // Saves a frame already captured (in-game snapshot copies, debug frames).
  void SubmitCaptured(Image frame, Source source, std::string suffix = "");
  // Waits until every queued request has been handled.
  void Flush();
  // Stops the worker after the request in hand; returns how many queued
  // requests were dropped. Called by the destructor too.
  size_t Stop();

  Result last_result() const;

 private:
  struct Job {
    bool capture = false;  // capture on the worker (hotkey)
    Image frame;
    Source source = Source::kHotkey;
    std::string suffix;
    Settings settings;
  };
  void Run();
  void Handle(Job& job);
  void Report(bool error, std::string text, std::filesystem::path path);

  Hooks hooks_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  std::deque<Job> jobs_;
  bool busy_ = false;
  bool stop_ = false;
  Result last_;
  std::thread worker_;
};

}  // namespace doa4::photo

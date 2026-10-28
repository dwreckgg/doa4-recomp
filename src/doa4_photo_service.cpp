// Full-quality photos: the service. See doa4_photo_service.h.

#include "doa4_photo_service.h"

#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace doa4::photo {

namespace {

const char* Extension(FileType type) { return type == FileType::kPng ? "png" : "jpg"; }

}  // namespace

std::vector<Output> PlanOutputs(const Image& frame, Source source, const std::string& suffix,
                                const Settings& settings, Size monitor) {
  // Framing first.
  std::vector<std::pair<Image, std::string>> framed;
  switch (source) {
    case Source::kHotkey:
      framed.emplace_back(frame, "");
      break;
    case Source::kDebug:
      framed.emplace_back(frame, suffix);
      break;
    case Source::kSnapshot:
      if (settings.framing != Framing::kFull) {
        framed.emplace_back(Crop(frame, GameCropRect(frame.width, frame.height)), "photo");
      }
      if (settings.framing != Framing::kCrop) framed.emplace_back(frame, "full");
      break;
  }
  // Then the format.
  std::vector<Output> outputs;
  for (auto& [image, name] : framed) {
    Output out;
    out.suffix = std::move(name);
    out.type = settings.format == Format::kJpgFull ? FileType::kJpg : FileType::kPng;
    if (settings.format == Format::kPngMonitor) {
      out.image = Resize(image, FitWithin({image.width, image.height}, monitor));
    } else {
      out.image = std::move(image);
    }
    outputs.push_back(std::move(out));
  }
  return outputs;
}

Service::Service(Hooks hooks) : hooks_(std::move(hooks)), worker_([this] { Run(); }) {}

Service::~Service() { Stop(); }

void Service::RequestHotkeyPhoto() {
  Job job;
  job.capture = true;
  job.source = Source::kHotkey;
  job.settings = hooks_.settings();
  std::lock_guard<std::mutex> lock(mutex_);
  if (stop_) return;
  jobs_.push_back(std::move(job));
  wake_.notify_one();
}

void Service::SubmitCaptured(Image frame, Source source, std::string suffix) {
  Job job;
  job.frame = std::move(frame);
  job.source = source;
  job.suffix = std::move(suffix);
  job.settings = hooks_.settings();
  std::lock_guard<std::mutex> lock(mutex_);
  if (stop_) return;
  jobs_.push_back(std::move(job));
  wake_.notify_one();
}

void Service::Flush() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return (jobs_.empty() && !busy_) || stop_; });
}

size_t Service::Stop() {
  size_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_ && !worker_.joinable()) return 0;
    stop_ = true;
    dropped = jobs_.size();
    jobs_.clear();
    wake_.notify_all();
    idle_.notify_all();
  }
  if (worker_.joinable()) worker_.join();
  return dropped;
}

Result Service::last_result() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_;
}

void Service::Run() {
  // Background priority (CPU, memory and disk): capturing and encoding a x4
  // frame (59 MB) must not take time from the game's threads. Measured at x4
  // before this: up to 280 of 600 game frames over 17.5 ms around a photo.
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
      busy_ = true;
    }
    Handle(job);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      busy_ = false;
      if (jobs_.empty()) idle_.notify_all();
    }
  }
}

void Service::Handle(Job& job) {
  if (job.capture && !hooks_.capture(&job.frame)) {
    Report(true, "No frame to capture yet.", {});
    return;
  }
  if (job.frame.width == 0 || job.frame.height == 0) {
    Report(true, "No frame to capture yet.", {});
    return;
  }
  const std::tm now = hooks_.now();
  std::string saved;
  std::filesystem::path last_path;
  for (const Output& out :
       PlanOutputs(job.frame, job.source, job.suffix, job.settings, hooks_.monitor())) {
    const std::string name = PhotoFileName(now, out.suffix, Extension(out.type));
    const std::filesystem::path path = UniquePath(job.settings.dir, name);
    std::string error = WriteImage(out.image, path, out.type);
    if (!error.empty()) {
      Report(true, "Photo not saved: " + error, {});
      return;
    }
    saved += (saved.empty() ? "" : ", ") + path.filename().string();
    last_path = path;
  }
  Report(false, "Saved " + saved, last_path);
}

void Service::Report(bool error, std::string text, std::filesystem::path path) {
  Result result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_.serial += 1;
    last_.error = error;
    last_.text = std::move(text);
    last_.path = std::move(path);
    result = last_;
  }
  if (hooks_.on_result) hooks_.on_result(result);
}

}  // namespace doa4::photo

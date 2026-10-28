// The first-run setup screen's logic. See doa4_setup_flow.h.

#include "doa4_setup_flow.h"

#include <cstdio>

#include "doa4_utf8.h"

namespace doa4::setup {

namespace fs = std::filesystem;
using gamefiles::Verdict;

std::string FormatGB(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / double(1ull << 30));
  return text;
}

std::string VerdictMessage(Verdict verdict, bool image) {
  switch (verdict) {
    case Verdict::kOk:
      return {};
    case Verdict::kNotXbox:
      return "This file is not an Xbox 360 disc image.";
    case Verdict::kDamaged:
      return "This disc image is damaged or incomplete.";
    case Verdict::kNoXex:
      return image ? "This disc image does not contain default.xex."
                   : "This folder does not contain default.xex. Choose the folder that holds "
                     "default.xex.";
    case Verdict::kWrongVersion:
      return "This is not the supported version. Dead or Alive 4 recomp needs " +
             std::string(gamefiles::kSupportedDisc) + ": its default.xex has SHA-1 " +
             std::string(gamefiles::kSupportedXexSha1) +
             ". The recompiled code matches that exact file; another version would crash.";
    case Verdict::kUnreadable:
      return image ? "The disc image could not be read." : "default.xex could not be read.";
  }
  return {};
}

void Flow::ChooseImage() {
  if (stage_ != Stage::kChoose) return;
  const auto iso = fx_.pick_iso(last_dir_);
  if (!iso) return;
  last_dir_ = iso->parent_path();
  message_.clear();
  stage_ = Stage::kChecking;
  const fs::path path = *iso;
  fx_.run_async([this, path] {
    gamefiles::ImageCheck check = fx_.check_image(path);
    fx_.post([this, path, check = std::move(check)]() mutable {
      OnImageChecked(path, std::move(check));
    });
  });
}

void Flow::ChooseFolder() {
  if (stage_ != Stage::kChoose) return;
  const auto dir = fx_.pick_folder(last_dir_);
  if (!dir) return;
  last_dir_ = *dir;
  message_.clear();
  stage_ = Stage::kChecking;
  const fs::path path = *dir;
  fx_.run_async([this, path] {
    const Verdict verdict = fx_.check_folder(path);
    fx_.post([this, path, verdict] { OnFolderChecked(path, verdict); });
  });
}

void Flow::OnImageChecked(const fs::path& iso, gamefiles::ImageCheck check) {
  if (check.verdict != Verdict::kOk) {
    stage_ = Stage::kChoose;
    message_ = VerdictMessage(check.verdict, true);
    return;
  }
  image_ = iso;
  volume_ = std::move(check.volume);
  target_ = iso.parent_path() / "DOA4";
  stage_ = Stage::kTarget;
  InspectTarget();
}

void Flow::OnFolderChecked(const fs::path& dir, Verdict verdict) {
  if (verdict != Verdict::kOk) {
    stage_ = Stage::kChoose;
    message_ = VerdictMessage(verdict, false);
    return;
  }
  stage_ = Stage::kFinished;
  fx_.done(dir);
}

void Flow::InspectTarget() {
  target_note_.clear();
  free_ = fx_.free_bytes(target_);
  switch (fx_.inspect_target(target_)) {
    case gamefiles::TargetState::kNotEmpty:
      target_note_ =
          "This folder is not empty. Choose an empty or new folder, or go back and use "
          "\"Choose extracted folder\" if the game is already there.";
      return;
    case gamefiles::TargetState::kNotAFolder:
      target_note_ = "This is a file, not a folder. Choose another place.";
      return;
    default:
      break;
  }
  if (!free_) {
    target_note_ = "The free space on this drive could not be read. Choose another place.";
  } else if (*free_ < need_bytes()) {
    target_note_ = "Not enough space: needs " + FormatGB(need_bytes()) + ", " +
                   FormatGB(*free_) + " free.";
  }
}

void Flow::ChangeTarget() {
  if (stage_ != Stage::kTarget) return;
  const auto dir = fx_.pick_folder(target_.parent_path());
  if (!dir) return;
  target_ = *dir;
  message_.clear();
  InspectTarget();
}

void Flow::Extract() {
  if (!CanExtract()) return;
  message_.clear();
  cancel_ = false;
  {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_ = {0, volume_.file_bytes, ""};
  }
  stage_ = Stage::kExtracting;
  fx_.run_async([this, iso = image_, target = target_, volume = volume_] {
    gamefiles::ExtractResult result =
        fx_.extract(iso, volume, target, cancel_, [this](const gamefiles::Progress& p) {
          std::lock_guard<std::mutex> lock(progress_mutex_);
          progress_ = p;
        });
    fx_.post([this, result = std::move(result)]() mutable { OnExtracted(std::move(result)); });
  });
}

void Flow::OnExtracted(gamefiles::ExtractResult result) {
  switch (result.outcome) {
    case gamefiles::ExtractOutcome::kDone:
      stage_ = Stage::kFinished;
      fx_.done(target_);
      return;
    case gamefiles::ExtractOutcome::kCancelled:
      message_ = "Extraction cancelled; the partial copy was removed.";
      break;
    case gamefiles::ExtractOutcome::kFailed:
      message_ = result.error;
      break;
  }
  stage_ = Stage::kTarget;
  InspectTarget();
}

void Flow::Cancel() {
  if (stage_ == Stage::kExtracting) cancel_ = true;
}

void Flow::Back() {
  if (stage_ != Stage::kTarget) return;
  stage_ = Stage::kChoose;
  message_.clear();
}

gamefiles::Progress Flow::progress() const {
  std::lock_guard<std::mutex> lock(progress_mutex_);
  return progress_;
}

}  // namespace doa4::setup

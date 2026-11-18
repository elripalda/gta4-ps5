// GTA IV PS5 - the hub's installer (see hub_ui.h, hub_install.h): PLAY
// opens it while the game isn't installed, and the Episodes page for an
// episode. It shows what's in install/ and the disc image's (or the episode
// package's) check, then installs with a progress bar. All on the controller;
// the hub's music keeps playing.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "hub_lang.h"
#include "hub_style.h"
#include "hub_ui.h"

namespace gta4ps5::hub {

using lang::F;
using lang::T;

namespace {

using Phase = install::Job::Phase;

enum RowStatus { kRowChecking, kRowOk, kRowBad, kRowNone };

int Minutes(float seconds) { return int(std::ceil(seconds / 60.0f)); }

std::string Rate(float bytes_per_second) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f MB/s", double(bytes_per_second) / double(1 << 20));
  return lang::Units(buf);
}

}  // namespace

std::filesystem::path Hub::InstallRoot() const {
  return paths_.install.empty() ? paths_.app / "UserData" / "LibertyRecomp" : paths_.install;
}

bool Hub::Busy() const { return (installer_ && job_phase_ == Phase::kInstalling) || tex_importing_; }

void Hub::OpenInstaller(int episode) {
  installer_ = true;
  installer_episode_ = episode;
  job_ = std::make_unique<install::Job>(paths_.app, InstallRoot(), episode);
  job_phase_ = Phase::kChecking;
  drive_ = install::DriveOf(InstallRoot());
  free_space_ = drive_.type.empty() ? install::FreeSpace(InstallRoot()) : drive_.free;
  shown_progress_ = 0;
  page_in_ = 0;
  page_dir_ = 1;
  focus_y_ = -1;
}

void Hub::CloseInstaller() {
  installer_ = false;
  job_.reset();
  LookAgain();
  page_in_ = 0;
  page_dir_ = -1;
}

// ---------------------------------------------------------------- drawing

void Hub::Spinner(float cx, float cy, float r, ImU32 col) {
  const float a = time_ * 5.5f;
  dl_->PathArcTo(P(cx, cy), r * s_, a, a + 4.4f, 32);
  dl_->PathStroke(col, 0, 3.2f * s_);
}

void Hub::Tick(float cx, float cy, float size, ImU32 col) {
  const float k = size / 20;
  dl_->PathLineTo(P(cx - 9 * k, cy + 0.5f * k));
  dl_->PathLineTo(P(cx - 3 * k, cy + 6.5f * k));
  dl_->PathLineTo(P(cx + 9.5f * k, cy - 6.5f * k));
  dl_->PathStroke(col, 0, 3.4f * k * s_);
}

void Hub::XMark(float cx, float cy, float size, ImU32 col) {
  const float k = size / 20 * 7;
  dl_->AddLine(P(cx - k, cy - k), P(cx + k, cy + k), col, 3.4f * size / 20 * s_);
  dl_->AddLine(P(cx - k, cy + k), P(cx + k, cy - k), col, 3.4f * size / 20 * s_);
}

// One of the setup's rows: what it is, the file, and how it checked out
// (a grey heading, the name, the result at the right; a rule under it).
void Hub::Row3(float y, const char* label, const std::string& name, int status, const std::string& note) {
  const float x = kListX, w = kPanelX - 90 - kListX, h = 104;
  TextMid(Bold(), 21, x, y + 24, kDimGray, Upper(T(label)), 0, 1.6f);
  const float note_w = note.empty() ? 0 : TextWidth(Bold(), 27, note) + 60;
  TextMid(Bold(), 34, x, y + 64, name.empty() ? kDimGray : kText,
          Fit(Bold(), 34, name.empty() ? T("Not found") : name, w - std::max(note_w, 90.0f)));
  const float cy = y + 64, icon_x = x + w - 18;
  switch (status) {
    case kRowChecking: Spinner(icon_x, cy, 13, kMenuGray); break;
    case kRowOk: Tick(icon_x, cy, 24, pal_.title); break;
    case kRowBad: XMark(icon_x, cy, 22, kBad); break;
    default: break;
  }
  if (!note.empty()) TextMid(Bold(), 27, icon_x - 34, cy, status == kRowBad ? kBad : kMenuGray, note, 1);
  Rect(x, y + h - 1, w, 2, Rgba(40, 40, 40));
}

void Hub::DrawInstaller() {
  const install::Job::Status st = job_->Get();
  // what changed since the last frame
  if (st.phase != job_phase_) {
    switch (st.phase) {
      case Phase::kProblem:
      case Phase::kFailed: Play(Sound::kError); break;
      case Phase::kDone:
        Play(Sound::kOn);
        LookAgain();
        break;
      default: break;
    }
    job_phase_ = st.phase;
    panel_in_ = 0;
    if (st.phase == Phase::kCancelled) {
      // back to the setup, looked at again
      job_ = std::make_unique<install::Job>(paths_.app, InstallRoot(), installer_episode_);
      job_phase_ = Phase::kChecking;
      shown_progress_ = 0;
      Toast(T("Install stopped. Nothing was kept."));
      return;
    }
  }
  const install::Sources& found = job_->Found();
  const int ep = installer_episode_;
  // (an episode: its package as the installer found it, else as the folder's scan did)
  const std::string episode_file = ep < 0 ? std::string() : !st.source.empty() ? st.source : found.episode[ep].filename().string();
  const uint64_t episode_bytes = ep < 0 ? 0 : st.source_bytes ? st.source_bytes : found.episode_bytes[ep];
  const uint64_t needed = ep >= 0 ? episode_bytes + (256ull << 20) : install::SpaceNeeded(found.disc_bytes);

  // ---- the setup: what's there, and whether it can install
  if (st.phase == Phase::kChecking || st.phase == Phase::kReady || st.phase == Phase::kProblem) {
    const bool checking = st.phase == Phase::kChecking;
    float y = kListY + 130;
    if (ep >= 0) {
      Heading(T(install::EpisodeName(ep)), T("One time only: the episode is copied off its Xbox 360 package."));
      const bool bad = st.phase == Phase::kProblem && (episode_file.empty() || st.disc.empty());
      Row3(y, "Episode Package", episode_file, checking ? kRowChecking : bad ? kRowBad : kRowOk,
           checking ? T("Checking") : bad ? T(episode_file.empty() ? "Missing" : "Can't use") : st.disc);
      y += 110;
    } else {
      Heading(T("Your game files"), T("One time only: the game is copied off your Xbox 360 disc image."));
      const bool disc_bad = st.phase == Phase::kProblem && (found.disc.empty() || st.disc.empty());
      Row3(y, found.disc.empty() || found.disc.has_extension() ? "Disc Image" : "Game Folder",
           found.disc.filename().string(), checking ? kRowChecking : disc_bad ? kRowBad : kRowOk,
           checking ? T("Checking") : disc_bad ? T(found.disc.empty() ? "Missing" : "Can't use") : st.disc);
      y += 110;
      Row3(y, "Title Update 8", found.update.filename().string(), found.update.empty() ? kRowBad : kRowOk,
           found.update.empty() ? T("Missing") : lang::Units(install::Bytes(found.update_bytes)));
      y += 110;
    }
    // (the free space, when it can be known: a PS5 title can't ask)
    const bool room = !free_space_ || free_space_ >= needed;
    Row3(y, "Space", F("Needs about {} free", {lang::Units(install::Bytes(needed))}),
         !free_space_ ? kRowNone : room ? kRowOk : kRowBad,
         free_space_ ? F("{} free", {lang::Units(install::Bytes(free_space_))}) : "");
    Panel([&](float px, float py, float pw) {
      py += 130;  // (under the heading's line)
      const ImU32 text = Rgba(196, 196, 196);
      if (checking) {
        TextMid(Bold(), 40, px, py + 20, kText, T("Checking"));
        Wrapped(Regular(), 28, px, py + 62, pw, text,
                T(ep >= 0 ? "Making sure the package is the episode. This takes a few seconds."
                          : "Making sure the disc image is the USA disc of GTA IV. This takes a few seconds."),
                1.38f);
      } else if (st.phase == Phase::kReady) {
        TextMid(Bold(), 40, px, py + 20, pal_.title, T("Ready"));
        py += 62;
        py += Wrapped(Regular(), 28, px, py, pw, text,
                      T(ep >= 0 ? "Press Cross to install. Keep the app open until it's done. The package isn't changed."
                                : "Press Cross to install. Keep the app open until it's done. Your disc image isn't changed."),
                      1.38f) + 26;
        TextMid(Bold(), 21, px, py + 12, kDimGray, Upper(T("Installs To")), 0, 1.6f);
        const std::string folder = drive_.folder.empty() ? "/data/homebrew/PPSA94004/UserData" : drive_.folder;
        py += 36;
        py += Wrapped(Bold(), 27, px, py, pw, kText, folder, 1.25f);
        if (!drive_.type.empty()) {
          std::string on =
              F("On {}", {drive_.Name()}) + (drive_.device.empty() ? "" : " (" + drive_.type + ", " + drive_.device + ")");
          Wrapped(Regular(), 26, px, py + 6, pw, kMenuGray, on);
        }
      } else {
        TextMid(Bold(), 40, px, py + 20, kBad, T("Can't install yet"));
        py += 62;
        py += Wrapped(Regular(), 28, px, py, pw, text, st.problem, 1.38f) + 26;
        TextMid(Bold(), 21, px, py + 12, kDimGray, Upper(T("The Install Folder")), 0, 1.6f);
        TextMid(Bold(), 27, px, py + 52, kText, "/data/homebrew/PPSA94004/install/");
      }
    });
    return;
  }

  // ---- installing, done, or stopped
  if (st.phase == Phase::kFailed) {
    Heading(T("The install stopped"), T("What was copied has been removed, so nothing is left half done."));
    const float x = kListX, w = 1100;
    float y = kListY + 140;
    TextMid(Bold(), 21, x, y + 12, kBad, Upper(T("What Happened")), 0, 1.6f);
    y += 40;
    y += Wrapped(Regular(), 30, x, y, w, kText, st.problem, 1.38f) + 30;
    Wrapped(Regular(), 27, x, y, w, kMenuGray,
            T("Press Cross to try again. If it stops again, send the log from /data/homebrew/PPSA94004/logs."), 1.38f);
    return;
  }

  const bool done = st.phase == Phase::kDone;
  Ease(shown_progress_, done ? 1.0f : st.progress, 3.5f);
  if (done && shown_progress_ > 0.98f) shown_progress_ = 1;
  const float p = std::clamp(shown_progress_, 0.0f, 1.0f);
  if (done && ep >= 0)
    Heading(T("Installed"), F("{} is added to your game. When the game starts it asks if you want to play it.",
                              {T(install::EpisodeName(ep))}));
  else if (done) Heading(T("Installed"), T("GTA IV is set up on your PS5. Press Cross to play."));
  else Heading(ep >= 0 ? F("Installing {}", {T(install::EpisodeName(ep))}) : std::string(T("Installing")),
               T("Keep the app open until it's done. The PS5 won't go to sleep."));
  // the percentage, big
  char pct[8];
  std::snprintf(pct, sizeof(pct), "%d%%", done ? 100 : std::min(99, int(p * 100 + 0.0001f)));
  TextMid(Bold(), 120, kPageL - 4, 462, kText, pct);
  // the step
  const std::string step = Upper(T(done ? "All Done"
                                   : st.stopping ? "Stopping"
                                   : st.step >= 3 ? "Finishing"
                                   : ep >= 0      ? "Copying"
                                   : st.step == 2 ? "Step 2 of 2"
                                                  : "Step 1 of 2"));
  const char* doing = T(done ? (ep >= 0 ? "The episode's files are in place" : "The game's files are in place")
                        : st.stopping  ? "Removing what was copied"
                        : st.step >= 3 ? (ep >= 0 ? "Checking the episode and putting it in place"
                                                  : "Checking the game and putting it in place")
                        : ep >= 0      ? "Copying the episode from its package"
                        : st.step == 2 ? "Unpacking the game's archives"
                                       : "Copying the game from your disc image");
  TextMid(Bold(), 21, kPageL, 566, kDimGray, step, 0, 1.6f);
  TextMid(Bold(), 34, kPageL, 604, kText, doing);
  // the bar
  const float bx = kPageL, by = 652, bw = kPageR - kPageL, bh = 14;
  Rect(bx, by, bw, bh, kBarBack);
  const float fw = std::max(bh, bw * p);
  Rect(bx, by, fw, bh, pal_.title);
  if (!done) {  // a light running along the filled part
    dl_->PushClipRect(P(bx, by), P(bx + fw, by + bh), true);
    const float run = std::fmod(time_ * 520.0f, fw + 400.0f) - 200.0f;
    dl_->AddRectFilledMultiColor(P(bx + run - 140, by), P(bx + run, by + bh), White(0), White(0.35f), White(0.35f),
                                 White(0));
    dl_->AddRectFilledMultiColor(P(bx + run, by), P(bx + run + 140, by + bh), White(0.35f), White(0), White(0),
                                 White(0.35f));
    dl_->PopClipRect();
  }
  // the numbers under it
  std::string left = F("{} written", {lang::Units(install::Bytes(st.written))});
  if (done) {
    const int m = Minutes(st.seconds > 0 ? st.seconds : 1);
    left = m <= 1 ? std::string(T("Took under a minute")) : F("Took {} min", {std::to_string(m)});
  }
  Text(Regular(), 27, bx, by + 34, kMenuGray, left);
  // (stopping, or checking at the end: nothing more being written, no speed or time left)
  if (!done && !st.stopping && st.step < 3) {
    std::string right;
    if (st.rate > 0) right = Rate(st.rate);
    if (st.eta >= 0) {
      const int m = Minutes(st.eta);
      right += (right.empty() ? "" : "  \xC2\xB7  ") +
               (m <= 1 ? std::string(T("Less than a minute left")) : F("About {} min left", {std::to_string(m)}));
    } else if (st.seconds > 2) {
      right += (right.empty() ? "" : "  \xC2\xB7  ") + std::string(T("Working out the time left"));
    }
    Text(Regular(), 27, bx + bw, by + 34, kMenuGray, right, 1);
    // (what the time goes to: reading the disc image or writing the game)
    if (st.read_rate > 0 || st.write_rate > 0) {
      Text(Regular(), 23, bx, by + 74, kDimGray,
           F("Reading at {}  \xC2\xB7  writing at {}", {Rate(st.read_rate), Rate(st.write_rate)}) +
               (st.drive.empty() ? "" : F(" to {}", {st.drive})));
    }
    // something to know about this install (no payload loader: installing in the app)
    float dy = by + 108;
    if (!st.note.empty()) {
      Text(Regular(), 23, bx, dy, kMenuGray, Fit(Regular(), 23, st.note, bw));
      dy += 32;
    }
    // the details, small: what it's writing, where the time goes, the drive
    for (const std::string* line : {&st.diag_file, &st.diag_time, &st.diag_drive}) {
      if (line->empty()) continue;
      Text(Regular(), 20, bx, dy, Rgba(88, 88, 88), Fit(Regular(), 20, *line, bw));
      dy += 26;
    }
  }
}

void Hub::InstallerHints(float r) {
  switch (job_phase_) {
    case Phase::kChecking: Hint(r, kCircle, "Back"); break;
    case Phase::kReady:
      r = Hint(r, kCircle, "Back");
      r = Hint(r, kTriangle, "Look Again");
      Hint(r, kCross, "Install");
      break;
    case Phase::kProblem:
      r = Hint(r, kCircle, "Back");
      Hint(r, kTriangle, "Look Again");
      break;
    case Phase::kInstalling: Hint(r, kCircle, "Cancel"); break;
    case Phase::kDone:
      r = Hint(r, kCircle, "Home");
      Hint(r, kCross, "Play");
      break;
    case Phase::kFailed:
    case Phase::kCancelled:
      r = Hint(r, kCircle, "Back");
      Hint(r, kCross, "Try Again");
      break;
  }
}

// ---------------------------------------------------------------- controls

void Hub::InstallerInput(uint32_t p) {
  auto again = [this] {
    job_ = std::make_unique<install::Job>(paths_.app, InstallRoot(), installer_episode_);
    job_phase_ = Phase::kChecking;
    drive_ = install::DriveOf(InstallRoot());
    free_space_ = drive_.type.empty() ? install::FreeSpace(InstallRoot()) : drive_.free;
    shown_progress_ = 0;
    panel_in_ = 0;
    Play(Sound::kSelect);
  };
  switch (job_phase_) {
    case Phase::kChecking:
      if (p & kCircle) Play(Sound::kBack), CloseInstaller();
      break;
    case Phase::kReady:
      if (p & kCross) {
        job_->Start();
        press_ = 1;
        Play(Sound::kStart);
      } else if (p & kTriangle) {
        again();
      } else if (p & kCircle) {
        Play(Sound::kBack), CloseInstaller();
      }
      break;
    case Phase::kProblem:
      if (p & (kTriangle | kCross)) again();
      else if (p & kCircle) Play(Sound::kBack), CloseInstaller();
      break;
    case Phase::kInstalling:
      if (p & kCircle) {
        Play(Sound::kSelect);
        Confirm(T("Stop installing?"), T("What's been copied so far is removed. You can start over later."),
                T("Stop"),
                [this] {
                  if (job_) job_->Cancel();
                },
                T("Keep Going"), nullptr);
      }
      break;
    case Phase::kDone:
      if (p & kCross) {
        press_ = 1, launch_ = true;  // (Draw fades out and starts the game)
      } else if (p & kCircle) {
        Play(Sound::kBack), CloseInstaller();
      }
      break;
    case Phase::kFailed:
    case Phase::kCancelled:
      if (p & kCross) again();
      else if (p & kCircle) Play(Sound::kBack), CloseInstaller();
      break;
  }
}

}  // namespace gta4ps5::hub

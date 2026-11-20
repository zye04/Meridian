#include "perf_overlay.h"

#include <imgui.h>
#include <meridian/frame_stats.h>

#include <algorithm>
#include <cfloat>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

constexpr double kWindowMs = 10000.0;
constexpr double kGraphMs = 5000.0;
constexpr double kRefreshMs = 250.0;

std::filesystem::path timestampedPath() {
  const std::time_t now = std::time(nullptr);
  char name[64];
  std::strftime(name, sizeof(name), "meridian-%Y%m%d-%H%M%S.csv", std::localtime(&now));
  return name;
}

} // namespace

void PerfOverlay::setVisible(bool visible) {
  if (visible && !visible_ && !window_.empty()) {
    visible_ = true;
    sumFrameMs_ = lastFrameMs_;
    sumCpuMs_ = lastCpuMs_;
    sumGpuMs_ = lastGpuMs_;
    sumCount_ = 1;
    refreshStats();
    return;
  }
  visible_ = visible;
}

void PerfOverlay::handleKeys(meridian::Window &window) {
  const bool f3Down = window.isKeyDown(meridian::Key::F3);
  if (f3Down && !f3WasDown_) {
    setVisible(!visible_);
  }
  f3WasDown_ = f3Down;
  const bool f4Down = window.isKeyDown(meridian::Key::F4);
  if (f4Down && !f4WasDown_) {
    if (recording_) {
      stopRecording();
    } else {
      startRecording();
    }
  }
  f4WasDown_ = f4Down;
}

void PerfOverlay::recordRow(float frameMs, float cpuMs, float gpuMs,
                            std::span<const meridian::PassTime> passes) {
  for (const meridian::PassTime &pass : passes) {
    if (std::none_of(names_.begin(), names_.end(),
                     [&](const char *n) { return std::strcmp(n, pass.name) == 0; })) {
      // A new column: widen the earlier rows with zeros.
      const size_t oldStride = 3 + names_.size();
      std::vector<float> widened;
      widened.reserve(rows_.capacity() / oldStride * (oldStride + 1) + oldStride + 1);
      for (size_t at = 0; at < rows_.size(); at += oldStride) {
        widened.insert(widened.end(), rows_.begin() + at, rows_.begin() + at + oldStride);
        widened.push_back(0.0f);
      }
      rows_ = std::move(widened);
      names_.push_back(pass.name);
    }
  }
  const size_t stride = 3 + names_.size();
  const size_t at = rows_.size();
  rows_.resize(at + stride, 0.0f);
  rows_[at] = frameMs;
  rows_[at + 1] = cpuMs;
  rows_[at + 2] = gpuMs;
  for (const meridian::PassTime &pass : passes) {
    for (size_t i = 0; i < names_.size(); ++i) {
      if (std::strcmp(names_[i], pass.name) == 0) {
        rows_[at + 3 + i] = pass.ms;
      }
    }
  }
}

void PerfOverlay::addFrame(float frameMs, const meridian::Renderer &renderer) {
  const float cpuMs = std::max(frameMs - renderer.cpuWaitMs(), 0.0f);
  const float gpuMs = renderer.gpuFrameMs();
  const auto passes = renderer.gpuPassTimes();

  window_.push_back(frameMs);
  windowMs_ += frameMs;
  while (windowMs_ > kWindowMs && window_.size() > 1) {
    windowMs_ -= window_.front();
    window_.pop_front();
  }
  if (recording_) {
    recordRow(frameMs, cpuMs, gpuMs, passes);
  }
  lastFrameMs_ = frameMs;
  lastCpuMs_ = cpuMs;
  lastGpuMs_ = gpuMs;
  latestPasses_.assign(passes.begin(), passes.end());
  if (!visible_) {
    return;
  }
  sumFrameMs_ += frameMs;
  sumCpuMs_ += cpuMs;
  sumGpuMs_ += gpuMs;
  ++sumCount_;
  sinceRefreshMs_ += frameMs;
  if (sinceRefreshMs_ >= kRefreshMs) {
    refreshStats();
  }
}

void PerfOverlay::refreshStats() {
  scratch_.assign(window_.begin(), window_.end());
  stats_ = meridian::summarizeFrameTimes(scratch_);
  shownFrameMs_ = sumFrameMs_ / sumCount_;
  shownCpuMs_ = sumCpuMs_ / sumCount_;
  shownGpuMs_ = sumGpuMs_ / sumCount_;
  shownPasses_ = latestPasses_;
  sumFrameMs_ = sumCpuMs_ = sumGpuMs_ = 0.0f;
  sumCount_ = 0;
  sinceRefreshMs_ = 0.0;
}

void PerfOverlay::draw(bool cursorFree) {
  if (sumCount_ == 0 && shownPasses_.empty()) {
    return; // nothing measured yet
  }
  const ImVec2 corner = ImGui::GetMainViewport()->Size;
  ImGui::SetNextWindowPos({corner.x - 16, 16}, ImGuiCond_Always, {1, 0});
  ImGui::SetNextWindowBgAlpha(0.6f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoNav;
  if (!cursorFree) {
    flags |= ImGuiWindowFlags_NoInputs;
  }
  ImGui::Begin("Performance", nullptr, flags);
  ImGui::Text("%.0f FPS", shownFrameMs_ > 0.0f ? 1000.0f / shownFrameMs_ : 0.0f);
  ImGui::Text("frame %.2f ms   CPU %.2f ms   GPU %.2f ms", shownFrameMs_, shownCpuMs_,
              shownGpuMs_);

  struct Graph {
    const std::deque<float> *times;
    size_t first;
  } graph{&window_, window_.size()};
  for (double ms = 0.0; graph.first > 0 && ms < kGraphMs;) {
    ms += window_[--graph.first];
  }
  ImGui::PlotLines(
      "##frametimes",
      [](void *data, int i) {
        const Graph &g = *static_cast<const Graph *>(data);
        return (*g.times)[g.first + static_cast<size_t>(i)];
      },
      &graph, static_cast<int>(window_.size() - graph.first), 0, nullptr, 0.0f, FLT_MAX,
      {360, 60});

  ImGui::Text("AVG %.0f   0.1%% %.0f   1%% %.0f   5%% %.0f   MAX %.0f FPS", stats_.averageFps,
              stats_.low0_1Fps, stats_.low1Fps, stats_.low5Fps, stats_.maxFps);
  ImGui::SeparatorText("GPU passes (ms)");
  for (const meridian::PassTime &pass : shownPasses_) {
    ImGui::Text("%-18s %6.2f", pass.name, pass.ms);
  }
  ImGui::Separator();
  if (ImGui::Button(recording_ ? "Stop" : "Record")) {
    if (recording_) {
      stopRecording();
    } else {
      startRecording();
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled(recording_ ? "recording %zu frames (F4)" : "F4", rowCount());
  ImGui::End();
}

void PerfOverlay::startRecording() {
  rows_.clear();
  names_.clear();
  rows_.reserve(3600 * 16);
  recording_ = true;
}

bool PerfOverlay::stopRecording(std::filesystem::path path) {
  recording_ = false;
  if (path.empty()) {
    path = timestampedPath();
  }
  const size_t stride = 3 + names_.size();
  const size_t count = rowCount();

  std::ofstream file(path);
  file << "frame,frame_ms,cpu_ms,gpu_ms";
  for (const char *name : names_) {
    file << ',' << name;
  }
  file << '\n';
  char number[32];
  for (size_t i = 0; i < count; ++i) {
    file << i;
    for (size_t c = 0; c < stride; ++c) {
      std::snprintf(number, sizeof(number), ",%.4f", rows_[i * stride + c]);
      file << number;
    }
    file << '\n';
  }
  file.flush();
  if (!file) {
    std::fprintf(stderr, "Could not write %zu recorded frames to %s (kept in memory)\n", count,
                 path.c_str());
    return false;
  }

  scratch_.clear();
  for (size_t i = 0; i < count; ++i) {
    scratch_.push_back(rows_[i * stride]);
  }
  const meridian::FrameStats stats = meridian::summarizeFrameTimes(scratch_);
  std::printf("Recorded %zu frames: AVG %.1f, 0.1%% low %.1f, 1%% low %.1f, 5%% low %.1f, "
              "MAX %.1f FPS -> %s\n",
              count, stats.averageFps, stats.low0_1Fps, stats.low1Fps, stats.low5Fps,
              stats.maxFps, path.c_str());
  rows_.clear();
  names_.clear();
  return true;
}

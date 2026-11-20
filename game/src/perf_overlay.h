#pragma once

#include <meridian/frame_stats.h>
#include <meridian/renderer.h>
#include <meridian/window.h>

#include <deque>
#include <filesystem>
#include <span>
#include <vector>

// F3 shows the performance overlay: FPS, frame, CPU and GPU ms, per-pass GPU
// ms, a frame-time graph (last ~5 s) and AVG / 0.1% / 1% / 5% low / MAX FPS
// over the last 10 s (definition in meridian/frame_stats.h). It is passive:
// the cursor stays captured. F4 or the Record/Stop button (clickable while
// the cursor is free) records every frame to a CSV:
//   frame,frame_ms,cpu_ms,gpu_ms,<one column per GPU pass>
// frame_ms is wall time between loop starts (present to present), cpu_ms is
// frame_ms minus the time blocked on the GPU and display, and the GPU columns
// belong to the newest finished frame, which is 2 frames behind.
class PerfOverlay {
public:
  bool visible() const { return visible_; }
  // Showing refreshes the shown values at once from the frames so far.
  void setVisible(bool visible);
  bool recording() const { return recording_; }
  // F3 toggles the overlay, F4 recording. Once per frame.
  void handleKeys(meridian::Window &window);

  // The previous frame's wall time, taken between loop starts; reads the
  // renderer's last frame (CPU wait, GPU times). Once per frame, every frame.
  void addFrame(float frameMs, const meridian::Renderer &renderer);
  // Issues the widgets (after Renderer::beginOverlay). cursorFree lets the
  // Record button take clicks.
  void draw(bool cursorFree);

  void startRecording();
  // Writes the CSV to path (default: meridian-<timestamp>.csv in the working
  // directory) and prints the summary and the path. If the file can't be
  // written the rows are kept (recording is off) and the error is printed.
  // Returns whether the file was written.
  bool stopRecording(std::filesystem::path path = {});

private:
  void refreshStats();
  void recordRow(float frameMs, float cpuMs, float gpuMs,
                 std::span<const meridian::PassTime> passes);

  bool visible_ = false;
  bool recording_ = false;
  bool f3WasDown_ = false;
  bool f4WasDown_ = false;

  std::deque<float> window_; // frame times of the last 10 s
  double windowMs_ = 0.0;
  // Recorded rows, flat: frame_ms, cpu_ms, gpu_ms, then one value per name in
  // names_ (in order of first appearance; the overlay pass comes and goes).
  std::vector<float> rows_;
  std::vector<const char *> names_;
  size_t rowCount() const { return rows_.size() / (3 + names_.size()); }

  // Shown values, refreshed a few times per second from the frames since.
  meridian::FrameStats stats_;
  float shownFrameMs_ = 0.0f, shownCpuMs_ = 0.0f, shownGpuMs_ = 0.0f;
  std::vector<meridian::PassTime> shownPasses_;
  double sinceRefreshMs_ = 1e9;
  float sumFrameMs_ = 0.0f, sumCpuMs_ = 0.0f, sumGpuMs_ = 0.0f;
  int sumCount_ = 0;
  std::vector<meridian::PassTime> latestPasses_;
  float lastFrameMs_ = 0.0f, lastCpuMs_ = 0.0f, lastGpuMs_ = 0.0f;
  std::vector<float> scratch_;
};

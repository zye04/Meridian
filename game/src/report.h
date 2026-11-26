#pragma once

#include "overlay.h"

#include <meridian/camera.h>
#include <meridian/renderer.h>

#include <filesystem>
#include <string>
#include <vector>

// The report key (F6, or --report-at N in a capture run): freezes the
// moment into ~/meridian-reports/<local date-time>/ (cli.md "Report").
struct ReportState {
  meridian::Camera camera;
  meridian::Vec3 sunDirection;
  Look look;
  double gameSeconds = 0.0;
  // What a --capture run of the repro line steps before its capture, so its
  // game time can be matched.
  int reproFrames = 0;
  float timeScale = 1.0f;
  // F7's freeze: the repro line keeps the clock at gameSeconds.
  bool timeFrozen = false;
  // F8's solo/hide in effect (in the shot; repro.txt doesn't redo it).
  meridian::DebugFilter filter;
};

class Report {
public:
  // argv as given; repro.txt keeps every flag that affects the image.
  Report(int argc, char **argv);

  // Before the frame's draw: makes the bundle directory, keeps the id
  // targets, turns any debug view off for the shot and requests it.
  void begin(meridian::Renderer &renderer);
  // After that draw: pixel.json at the crosshair, every view, repro.txt;
  // restores the debug view. Prints the bundle's path.
  void finish(meridian::Renderer &renderer, const ReportState &state);
  bool pending() const { return !directory_.empty(); }

private:
  std::filesystem::path executable_;
  std::vector<std::string> keptArgs_;
  // The kept options a --capture-views line can carry.
  std::vector<std::string> viewArgs_;
  std::filesystem::path directory_;
  meridian::DebugView restoreView_ = meridian::DebugView::None;
};

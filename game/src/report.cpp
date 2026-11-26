#include "report.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <string_view>

namespace {

// Options that a report replaces (pose, sun, look, run control) or that
// don't affect the image, with their value counts.
const std::map<std::string_view, int> kDroppedOptions = {
    {"--camera", 5},          {"--sun", 3},           {"--camera-velocity", 3},
    {"--camera-angular-velocity", 2}, {"--camera-yaw-rate", 1}, {"--grade", 1},
    {"--overlay", 1},         {"--benchmark", 2},     {"--route", 1},
    {"--capture", 1},         {"--capture-sequence", 1}, {"--capture-albedo", 1},
    {"--frames", 1},          {"--dump-weather-map", 1}, {"--score-forest", 1},
    {"--reference", 1},       {"--reference-mode", 1}, {"--debug-view", 1},
    {"--report-at", 1},       {"--game-time", 1}};

std::string quoted(const std::string &text) {
  if (text.find_first_of(" \t'\"\\$`*?") == std::string::npos && !text.empty()) {
    return text;
  }
  std::string out = "'";
  for (char c : text) {
    out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  }
  return out + "'";
}

std::string jsonString(const std::string &text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
      out += escaped;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

std::string number(double value) {
  if (!std::isfinite(value)) {
    return "null";
  }
  char text[32];
  std::snprintf(text, sizeof(text), "%.9g", value);
  return text;
}

std::filesystem::path reportsRoot() {
  const char *home = std::getenv("HOME");
  return std::filesystem::path(home ? home : ".") / "meridian-reports";
}

} // namespace

Report::Report(int argc, char **argv) {
  std::error_code error;
  executable_ = std::filesystem::absolute(argv[0], error).lexically_normal();
  for (int i = 1; i < argc; ++i) {
    const auto dropped = kDroppedOptions.find(argv[i]);
    if (dropped != kDroppedOptions.end()) {
      i += dropped->second;
      continue;
    }
    keptArgs_.emplace_back(argv[i]);
  }
}

void Report::begin(meridian::Renderer &renderer) {
  const std::time_t now = std::time(nullptr);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
  directory_ = reportsRoot() / stamp;
  for (int n = 2; std::filesystem::exists(directory_); ++n) {
    directory_ = reportsRoot() / (std::string(stamp) + "_" + std::to_string(n));
  }
  std::filesystem::create_directories(directory_ / "views");
  restoreView_ = renderer.debugView();
  renderer.setDebugView(meridian::DebugView::None);
  renderer.keepDebugTargets(true);
  renderer.requestCapture(directory_ / "shot.png");
}

void Report::finish(meridian::Renderer &renderer, const ReportState &state) {
  const uint32_t x = renderer.renderWidth() / 2;
  const uint32_t y = renderer.renderHeight() / 2;
  const std::optional<meridian::PixelPick> pick = renderer.pick(x, y);
  for (uint32_t v = 1; v < uint32_t(meridian::DebugView::Count); ++v) {
    const auto view = meridian::DebugView(v);
    renderer.saveDebugView(view, directory_ / "views" /
                                     (std::string(meridian::debugViewName(view)) + ".png"));
  }
  renderer.keepDebugTargets(false);
  renderer.setDebugView(restoreView_);

  std::ofstream json(directory_ / "pixel.json");
  json << "{\n";
  json << "  \"render_pixel\": [" << x << ", " << y << "],\n";
  if (pick) {
    auto optional = [](const std::optional<uint32_t> &value) {
      return value ? std::to_string(*value) : std::string("null");
    };
    json << "  \"kind\": " << jsonString(meridian::pickKindName(pick->kind)) << ",\n";
    json << "  \"instance\": " << pick->instance << ",\n";
    json << "  \"placed\": " << (pick->placed ? "true" : "false") << ",\n";
    json << "  \"mesh\": " << optional(pick->mesh) << ",\n";
    json << "  \"mesh_name\": " << jsonString(pick->meshName) << ",\n";
    json << "  \"material\": " << optional(pick->material) << ",\n";
    json << "  \"material_name\": " << jsonString(pick->materialName) << ",\n";
    json << "  \"cluster\": " << optional(pick->cluster) << ",\n";
    json << "  \"lod_level\": " << optional(pick->lodLevel) << ",\n";
    json << "  \"far_cell\": " << optional(pick->farCell) << ",\n";
    json << "  \"distance_m\": " << number(pick->distance) << ",\n";
    json << "  \"views\": {\n";
    json << "    \"base-color\": [" << number(pick->baseColor[0]) << ", "
         << number(pick->baseColor[1]) << ", " << number(pick->baseColor[2]) << "],\n";
    json << "    \"lod\": {\"kind\": " << jsonString(meridian::pickKindName(pick->kind))
         << ", \"level\": " << optional(pick->lodLevel) << "}\n";
    json << "  }\n";
  } else {
    json << "  \"error\": \"no debug targets\"\n";
  }
  json << "}\n";

  std::ofstream(directory_ / "look.json") << lookToJson(state.look);
  std::ofstream(directory_ / "legend.txt") << meridian::lodViewLegend();

  // A --capture run steps game time by 1/60 s per frame; start it so its
  // last frame lands on this one's game time.
  const double reproSeconds = state.reproFrames / 60.0 * state.timeScale;
  std::string line = "flock /tmp/meridian-gpu.lock " + quoted(executable_.string());
  for (const std::string &arg : keptArgs_) {
    line += " " + quoted(arg);
  }
  const meridian::Camera &c = state.camera;
  line += " --camera " + number(c.position.x) + " " + number(c.position.y) + " " +
          number(c.position.z) + " " + number(c.yaw) + " " + number(c.pitch);
  line += " --sun " + number(state.sunDirection.x) + " " + number(state.sunDirection.y) + " " +
          number(state.sunDirection.z);
  line += " --grade " + quoted((directory_ / "look.json").string());
  if (std::abs(state.gameSeconds - reproSeconds) > 1e-6) {
    line += " --game-time " + number(state.gameSeconds - reproSeconds);
  }
  line += " --frames " + std::to_string(state.reproFrames);
  line += " --capture " + quoted((directory_ / "repro.png").string());
  std::ofstream(directory_ / "repro.txt") << line << "\n";

  std::printf("Report: %s\n", directory_.c_str());
  std::fflush(stdout);
  directory_.clear();
}

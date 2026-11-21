#pragma once

#include <algorithm>
#include <cmath>
#include <istream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "math/vec3.h"

namespace meridian {

// A key's label names the segment that starts at it (empty: continues the previous one).
struct CameraKey {
  float time;
  Vec3 position;
  float yaw;
  float pitch;
  std::string label;
};

struct CameraPose {
  Vec3 position;
  float yaw;
  float pitch;
};

// One key per line: TIME X Y Z YAW PITCH [LABEL]; '#' comments and blank lines are skipped.
inline std::vector<CameraKey> parseRoute(std::istream &in) {
  std::vector<CameraKey> keys;
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
    std::istringstream fields(line);
    float time, x, y, z, yaw, pitch;
    if (!(fields >> time)) {
      if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    }
    if (!(fields) || !(fields >> x >> y >> z >> yaw >> pitch)) {
      throw std::runtime_error("route line " + std::to_string(number) +
                               ": expected TIME X Y Z YAW PITCH [LABEL]");
    }
    std::string label;
    fields >> label;
    if (!keys.empty() && !(time > keys.back().time)) {
      throw std::runtime_error("route line " + std::to_string(number) + ": time must increase");
    }
    keys.push_back({time, {x, y, z}, yaw, pitch, label});
  }
  if (keys.size() < 2) throw std::runtime_error("route needs at least two keys");
  return keys;
}

inline float routeDuration(const std::vector<CameraKey> &keys) {
  return keys.back().time - keys.front().time;
}

inline CameraPose routePose(const std::vector<CameraKey> &keys, float time) {
  if (time <= keys.front().time) {
    return {keys.front().position, keys.front().yaw, keys.front().pitch};
  }
  if (time >= keys.back().time) {
    return {keys.back().position, keys.back().yaw, keys.back().pitch};
  }
  size_t i = 1;
  while (keys[i].time < time) ++i;
  const CameraKey &a = keys[i - 1];
  const CameraKey &b = keys[i];
  const float f = (time - a.time) / (b.time - a.time);
  const float twoPi = 6.2831853f;
  return {{a.position.x + (b.position.x - a.position.x) * f,
           a.position.y + (b.position.y - a.position.y) * f,
           a.position.z + (b.position.z - a.position.z) * f},
          a.yaw + std::remainder(b.yaw - a.yaw, twoPi) * f,
          a.pitch + (b.pitch - a.pitch) * f};
}

}  // namespace meridian

#pragma once
#include <array>
#include <cmath>
#include <optional>

struct lua_State;
namespace rise {
struct CameraMatrices {
  std::array<float, 16> view, projection;

  std::optional<std::array<float, 2>> screen(const float *xyz, float width, float height) const {
    // RE's via.mat4 stores the translation at indices 12..14.
    auto multiply = [](const std::array<float, 4> &v, const std::array<float, 16> &m) {
      std::array<float, 4> result{};
      for (unsigned col = 0; col < 4; ++col)
        result[col] = v[0]*m[col] + v[1]*m[4+col] + v[2]*m[8+col] + v[3]*m[12+col];
      return result;
    };
    auto camera = multiply({xyz[0], xyz[1], xyz[2], 1}, view);
    if (camera[2] >= 0) return std::nullopt;
    auto clip = multiply(camera, projection);
    if (clip[3] == 0) return std::nullopt;
    std::array<float, 2> pixel{(1 + clip[0]/clip[3]) * width * .5f,
                               (1 - clip[1]/clip[3]) * height * .5f};
    if (!std::isfinite(pixel[0]) || !std::isfinite(pixel[1])) return std::nullopt;
    return pixel;
  }
};
std::optional<CameraMatrices> camera_matrices(lua_State *);
}

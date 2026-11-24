#include "../../runtime/src/loader/screen_projection.h"
#include <cassert>
#include <limits>

int main() {
  // Camera at (10, 2, 1), looking down -Z. Perspective clip.w is -view.z.
  rise::CameraMatrices camera{
      {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -10, -2, -1, 1},
      {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -1, 0, 0, 0, 0}};
  const float center[]{10, 2, -1}, top_right[]{12, 4, -1}, outside[]{14, 2, -1};
  const float behind[]{10, 2, 3}, at_camera[]{10, 2, 1};
  auto xy = camera.screen(center, 800, 600);
  assert(xy && (*xy)[0] == 400 && (*xy)[1] == 300);
  xy = camera.screen(top_right, 800, 600);
  assert(xy && (*xy)[0] == 800 && (*xy)[1] == 0);
  xy = camera.screen(outside, 800, 600);
  assert(xy && (*xy)[0] == 1200);
  assert(!camera.screen(behind, 800, 600) && !camera.screen(at_camera, 800, 600));
  camera.projection[11] = 0;
  assert(!camera.screen(center, 800, 600));
  camera.projection[15] = 1; // Orthographic projection uses a constant W.
  xy = camera.screen(top_right, 800, 600);
  assert(xy && (*xy)[0] == 1200 && (*xy)[1] == -300);
  assert(!camera.screen(behind, 800, 600));
  const float invalid[]{std::numeric_limits<float>::quiet_NaN(), 0, -1};
  assert(!camera.screen(invalid, 800, 600));
}

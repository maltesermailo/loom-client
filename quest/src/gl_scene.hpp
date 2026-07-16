#pragma once
// What the app draws itself: a floor grid in the eye buffers, and the static
// test texture that stands in for decoded video until M3.2.
//
// Deliberately minimal. ARCHITECTURE §6.2: the desktop is *not* rendered into
// the eye buffers — it lives in the cylinder layer, which the compositor samples
// directly. The projection layer exists only to give the void a horizon.

#include <GLES3/gl3.h>

#include <cstdint>

namespace loom::quest {

// Column-major 4x4, matching GLES uniform layout.
struct Mat4 {
  float m[16];
};

Mat4 multiply(const Mat4& a, const Mat4& b);

class FloorGrid {
 public:
  ~FloorGrid();

  bool create();

  // Draws into the currently bound framebuffer.
  void draw(const Mat4& view_proj);

 private:
  GLuint program_ = 0;
  GLuint vao_ = 0;
  GLuint vbo_ = 0;
  GLint view_proj_loc_ = -1;
  GLsizei vertex_count_ = 0;
};

// Uploads a generated test image: a coloured grid with a bright border, so
// stretching, aspect-ratio and sharpening errors are obvious in-headset.
GLuint create_test_texture(int width, int height);

}  // namespace loom::quest

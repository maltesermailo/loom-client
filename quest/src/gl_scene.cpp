#include "gl_scene.hpp"

#include <cmath>
#include <vector>

#include "log.hpp"

namespace loom::quest {
namespace {

constexpr const char* kGridVertexShader = R"(#version 300 es
uniform mat4 uViewProj;
in vec3 aPosition;
in vec3 aColor;
out vec3 vColor;
void main() {
  gl_Position = uViewProj * vec4(aPosition, 1.0);
  vColor = aColor;
}
)";

constexpr const char* kGridFragmentShader = R"(#version 300 es
precision mediump float;
in vec3 vColor;
out vec4 outColor;
void main() {
  outColor = vec4(vColor, 1.0);
}
)";

constexpr float kGridExtent = 6.0f;
constexpr float kGridSpacing = 0.5f;
constexpr float kFloorY = -1.5f;

GLuint compile_shader(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok == GL_FALSE) {
    char log[512];
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    LOOM_LOGE("shader compile failed: %s", log);
    glDeleteShader(shader);
    return 0;
  }

  return shader;
}

}  // namespace

Mat4 multiply(const Mat4& a, const Mat4& b) {
  Mat4 out{};
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a.m[k * 4 + row] * b.m[col * 4 + k];
      }
      out.m[col * 4 + row] = sum;
    }
  }
  return out;
}

FloorGrid::~FloorGrid() {
  if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
  if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
  if (program_ != 0) glDeleteProgram(program_);
}

bool FloorGrid::create() {
  const GLuint vs = compile_shader(GL_VERTEX_SHADER, kGridVertexShader);
  const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kGridFragmentShader);
  if (vs == 0 || fs == 0) return false;

  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glBindAttribLocation(program_, 0, "aPosition");
  glBindAttribLocation(program_, 1, "aColor");
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linked = GL_FALSE;
  glGetProgramiv(program_, GL_LINK_STATUS, &linked);
  if (linked == GL_FALSE) {
    char log[512];
    glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
    LOOM_LOGE("grid program link failed: %s", log);
    return false;
  }
  view_proj_loc_ = glGetUniformLocation(program_, "uViewProj");

  // Grid lines on the floor plane, brighter on the two axes through the origin
  // so a recenter is visually unambiguous.
  std::vector<float> vertices;
  for (float offset = -kGridExtent; offset <= kGridExtent + 0.001f; offset += kGridSpacing) {
    const bool axis = std::fabs(offset) < 0.001f;
    const float r = axis ? 0.55f : 0.16f;
    const float g = axis ? 0.75f : 0.22f;
    const float b = axis ? 0.95f : 0.30f;

    const float line[4][3] = {{offset, kFloorY, -kGridExtent},
                              {offset, kFloorY, kGridExtent},
                              {-kGridExtent, kFloorY, offset},
                              {kGridExtent, kFloorY, offset}};
    for (const auto& p : line) {
      vertices.insert(vertices.end(), {p[0], p[1], p[2], r, g, b});
    }
  }
  vertex_count_ = static_cast<GLsizei>(vertices.size() / 6);

  glGenVertexArrays(1, &vao_);
  glBindVertexArray(vao_);

  glGenBuffers(1, &vbo_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
               vertices.data(), GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                        reinterpret_cast<void*>(3 * sizeof(float)));

  glBindVertexArray(0);

  return true;
}

void FloorGrid::draw(const Mat4& view_proj) {
  glUseProgram(program_);
  glUniformMatrix4fv(view_proj_loc_, 1, GL_FALSE, view_proj.m);

  glBindVertexArray(vao_);
  glDrawArrays(GL_LINES, 0, vertex_count_);

  glBindVertexArray(0);
  glUseProgram(0);
}

GLuint create_test_texture(int width, int height) {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);

  constexpr int kCell = 64;
  constexpr int kBorder = 8;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const bool checker = ((x / kCell) + (y / kCell)) % 2 == 0;
      const bool border =
          x < kBorder || y < kBorder || x >= width - kBorder || y >= height - kBorder;

      std::uint8_t* p = &pixels[(static_cast<std::size_t>(y) * width + x) * 4];
      if (border) {
        p[0] = 255;
        p[1] = 80;
        p[2] = 0;
      } else if (checker) {
        // A horizontal ramp inside the light cells makes compositor filtering
        // and sharpening visible at a glance.
        p[0] = static_cast<std::uint8_t>(40 + (200 * x) / width);
        p[1] = 70;
        p[2] = static_cast<std::uint8_t>(200 - (160 * y) / height);
      } else {
        p[0] = 16;
        p[1] = 18;
        p[2] = 24;
      }
      p[3] = 255;
    }
  }

  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               pixels.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);

  return texture;
}

}  // namespace loom::quest

#include "overlay.hpp"

#include <unordered_map>

#include "log.hpp"

namespace loom::quest {
namespace {

// Quad swapchain resolution and world size (metres). Sized for three ~30-char
// lines of the debug fields at 4x scale.
constexpr uint32_t kWidth = 768;
constexpr uint32_t kHeight = 192;
constexpr int kScale = 4;   // pixels per font pixel
constexpr int kGlyphW = 6;  // 5px glyph + 1px spacing
constexpr int kGlyphH = 8;  // 7px glyph + 1px spacing

// Tiny 5x7 bitmap font, one entry per character, drawn as ASCII art so it is
// legible and correct by inspection. Only the characters the overlay uses are
// defined; anything else renders blank.
struct Glyph {
  const char* rows[7];
};

const std::unordered_map<char, Glyph>& font() {
  static const std::unordered_map<char, Glyph> f = {
      {' ', {{"     ", "     ", "     ", "     ", "     ", "     ", "     "}}},
      {'0', {{" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}}},
      {'1', {{"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}}},
      {'2', {{" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}}},
      {'3', {{"#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "}}},
      {'4', {{"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}}},
      {'5', {{"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}}},
      {'6', {{" ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "}}},
      {'7', {{"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}}},
      {'8', {{" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}}},
      {'9', {{" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "}}},
      {'.', {{"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}}},
      {'%', {{"##  #", "##  #", "   # ", "  #  ", " #   ", "#  ##", "#  ##"}}},
      {'-', {{"     ", "     ", "     ", "#####", "     ", "     ", "     "}}},
      {'a', {{"     ", "     ", " ### ", "    #", " ####", "#   #", " ####"}}},
      {'b', {{"#    ", "#    ", "#### ", "#   #", "#   #", "#   #", "#### "}}},
      {'c', {{"     ", "     ", " ####", "#    ", "#    ", "#    ", " ####"}}},
      {'d', {{"    #", "    #", " ####", "#   #", "#   #", "#   #", " ####"}}},
      {'e', {{"     ", "     ", " ### ", "#   #", "#####", "#    ", " ####"}}},
      {'i', {{"  #  ", "     ", " ##  ", "  #  ", "  #  ", "  #  ", " ### "}}},
      {'k', {{"#    ", "#    ", "#  # ", "# #  ", "##   ", "# #  ", "#  # "}}},
      {'l', {{" ##  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}}},
      {'m', {{"     ", "     ", "## # ", "# # #", "# # #", "# # #", "# # #"}}},
      {'o', {{"     ", "     ", " ### ", "#   #", "#   #", "#   #", " ### "}}},
      {'p', {{"     ", "     ", "#### ", "#   #", "#### ", "#    ", "#    "}}},
      {'r', {{"     ", "     ", "# ###", "##   ", "#    ", "#    ", "#    "}}},
      {'s', {{"     ", "     ", " ####", "#    ", " ### ", "    #", "#### "}}},
      {'t', {{" #   ", " #   ", "#####", " #   ", " #   ", " #  #", "  ## "}}},
  };
  return f;
}

// Writes one glyph at (px,py) into an RGBA buffer, scaled by kScale. `height` is
// the buffer height: rows are flipped into it because glTexSubImage2D uploads
// with GL's bottom-left origin, so an un-flipped buffer shows upside-down.
void draw_glyph(std::uint8_t* rgba, int stride, int height, char c, int px, int py) {
  const auto it = font().find(c);
  if (it == font().end()) return;
  for (int row = 0; row < 7; ++row) {
    const char* line = it->second.rows[row];
    for (int col = 0; col < 5; ++col) {
      if (line[col] != '#') continue;
      for (int sy = 0; sy < kScale; ++sy) {
        for (int sx = 0; sx < kScale; ++sx) {
          const int x = px + col * kScale + sx;
          const int y = (height - 1) - (py + row * kScale + sy);
          std::uint8_t* p = rgba + y * stride + x * 4;
          p[0] = 80;
          p[1] = 255;
          p[2] = 160;  // bright green text
          p[3] = 255;
        }
      }
    }
  }
}

}  // namespace

Overlay::~Overlay() {
  if (swapchain_ != XR_NULL_HANDLE) xrDestroySwapchain(swapchain_);
}

bool Overlay::create(XrSession session) {
  width_ = kWidth;
  height_ = kHeight;
  pixels_.resize(static_cast<std::size_t>(width_) * height_ * 4);

  XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  info.format = GL_RGBA8;
  info.sampleCount = 1;
  info.width = width_;
  info.height = height_;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  if (XR_FAILED(xrCreateSwapchain(session, &info, &swapchain_))) {
    LOOM_LOGE("overlay xrCreateSwapchain failed");
    return false;
  }

  uint32_t count = 0;
  xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr);
  images_.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  xrEnumerateSwapchainImages(swapchain_, count, &count,
                             reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));

  return true;
}

bool Overlay::build_layer(XrSpace space, const std::vector<std::string>& lines,
                          XrCompositionLayerQuad* out) {
  // Rasterize the text into the scratch buffer: translucent dark background so it
  // is readable over the void, bright text on top.
  for (std::size_t i = 0; i + 3 < pixels_.size(); i += 4) {
    pixels_[i] = 0;
    pixels_[i + 1] = 0;
    pixels_[i + 2] = 0;
    pixels_[i + 3] = 150;
  }
  const int stride = static_cast<int>(width_) * 4;
  for (std::size_t li = 0; li < lines.size(); ++li) {
    const int py = 8 + static_cast<int>(li) * (kGlyphH * kScale);
    int px = 8;
    for (char c : lines[li]) {
      draw_glyph(pixels_.data(), stride, static_cast<int>(height_), c, px, py);
      px += kGlyphW * kScale;
    }
  }

  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &acquire, &index))) return false;
  XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  xrWaitSwapchainImage(swapchain_, &wait);

  glBindTexture(GL_TEXTURE_2D, images_[index].image);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width_),
                  static_cast<GLsizei>(height_), GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
  glBindTexture(GL_TEXTURE_2D, 0);

  XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(swapchain_, &release);

  // A quad floating below-forward of the recenter pose, facing the user.
  *out = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  out->space = space;
  out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  out->subImage.swapchain = swapchain_;
  out->subImage.imageRect.offset = {0, 0};
  out->subImage.imageRect.extent = {static_cast<int32_t>(width_), static_cast<int32_t>(height_)};
  out->pose.orientation.w = 1.0f;
  out->pose.position = {0.0f, -0.6f, -1.3f};
  out->size = {0.7f, 0.7f * static_cast<float>(height_) / static_cast<float>(width_)};

  return true;
}

}  // namespace loom::quest

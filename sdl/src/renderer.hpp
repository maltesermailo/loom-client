#pragma once
// Renderer — an SDL2 window that displays decoded I420 frames via a streaming
// YUV texture (the compositor/GPU does the YUV→RGB conversion, no swscale).
// Deliberately ugly (ARCHITECTURE §7): it must never grow features the Quest
// client lacks. The on-screen overlay (latency/decode/loss) lands in M1.3.

#include <string>
#include <vector>

#include "decoder.hpp"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct TTF_Font;

namespace loom::sdl {

class Renderer {
 public:
  Renderer(const std::string& title, int width, int height);
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool ok() const { return renderer_ != nullptr; }

  // Present one frame with an optional text overlay (each string a line, top-left).
  // Recreates the video texture if the frame size changed.
  void present(const DecodedFrame& frame, const std::vector<std::string>& overlay);

  // Pump SDL events; returns true if the user asked to quit (window close / Esc).
  bool poll_quit();

 private:
  void ensure_texture(int width, int height);
  void draw_overlay(const std::vector<std::string>& lines);

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  TTF_Font* font_ = nullptr;  // forward-declared; SDL_ttf.h stays in the .cpp
  int tex_w_ = 0;
  int tex_h_ = 0;
};

}  // namespace loom::sdl

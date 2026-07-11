#include "renderer.hpp"

#include <SDL.h>

namespace loom::sdl {

Renderer::Renderer(const std::string& title, int width, int height) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    return;
  }
  window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (window_ == nullptr) {
    return;
  }
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (renderer_ == nullptr) {
    renderer_ = SDL_CreateRenderer(window_, -1, 0);  // fall back to software
  }
}

Renderer::~Renderer() {
  if (texture_ != nullptr) SDL_DestroyTexture(texture_);
  if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
  if (window_ != nullptr) SDL_DestroyWindow(window_);
  SDL_Quit();
}

void Renderer::ensure_texture(int width, int height) {
  if (texture_ != nullptr && tex_w_ == width && tex_h_ == height) {
    return;
  }
  if (texture_ != nullptr) {
    SDL_DestroyTexture(texture_);
  }
  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                               width, height);
  tex_w_ = width;
  tex_h_ = height;
}

void Renderer::present(const DecodedFrame& frame) {
  if (renderer_ == nullptr || frame.width == 0) {
    return;
  }
  ensure_texture(frame.width, frame.height);
  SDL_UpdateYUVTexture(texture_, nullptr, frame.y.data(), frame.width, frame.u.data(),
                       frame.width / 2, frame.v.data(), frame.width / 2);
  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
  SDL_RenderPresent(renderer_);
}

bool Renderer::poll_quit() {
  SDL_Event e;
  while (SDL_PollEvent(&e) != 0) {
    if (e.type == SDL_QUIT) {
      return true;
    }
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
      return true;
    }
  }
  return false;
}

}  // namespace loom::sdl

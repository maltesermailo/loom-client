#include "renderer.hpp"

#include <SDL.h>
#include <SDL_ttf.h>

#include <cstdlib>

namespace loom::sdl {
namespace {

// Try a few monospace system fonts (macOS dev box) unless LOOM_OVERLAY_FONT is
// set. Returns nullptr if none load — the overlay is then simply skipped.
TTF_Font* open_overlay_font() {
  const char* env = std::getenv("LOOM_OVERLAY_FONT");
  const char* candidates[] = {
      env,
      "/System/Library/Fonts/Menlo.ttc",
      "/System/Library/Fonts/Monaco.ttf",
      "/System/Library/Fonts/SFNSMono.ttf",
      "/System/Library/Fonts/Courier.ttc",
  };
  for (const char* path : candidates) {
    if (path == nullptr) continue;
    if (TTF_Font* f = TTF_OpenFont(path, 16)) {
      return f;
    }
  }
  return nullptr;
}

}  // namespace

Renderer::Renderer(const std::string& title, int width, int height) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    return;
  }
  window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width,
                             height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (window_ == nullptr) {
    return;
  }
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (renderer_ == nullptr) {
    renderer_ = SDL_CreateRenderer(window_, -1, 0);  // fall back to software
  }
  if (TTF_Init() == 0) {
    font_ = open_overlay_font();  // nullptr → overlay silently disabled
  }
}

Renderer::~Renderer() {
  if (font_ != nullptr) TTF_CloseFont(font_);
  if (TTF_WasInit()) TTF_Quit();
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
  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width,
                               height);
  tex_w_ = width;
  tex_h_ = height;
}

void Renderer::present(const DecodedFrame& frame, const std::vector<std::string>& overlay) {
  if (renderer_ == nullptr || frame.width == 0) {
    return;
  }
  ensure_texture(frame.width, frame.height);
  SDL_UpdateYUVTexture(texture_, nullptr, frame.y.data(), frame.width, frame.u.data(),
                       frame.width / 2, frame.v.data(), frame.width / 2);
  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
  draw_overlay(overlay);
  SDL_RenderPresent(renderer_);
}

void Renderer::draw_overlay(const std::vector<std::string>& lines) {
  if (font_ == nullptr || lines.empty()) {
    return;
  }
  const SDL_Color fg = {0, 255, 0, 255};  // ugly debug green (keep it dumb)
  int y = 6;
  for (const auto& line : lines) {
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font_, line.c_str(), fg);
    if (surf == nullptr) continue;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    SDL_Rect bg = {4, y - 2, surf->w + 8, surf->h + 4};
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 160);
    SDL_RenderFillRect(renderer_, &bg);
    SDL_Rect dst = {8, y, surf->w, surf->h};
    SDL_RenderCopy(renderer_, tex, nullptr, &dst);
    y += surf->h + 2;
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
  }
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

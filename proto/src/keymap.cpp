#include "loom/proto/keymap.hpp"

#include <charconv>

namespace loom::proto::keymap {

static std::string_view trim(std::string_view s) {
  const char* ws = " \t\r";
  const auto b = s.find_first_not_of(ws);
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

static std::optional<std::int64_t> parse_int(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  std::int64_t v = 0;
  const auto* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, v);
  if (ec != std::errc() || ptr != end) return std::nullopt;
  return v;
}

Keymap Keymap::from_csv(std::string_view text) {
  Keymap km;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const auto nl = text.find('\n', pos);
    const auto line_end = (nl == std::string_view::npos) ? text.size() : nl;
    const std::string_view line = trim(text.substr(pos, line_end - pos));
    pos = (nl == std::string_view::npos) ? text.size() : nl + 1;

    if (line.empty() || line[0] == '#') continue;
    const auto comma = line.find(',');
    if (comma == std::string_view::npos) continue;
    const auto from = parse_int(line.substr(0, comma));
    const auto to = parse_int(line.substr(comma + 1));
    if (from && to) km.map_[*from] = *to;
  }
  return km;
}

std::optional<std::int64_t> Keymap::get(std::int64_t code) const {
  const auto it = map_.find(code);
  if (it == map_.end()) return std::nullopt;
  return it->second;
}

}  // namespace loom::proto::keymap

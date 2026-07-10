#pragma once
// Keymap tables — PROTOCOL.md §3.5 / VECTORS.md §3. evdev keycodes are the wire
// format; the tables (AKEYCODE->evdev, evdev->CGKeyCode) live as CSV data in the
// spec's keymaps/ so both implementations share one source of truth.
//
// This class only parses CSV text (no I/O — the caller reads the file), keeping
// the library filesystem-free.
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace loom::proto::keymap {

class Keymap {
public:
  // Parse "from,to" integer rows; '#' comments and blank lines are ignored, and
  // surrounding whitespace on fields is tolerated. Malformed rows are skipped.
  static Keymap from_csv(std::string_view text);

  // Look up a code; nullopt for unmapped codes (which the caller MUST swallow).
  std::optional<std::int64_t> get(std::int64_t code) const;

  std::size_t size() const { return map_.size(); }

private:
  std::unordered_map<std::int64_t, std::int64_t> map_;
};

} // namespace loom::proto::keymap

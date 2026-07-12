#pragma once
// Test alias for the shared proto hex helpers (see loom/proto/hex.hpp).
#include "loom/proto/hex.hpp"

namespace loomtest {
using loom::proto::hex::from_hex;
using loom::proto::hex::to_hex;
}  // namespace loomtest

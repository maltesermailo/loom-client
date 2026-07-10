#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "loom/proto/result.hpp"

using loom::Err;
using loom::Ok;
using loom::Result;

TEST_CASE("Result holds either a value or an error, even when T == E") {
  Result<int, int> ok = Ok(5);
  CHECK(ok.has_value());
  CHECK(static_cast<bool>(ok));
  CHECK(ok.value() == 5);

  Result<int, int> err = Err(3);
  CHECK_FALSE(err.has_value());
  CHECK(err.error() == 3);
}

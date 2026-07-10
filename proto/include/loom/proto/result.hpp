#pragma once
// A tiny expected-style result type. C++20 has no std::expected (that lands in
// C++23), and the protocol layer's error handling is simple sum types, so we
// carry our own zero-dependency Result rather than vendor tl::expected.
//
// It never throws across module boundaries: misuse (reading value() on an error,
// or vice versa) is a programming error caught by std::get's terminate path, not
// an exception we expect callers to handle.

#include <utility>
#include <variant>

namespace loom {

// Distinct wrapper types so Result<T, E> is unambiguous even when T == E
// (e.g. Result<int, int>): Ok<int> and Err<int> are different alternatives.
template <class T> struct Ok {
  T value;
};
template <class E> struct Err {
  E error;
};

// Deduction guides so `Ok(x)` / `Err(x)` infer their payload type.
template <class T> Ok(T) -> Ok<T>;
template <class E> Err(E) -> Err<E>;

template <class T, class E> class Result {
public:
  Result(Ok<T> ok) : data_(std::move(ok)) {}    // NOLINT(google-explicit-constructor)
  Result(Err<E> err) : data_(std::move(err)) {} // NOLINT(google-explicit-constructor)

  bool has_value() const { return std::holds_alternative<Ok<T>>(data_); }
  explicit operator bool() const { return has_value(); }

  const T& value() const { return std::get<Ok<T>>(data_).value; }
  T& value() { return std::get<Ok<T>>(data_).value; }
  const E& error() const { return std::get<Err<E>>(data_).error; }
  E& error() { return std::get<Err<E>>(data_).error; }

private:
  std::variant<Ok<T>, Err<E>> data_;
};

} // namespace loom

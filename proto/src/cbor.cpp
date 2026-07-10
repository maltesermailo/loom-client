#include "loom/proto/cbor.hpp"

#include <algorithm>

namespace loom::proto::cbor {

Value Value::null() { return Value{}; }
Value Value::boolean(bool b) { return Value(Variant{b}); }
Value Value::integer(std::int64_t i) { return Value(Variant{i}); }
Value Value::bytes(std::vector<std::uint8_t> b) { return Value(Variant{std::move(b)}); }
Value Value::text(std::string s) { return Value(Variant{std::move(s)}); }
Value Value::array(Array a) { return Value(Variant{std::move(a)}); }
Value Value::map(Map m) { return Value(Variant{std::move(m)}); }

Value::Type Value::type() const { return static_cast<Type>(v_.index()); }
bool Value::as_bool() const { return std::get<bool>(v_); }
std::int64_t Value::as_int() const { return std::get<std::int64_t>(v_); }
const std::vector<std::uint8_t>& Value::as_bytes() const { return std::get<std::vector<std::uint8_t>>(v_); }
const std::string& Value::as_text() const { return std::get<std::string>(v_); }
const Value::Array& Value::as_array() const { return std::get<Array>(v_); }
const Value::Map& Value::as_map() const { return std::get<Map>(v_); }

// ---------------------------------------------------------------- encode

static void put_head(std::vector<std::uint8_t>& out, std::uint8_t major, std::uint64_t arg) {
  const std::uint8_t mt = static_cast<std::uint8_t>(major << 5);
  auto push_be = [&](int nbytes) {
    for (int shift = (nbytes - 1) * 8; shift >= 0; shift -= 8)
      out.push_back(static_cast<std::uint8_t>(arg >> shift));
  };
  if (arg < 24) {
    out.push_back(static_cast<std::uint8_t>(mt | arg));
  } else if (arg <= 0xff) {
    out.push_back(mt | 24);
    push_be(1);
  } else if (arg <= 0xffff) {
    out.push_back(mt | 25);
    push_be(2);
  } else if (arg <= 0xffffffff) {
    out.push_back(mt | 26);
    push_be(4);
  } else {
    out.push_back(mt | 27);
    push_be(8);
  }
}

static void encode_into(std::vector<std::uint8_t>& out, const Value& v) {
  switch (v.type()) {
  case Value::Type::Null:
    out.push_back(0xf6);
    break;
  case Value::Type::Bool:
    out.push_back(v.as_bool() ? 0xf5 : 0xf4);
    break;
  case Value::Type::Int: {
    const std::int64_t i = v.as_int();
    if (i >= 0)
      put_head(out, 0, static_cast<std::uint64_t>(i));
    else
      put_head(out, 1, static_cast<std::uint64_t>(-(i + 1)));
    break;
  }
  case Value::Type::Bytes: {
    const auto& b = v.as_bytes();
    put_head(out, 2, b.size());
    out.insert(out.end(), b.begin(), b.end());
    break;
  }
  case Value::Type::Text: {
    const auto& s = v.as_text();
    put_head(out, 3, s.size());
    out.insert(out.end(), s.begin(), s.end());
    break;
  }
  case Value::Type::Array: {
    const auto& a = v.as_array();
    put_head(out, 4, a.size());
    for (const auto& e : a) encode_into(out, e);
    break;
  }
  case Value::Type::Map: {
    // Canonical: sort entries by the bytewise encoding of their keys.
    std::vector<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> items;
    for (const auto& [k, val] : v.as_map())
      items.push_back({encode_canonical(k), encode_canonical(val)});
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    put_head(out, 5, items.size());
    for (const auto& [kb, vb] : items) {
      out.insert(out.end(), kb.begin(), kb.end());
      out.insert(out.end(), vb.begin(), vb.end());
    }
    break;
  }
  }
}

std::vector<std::uint8_t> encode_canonical(const Value& v) {
  std::vector<std::uint8_t> out;
  encode_into(out, v);
  return out;
}

// ---------------------------------------------------------------- decode

static bool read_be(std::span<const std::uint8_t> b, std::size_t& pos, int nbytes,
                    std::uint64_t& out) {
  if (static_cast<std::size_t>(nbytes) > b.size() - pos) return false;
  std::uint64_t v = 0;
  for (int k = 0; k < nbytes; ++k) v = (v << 8) | b[pos++];
  out = v;
  return true;
}

static std::optional<Value> decode_item(std::span<const std::uint8_t> b, std::size_t& pos) {
  if (pos >= b.size()) return std::nullopt;
  const std::uint8_t ib = b[pos++];
  const std::uint8_t major = ib >> 5;
  const std::uint8_t ai = ib & 0x1f;

  std::uint64_t arg = 0;
  if (ai < 24)
    arg = ai;
  else if (ai == 24 && !read_be(b, pos, 1, arg))
    return std::nullopt;
  else if (ai == 25 && !read_be(b, pos, 2, arg))
    return std::nullopt;
  else if (ai == 26 && !read_be(b, pos, 4, arg))
    return std::nullopt;
  else if (ai == 27 && !read_be(b, pos, 8, arg))
    return std::nullopt;
  else if (ai >= 28)
    return std::nullopt; // 28-30 reserved, 31 indefinite: unsupported

  switch (major) {
  case 0:
    if (arg > static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
    return Value::integer(static_cast<std::int64_t>(arg));
  case 1:
    if (arg > static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
    return Value::integer(-1 - static_cast<std::int64_t>(arg));
  case 2:
  case 3: {
    if (arg > b.size() - pos) return std::nullopt;
    const std::uint8_t* p = b.data() + pos;
    const std::size_t n = static_cast<std::size_t>(arg);
    pos += n;
    if (major == 2) return Value::bytes(std::vector<std::uint8_t>(p, p + n));
    return Value::text(std::string(reinterpret_cast<const char*>(p), n));
  }
  case 4: {
    Value::Array a; // no reserve: arg is untrusted
    for (std::uint64_t k = 0; k < arg; ++k) {
      auto item = decode_item(b, pos);
      if (!item) return std::nullopt;
      a.push_back(std::move(*item));
    }
    return Value::array(std::move(a));
  }
  case 5: {
    Value::Map m;
    for (std::uint64_t k = 0; k < arg; ++k) {
      auto key = decode_item(b, pos);
      if (!key) return std::nullopt;
      auto val = decode_item(b, pos);
      if (!val) return std::nullopt;
      m.emplace_back(std::move(*key), std::move(*val));
    }
    return Value::map(std::move(m));
  }
  case 7:
    if (ai == 20) return Value::boolean(false);
    if (ai == 21) return Value::boolean(true);
    if (ai == 22) return Value::null();
    return std::nullopt; // floats (25-27) added in step 4; other simples unused
  default:
    return std::nullopt;
  }
}

std::optional<Value> decode(std::span<const std::uint8_t> bytes) {
  std::size_t pos = 0;
  return decode_item(bytes, pos);
}

} // namespace loom::proto::cbor

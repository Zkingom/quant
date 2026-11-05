#include "lob/soup/soup.hpp"

#include <algorithm>

namespace lob::soup {

bool OutBuffer::append_packet(char t, Bytes payload) noexcept {
  const std::size_t need = 3 + payload.size();
  if (payload.size() + 1 > 0xffff || need > free_space()) return false;
  if (end_ + need > buf_.size()) {  // compact: move the unsent bytes to the front
    std::copy(buf_.begin() + static_cast<std::ptrdiff_t>(begin_), buf_.begin() + static_cast<std::ptrdiff_t>(end_),
              buf_.begin());
    end_ -= begin_;
    begin_ = 0;
  }
  const std::size_t len = payload.size() + 1;
  buf_[end_++] = static_cast<std::uint8_t>(len >> 8U);
  buf_[end_++] = static_cast<std::uint8_t>(len & 0xffU);
  buf_[end_++] = static_cast<std::uint8_t>(t);
  std::copy(payload.begin(), payload.end(), buf_.begin() + static_cast<std::ptrdiff_t>(end_));
  end_ += payload.size();
  return true;
}

void OutBuffer::consume(std::size_t n) noexcept {
  begin_ += std::min(n, end_ - begin_);
  if (begin_ == end_) begin_ = end_ = 0;
}

std::string_view to_string(FrameError e) noexcept {
  switch (e) {
    case FrameError::None: return "None";
    case FrameError::ZeroLength: return "ZeroLength";
    case FrameError::TooLong: return "TooLong";
  }
  return "?";
}

FrameError Deframer::check(std::size_t len, int t) const noexcept {
  if (len == 0) return FrameError::ZeroLength;
  if (len > kMaxDebugPacket) return FrameError::TooLong;
  if (t >= 0 && static_cast<char>(t) != type::kDebug && len > kMaxPacket) return FrameError::TooLong;
  return FrameError::None;
}

namespace {

/// ASCII number, left-padded with spaces, in a field of `width` characters.
void put_number(std::uint8_t* p, std::size_t width, std::uint64_t v) noexcept {
  std::fill(p, p + width, static_cast<std::uint8_t>(' '));
  std::size_t i = width;
  do {
    p[--i] = static_cast<std::uint8_t>('0' + v % 10);
    v /= 10;
  } while (v != 0 && i > 0);
}

/// Spaces then digits (at least one); also accepts right-padding, which
/// some implementations send. Overflow of 20 digits is rejected.
bool get_number(const std::uint8_t* p, std::size_t width, std::uint64_t& out) noexcept {
  std::size_t i = 0;
  while (i < width && p[i] == ' ') ++i;
  if (i == width) return false;
  std::uint64_t v = 0;
  std::size_t digits = 0;
  for (; i < width && p[i] != ' '; ++i, ++digits) {
    if (p[i] < '0' || p[i] > '9') return false;
    const std::uint64_t d = p[i] - '0';
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  for (; i < width; ++i) {
    if (p[i] != ' ') return false;  // "12 3" is not a number
  }
  out = v;
  return digits > 0;
}

}  // namespace

bool put_login_request(OutBuffer& out, std::string_view user, std::string_view password, const SessionId& session,
                       std::uint64_t sequence) noexcept {
  std::array<std::uint8_t, kLoginRequestLength - 1> p{};
  const auto u = field<6>(user);
  const auto w = field<10>(password);
  std::copy(u.begin(), u.end(), p.begin());
  std::copy(w.begin(), w.end(), p.begin() + 6);
  std::copy(session.begin(), session.end(), p.begin() + 16);
  put_number(p.data() + 26, 20, sequence);
  return out.append_packet(type::kLoginRequest, p);
}

bool put_login_accepted(OutBuffer& out, const SessionId& session, std::uint64_t sequence) noexcept {
  std::array<std::uint8_t, kLoginAcceptedLength - 1> p{};
  std::copy(session.begin(), session.end(), p.begin());
  put_number(p.data() + 10, 20, sequence);
  return out.append_packet(type::kLoginAccepted, p);
}

bool parse_login_request(Bytes payload, LoginRequest& out) noexcept {
  if (payload.size() != kLoginRequestLength - 1) return false;
  LoginRequest r;
  std::copy_n(payload.begin(), 6, r.user.begin());
  std::copy_n(payload.begin() + 6, 10, r.password.begin());
  std::copy_n(payload.begin() + 16, 10, r.session.begin());
  if (!get_number(payload.data() + 26, 20, r.sequence)) return false;
  out = r;
  return true;
}

bool parse_login_accepted(Bytes payload, LoginAccepted& out) noexcept {
  if (payload.size() != kLoginAcceptedLength - 1) return false;
  LoginAccepted a;
  std::copy_n(payload.begin(), 10, a.session.begin());
  if (!get_number(payload.data() + 10, 20, a.sequence)) return false;
  out = a;
  return true;
}

bool same_field(std::string_view a, std::string_view b, bool case_insensitive) noexcept {
  auto trim = [](std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
  };
  a = trim(a);
  b = trim(b);
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (case_insensitive) {
      if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
      if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
    }
    if (x != y) return false;
  }
  return true;
}

}  // namespace lob::soup

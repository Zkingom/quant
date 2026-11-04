#include "lob/pipeline/pipeline.hpp"

#include <cerrno>
#include <unistd.h>

#include <algorithm>

namespace lob::pipeline {

std::ptrdiff_t FdSource::read(std::uint8_t* dst, std::size_t capacity) {
  for (;;) {
    const ssize_t r = ::read(fd_, dst, capacity);
    if (r >= 0) {
      return static_cast<std::ptrdiff_t>(r);
    }
    if (errno != EINTR) {
      return -1;
    }
  }
}

const char* hop_name(Hop h) noexcept {
  switch (h) {
    case kDecode: return "decode";
    case kQueue: return "queue";
    case kBook: return "book";
    case kStrategy: return "strategy";
    case kRisk: return "risk";
    case kEngine: return "engine";
    case kHopCount: break;
  }
  return "?";
}

LatencyRecorder::LatencyRecorder(std::size_t capacity_per_hop, std::uint64_t stride)
    : stride_(stride == 0 ? 1 : stride) {
  for (auto& v : samples_) {
    v.resize(capacity_per_hop);
  }
}

LatencyRecorder::Summary LatencyRecorder::summarize(Hop h, const ClockCalibration& c) const {
  Summary s;
  s.n = count_[h];
  s.dropped = dropped_[h];
  if (s.n == 0) {
    return s;
  }
  std::vector<std::uint64_t> v(samples_[h].begin(), samples_[h].begin() + static_cast<long>(s.n));
  std::sort(v.begin(), v.end());
  auto at = [&](double q) {
    // Nearest-rank percentile on the exact sorted samples.
    auto idx = static_cast<std::size_t>(q * static_cast<double>(s.n));
    idx = std::min(idx, s.n - 1);
    const double t = static_cast<double>(v[idx]) - c.overhead_ticks;
    return std::max(0.0, t) * c.ns_per_tick;
  };
  s.p50 = at(0.50);
  s.p99 = at(0.99);
  s.p999 = at(0.999);
  s.max = at(1.0);
  return s;
}

}  // namespace lob::pipeline

// Mechanism probe for BENCHMARK §12: does capacity that std::vector::reserve()
// allocated but nobody wrote count in ru_maxrss on this kernel + glibc?
// Usage: reserve_rss MODE MiB    MODE: none | reserve | touch-half | touch-all
#include <sys/resource.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Rec {  // same size as lob::io::CommittedCommand (32 bytes)
  unsigned long long a, b, c;
  bool d;
};

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "none";
  const std::size_t mib = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 16;
  const std::size_t n = mib * 1024 * 1024 / sizeof(Rec);
  std::vector<Rec> v;
  if (std::strcmp(mode, "none") != 0) {
    v.reserve(n);
  }
  std::size_t touch = 0;
  if (std::strcmp(mode, "touch-half") == 0) touch = n / 2;
  if (std::strcmp(mode, "touch-all") == 0) touch = n;
  for (std::size_t i = 0; i < touch; ++i) {
    v.push_back(Rec{i, i, i, true});
  }
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  std::printf("%s %zu MiB reserved: capacity=%zu size=%zu max_rss_kib=%ld\n", mode, mib, v.capacity(), v.size(),
              ru.ru_maxrss);
  return v.empty() ? 0 : static_cast<int>(v.back().a & 1);
}

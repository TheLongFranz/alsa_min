// libstdc++ 16 snapshots use the C++26-deprecated std::is_trivial_v inside
// <inplace_vector>; clang reports it at the instantiation site even though it
// lives in a system header. Silence it for this header only.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <inplace_vector>
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include <print>
#include <ranges>
#include <string>

#include <alsa/asoundlib.h>

inline auto hello_world() -> std::string {
  static constexpr auto vec =
    std::views::iota(0, 16) | std::ranges::to<std::inplace_vector<int, 256>>();
  const auto msg{std::string{"Hello, world!"}};
  std::println("{}", msg);
  for (const auto it: vec) {
    std::println("{}", it);
  }
  return msg;
}

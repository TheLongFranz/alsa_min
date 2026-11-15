// third party stuff
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

// STL stuff
import std;

// our stuff
import alsa;

// NOLINTBEGIN
#define CHECK_RESULT(var_expr)                                                                                                             \
  do {                                                                                                                                     \
    const auto& _result = (var_expr);                                                                                                      \
    if (!_result.has_value()) {                                                                                                            \
      UNSCOPED_INFO(std::format("{}", _result.error().err_msg));                                                                           \
    }                                                                                                                                      \
    REQUIRE(_result.has_value());                                                                                                          \
  } while (0)
// NOLINTEND

TEST_CASE("sine wave") {
  using namespace std::chrono_literals;
  const auto device = audio_device::open("default", {});
  CHECK_RESULT(device);

  alsa_result result{};

  {
    std::jthread audio{[&](const std::stop_token& stop) { result = device->run(stop); }};
    std::this_thread::sleep_for(4s);
  }
}
// third party stuff
#include <catch2/catch_approx.hpp>
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

static constexpr double default_sine_frequency{440.0};

TEST_CASE("sine wave plays without underruns") {
  using namespace std::chrono_literals;
  constexpr auto duration = 4s;

  audio_device_config config{}; // NOLINT
  auto                device = audio_device::open("default", config);
  CHECK_RESULT(device);

  // open() writes the negotiated values back into config
  std::println("rate {}  period {}  buffer {}", config.sample_rate, config.period_size, config.buffer_size);

  sine        proc{default_sine_frequency}; // NOLINT
  audio_stats stats;
  alsa_result result{};

  {
    std::jthread audio{[&](const std::stop_token& stop) { result = device->run(stop, proc, stats); }};
    std::this_thread::sleep_for(duration);
  }

  CHECK_RESULT(result);
  CHECK(stats.xruns.load() == 0);

  const auto expected = config.sample_rate * std::chrono::duration<double>(duration).count();
  CHECK(static_cast<double>(stats.frames.load()) == Catch::Approx(expected).epsilon(0.1)); // NOLINT
}

// NOLINTBEGIN
TEST_CASE("README") {
  using namespace std::chrono_literals;

  audio_device_config config{};
  sine                proc(440);                                      // audio processor that outputs a sine
  auto                device = audio_device::open("default", config); // factory that return std::unexpected on failure

  if (device) {
    std::jthread audio{[&](const std::stop_token& stop) { const auto result = device->run(stop, proc); }};
    std::this_thread::sleep_for(4s);
  } else {
    std::println("{}", device.error().err_msg);
  }

  REQUIRE(device.has_value());
}
// NOLINTEND
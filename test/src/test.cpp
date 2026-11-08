// stdlib stuff
#include <thread>

// third party stuff
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

// linux system stuff
#include <alsa/asoundlib.h>

// our stuff
#include <alsa_min/alsa_min.hpp>

TEST_CASE("sine wave") {
  using namespace std::chrono_literals;
  const std::jthread audio{interrupt};
  std::this_thread::sleep_for(2s);
}
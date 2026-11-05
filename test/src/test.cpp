// third party stuff
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

// linux system stuff
#include <alsa/asoundlib.h>

// our stuff
#include <alsa_min/alsa_min.hpp>

TEST_CASE("Hello, World") {
  REQUIRE(hello_world() == "Hello, world!");
}

TEST_CASE("play silence") {
  CHECK(play_silence() == 0);
  REQUIRE(play_silence() == 0);
}
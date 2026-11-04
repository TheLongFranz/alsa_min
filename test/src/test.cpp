#include<alsa/alsa.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("Hello, World") {
    REQUIRE(hello_world() == "Hello, world!");
}
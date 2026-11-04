#include<print>
#include<string>

#include <alsa/asoundlib.h>

auto hello_world() -> std::string {
    const auto msg{std::string{"Hello, world!"}};
    std::println("{}", msg);
    return msg;
} 

s
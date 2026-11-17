# alsa_min

## Introduction

Bare bones example of getting an alsa audio device working before committing over in stompd. So if you see a large number of files being dropped in in one commit it was from here.

## Minimal example

```cpp
import std;

import audio_processor;
import audio_device;

int main() {
  using namespace std::chrono_literals;
  alsa_min::audio_device_config config{};
  alsa_min::sine                proc(440);                                                // audio processor that outputs a sine
  auto                          device = alsa_min::audio_device::open("default", config); // factory that return std::unexpected on failure
  if (device) {
    std::jthread audio{[&](const std::stop_token& stop) { const auto result = device->run(stop, proc); }};
    std::this_thread::sleep_for(4s);
  } else {
    std::println("{}", device.error().err_msg);
  }
}
```
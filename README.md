# alsa

## Introduction

Bare bones example of getting an alsa audio device working before committing over in stompd.

## Minimal example

```cpp
import std;

import alsa;

int main() {
  using namespace std::chrono_literals;

  audio_device_config config{};
  sine                proc(440);                                      // audio processor that outputs a sine wave
  auto                device = audio_device::open("default", config); // factory that return std::unexpected on failure

  if (device) {
    std::jthread audio{[&](const std::stop_token& stop) { const auto result = device->run(stop, proc); }};
    std::this_thread::sleep_for(4s); // audio thread will continue until joined
  } else {
    std::println("{}", device.error().err_msg);
  }
}
```
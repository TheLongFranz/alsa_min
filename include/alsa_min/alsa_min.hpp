// libstdc++ 16 snapshots use the C++26-deprecated std::is_trivial_v inside
// <inplace_vector>; clang reports it at the instantiation site even though it
// lives in a system header. Silence it for this header only.
#include <array>
#include <cstdint>
#include <cstdlib>
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <inplace_vector>
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include <cstdio>
#include <print>
#include <ranges>
#include <string>
#include <utility>

#include <alsa/asoundlib.h>

static constexpr auto buffer_size{std::size_t{128}};
static constexpr auto sample_rate{std::size_t{44100}};

inline auto hello_world() -> std::string {
  static constexpr auto vec =
    std::views::iota(0, 16) | std::ranges::to<std::inplace_vector<int, 16>>();
  const auto msg{std::string{"Hello, world!"}};
  std::println("{}", msg);
  for (const auto it: vec) {
    std::println("{}", it);
  }
  return msg;
}

// TODO make a nice abstraction around play_silence
// TODO get_audio_devices
// TODO test signals, sin, square, sawtooth, triangle, noise

struct audio_device_config {
  std::array<std::int16_t, buffer_size> buf{};
  snd_pcm_t*                            playback_handle{};
  snd_pcm_hw_params_t*                  hw_params{};
  // const char*                           name{std::getenv("MY_LIB_PCM_DEVICE")}; // NOLINT
  unsigned int fs = static_cast<unsigned int>(sample_rate);
};

int play_silence() { // NOLINT
  int i{};
  auto [buf, playback_handle, hw_params, fs] = audio_device_config{};

  if (
    const int err = snd_pcm_open(&playback_handle, "default", SND_PCM_STREAM_PLAYBACK, 0); err < 0
  ) {
    std::println(stderr, "cannot open audio device {} ({})", "default", snd_strerror(err));
    return 1;
  }

  if (const int err = snd_pcm_hw_params_malloc(&hw_params); err < 0) {
    std::println(stderr, "cannot allocate hardware parameter structure ({})", snd_strerror(err));
    return 1;
  }

  if (const int err = snd_pcm_hw_params_any(playback_handle, hw_params); err < 0) {
    std::println(stderr, "cannot initialize hardware parameter structure ({})", snd_strerror(err));
    return 1;
  }

  if (
    const int err =
      snd_pcm_hw_params_set_access(playback_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    err < 0
  ) {
    std::println(stderr, "cannot set access type ({})", snd_strerror(err));
    return 1;
  }

  if (
    const int err = snd_pcm_hw_params_set_format(playback_handle, hw_params, SND_PCM_FORMAT_S16_LE);
    err < 0
  ) {
    std::println(stderr, "cannot set sample format ({})", snd_strerror(err));
    return 1;
  }

  if (
    const int err = snd_pcm_hw_params_set_rate_near(playback_handle, hw_params, &fs, nullptr);
    err < 0
  ) {
    std::println(stderr, "cannot set sample rate ({})", snd_strerror(err));
    return 1;
  }

  if (const int err = snd_pcm_hw_params_set_channels(playback_handle, hw_params, 2); err < 0) {
    std::println(stderr, "cannot set channel count ({})", snd_strerror(err));
    return 1;
  }

  if (const int err = snd_pcm_hw_params(playback_handle, hw_params); err < 0) {
    std::println(stderr, "cannot set parameters ({})", snd_strerror(err));
    return 1;
  }

  snd_pcm_hw_params_free(hw_params);

  if (const int err = snd_pcm_prepare(playback_handle); err < 0) {
    std::println(stderr, "cannot prepare audio interface for use ({})", snd_strerror(err));
    return 1;
  }

  for (i = 0; i < 10; ++i) { // NOLINT
    if (
      std::int64_t err = snd_pcm_writei(playback_handle, buf.data(), buffer_size);
      std::cmp_not_equal(err, buffer_size)
    ) {
      std::println(
        stderr, "write to audio interface failed ({})", snd_strerror(static_cast<int>(err))
      );
      return 1;
    }
  }

  snd_pcm_close(playback_handle);
  return 0;
}
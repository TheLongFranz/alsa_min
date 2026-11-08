#include <algorithm>
#include <alsa/asoundlib.h>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ///> shenanigans
// #ifdef __clang__
// #pragma clang diagnostic push
// #pragma clang diagnostic ignored "-Wdeprecated-declarations"
// #include <inplace_vector>
// #pragma clang diagnostic pop
// #endif

#include <poll.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <print>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <utility>

static constexpr auto buffer_size{std::size_t{4096}};
static constexpr auto max_loop{std::size_t{512}};
static constexpr auto channel_count{std::int16_t{2}};
static constexpr auto device_name{std::string{"default"}};
static constexpr auto timeout{std::size_t{1000}};
static auto           sample_rate{std::uint32_t{96'000}}; // NOLINT
snd_pcm_t*            playback_handle;                    // NOLINT
// short      buf[4096];
static std::array<std::int16_t, buffer_size * channel_count> buf{}; // NOLINT

struct wave_config {
  double freq{}, rate{};
};

inline void sine(
  std::span<std::int16_t> out, std::size_t channels, const wave_config& config, double& phase
) noexcept {
  constexpr auto tau{2.0 * std::numbers::pi};
  constexpr auto amp{0.5 * std::numeric_limits<std::int16_t>::max()};
  const auto     step{tau * config.freq / config.rate};

  for (const auto frame: out | std::views::chunk(channels)) {
    std::ranges::fill(frame, static_cast<int16_t>(amp * std::sin(phase)));
    phase += step;
    if (phase >= tau) {
      phase -= tau;
    }
  }
}

inline int playback_callback(snd_pcm_sframes_t nframes) {
  int               err{};
  static auto       phase{double{0}}; // NOLINT
  const wave_config config{.freq = timeout, .rate = static_cast<double>(sample_rate)};

  std::println("playback callback called with {} frames", nframes);

  /* ... fill buf with data ... */
  sine(buf, channel_count, config, phase);

  if (err = static_cast<int>(snd_pcm_writei(playback_handle, buf.data(), nframes)); err < 0) {
    std::println("write failed {}", snd_strerror(err));
  }

  return err;
}

int interrupt(std::stop_token stop) { // NOLINT

  snd_pcm_hw_params_t* hw_params{};
  snd_pcm_sw_params_t* sw_params{};
  snd_pcm_sframes_t    frames_to_deliver{};
  // int                  nfds{};
  int err{};
  // struct pollfd*       pfds{};

  if (
    err = snd_pcm_open(&playback_handle, device_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0); err < 0
  ) {
    std::println("cannot open audio device {} ({})", device_name, snd_strerror(err));
    return 1;
  }

  if (err = snd_pcm_hw_params_malloc(&hw_params); err < 0) {
    std::println("cannot allocate hardware parameter structure ({})", snd_strerror(err));
    return 1;
  }

  if (err = snd_pcm_hw_params_any(playback_handle, hw_params); err < 0) {
    std::println("cannot initialize hardware parameter structure ({})", snd_strerror(err));
    return 1;
  }

  if (
    err = snd_pcm_hw_params_set_access(playback_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    err < 0
  ) {
    std::println("cannot set access type ({})", snd_strerror(err));
    return 1;
  }

  if (
    err = snd_pcm_hw_params_set_format(playback_handle, hw_params, SND_PCM_FORMAT_S16_LE); err < 0
  ) {
    std::println("cannot set sample format ({})", snd_strerror(err));
    return 1;
  }

  if (
    err = snd_pcm_hw_params_set_rate_near(playback_handle, hw_params, &sample_rate, nullptr);
    err < 0
  ) {
    std::println("cannot set sample rate ({})", snd_strerror(err));
    return 1;
  }

  if (err = snd_pcm_hw_params_set_channels(playback_handle, hw_params, channel_count); err < 0) {
    std::println("cannot set channel count ({})", snd_strerror(err));
    return 1;
  }

  if (err = snd_pcm_hw_params(playback_handle, hw_params); err < 0) {
    std::println("cannot set parameters ({})", snd_strerror(err));
    return 1;
  }

  snd_pcm_hw_params_free(hw_params);

  /* tell ALSA to wake us up whenever 4096 or more frames
     of playback data can be delivered. Also, tell
     ALSA that we'll start the device ourselves.
  */

  if (err = snd_pcm_sw_params_malloc(&sw_params); err < 0) {
    std::println("cannot allocate software parameters structure ({})", snd_strerror(err));
    return 1;
  }
  if (err = snd_pcm_sw_params_current(playback_handle, sw_params); err < 0) {
    std::println("cannot initialize software parameters structure ({})", snd_strerror(err));
    return 1;
  }
  if (err = snd_pcm_sw_params_set_avail_min(playback_handle, sw_params, buffer_size); err < 0) {
    std::println("cannot set minimum available count ({})", snd_strerror(err));
    return 1;
  }
  if (err = snd_pcm_sw_params_set_start_threshold(playback_handle, sw_params, 0U); err < 0) {
    std::println("cannot set start mode ({})", snd_strerror(err));
    return 1;
  }
  if (err = snd_pcm_sw_params(playback_handle, sw_params); err < 0) {
    std::println("cannot set software parameters ({})", snd_strerror(err));
    return 1;
  }

  /* the interface will interrupt the kernel every 4096 frames, and ALSA
     will wake up this program very soon after that.
  */

  if (err = snd_pcm_prepare(playback_handle); err < 0) {
    std::println("cannot prepare audio interface for use ({})", snd_strerror(err));
    return 1;
  }

  while (!stop.stop_requested()) {
    /* wait till the interface is ready for data, or 1 second
       has elapsed.
    */

    if (err = snd_pcm_wait(playback_handle, timeout); err < 0) {
      std::println("poll failed ({})", snd_strerror(errno));
      break;
    }

    /* find out how much space is available for playback data */

    if (frames_to_deliver = snd_pcm_avail_update(playback_handle); frames_to_deliver < 0) {
      if (frames_to_deliver == -EPIPE) {
        std::println("an xrun occured");
        break;
      }
      std::println("unknown ALSA avail update return value ({})", frames_to_deliver);
      break;
    }

    frames_to_deliver =
      std::cmp_greater(frames_to_deliver, buffer_size) ? buffer_size : frames_to_deliver;

    /* deliver the data */

    if (playback_callback(frames_to_deliver) != frames_to_deliver) {
      std::println("playback callback failed");
      break;
    }
  }

  snd_pcm_close(playback_handle);
  return 0;
}
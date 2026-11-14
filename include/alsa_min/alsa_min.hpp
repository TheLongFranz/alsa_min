// STL stuff
import std;

// Linux stuff
#include <alsa/asoundlib.h>

static constexpr auto                                        buffer_size{std::size_t{16}};
static constexpr auto                                        channel_count{std::int16_t{2}};
static constexpr auto                                        device_name{std::string{"default"}};
static constexpr auto                                        timeout{std::size_t{440}};
static auto                                                  sample_rate{std::uint32_t{96'000}}; // NOLINT
snd_pcm_t*                                                   playback_handle;                    // NOLINT
static std::array<std::int16_t, buffer_size * channel_count> buf{};                              // NOLINT

struct wave_config {
  double freq{}, rate{};
};

inline void sine(std::span<std::int16_t> out, std::size_t channels, const wave_config& config, double& phase) noexcept {
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

struct alsa_error {
  int         err_num{};
  std::string err_msg;
};

using alsa_result = std::expected<void, alsa_error>;

[[nodiscard]] inline auto
make_alsa_error(int result, std::string_view message, bool close_device = false, snd_pcm_t* handle = nullptr) // NOLINT
  -> alsa_result {
  if (close_device && handle != nullptr) {
    snd_pcm_close(handle);
  } else {
    return std::unexpected(alsa_error{.err_num = 1, .err_msg = "cannot close a null playback_handle"});
  }
  return std::unexpected(alsa_error{.err_num = result, .err_msg = std::format("cannot {} {}", message, snd_strerror(result))});
}

[[nodiscard]] auto try_to(std::string_view message, auto func, auto... args) noexcept {
  return [=] -> alsa_result {
    if (const int result = func(args...); result < 0) {
      return make_alsa_error(result, message);
    }
    return {};
  };
}

[[nodiscard]] inline int playback_callback(snd_pcm_sframes_t nframes) { // this is basically the 'process' block
  auto              err{int{}};
  static auto       phase{double{}};
  const wave_config config{.freq = timeout, .rate = static_cast<double>(sample_rate)};

  std::println("playback callback called with {} frames", nframes);

  // ... fill buf with data ...
  sine(buf, channel_count, config, phase);

  if (err = static_cast<int>(snd_pcm_writei(playback_handle, buf.data(), nframes)); err < 0) {
    std::println("write failed {}", snd_strerror(err));
  }

  return err;
}

[[nodiscard]] inline auto interrupt(const std::stop_token& stop) -> alsa_result {

  snd_pcm_hw_params_t* hw_params{};
  snd_pcm_sw_params_t* sw_params{};
  snd_pcm_sframes_t    frames_to_deliver{};
  int                  err{};

  const auto configure_hw = [&](std::string_view message, auto func, auto... args) {
    return try_to(message, func, playback_handle, hw_params, args...);
  };

  const auto configure_hardware =
    alsa_result{}
      .and_then(try_to("open audio device", snd_pcm_open, &playback_handle, device_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0))
      .and_then(try_to("allocate hardware parameter structure", snd_pcm_hw_params_malloc, &hw_params))
      .and_then(configure_hw("initialize hardware parameter structure", snd_pcm_hw_params_any))
      .and_then(configure_hw("set access type", snd_pcm_hw_params_set_access, SND_PCM_ACCESS_RW_INTERLEAVED))
      .and_then(configure_hw("set sample format", snd_pcm_hw_params_set_format, SND_PCM_FORMAT_S16_LE))
      .and_then(configure_hw("set sample rate", snd_pcm_hw_params_set_rate_near, &sample_rate, nullptr))
      .and_then(configure_hw("set channel count", snd_pcm_hw_params_set_channels, channel_count))
      .and_then(configure_hw("set parameters", snd_pcm_hw_params));

  if (!configure_hardware.has_value()) {
    return configure_hardware;
  }

  snd_pcm_hw_params_free(hw_params);

  // Tell ALSA to wake us up whenever 4096 or more frames of playback data can be delivered.
  // Also, tell ALSA that we'll start the device ourselves.

  const auto configure_sw = [&](std::string_view message, auto func, auto... args) {
    return try_to(message, func, playback_handle, sw_params, args...);
  };

  const auto configure_software =
    alsa_result{}
      .and_then(try_to("allocate software parameters structure", snd_pcm_sw_params_malloc, &sw_params))
      .and_then(configure_sw("initialize software parameters structure", snd_pcm_sw_params_current))
      .and_then(configure_sw("set minimum available count", snd_pcm_sw_params_set_avail_min, buffer_size))
      .and_then(configure_sw("set start mode", snd_pcm_sw_params_set_start_threshold, 0U))
      .and_then(configure_sw("set software parameters", snd_pcm_sw_params))
      // the interface will interrupt the kernel every 4096 frames, and ALSA will wake up this program very soon after that.
      .and_then(try_to("prepare audio interface for use", snd_pcm_prepare, playback_handle));

  if (!configure_software.has_value()) {
    return configure_software;
  }

  while (!stop.stop_requested()) {
    // wait till the interface is ready for data, or 1 second has elapsed.

    if (err = snd_pcm_wait(playback_handle, timeout); err < 0) {
      return make_alsa_error(errno, "poll failed", /*close_device=*/true, playback_handle);
    }

    // find out how much space is available for playback data

    if (frames_to_deliver = snd_pcm_avail_update(playback_handle); frames_to_deliver < 0) {
      if (frames_to_deliver == -EPIPE) {
        return make_alsa_error(-EPIPE, "an xrun occured", /*close_device=*/true, playback_handle);
      }
      return make_alsa_error(
        1, std::format("unknown ALSA avail update return value {}", frames_to_deliver), /*close_device=*/true, playback_handle
      );
    }

    frames_to_deliver = std::cmp_greater(frames_to_deliver, buffer_size) ? buffer_size : frames_to_deliver;

    // deliver the data

    if (playback_callback(frames_to_deliver) != frames_to_deliver) {
      return make_alsa_error(1, "playback callback failed", /*close_device=*/true, playback_handle);
    }
  }

  return {};
}
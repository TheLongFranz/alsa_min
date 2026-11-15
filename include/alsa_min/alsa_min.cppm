module;

// Linux stuff
#include <alsa/asoundlib.h>

export module alsa;

// STL STUFF
import std;

template<auto Free>
struct c_deleter {
  void operator()(auto* p) const noexcept { Free(p); }
};
using pcm_handle       = std::unique_ptr<snd_pcm_t, c_deleter<snd_pcm_close>>;
using hw_params_handle = std::unique_ptr<snd_pcm_hw_params_t, c_deleter<snd_pcm_hw_params_free>>;
using sw_params_handle = std::unique_ptr<snd_pcm_sw_params_t, c_deleter<snd_pcm_sw_params_free>>;

struct audio_device_config {
  std::int16_t      channel_count{2};
  std::uint32_t     sample_rate{96'000}; // NOLINT
  snd_pcm_uframes_t period_size{1024};   // NOLINT
  snd_pcm_uframes_t buffer_size{4096};   // NOLINT
};

class audio_device {
  public:
  auto open() {}
  auto run() {}
  // [[nodiscard]] auto get_pcm_handle(this auto&& self) noexcept { return std::forward_like<decltype(self)>(self).m_pcm.get();
  // }
  // [[nodiscard]] auto get_buffer_size(this auto&& self) noexcept { return std::forward_like<decltype(self)>(self).m_config.buffer_size; }
  // [[nodiscard]] auto get_period_size(this auto&& self) noexcept { return std::forward_like<decltype(self)>(self).m_config.period_size; }

  audio_device_config       m_config{};
  pcm_handle                m_pcm;
  std::vector<float>        m_scratch_buffer;
  std::vector<std::int16_t> m_out_buffer;
};

struct alsa_error {
  int         err_num{};
  std::string err_msg;
};

using alsa_result = std::expected<void, alsa_error>;

// pcm_handle       playback_handle; // NOLINT
std::atomic<int> xrun_count{}; // NOLINT

struct wave_config {
  double freq{}, rate{};
};

// constexpr auto buffer_size{std::size_t{4096}};
// constexpr auto channel_count{std::int16_t{2}};
constexpr auto device_name{std::string{"default"}};
constexpr auto timeout{std::size_t{1000}};
constexpr auto sine_frequency{std::size_t{440}};
// auto           buf{audio_buffer{}}; // NOLINT

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

[[nodiscard]] inline auto make_alsa_error(int result, std::string_view message) // NOLINT
  -> alsa_result {
  return std::unexpected(alsa_error{.err_num = result, .err_msg = std::format("{}: {}", message, snd_strerror(result))});
}

[[nodiscard]] auto try_to(std::string_view message, auto func, auto... args) noexcept {
  return [=] -> alsa_result {
    if (const int result = func(args...); result < 0) {
      return make_alsa_error(result, message);
    }
    return {};
  };
}

[[nodiscard]] inline int playback_callback(snd_pcm_sframes_t nframes, const audio_device& device) { // this is basically the 'process' block
  auto err{int{}};
  // static auto       phase{double{}};
  // const wave_config config{.freq = sine_frequency, .rate = static_cast<double>(device.m_config.sample_rate)};

  // ... fill buf with data ...
  // sine(std::span{device.m_out_buffer}.first(nframes * device.m_config.channel_count), device.m_config.channel_count, config, phase);

  if (err = static_cast<int>(snd_pcm_writei(device.m_pcm.get(), device.m_out_buffer.data(), nframes)); err < 0) {
    std::println("write failed {}", snd_strerror(err));
  }

  return err;
}

export [[nodiscard]] inline auto interrupt(const std::stop_token& stop) -> alsa_result {
  // audio_device_config ad_config{}; // NOLINT
  audio_device      device{};
  hw_params_handle  hw_params{};
  sw_params_handle  sw_params{};
  snd_pcm_sframes_t frames_to_deliver{};

  const auto configure_hw = [&](std::string_view message, auto func, auto... args) {
    return try_to(message, func, device.m_pcm.get(), hw_params.get(), args...);
  };

  // Both the playback handle and hardware parameters must be allocated before setting up
  if (const int err = snd_pcm_open(std::out_ptr(device.m_pcm), device_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0); err < 0) {
    return make_alsa_error(err, "open audio device");
  }

  if (const int err = snd_pcm_hw_params_malloc(std::out_ptr(hw_params)); err < 0) {
    return make_alsa_error(err, "allocate hardware parameter structure");
  }

  const auto configure_hardware =
    alsa_result{}
      .and_then(configure_hw("initialize hardware parameter structure", snd_pcm_hw_params_any))
      .and_then(configure_hw("set access type", snd_pcm_hw_params_set_access, SND_PCM_ACCESS_RW_INTERLEAVED))
      .and_then(configure_hw("set sample format", snd_pcm_hw_params_set_format, SND_PCM_FORMAT_S16_LE))
      .and_then(configure_hw("set channel count", snd_pcm_hw_params_set_channels, device.m_config.channel_count))
      .and_then(configure_hw("set sample rate", snd_pcm_hw_params_set_rate_near, &device.m_config.sample_rate, nullptr))
      .and_then(configure_hw("set period size", snd_pcm_hw_params_set_period_size_near, &device.m_config.period_size, nullptr))
      .and_then(configure_hw("set buffer size", snd_pcm_hw_params_set_buffer_size_near, &device.m_config.buffer_size))
      .and_then(configure_hw("set parameters", snd_pcm_hw_params));

  if (!configure_hardware.has_value()) {
    return configure_hardware;
  }

  // Tell ALSA to wake us up whenever 4096 or more frames of playback data can be delivered.
  // Also, tell ALSA that we'll start the device ourselves.

  const auto configure_sw = [&](std::string_view message, auto func, auto... args) {
    return try_to(message, func, device.m_pcm.get(), sw_params.get(), args...);
  };

  if (const int err = snd_pcm_sw_params_malloc(std::out_ptr(sw_params)); err < 0) {
    return make_alsa_error(err, "allocate software parameters structure");
  }

  const auto configure_software =
    alsa_result{}
      .and_then(configure_sw("initialize software parameters structure", snd_pcm_sw_params_current))
      .and_then(configure_sw("set minimum available count", snd_pcm_sw_params_set_avail_min, device.m_config.period_size))
      .and_then(configure_sw("set minimum available count", snd_pcm_sw_params_set_start_threshold, device.m_config.buffer_size))
      .and_then(configure_sw("set start mode", snd_pcm_sw_params_set_start_threshold, 0U))
      .and_then(configure_sw("set software parameters", snd_pcm_sw_params))
      // the interface will interrupt the kernel every 4096 frames, and ALSA will wake up this program very soon after that.
      .and_then(try_to("prepare audio interface for use", snd_pcm_prepare, device.m_pcm.get()));

  if (!configure_software.has_value()) {
    return configure_software;
  }

  while (!stop.stop_requested()) {
    // wait till the interface is ready for data, or 1 second has elapsed.

    if (const int err = snd_pcm_wait(device.m_pcm.get(), timeout); err < 0) {
      std::println("poll failed {}", snd_strerror(err));
      break;
    }

    // find out how much space is available for playback data

    if (frames_to_deliver = snd_pcm_avail_update(device.m_pcm.get()); frames_to_deliver < 0) {
      if (frames_to_deliver == -EPIPE) {
        std::println("an xrun occured {}", snd_strerror(-EPIPE));
        snd_pcm_recover(device.m_pcm.get(), -EPIPE, 0);
        // break;
      }
      std::println("unknown ALSA avail update return value {}", frames_to_deliver);
      break;
    }

    frames_to_deliver = std::cmp_greater(frames_to_deliver, device.m_config.buffer_size)
                          ? static_cast<std::int64_t>(device.m_config.buffer_size)
                          : frames_to_deliver;

    // deliver the data

    if (playback_callback(frames_to_deliver, device) != frames_to_deliver) {
      std::println("playback callback failed");
      break;
    }
  }
  return {};
}

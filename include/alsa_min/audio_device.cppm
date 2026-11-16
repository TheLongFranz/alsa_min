module;

// Linux stuff
#include <alsa/asoundlib.h>

export module audio_device;

// STL STUFF
import std;

// Our stuff
import audio_processor;

static constexpr std::int16_t      default_channel_count{2};
static constexpr std::uint32_t     default_sample_rate{48'000};
static constexpr snd_pcm_uframes_t default_period_size{1024};
static constexpr snd_pcm_uframes_t default_buffer_size{4096};

export struct audio_stats {
  std::atomic<std::uint64_t> wakeups;
  std::atomic<std::uint64_t> frames;
  std::atomic<std::uint64_t> xruns;
};

export struct audio_device_config {
  std::int16_t      channel_count{default_channel_count};
  std::uint32_t     sample_rate{default_sample_rate};
  snd_pcm_uframes_t period_size{default_period_size};
  snd_pcm_uframes_t buffer_size{default_buffer_size};
};

template<auto Free>
struct c_deleter {
  void operator()(auto* p) const noexcept { Free(p); }
};
using pcm_handle       = std::unique_ptr<snd_pcm_t, c_deleter<snd_pcm_close>>;
using hw_params_handle = std::unique_ptr<snd_pcm_hw_params_t, c_deleter<snd_pcm_hw_params_free>>;
using sw_params_handle = std::unique_ptr<snd_pcm_sw_params_t, c_deleter<snd_pcm_sw_params_free>>;

struct alsa_error {
  int         err_num{};
  std::string err_msg;
};

export using alsa_result = std::expected<void, alsa_error>;

[[nodiscard]] inline auto make_alsa_error(int result, std::string_view message) -> alsa_result {
  return std::unexpected(alsa_error{.err_num = result, .err_msg = std::format("{}: {}", message, snd_strerror(result))});
}

export class audio_device {
  [[nodiscard]] static auto try_to(std::string_view message, auto func, auto... args) noexcept {
    return [=] -> alsa_result {
      if (const int result = func(args...); result < 0) {
        return make_alsa_error(result, message);
      }
      return {};
    };
  }

  [[nodiscard]] auto recover(snd_pcm_sframes_t code, std::string_view message) -> alsa_result {
    if (const int err = snd_pcm_recover(m_pcm.get(), static_cast<int>(code), 1); err < 0) {
      return make_alsa_error(err, message);
    }
    return {};
  }

  [[nodiscard]] auto handle_error(snd_pcm_sframes_t code, std::string_view what, audio_stats& stats) -> alsa_result {
    if (code == -EPIPE) {
      stats.xruns.fetch_add(1, std::memory_order_relaxed);
    }
    return recover(code, what);
  };

  static auto to_pcm_16(const float& sample) noexcept {
    return static_cast<std::int16_t>(std::clamp(sample, -1.0F, 1.0F) * static_cast<float>(std::numeric_limits<std::int16_t>::max()));
  }

  public:
  [[nodiscard]] audio_device(pcm_handle pcm, const audio_device_config& negotiated) : m_config(negotiated), m_pcm(std::move(pcm)) {
    const auto size{m_config.channel_count * m_config.period_size};
    m_scratch_buffer.resize(size);
    m_out_buffer.resize(size);
  }

  [[nodiscard]] static auto open(const std::string& device_name, audio_device_config& requested_config)
    -> std::expected<audio_device, alsa_error> {
    hw_params_handle hw_params{};
    sw_params_handle sw_params{};
    pcm_handle       pcm;

    const auto configure_hw = [&](std::string_view message, auto func, auto... args) {
      return try_to(message, func, pcm.get(), hw_params.get(), args...);
    };

    if (const int err = snd_pcm_open(std::out_ptr(pcm), device_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0); err < 0) {
      return std::unexpected(alsa_error{.err_num = err, .err_msg = std::format("{}: {}", "cannot open audio device", snd_strerror(err))});
    }

    if (const int err = snd_pcm_hw_params_malloc(std::out_ptr(hw_params)); err < 0) {
      return std::unexpected(
        alsa_error{.err_num = err, .err_msg = std::format("{}: {}", "cannot allocate hardware parameter structure", snd_strerror(err))}
      );
    }

    const auto configure_hardware =
      alsa_result{}
        .and_then(configure_hw("initialize hardware parameter structure", snd_pcm_hw_params_any))
        .and_then(configure_hw("set access type", snd_pcm_hw_params_set_access, SND_PCM_ACCESS_RW_INTERLEAVED))
        .and_then(configure_hw("set sample format", snd_pcm_hw_params_set_format, SND_PCM_FORMAT_S16_LE))
        .and_then(configure_hw("set channel count", snd_pcm_hw_params_set_channels, requested_config.channel_count))
        .and_then(
          configure_hw("set sample rate", snd_pcm_hw_params_set_rate_near, &requested_config.sample_rate, nullptr)
        ) // it turns out that dir = direction, that's fun to know!
        .and_then(
          configure_hw("set period size", snd_pcm_hw_params_set_period_size_near, &requested_config.period_size, nullptr)
        ) // it turns out that dir = direction, that's fun to know!
        .and_then(configure_hw("set buffer size", snd_pcm_hw_params_set_buffer_size_near, &requested_config.buffer_size))
        .and_then(configure_hw("set parameters", snd_pcm_hw_params));

    if (!configure_hardware.has_value()) {
      return std::unexpected(configure_hardware.error());
    }

    const auto configure_sw = [&](std::string_view message, auto func, auto... args) {
      return try_to(message, func, pcm.get(), sw_params.get(), args...);
    };

    if (const int err = snd_pcm_sw_params_malloc(std::out_ptr(sw_params)); err < 0) {
      return std::unexpected(
        alsa_error{.err_num = err, .err_msg = std::format("{}: {}", "cannot allocate software parameters structure", snd_strerror(err))}
      );
    }

    const auto configure_software =
      alsa_result{}
        .and_then(configure_sw("initialize software parameters structure", snd_pcm_sw_params_current))
        .and_then(configure_sw("set minimum available count", snd_pcm_sw_params_set_avail_min, requested_config.period_size))
        .and_then(configure_sw("set minimum start threshold", snd_pcm_sw_params_set_start_threshold, requested_config.buffer_size))
        .and_then(configure_sw("set software parameters", snd_pcm_sw_params))
        .and_then(try_to("prepare audio interface for use", snd_pcm_prepare, pcm.get()));

    if (!configure_software.has_value()) {
      return std::unexpected(configure_software.error());
    }

    return audio_device(std::move(pcm), requested_config);
  }

  [[nodiscard]] auto run(const std::stop_token& stop, audio_processor& proc, audio_stats& stats) -> alsa_result {
    proc.prepare({.sample_rate = m_config.sample_rate, .max_frames = m_config.period_size, .channels = m_config.channel_count});

    while (!stop.stop_requested()) {
      const int ready = snd_pcm_wait(m_pcm.get(), 1000);
      if (ready == 0) {
        return make_alsa_error(-EIO, "device stopped responding");
      }

      if (ready < 0) {
        if (auto r = handle_error(ready, "wait for device", stats); !r.has_value()) {
          return r;
        }
        continue;
      }

      stats.wakeups.fetch_add(1, std::memory_order_relaxed);

      const auto avail = snd_pcm_avail_update(m_pcm.get());
      if (avail < 0) {
        if (auto r = handle_error(avail, "check available space", stats); !r.has_value()) {
          return r;
        }
        continue;
      }

      if (std::cmp_less(avail, m_config.period_size)) {
        continue;
      }

      proc.process(m_scratch_buffer);

      std::ranges::transform(m_scratch_buffer, m_out_buffer.begin(), to_pcm_16);

      auto pending = std::span<const std::int16_t>{m_out_buffer};

      while (!pending.empty()) {
        const auto frames  = static_cast<snd_pcm_uframes_t>(pending.size() / m_config.channel_count);
        const auto written = snd_pcm_writei(m_pcm.get(), pending.data(), frames);
        if (written < 0) {
          if (auto r = handle_error(written, "write to device", stats); !r) {
            return r;
          }
          break;
        }
        stats.frames.fetch_add(static_cast<std::uint64_t>(written), std::memory_order_relaxed);
        pending = pending.subspan(static_cast<std::size_t>(written) * m_config.channel_count);
      }
    }
    return {};
  }

  auto run(const std::stop_token& stop, audio_processor& proc) -> alsa_result { // NOLINT
    proc.prepare({.sample_rate = m_config.sample_rate, .max_frames = m_config.period_size, .channels = m_config.channel_count});

    while (!stop.stop_requested()) {
      const int ready = snd_pcm_wait(m_pcm.get(), 1000);
      if (ready == 0) {
        return make_alsa_error(-EIO, "device stopped responding");
      }

      if (ready < 0) {
        if (auto r = recover(ready, "wait for device"); !r.has_value()) {
          return r;
        }
        continue;
      }

      const auto avail = snd_pcm_avail_update(m_pcm.get());
      if (avail < 0) {
        if (auto r = recover(avail, "check available space"); !r.has_value()) {
          return r;
        }
        continue;
      }

      if (std::cmp_less(avail, m_config.period_size)) {
        continue;
      }

      proc.process(m_scratch_buffer);

      std::ranges::transform(m_scratch_buffer, m_out_buffer.begin(), to_pcm_16);

      auto pending = std::span<const std::int16_t>{m_out_buffer};

      while (!pending.empty()) {
        const auto frames  = static_cast<snd_pcm_uframes_t>(pending.size() / m_config.channel_count);
        const auto written = snd_pcm_writei(m_pcm.get(), pending.data(), frames);
        if (written < 0) {
          if (auto r = recover(written, "write to device"); !r) {
            return r;
          }
          break;
        }
        pending = pending.subspan(static_cast<std::size_t>(written) * m_config.channel_count);
      }
    }
    return {};
  }

  private:
  audio_device_config       m_config;
  pcm_handle                m_pcm;
  std::vector<float>        m_scratch_buffer;
  std::vector<std::int16_t> m_out_buffer;
};

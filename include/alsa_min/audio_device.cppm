module;

// Linux stuff
#include <alsa/asoundlib.h>

export module audio_device;

// STL STUFF
import std;

// Our stuff
import audio_processor;

namespace alsa_min {
  static constexpr std::uint32_t     default_channel_count{2};
  static constexpr std::uint32_t     default_sample_rate{48'000};
  static constexpr snd_pcm_uframes_t default_period_size{1024};
  static constexpr snd_pcm_uframes_t default_buffer_size{4096};

  /**
   * @struct audio_stats: Optional stats used for monitoring the audio device state
   * @param wakeups: How many times alsa has awakened the device
   * @param frames:  How many frames have been processed by the audio device
   * @param xruns:   How many sample underrun or overruns have occurred, this will count +/- 1 per frame
   */
  export struct audio_stats {
    std::atomic<std::uint64_t> wakeups;
    std::atomic<std::uint64_t> frames;
    std::atomic<std::uint64_t> xruns;
  };

  /**
   * @struct audio_device_config:
   *
   * struct for keeping device parameters neat and tidy, best to use designated initialisers
   * @param channel_count: Number of channels
   * @param sample_rate:   Number of samples per second
   * @param period_size:   Number of samples that alsa will provide at a time
   * @param buffer_size:   Maximum buffer size
   */
  export struct audio_device_config {
    std::uint32_t     channel_count{default_channel_count};
    std::uint32_t     sample_rate{default_sample_rate};
    snd_pcm_uframes_t period_size{default_period_size};
    snd_pcm_uframes_t buffer_size{default_buffer_size};
  };

  /**
   * @struct unique_ptr deleter allowing us to use RAII for the playback handle & hardware/software parameters
   */
  template<auto Free>
  struct c_deleter {
    void operator()(auto* p) const noexcept
      requires(std::invocable<decltype(Free), decltype(p)>)
    {
      Free(p);
    }
  };

  ///> @typedef unique_ptr with custom deleter for playback handle
  export using pcm_handle = std::unique_ptr<snd_pcm_t, c_deleter<snd_pcm_close>>;
  ///> @typedef unique_ptr with custom deleter for hardware parameters
  export using hw_params_handle = std::unique_ptr<snd_pcm_hw_params_t, c_deleter<snd_pcm_hw_params_free>>;
  ///> @typedef unique_ptr with custom deleter for software parameters
  export using sw_params_handle = std::unique_ptr<snd_pcm_sw_params_t, c_deleter<snd_pcm_sw_params_free>>;

  /**
   * @struct alsa_error:
   */
  export struct alsa_error {
    int         err_num{};
    std::string err_msg;
  };

  ///> @typedef helper alias for functions that use expected for error handling, which is most of them.
  export using alsa_result = std::expected<void, alsa_error>;

  /**
   * @fn make_alsa_error:
   * @brief Helper function for dealing with return values from functions with c-style error handling i.e. returning an error code
   * @param result: Result will always be an error value from alsa
   * @param message: Usually a descriptive message of what we were trying to accomplish
   */
  [[nodiscard]] inline auto make_alsa_error(int result, std::string_view message) -> std::unexpected<alsa_error> {
    return std::unexpected(alsa_error{.err_num = result, .err_msg = std::format("{}: {}", message, snd_strerror(result))});
  }

  template<class Func, class... Args>
  concept alsa_call = std::invocable<Func, Args...> && std::same_as<std::invoke_result_t<Func, Args...>, int>;

  /**
   * @class audio_device
   * @brief Always create this class using the open() factory function,
   * if the device fails to be open you'll receive an unexpected instead of an unusable object
   *
   */
  export class audio_device {
    audio_device_config       m_config;
    pcm_handle                m_pcm;
    std::vector<float>        m_scratch_buffer;
    std::vector<std::int32_t> m_out_buffer;

    template<class Func, class... Args>
    [[nodiscard]] static auto try_to(std::string_view message, Func func, Args... args) noexcept
      requires(alsa_call<Func, Args...>)
    {
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
    }

    static auto to_pcm_32(const float& sample) noexcept -> std::int32_t {
      return static_cast<std::int32_t>(std::clamp(sample, -1.0F, 1.0F) * static_cast<double>(std::numeric_limits<std::int32_t>::max()));
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

      const auto hw_step = [&](std::string_view message, auto func, auto... args) {
        return try_to(message, func, pcm.get(), hw_params.get(), args...);
      };

      if (const int err = snd_pcm_open(std::out_ptr(pcm), device_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0); err < 0) {
        return make_alsa_error(err, "open audio device");
      }

      if (const int err = snd_pcm_hw_params_malloc(std::out_ptr(hw_params)); err < 0) {
        return make_alsa_error(err, "allocate hardware parameter structure");
      }

      const auto hw_result =
        alsa_result{}
          .and_then(hw_step("initialize hardware parameter structure", snd_pcm_hw_params_any))
          .and_then(hw_step("set access type", snd_pcm_hw_params_set_access, SND_PCM_ACCESS_RW_INTERLEAVED))
          .and_then(hw_step("set sample format", snd_pcm_hw_params_set_format, SND_PCM_FORMAT_S32_LE))
          .and_then(hw_step("set channel count", snd_pcm_hw_params_set_channels, requested_config.channel_count))
          .and_then(hw_step("set sample rate", snd_pcm_hw_params_set_rate_near, &requested_config.sample_rate, nullptr))
          .and_then(hw_step("set period size", snd_pcm_hw_params_set_period_size_near, &requested_config.period_size, nullptr))
          .and_then(hw_step("set buffer size", snd_pcm_hw_params_set_buffer_size_near, &requested_config.buffer_size))
          .and_then(hw_step("set parameters", snd_pcm_hw_params));

      if (!hw_result.has_value()) {
        return std::unexpected(hw_result.error());
      }

      const auto sw_step = [&](std::string_view message, auto func, auto... args) {
        return try_to(message, func, pcm.get(), sw_params.get(), args...);
      };

      if (const int err = snd_pcm_sw_params_malloc(std::out_ptr(sw_params)); err < 0) {
        return make_alsa_error(err, "allocate software parameters structure");
      }

      const auto sw_result =
        alsa_result{}
          .and_then(sw_step("initialize software parameters structure", snd_pcm_sw_params_current))
          .and_then(sw_step("set minimum available count", snd_pcm_sw_params_set_avail_min, requested_config.period_size))
          .and_then(sw_step("set minimum start threshold", snd_pcm_sw_params_set_start_threshold, requested_config.buffer_size))
          .and_then(sw_step("set software parameters", snd_pcm_sw_params))
          .and_then(try_to("prepare audio interface for use", snd_pcm_prepare, pcm.get()));

      if (!sw_result.has_value()) {
        return std::unexpected(sw_result.error());
      }

      return audio_device(std::move(pcm), requested_config);
    }

    /**
     * @fn run()
     * @brief run prepares the audio_processor/device before looping infinitely until the thread tells it to stop
     * @param audio_processor: class which can be inherited from to implement custom effects.
     * @param stats: object outside of this class which is updated atomically
     * @return std::expected<void> on success, std::unexpected<alsa_error> on failure
     */
    [[nodiscard]] auto run(const std::stop_token& stop, audio_processor& proc, audio_stats& stats) -> alsa_result {
      proc.prepare({.sample_rate = m_config.sample_rate, .channels = m_config.channel_count, .max_frames = m_config.period_size});

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

        std::ranges::transform(m_scratch_buffer, m_out_buffer.begin(), to_pcm_32);

        auto pending = std::span{m_out_buffer};

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

    /**
     * @fn run()
     * @brief override of run() where we throw away the stats as I think they should be optional.
     * @param audio_processor: class which can be inherited from to implement custom effects.
     * I'll need to have a think about how the chaining of effects will work.
     * @return std::expected<void> on success, std::unexpected<alsa_error> on failure
     */
    auto run(const std::stop_token& stop, audio_processor& proc) -> alsa_result {
      audio_stats stats; // Don't (always) want this
      return run(stop, proc, stats);
    }
  };
} // namespace alsa_min
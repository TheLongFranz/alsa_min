module;

export module audio_processor;

import std;

export struct audio_process_spec {
  std::uint32_t sample_rate;
  std::uint32_t channels;
  std::size_t   max_frames;
};

export class audio_processor {
  public:
  virtual ~audio_processor()                                           = default;
  virtual auto prepare(const audio_process_spec& process_spec) -> void = 0;
  virtual auto process(std::span<float> out) -> void                   = 0;
};

export class sine final : public audio_processor {
  public:
  explicit sine(const double& freq) : m_freq(freq) {}
  auto prepare(const audio_process_spec& process_spec) -> void override {
    m_process_spec = process_spec;
    m_step         = {m_tau * m_freq / static_cast<double>(m_process_spec.sample_rate)};
  }

  auto process(std::span<float> out) noexcept -> void override {
    for (const auto frame: out | std::views::chunk(m_process_spec.channels)) {
      std::ranges::fill(frame, m_amp * std::sin(m_phase));
      m_phase += m_step;
      if (m_phase >= m_tau) {
        m_phase -= m_tau;
      }
    }
  }

  private:
  audio_process_spec    m_process_spec{};
  double                m_freq{}, m_phase{0}, m_step{};
  static constexpr auto m_amp{0.5};
  static constexpr auto m_tau{2.0 * std::numbers::pi};
};
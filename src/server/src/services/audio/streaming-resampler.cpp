#include "services/audio/streaming-resampler.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace Audio {

namespace {
constexpr int PHASES = 64;
constexpr std::int64_t TRIM_THRESHOLD = 8192;
} // namespace

StreamingResampler::StreamingResampler(int inRate, int outRate)
    : m_inRate(inRate), m_outRate(outRate), m_step(static_cast<double>(inRate) / outRate) {
  if (inRate == outRate) return;

  const double cutoff = std::min(1.0, 1.0 / m_step);
  m_half = static_cast<int>(std::ceil(16.0 * std::max(1.0, m_step)));
  m_taps = 2 * m_half;
  m_kernel.resize(static_cast<std::size_t>(PHASES) * m_taps);

  for (int phase = 0; phase < PHASES; ++phase) {
    const auto weights = std::span(m_kernel).subspan(static_cast<std::size_t>(phase) * m_taps, m_taps);
    const double frac = static_cast<double>(phase) / PHASES;
    double norm = 0.0;
    for (int j = 0; j < m_taps; ++j) {
      const double x = frac + m_half - 1 - j;
      const double window = 0.5 + 0.5 * std::cos(std::numbers::pi * x / m_half);
      const double arg = std::numbers::pi * cutoff * x;
      const double sinc = std::abs(arg) < 1e-9 ? 1.0 : std::sin(arg) / arg;
      weights[j] = sinc * window;
      norm += weights[j];
    }
    for (auto &weight : weights) {
      weight /= norm;
    }
  }
}

void StreamingResampler::produce(std::vector<float> &out, std::int64_t limit) {
  const auto available = m_base + static_cast<std::int64_t>(m_history.size());

  while (m_produced < limit) {
    const double center = static_cast<double>(m_produced) * m_step;
    const auto i0 = static_cast<std::int64_t>(std::floor(center));
    const std::int64_t first = i0 - m_half + 1;
    const std::int64_t last = first + m_taps - 1;
    if (last >= available) break;

    const int phase = std::min(static_cast<int>((center - static_cast<double>(i0)) * PHASES + 0.5), PHASES - 1);
    const auto weights = std::span(m_kernel).subspan(static_cast<std::size_t>(phase) * m_taps, m_taps);
    double acc = 0.0;
    for (std::int64_t i = std::max<std::int64_t>(first, m_base); i <= last; ++i) {
      acc += weights[static_cast<std::size_t>(i - first)] * m_history[static_cast<std::size_t>(i - m_base)];
    }
    out.emplace_back(static_cast<float>(acc));
    ++m_produced;
  }

  // keep what the next output's window still reaches
  const double center = static_cast<double>(m_produced) * m_step;
  const std::int64_t keepFrom = static_cast<std::int64_t>(std::floor(center)) - m_half + 1;
  const auto drop = std::clamp<std::int64_t>(keepFrom - m_base, 0, static_cast<std::int64_t>(m_history.size()));
  if (drop >= TRIM_THRESHOLD) {
    m_history.erase(m_history.begin(), m_history.begin() + drop);
    m_base += drop;
  }
}

std::vector<float> StreamingResampler::push(std::span<const float> in) {
  if (m_inRate == m_outRate) return {in.begin(), in.end()};

  m_history.insert(m_history.end(), in.begin(), in.end());
  m_totalIn += static_cast<std::int64_t>(in.size());

  std::vector<float> out;
  out.reserve(static_cast<std::size_t>(static_cast<double>(in.size()) / m_step) + 1);
  produce(out, static_cast<std::int64_t>(static_cast<double>(m_totalIn) / m_step));
  return out;
}

std::vector<float> StreamingResampler::flush() {
  if (m_inRate == m_outRate) return {};

  m_history.insert(m_history.end(), static_cast<std::size_t>(m_taps), 0.0f);
  std::vector<float> out;
  produce(out, static_cast<std::int64_t>(static_cast<double>(m_totalIn) / m_step));
  return out;
}

} // namespace Audio

#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace Audio {

/**
 * Converts the sample rate of a mono stream that arrives in chunks, with the same windowed-sinc filter the
 * recorder uses for whole recordings. Output is delayed by half the filter length until `flush`.
 */
class StreamingResampler {
public:
  StreamingResampler(int inRate, int outRate);

  std::vector<float> push(std::span<const float> in);
  /** Everything still held back, as if the stream was followed by silence. */
  std::vector<float> flush();

  int inRate() const { return m_inRate; }
  int outRate() const { return m_outRate; }

private:
  void produce(std::vector<float> &out, std::int64_t limit);

  int m_inRate;
  int m_outRate;
  double m_step;
  int m_half = 0;
  int m_taps = 0;
  std::vector<double> m_kernel;
  std::vector<float> m_history;
  // absolute index of m_history[0]
  std::int64_t m_base = 0;
  std::int64_t m_totalIn = 0;
  std::int64_t m_produced = 0;
};

} // namespace Audio

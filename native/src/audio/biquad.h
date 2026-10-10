#pragma once

// RBJ audio-EQ-cookbook biquads (Robert Bristow-Johnson). Direct form II transposed in double, so
// low frequencies at 48 kHz keep their precision.

#include <cmath>

namespace sf::audio {

enum class BiquadType { LowPass, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf };

struct BiquadCoeffs {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; // a0 normalised to 1

  // |H| at `freq` Hz
  double magnitude(double freq, double sampleRate) const {
    const double w = 2.0 * 3.14159265358979323846 * freq / sampleRate;
    const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    const double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
  }
};

inline BiquadCoeffs designBiquad(BiquadType type, double sampleRate, double freq, double gainDb = 0, double q = 0.7071067811865476) {
  const double pi = 3.14159265358979323846;
  freq = std::fmin(std::fmax(freq, 1.0), sampleRate * 0.499);
  q = std::fmax(q, 0.01);
  const double w0 = 2 * pi * freq / sampleRate;
  const double cs = std::cos(w0), sn = std::sin(w0);
  const double alpha = sn / (2 * q);
  const double A = std::pow(10.0, gainDb / 40.0);
  double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
  switch (type) {
    case BiquadType::LowPass:
      b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = b0; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
      break;
    case BiquadType::HighPass:
      b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = b0; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
      break;
    case BiquadType::BandPass:
      b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
      break;
    case BiquadType::Notch:
      b0 = 1; b1 = -2 * cs; b2 = 1; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
      break;
    case BiquadType::Peak:
      b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A;
      break;
    case BiquadType::LowShelf: {
      const double t = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) - (A - 1) * cs + t);
      b1 = 2 * A * ((A - 1) - (A + 1) * cs);
      b2 = A * ((A + 1) - (A - 1) * cs - t);
      a0 = (A + 1) + (A - 1) * cs + t;
      a1 = -2 * ((A - 1) + (A + 1) * cs);
      a2 = (A + 1) + (A - 1) * cs - t;
      break;
    }
    case BiquadType::HighShelf: {
      const double t = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) + (A - 1) * cs + t);
      b1 = -2 * A * ((A - 1) + (A + 1) * cs);
      b2 = A * ((A + 1) + (A - 1) * cs - t);
      a0 = (A + 1) - (A - 1) * cs + t;
      a1 = 2 * ((A - 1) - (A + 1) * cs);
      a2 = (A + 1) - (A - 1) * cs - t;
      break;
    }
  }
  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

class Biquad {
public:
  void set(const BiquadCoeffs& c) { c_ = c; }
  void reset() { z1_ = z2_ = 0; }
  float process(float x) {
    const double in = x;
    const double y = c_.b0 * in + z1_;
    z1_ = c_.b1 * in - c_.a1 * y + z2_;
    z2_ = c_.b2 * in - c_.a2 * y;
    return static_cast<float>(y);
  }
  double processD(double in) {
    const double y = c_.b0 * in + z1_;
    z1_ = c_.b1 * in - c_.a1 * y + z2_;
    z2_ = c_.b2 * in - c_.a2 * y;
    return y;
  }
  const BiquadCoeffs& coeffs() const { return c_; }

private:
  BiquadCoeffs c_;
  double z1_ = 0, z2_ = 0;
};

} // namespace sf::audio

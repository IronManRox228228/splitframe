#pragma once
// One Euro filter: smooths a noisy signal over time with an adaptive cutoff.
//   cutoff = minCutoff + beta * |filtered derivative|,   alpha(cutoff, dt) = 1 / (1 + 1/(cutoff * dt))
// The derivative itself is low-passed at dCutoff (default 1.0). Deterministic: no model, no randomness.
// Call filter(x, t) once per sample in increasing time order; the first call returns x unchanged.
class OneEuroFilter {
public:
  OneEuroFilter(double minCutoff = 1.0, double beta = 0.05, double dCutoff = 1.0)
    : minCutoff_(minCutoff), beta_(beta), dCutoff_(dCutoff) {}

  double filter(double x, double t) {
    if (first_) {
      first_ = false;
      xPrev_ = x;
      dxPrev_ = 0.0;
      tPrev_ = t;
      return x;
    }
    const double dt = t - tPrev_;
    const double dx = dt > 0.0 ? (x - xPrev_) / dt : 0.0;
    const double aD = alpha(dCutoff_, dt);
    const double dxHat = aD * dx + (1.0 - aD) * dxPrev_;
    const double cutoff = minCutoff_ + beta_ * std::abs(dxHat);
    const double a = alpha(cutoff, dt);
    const double xHat = a * x + (1.0 - a) * xPrev_;
    xPrev_ = xHat;
    dxPrev_ = dxHat;
    tPrev_ = t;
    return xHat;
  }

private:
  static double alpha(double cutoff, double dt) {
    if (dt <= 0.0 || cutoff <= 0.0) return 1.0;
    return 1.0 / (1.0 + 1.0 / (cutoff * dt));
  }

  double minCutoff_ = 1.0;
  double beta_ = 0.05;
  double dCutoff_ = 1.0;
  double xPrev_ = 0.0;
  double dxPrev_ = 0.0;
  double tPrev_ = 0.0;
  bool first_ = true;
};

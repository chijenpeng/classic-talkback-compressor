#pragma once
#include <array>
#include <cmath>
#include <cstddef>

namespace lmc::device {
// Forward chain differentiation evaluates the exact selected constitutive
// branch. Independent central/one-sided differences are implemented outside
// this library.
template <std::size_t N> struct Dual {
  double value{};
  std::array<double, N> derivative{};
  Dual() = default;
  Dual(double v) : value(v) {}
  static Dual variable(double v, std::size_t i) {
    Dual r(v);
    r.derivative.at(i) = 1.0;
    return r;
  }
};
template <std::size_t N> Dual<N> operator+(Dual<N> a, Dual<N> b) {
  Dual<N> r(a.value + b.value);
  for (std::size_t i = 0; i < N; ++i)
    r.derivative[i] = a.derivative[i] + b.derivative[i];
  return r;
}
template <std::size_t N> Dual<N> operator-(Dual<N> a, Dual<N> b) {
  Dual<N> r(a.value - b.value);
  for (std::size_t i = 0; i < N; ++i)
    r.derivative[i] = a.derivative[i] - b.derivative[i];
  return r;
}
template <std::size_t N> Dual<N> operator-(Dual<N> a) { return Dual<N>(0) - a; }
template <std::size_t N> Dual<N> operator*(Dual<N> a, Dual<N> b) {
  Dual<N> r(a.value * b.value);
  for (std::size_t i = 0; i < N; ++i)
    r.derivative[i] = a.derivative[i] * b.value + a.value * b.derivative[i];
  return r;
}
template <std::size_t N> Dual<N> operator/(Dual<N> a, Dual<N> b) {
  Dual<N> r(a.value / b.value);
  for (std::size_t i = 0; i < N; ++i)
    r.derivative[i] = (a.derivative[i] - r.value * b.derivative[i]) / b.value;
  return r;
}
template <std::size_t N> Dual<N> lift(Dual<N> a, double value, double slope) {
  Dual<N> r(value);
  for (std::size_t i = 0; i < N; ++i)
    r.derivative[i] = slope * a.derivative[i];
  return r;
}
template <std::size_t N> Dual<N> positive(Dual<N> a) {
  return a.value > 0 ? a : Dual<N>(0);
}
template <std::size_t N> Dual<N> limit(Dual<N> a, double low, double high) {
  return a.value <= low ? Dual<N>(low) : (a.value >= high ? Dual<N>(high) : a);
}
} // namespace lmc::device

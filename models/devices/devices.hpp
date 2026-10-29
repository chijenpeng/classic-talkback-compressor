#pragma once
#include "dual.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <stdexcept>

namespace lmc::device {
inline constexpr double boltzmannOverCharge = 8.617333262145e-5;
inline constexpr double nominalTemperature = 298.15;
inline bool finite(std::initializer_list<double> values) {
  for (double value : values)
    if (!std::isfinite(value))
      return false;
  return true;
}

struct DiodeParameters {
  double saturationCurrent = 5e-10, ideality = 1.7, seriesResistance = 2;
  double junctionCapacitance = 2e-12, junctionPotential = .8, grading = .5;
  double transitTime = 2e-9, temperature = nominalTemperature;
  void validate() const {
    if (!finite({saturationCurrent, ideality, seriesResistance,
                 junctionCapacitance, junctionPotential, grading, transitTime,
                 temperature}) ||
        !(saturationCurrent > 0 && ideality > 0 && seriesResistance >= 0 &&
          junctionCapacitance >= 0 && junctionPotential > 0 && grading >= 0 &&
          grading < 1 && transitTime >= 0 && temperature > 0))
      throw std::domain_error("invalid diode parameter");
  }
};
struct TwoTerminal {
  double current{}, charge{}, conductance{}, capacitance{}, junctionVoltage{};
};
// Current and charge enter the anode; cathode receives their exact negatives.
// DC CHARACTERIZATION ONLY: series resistance is solved by safeguarded Newton.
// Returned Q and dQ/dV follow the DC manifold, NOT an eliminated dynamic
// device. Full transient MNA must use seriesJunction() below with independent
// junction voltage.
inline TwoTerminal diodeLogSaturation(double voltage, const DiodeParameters &p,
                                      double logSaturation) {
  p.validate();
  if (!std::isfinite(logSaturation))
    throw std::domain_error("invalid log saturation");
  const double saturation = std::exp(logSaturation);
  if (!std::isfinite(voltage) || std::abs(voltage) > 30)
    throw std::domain_error("diode voltage domain");
  const double scale = p.ideality * boltzmannOverCharge * p.temperature;
  auto intrinsic = [&](double v) {
    const double logForward = logSaturation + v / scale;
    // Guard only unconverged trial points. Accepted domains stay far below
    // exp(680).
    const double forward = std::exp(std::min(680.0, logForward));
    return std::array<double, 2>{forward - saturation, forward / scale};
  };
  double junction = voltage;
  if (p.seriesResistance > 0) {
    double low = std::min(voltage, 0.0), high = std::max(voltage, 0.0);
    junction = (low + high) * .5;
    bool converged = false;
    for (int iteration = 0; iteration < 160; ++iteration) {
      auto ig = intrinsic(junction);
      const double f = junction + p.seriesResistance * ig[0] - voltage;
      if (f > 0)
        high = junction;
      else
        low = junction;
      if (std::abs(f) < 2e-14 || high - low < 2e-14) {
        converged = true;
        break;
      }
      const double trial = junction - f / (1 + p.seriesResistance * ig[1]);
      const double margin = .25 * (high - low);
      junction = (trial > low + margin && trial < high - margin)
                     ? trial
                     : (low + high) * .5;
    }
    if (!converged)
      throw std::runtime_error("diode solve did not converge");
  }
  if (logSaturation + junction / scale > 680)
    throw std::domain_error(
        "accepted diode point exceeds representable domain");
  const auto ig = intrinsic(junction);
  const double dvj = 1 / (1 + p.seriesResistance * ig[1]);
  const double conductance = ig[1] * dvj;
  const double fc = .5, boundary = fc * p.junctionPotential;
  double q = 0, c = 0;
  if (junction <= boundary) {
    const double factor = 1 - junction / p.junctionPotential;
    q = p.junctionCapacitance * p.junctionPotential / (1 - p.grading) *
        (1 - std::pow(factor, 1 - p.grading));
    c = p.junctionCapacitance * std::pow(factor, -p.grading);
  } else {
    const double cb = p.junctionCapacitance * std::pow(1 - fc, -p.grading);
    const double qb = p.junctionCapacitance * p.junctionPotential /
                      (1 - p.grading) * (1 - std::pow(1 - fc, 1 - p.grading));
    const double slope = cb * p.grading / (p.junctionPotential * (1 - fc));
    const double delta = junction - boundary;
    q = qb + cb * delta + .5 * slope * delta * delta;
    c = cb + slope * delta;
  }
  // Transit excess charge is referenced to zero bias: Qdiff=tau*I.
  // Adding the constant tau*Is gives SPICE absolute carrier charge; derivatives
  // and all transient charge differences are identical, including reverse bias.
  q += p.transitTime * ig[0];
  c += p.transitTime * ig[1];
  return {ig[0], q, conductance, c * dvj, junction};
}

inline TwoTerminal diode(double voltage, const DiodeParameters &p) {
  p.validate();
  return diodeLogSaturation(voltage, p, std::log(p.saturationCurrent));
}

// Intrinsic charged junction plus explicit resistor. Full MNA stamps:
// external anode current=(Va-Vj)/Rs; internal row=Ij(Vj)-(Va-Vj)/Rs+dQj/dt;
// cathode current=-Ij(Vj)-dQj/dt. No displacement current bypasses Rs.
struct SeriesJunction {
  double seriesCurrent{}, residual{}, charge{};
  std::array<double, 2> residualDerivative{}, chargeDerivative{};
  double intrinsicCurrent{}, intrinsicConductance{}, intrinsicCapacitance{};
};
inline SeriesJunction seriesJunction(double terminalVoltage,
                                     double junctionVoltage,
                                     const DiodeParameters &p) {
  p.validate();
  if (!finite({terminalVoltage, junctionVoltage}) || !(p.seriesResistance > 0))
    throw std::domain_error(
        "series junction needs finite state and positive resistance");
  DiodeParameters intrinsic = p;
  intrinsic.seriesResistance = 0;
  const auto j = diode(junctionVoltage, intrinsic);
  const double current =
      (terminalVoltage - junctionVoltage) / p.seriesResistance;
  return {current,
          j.current - current,
          j.charge,
          {-1 / p.seriesResistance, j.conductance + 1 / p.seriesResistance},
          {0, j.capacitance},
          j.current,
          j.conductance,
          j.capacitance};
}

struct JfetParameters {
  double idss = .014, cutoff = -4, lambda = .001;
  double gateCapacitance = 3e-12, gatePotential = .8, gateGrading = .5;
  double gateSaturation = 5e-11, gateIdeality = 1.5,
         drainSourceCapacitance = 1e-12;
  double gateTransitTime = 1e-9, temperature = nominalTemperature;
  double pinchOff() const {
    return cutoff /
           (1 - std::sqrt((1e-8 - gateSaturation) / (idss - gateSaturation)));
  }
  double beta() const {
    double v = pinchOff();
    return (idss - gateSaturation) / (v * v * (1 + 15 * lambda));
  }
  void validate() const {
    if (!finite({idss, cutoff, lambda, gateCapacitance, gatePotential,
                 gateGrading, gateSaturation, gateIdeality,
                 drainSourceCapacitance, gateTransitTime, temperature}) ||
        !(idss > 1e-8 && cutoff < 0 && lambda >= 0 && gateCapacitance >= 0 &&
          gatePotential > 0 && gateGrading >= 0 && gateGrading < 1 &&
          gateSaturation > 0 && gateSaturation < 1e-8 && gateIdeality > 0 &&
          drainSourceCapacitance >= 0 && gateTransitTime >= 0 &&
          temperature > 0))
      throw std::domain_error("invalid JFET parameter");
  }
};
struct ThreeTerminal {
  std::array<double, 3> current{}, charge{};
  std::array<std::array<double, 3>, 3> currentJacobian{}, chargeJacobian{};
};
inline ThreeTerminal jfet(double drain, double gate, double source,
                          const JfetParameters &p) {
  p.validate();
  if (!(std::isfinite(drain) && std::isfinite(gate) && std::isfinite(source)) ||
      std::max({std::abs(drain - gate), std::abs(source - gate),
                std::abs(drain - source)}) > 25)
    throw std::domain_error("JFET domain excludes avalanche");
  using D = Dual<3>;
  const D vd = D::variable(drain, 0), vg = D::variable(gate, 1),
          vs = D::variable(source, 2);
  const bool forward = drain >= source;
  const D high = forward ? vd : vs, low = forward ? vs : vd;
  const D v = high - low, u = vg - low - D(p.pinchOff());
  D channel;
  if (u.value > 0) {
    const D base = v.value < u.value ? D(p.beta()) * (D(2) * u * v - v * v)
                                     : D(p.beta()) * u * u;
    channel = base * (D(1) + D(p.lambda) * v);
  }
  if (!forward)
    channel = -channel;
  DiodeParameters gp{p.gateSaturation,  p.gateIdeality,  0,
                     p.gateCapacitance, p.gatePotential, p.gateGrading,
                     p.gateTransitTime, p.temperature};
  auto gd = diode(gate - drain, gp), gs = diode(gate - source, gp);
  const D igd = lift(vg - vd, gd.current, gd.conductance);
  const D igs = lift(vg - vs, gs.current, gs.conductance);
  const D qgd = lift(vg - vd, gd.charge, gd.capacitance);
  const D qgs = lift(vg - vs, gs.charge, gs.capacitance);
  const D qds = D(p.drainSourceCapacitance) * (vd - vs);
  std::array<D, 3> currents{channel - igd, igd + igs, -channel - igs};
  std::array<D, 3> charges{-qgd + qds, qgd + qgs, -qgs - qds};
  ThreeTerminal result;
  for (std::size_t i = 0; i < 3; ++i) {
    result.current[i] = currents[i].value;
    result.charge[i] = charges[i].value;
    result.currentJacobian[i] = currents[i].derivative;
    result.chargeJacobian[i] = charges[i].derivative;
  }
  return result;
}

struct ZenerParameters {
  double voltageAt5mA = 10, resistanceAt5mA = 10, resistanceAt1mA = 40;
  double capacitance = 50e-12, temperature = nominalTemperature;
  double avalancheScale() const {
    return (resistanceAt1mA - resistanceAt5mA) / 800;
  }
  double seriesResistance() const {
    return resistanceAt5mA - avalancheScale() / .005;
  }
  void validate() const {
    if (!finite({voltageAt5mA, resistanceAt5mA, resistanceAt1mA, capacitance,
                 temperature}) ||
        !(voltageAt5mA > 0 && avalancheScale() > 0 && seriesResistance() >= 0 &&
          capacitance >= 0 && temperature > 0))
      throw std::domain_error("invalid zener parameter");
  }
};
inline TwoTerminal zener(double voltage, const ZenerParameters &p) {
  p.validate();
  const double a = p.avalancheScale(), rs = p.seriesResistance();
  const double logSaturation =
      std::log(.005) + (-p.voltageAt5mA + rs * .005) / a;
  DiodeParameters reverse{
      1e-12,        a / (boltzmannOverCharge * p.temperature), rs, 0, .8, .5, 0,
      p.temperature};
  DiodeParameters forward{1e-12, 1.5, 2, 0, .8, .5, 0, p.temperature};
  const auto r = diodeLogSaturation(-voltage, reverse, logSaturation),
             f = diode(voltage, forward);
  return {f.current - r.current, p.capacitance * voltage,
          f.conductance + r.conductance, p.capacitance, voltage};
}

struct Transformer {
  double secondaryOverPrimary = 4, phase = 1;
  // Ideal lossless multiport constraint, plus separately stamped R/L/C family.
  std::array<double, 2> residual(double vp, double vs, double ip,
                                 double is) const {
    if (!finite({secondaryOverPrimary, phase, vp, vs, ip, is}) ||
        !(secondaryOverPrimary > 0 && (phase == 1 || phase == -1)))
      throw std::domain_error("invalid transformer");
    const double n = phase * secondaryOverPrimary;
    return {vs - n * vp, ip + n * is};
  }
};
inline TwoTerminal resistor(double voltage, double resistance) {
  if (!finite({voltage, resistance}) || !(resistance > 0))
    throw std::domain_error("resistance must be positive");
  return {voltage / resistance, 0, 1 / resistance, 0, voltage};
}
inline TwoTerminal capacitor(double voltage, double capacitance) {
  if (!finite({voltage, capacitance}) || !(capacitance >= 0))
    throw std::domain_error("capacitance must be nonnegative");
  return {0, capacitance * voltage, 0, capacitance, voltage};
}
struct PotentiometerBranches {
  double upperResistance{}, lowerResistance{};
};
inline PotentiometerBranches potentiometer(double totalResistance,
                                           double fraction) {
  if (!finite({totalResistance, fraction}) || totalResistance <= 0 ||
      fraction < 0 || fraction > 1)
    throw std::domain_error("invalid potentiometer");
  // fraction0 shorts wiper to lower terminal; fraction1 shorts it to upper.
  // A zero branch is an exact voltage constraint, never an invented tiny R.
  return {totalResistance * (1 - fraction), totalResistance * fraction};
}
struct InductorState {
  double flux{}, dFluxDCurrent{};
};
inline InductorState inductor(double current, double inductance) {
  if (!finite({current, inductance}) || inductance < 0)
    throw std::domain_error("invalid inductor");
  return {inductance * current, inductance};
}
// Chosen external capsule DC boundary only; AC source/impedance belongs to its
// separately declared harness. No noise, FET identity or historic capsule
// claim.
inline TwoTerminal capsuleBias(double voltage, double sinkCurrent) {
  if (!finite({voltage, sinkCurrent}) || sinkCurrent < 0)
    throw std::domain_error("invalid capsule bias");
  const double fraction = std::clamp(voltage, 0.0, 1.0);
  return {sinkCurrent * fraction, 0,
          voltage > 0 && voltage < 1 ? sinkCurrent : 0, 0, voltage};
}
} // namespace lmc::device

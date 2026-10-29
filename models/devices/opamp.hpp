#pragma once
#include "devices.hpp"

namespace lmc::device {
struct OpampParameters {
  double dcGain = 100000.0, c0 = 2e-11, g0 = 7.578308207955745e-08;
  double auxiliaryGain = 0.5830280657119974, auxiliaryG = 0.01,
         auxiliaryC = 1.366934547121868e-10;
  double leadTime = 5.197047669137634e-09, leadGain = 112.37685282793518;
  double outputTime = 1.0725501528599353e-06, memoryC = 1e-10, outputC = 1e-10;
  double inputSlewCurrent = 0.0002, outputSlew = 16000000.0;
  double auxiliaryCurrent = 0.05, outputCurrent = 0.038,
         outputResistance = 18.968201458314734;
  double headroom = 0.5, clampResistance = 100, inputResistance = 100000.0;
  double biasCurrent = 5e-07, offset = 0, quiescentCurrent = 0.004;
  double outputParallelResistance = 119.15995260662511,
         outputInductance = 0.00011915995260662516, outputStageGain = 2;
  bool differentialProtection = true;
  void validate() const {
    if (!finite({dcGain,
                 c0,
                 g0,
                 auxiliaryGain,
                 auxiliaryG,
                 auxiliaryC,
                 leadTime,
                 leadGain,
                 outputTime,
                 memoryC,
                 outputC,
                 inputSlewCurrent,
                 outputSlew,
                 auxiliaryCurrent,
                 outputCurrent,
                 outputResistance,
                 headroom,
                 clampResistance,
                 inputResistance,
                 biasCurrent,
                 offset,
                 quiescentCurrent,
                 outputParallelResistance,
                 outputInductance,
                 outputStageGain}) ||
        !(dcGain > 0 && c0 > 0 && g0 > 0 && auxiliaryGain >= 0 &&
          auxiliaryG > 0 && auxiliaryC > 0 && leadTime > 0 && leadGain >= 0 &&
          outputTime > 0 && memoryC > 0 && outputC > 0 &&
          inputSlewCurrent > 0 && outputSlew > 0 && auxiliaryCurrent > 0 &&
          outputCurrent > 0 && outputResistance > 0 && headroom >= .2 &&
          clampResistance > 0 && inputResistance > 0 && biasCurrent >= 0 &&
          quiescentCurrent >= 0 && outputParallelResistance > 0 &&
          outputInductance > 0 && outputStageGain > 0))
      throw std::domain_error("invalid opamp parameter");
  }
};
// Unknowns0..8 are PHYSICAL node voltages:
// +input,-input,+rail,-rail,out,x,y,z,w. Unknown9 is Foster branch current IL
// in AMPERES. Its separate DAE row is d(flux_Wb)/dt + fluxResidual_V =0. It is
// not a nodal charge/KCL row. x/y are the 5534 behavioral compensation
// ports5/8. z/w remain internal. External compensation is NOT part of this
// device. Stamp each factory CC_* manifest component exactly once using
// stampCapacitor() or the full-card capacitor kernel. No time integration,
// previous samples, detector laws or instantaneous state resets.
struct OpampResidual {
  std::array<double, 9> current{}, charge{};
  std::array<std::array<double, 10>, 9> currentJacobian{}, chargeJacobian{};
  double fluxResidual{}, flux{};
  std::array<double, 10> fluxResidualJacobian{}, fluxJacobian{};
};
inline OpampResidual opamp(const std::array<double, 10> &volts,
                           const OpampParameters &p) {
  p.validate();
  for (std::size_t k = 0; k < 9; ++k)
    if (!std::isfinite(volts[k]) || std::abs(volts[k]) > 100)
      throw std::domain_error("opamp voltage domain");
  if (!std::isfinite(volts[9]) || std::abs(volts[9]) > .2)
    throw std::domain_error("output inductor current domain");
  if (volts[2] < volts[3] || volts[2] - volts[3] > 44)
    throw std::domain_error("opamp supply domain");
  using D = Dual<10>;
  std::array<D, 10> v;
  std::array<D, 9> i, q;
  for (std::size_t k = 0; k < 10; ++k)
    v[k] = D::variable(volts[k], k);
  const D midpoint = (v[2] + v[3]) / D(2);
  const D x = v[5] - midpoint, y = v[6] - midpoint, z = v[7] - midpoint,
          w = v[8] - midpoint;
  const D powered = limit((v[2] - v[3] - D(2)) / D(4), 0, 1);
  // Explicit brownout candidate: no active drive/Iq below2V span, full above6V.
  // Stored passive charge is retained when unpowered.
  auto unitCompliance = [](D fraction) {
    // Select the interior one-sided derivative at both joins. At a state
    // exactly on its supplying rail this gives a valid nonzero aggregate
    // compliance/clamp Jacobian instead of a falsely singular zero slope.
    if (fraction.value < 0)
      return D(0);
    if (fraction.value > 1)
      return D(1);
    return fraction;
  };
  auto injection = [&](std::size_t node, D amperes, D complianceVoltage) {
    const bool sourcing = amperes.value >= 0;
    // Fixed0.2V source compliance region: active current cannot pump a state
    // above its supplying rail or below its return rail. Charge is retained.
    if (sourcing)
      amperes = amperes * unitCompliance((v[2] - complianceVoltage) / D(.2));
    else
      amperes = amperes * unitCompliance((complianceVoltage - v[3]) / D(.2));
    i[node] = i[node] - amperes;
    if (sourcing)
      i[2] = i[2] + amperes;
    else
      i[3] = i[3] + amperes;
    return amperes;
  };
  const D jx = powered * limit(D(p.dcGain * p.g0 / p.outputStageGain) *
                                   (v[0] - v[1] - D(p.offset)),
                               -p.inputSlewCurrent, p.inputSlewCurrent);
  const D jy = powered * limit(-D(p.auxiliaryG) * (y + D(p.auxiliaryGain) * x),
                               -p.auxiliaryCurrent, p.auxiliaryCurrent);
  const D jz = powered * limit(D(p.memoryC / p.leadTime) * (x - z),
                               -p.auxiliaryCurrent, p.auxiliaryCurrent);
  const D jw =
      powered * limit(D(p.outputC / p.outputTime) *
                          (D(p.outputStageGain) *
                               (D(1 + p.leadGain) * x - D(p.leadGain) * z) -
                           w),
                      -p.outputC * p.outputSlew, p.outputC * p.outputSlew);
  injection(5, jx, v[5]);
  injection(6, jy, v[6]);
  injection(7, jz, v[7]);
  injection(8, jw, v[8]);
  const D leak = D(p.g0) * x;
  i[5] = i[5] + leak;
  i[2] = i[2] - leak / D(2);
  i[3] = i[3] - leak / D(2);
  // Rail-connected finite conductance clamps retain excess internal charge.
  const D boundaryHeadroom(0); // Passive clamps connect to actual rails, not an
                               // uphill headroom reference.
  for (std::size_t k = 5; k < 9; ++k) {
    const D upper =
        positive(v[k] - v[2] + boundaryHeadroom) / D(p.clampResistance);
    const D lower =
        positive(v[3] + boundaryHeadroom - v[k]) / D(p.clampResistance);
    const D offLeak =
        (D(1) - powered) * (v[k] - midpoint) / D(p.clampResistance);
    i[k] = i[k] + powered * (upper - lower) + offLeak;
    i[2] = i[2] - powered * upper - offLeak / D(2);
    i[3] = i[3] + powered * lower - offLeak / D(2);
  }
  // Output drive is supplied by the physical rails. A loaded output can never
  // be powered by a ground-referenced voltage source hidden from rail KCL.
  D target = v[8];
  if (v[2].value - v[3].value > 2 * p.headroom) {
    if (target.value > v[2].value - p.headroom)
      target = v[2] - D(p.headroom);
    if (target.value < v[3].value + p.headroom)
      target = v[3] + D(p.headroom);
  } else
    target = midpoint;
  // Exact static elimination of R0+(Rh||L), retaining the inductor state.
  // At activation a, Iout=a*clip((U-Vout+Rh*IL)/(R0+a*Rh)).
  // VL=Rh*(Iout-IL). This also preserves the exact brownout denominator. At
  // current limit U follows the same resistor equations instead of forcing a
  // hidden voltage.
  const D proposed =
      powered * limit((target - v[4] + D(p.outputParallelResistance) * v[9]) /
                          (D(p.outputResistance) +
                           powered * D(p.outputParallelResistance)),
                      -p.outputCurrent, p.outputCurrent);
  const D driveVoltage =
      v[4] + D(p.outputParallelResistance) * (proposed - v[9]);
  const D output = injection(4, proposed, driveVoltage);
  const D fluxResidual = -D(p.outputParallelResistance) * (output - v[9]);
  const D flux = D(p.outputInductance) * v[9];
  const D differential = (v[0] - v[1]) / D(p.inputResistance);
  i[0] = i[0] + differential + D(p.biasCurrent) * powered;
  i[1] = i[1] - differential + D(p.biasCurrent) * powered;
  i[3] = i[3] - D(2 * p.biasCurrent) * powered;
  if (p.differentialProtection) {
    DiodeParameters protection{1e-14, 1,  0, 2e-12,
                               .8,    .5, 0, nominalTemperature};
    // Full input domain excludes destructive differential overdrive>0.9V;
    // series external source impedances must bound the resulting currents.
    if (std::abs(volts[0] - volts[1]) > .9)
      throw std::domain_error("5534 input protection domain");
    const auto plus = diode(volts[0] - volts[1], protection);
    const auto minus = diode(volts[1] - volts[0], protection);
    const D drive = lift(v[0] - v[1], plus.current + (-minus.current),
                         plus.conductance + minus.conductance);
    i[0] = i[0] + drive;
    i[1] = i[1] - drive;
    const D charge = lift(v[0] - v[1], plus.charge - minus.charge,
                          plus.capacitance + minus.capacitance);
    q[0] = q[0] + charge;
    q[1] = q[1] - charge;
  }
  const D iq = D(p.quiescentCurrent) * powered;
  i[2] = i[2] + iq;
  i[3] = i[3] - iq;
  q[5] = q[5] + D(p.c0) * x;
  q[6] = q[6] + D(p.auxiliaryC) * y;
  q[7] = q[7] + D(p.memoryC) * z;
  q[8] = q[8] + D(p.outputC) * w;
  const D referenceCharge = -(q[5] + q[6] + q[7] + q[8]) / D(2);
  q[2] = q[2] + referenceCharge;
  q[3] = q[3] + referenceCharge;
  OpampResidual r;
  for (std::size_t k = 0; k < 9; ++k) {
    r.current[k] = i[k].value;
    r.charge[k] = q[k].value;
    r.currentJacobian[k] = i[k].derivative;
    r.chargeJacobian[k] = q[k].derivative;
  }
  r.fluxResidual = fluxResidual.value;
  r.flux = flux.value;
  r.fluxResidualJacobian = fluxResidual.derivative;
  r.fluxJacobian = flux.derivative;
  return r;
}
// Generic fixture stamp. Full-card code owns the CC_* manifest component stamp;
// this helper does not auto-add a default capacitance or modify the opamp
// model.
inline void stampCapacitor(OpampResidual &r, const std::array<double, 10> &v,
                           std::size_t positiveNode, std::size_t negativeNode,
                           double capacitance) {
  if (!std::isfinite(capacitance) || capacitance < 0 || positiveNode >= 9 ||
      negativeNode >= 9 || positiveNode == negativeNode)
    throw std::domain_error("invalid external capacitor stamp");
  if (!finite({v[positiveNode], v[negativeNode]}))
    throw std::domain_error("nonfinite external capacitor voltage");
  const double charge = capacitance * (v[positiveNode] - v[negativeNode]);
  r.charge[positiveNode] += charge;
  r.charge[negativeNode] -= charge;
  r.chargeJacobian[positiveNode][positiveNode] += capacitance;
  r.chargeJacobian[positiveNode][negativeNode] -= capacitance;
  r.chargeJacobian[negativeNode][positiveNode] -= capacitance;
  r.chargeJacobian[negativeNode][negativeNode] += capacitance;
}
} // namespace lmc::device

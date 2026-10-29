#pragma once

// The plug-in's alignments and ranges: project decisions (labelled Custom in the README), derived from the engine once and recorded
// here and in docs/parameters.md. IDs and ranges are permanent from 0.1.0.

namespace lmc::alignment
{
// A sample value of 1.0 (full scale, peak) at Input 0 dB is this many volts of EMF at the mic input. Chosen so that a 1 kHz sine at
// -18 dBFS RMS (peak 0.178 of full scale; a full-scale sine is -3.01 dBFS RMS) gives 10.0 dB of gain reduction (the fundamental against
// the small-signal gain, as the S1 fixtures measure it) on the Early circuit at Bias 0.5, 48 kHz x 4. The same value serves both circuits.
inline constexpr double kInputVoltsPerFullScale = 0.011654;

// The pin15 volts that map to a sample value of 1.0. Chosen so that the same sine comes out at -18 dBFS RMS (all harmonics included)
// on Early at the default settings.
inline constexpr double kOutputVoltsPerFullScale = 16.005;

// The BIAS_TRIM wiper (fraction from the bottom of the 10 k pot). Below about 0.245 the idle JFET conducts and the card idles in gain
// reduction; the span 0.30 to 1.00 keeps an uncompressed idle state and a threshold that moves 21 dB. The knob's 12 o'clock is 0.5.
inline constexpr float kBiasMin = 0.30f, kBiasMax = 1.00f, kBiasDefault = 0.5f;

inline constexpr float kDbMin = -24.0f, kDbMax = 24.0f;

inline constexpr int kOversampling = 4;

// Latency, host samples. The 4x half-band cascade delays the card by 36.5 samples (2 x (31 / 2 + 11 / 4); the cascade's own peak sits
// between two host samples, so Card::latencySamples() says 36). A half-integer cannot be reported or matched bit for bit by the dry
// path, so the processed signal passes a 16-tap windowed-sinc half-sample delay (7.5 samples, flat to 0.002 dB below 12 kHz at 48 kHz)
// after the card: 36.5 + 7.5 = 44 whole samples, which the dry path and the host's compensation both use.
inline constexpr int kLatencySamples = 44;
inline constexpr int kTrimTaps = 16;
inline constexpr double kTrimKaiserBeta = 7.0;
inline constexpr int kStateVersion = 1;

inline constexpr double kSmoothSeconds = 0.020;       // Input, Output, Mix
inline constexpr double kBiasSmoothSeconds = 0.030;   // Bias, applied in steps of kChunk samples
inline constexpr double kCircuitFadeSeconds = 0.060;  // a circuit switch crosses over this long
inline constexpr int kChunk = 64;                     // samples per processing step
} // namespace lmc::alignment

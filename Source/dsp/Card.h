#pragma once

// The reduced-circuit engine for the CF82B33 talkback compressor card: the netlist values of the
// two factory revisions (reference/spice/{early,rev6a}/card.cir) derived into small trapezoidal
// state-space blocks, with two scalar nonlinear solves per sample (the T7 JFET with its gate
// network, and the T5/T6 precision rectifier into C14). See docs/validation/engine.md.
//
// Mono, JUCE-free. prepare() and reset() allocate nothing but take tens of microseconds to a
// few milliseconds and belong off the audio thread. The setters only record a value; process()
// (or prepare()/reset()) applies it. Changing the revision, bias or gain mid-stream rebuilds the
// matrices but keeps the state, so call reset() afterwards for a clean start.

#include <memory>

namespace lmc
{
enum class Revision { Early, Rev6a };

class Card
{
public:
    Card();
    ~Card();
    Card (const Card&) = delete;
    Card& operator= (const Card&) = delete;

    /** Builds the blocks for hostRate x oversampling factor and settles the DC operating point. */
    void prepare (double hostRate);
    /** Settles every state to the DC operating point with the present settings. */
    void reset();

    void setRevision (Revision r) noexcept;
    /** BIAS_TRIM wiper, fraction from the bottom of the 10 k pot (the S1 reference uses 0.5). */
    void setBias (double fractionFromBottom) noexcept;
    /** External 250 k listen-gain rheostat, fraction of full scale (default 1). */
    void setGain (double fraction) noexcept;
    /** EMF volts at the mic input for a sample value of 1.0. */
    void setInputVoltsPerFullScale (double volts) noexcept;
    /** pin15 volts that map to a sample value of 1.0 at the output. */
    void setOutputVoltsPerFullScale (double volts) noexcept;
    /** 1, 2, 4 or 8; takes effect at the next prepare(). */
    void setOversampling (int factor) noexcept;

    /** In place: floats in (EMF / V-per-FS), floats out (pin15 / V-per-FS). */
    void process (float* samples, int numSamples) noexcept;

    /** Two cards with their C14 envelope nodes (the detector storage) tied, solved jointly in every internal sample: `l` runs
        through `a` and `r` through `b`, in place. `tie` is the amount of the tie (0 none, 1 a wire between the nodes; in between a
        resistance falling from 1 Mohm to 1 ohm, in decades) and moves towards `tieTarget` by `tieStep` per host sample, so engaging
        and releasing the tie has no step. Both cards need the same oversampling, rate and settings. With identical inputs the two
        cards stay identical and the result equals two independent cards. */
    static void processTied (Card& a, Card& b, float* l, float* r, int numSamples, double& tie, double tieTarget, double tieStep) noexcept;

    //== Observables of the last internal sample (volts) =================================
    double gateVolts() const noexcept;
    double envelopeVolts() const noexcept;          // C14
    double sourceBiasVolts() const noexcept;        // T7 source, C13
    double t4OutVolts() const noexcept;
    double pre1OutVolts() const noexcept;
    /** Gain reduction of the attenuator estimated from T7's channel conductance at the present gate
        voltage (small-signal, vds = 0): 20 log10 of the node's conductance with and without it. */
    double grEstimateDb() const noexcept;

    /** Off by default. When on, process() and processTied() accumulate the attenuator's gain-reduction estimate once per host sample (no
        effect on the audio); takeGainReductionDb() returns its mean over the samples since the last call, in dB (0 when there were none),
        and starts again. It is a mean over every host sample, so the ripple of the detector does not alias into it. */
    void trackGainReduction (bool on) noexcept;
    double takeGainReductionDb() noexcept;

    /** Group delay of the oversampling filters, host samples (0 at 1x). */
    double latencySamples() const noexcept;
    int oversampling() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace lmc

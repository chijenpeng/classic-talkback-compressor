#pragma once

// The plug-in's stereo modes around two cards: Dual (two independent cards), Link (the two cards' C14 envelope nodes tied, solved
// jointly in the same sample: Card::processTied) and Mid-Side (the cards see M and S). No JUCE dependency, so the offline tests use
// it as the plug-in does. The structure follows 1167's StereoRunner (legacy/dev-1167/Source/dsp/StereoModes.h).

#include "Card.h"

#include <algorithm>

namespace lmc
{
/** In the order of the plug-in's `stereo` parameter: append, never reorder. */
enum class StereoMode { dual = 0, link = 1, midSide = 2 };

/** Runs a stereo pair of cards in a StereoMode. Nothing here allocates or locks.

    Mid-Side is a matrix around the two cards: M = (L + R) / 2 and S = (L - R) / 2 in, L = M + S and R = M - S out. Moving into or out
    of it the matrix is morphed from the identity over about 10 ms (encoder and decoder, per sample), so the cards' signals never
    jump from L / R to M / S and the output never jumps back; the cards' own states continue throughout. Link is engaged and released
    by the tie amount of Card::processTied, which moves over about 20 ms, so it has no step either. Dual, with the tie fully
    released, is exactly two independent cards. */
class StereoRunner
{
public:
    void prepare (double sampleRate) noexcept
    {
        msStep = 1.0 / (0.010 * sampleRate);
        tieStep = 1.0 / (0.020 * sampleRate);
        blend = 0.0;
        tie = 0.0;
    }
    /** Takes the mode at once, with no morph (for a host that sets the mode before it starts playing). */
    void snapTo (StereoMode mode) noexcept
    {
        blend = mode == StereoMode::midSide ? 1.0 : 0.0;
        tie = mode == StereoMode::link ? 1.0 : 0.0;
    }

    /** 0 in Dual and Link, 1 in a settled Mid-Side, between while the matrix morphs. */
    double midSideBlend() const noexcept { return blend; }

    void process (Card& a, Card& b, float* left, float* right, int numSamples, StereoMode mode) noexcept
    {
        const double target = mode == StereoMode::midSide ? 1.0 : 0.0;
        const double tieTarget = mode == StereoMode::link ? 1.0 : 0.0;
        if (blend == 0.0 && target == 0.0)
        {
            run (a, b, left, right, numSamples, tieTarget);
            return;
        }
        const double start = blend;
        const double dir = target > start ? 1.0 : (target < start ? -1.0 : 0.0);
        const auto at = [&] (int i) { return clampToward (start + dir * msStep * (double) (i + 1), target, dir); };
        for (int i = 0; i < numSamples; ++i)
        {
            const double m = at (i);
            const double l = left[i], r = right[i];
            left[i] = (float) (l * (1.0 - 0.5 * m) + r * (0.5 * m));
            right[i] = (float) ((1.0 - m) * r + m * 0.5 * (l - r));
        }
        run (a, b, left, right, numSamples, tieTarget);
        for (int i = 0; i < numSamples; ++i)
        {
            const double m = at (i);
            const double y0 = left[i], y1 = right[i];
            left[i] = (float) (y0 + m * y1);
            right[i] = (float) (y1 * (1.0 - 2.0 * m) + m * y0);
        }
        blend = at (numSamples - 1);
    }

private:
    static double clampToward (double v, double target, double dir) noexcept
    {
        return dir > 0.0 ? (v > target ? target : v) : (dir < 0.0 ? (v < target ? target : v) : target);
    }
    void run (Card& a, Card& b, float* l, float* r, int n, double tieTarget) noexcept
    {
        if (tie == 0.0 && tieTarget == 0.0)
        {
            a.process (l, n);
            b.process (r, n);
        }
        else
            Card::processTied (a, b, l, r, n, tie, tieTarget, tieStep);
    }

    double msStep = 0.0, tieStep = 0.0, blend = 0.0, tie = 0.0;
};
} // namespace lmc

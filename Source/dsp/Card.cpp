#include "Card.h"

#include "Oversampler.h"
#include "TrapNet.h"
#include "devices.hpp"

#include <algorithm>
#include <cmath>

namespace lmc
{
using dsp::Oversampler;
using dsp::TrapNet;

namespace
{
constexpr double kPi = 3.14159265358979323846;

// Values read from reference/spice/{early,rev6a}/card.cir (the S1 decks); everything not listed
// here is the same in both revisions and is written in the block builders below.
struct RevData
{
    double c10, c12, c13, c14, c16, c17;   // F
    double r40;                            // ohm, 0 = absent
    double railP, railN;                   // filtered rails of the S1 idle point (dc.json), V
};
constexpr RevData kEarly { 15e-6, 10e-6, 15e-6, 15e-6, 15e-6, 100e-12, 1e6, 19.77834, -19.784 };
constexpr RevData kRev6a { 10e-6, 100e-6, 10e-6, 10e-6, 100e-6, 4.7e-9, 0.0, 17.78728, -17.788 };

constexpr double kIbBig = 500e-9;      // T2..T5 input bias current (TDA1034 / 5534N macros)
constexpr double kIb741 = 80e-9;       // T6
constexpr double kHeadroom = 0.5;      // T2/T4 macro output headroom to the rail
// One real pole after T2: the closed-loop response of the macros (GBW and their second pole) loses
// 0.115 dB at 20 kHz and 0.5 dB at 40 kHz against ideal opamps in the S1 AC fixture; 118 kHz fits it.
#ifndef LMC_OPAMP_POLE_HZ
#define LMC_OPAMP_POLE_HZ 118e3   // experiments override this (docs/validation/engine.md, dropped elements)
#endif
constexpr double kOpampPoleHz = LMC_OPAMP_POLE_HZ;
constexpr int kStates = 8;             // capacitors of the card block: C10, C17, C14, C13, C11, C15, C12, C16
constexpr double kRx = 1e5;            // DC solve only: a helper resistor on node E, cancelled in the injection

// T7: Shichman-Hodges with the (1 + lambda vds) factor in both regions, symmetric in drain and
// source, as ngspice's njf and models/devices/devices.hpp. Returns the current drain -> source.
struct Jfet
{
    double beta, vp, lambda;
    explicit Jfet (const lmc::device::JfetParameters& p) : beta (p.beta()), vp (p.pinchOff()), lambda (p.lambda) {}

    double eval (double vgs, double vds, double& dVgs, double& dVds) const noexcept
    {
        const bool fwd = vds >= 0.0;
        const double v = fwd ? vds : -vds;
        const double u = (fwd ? vgs : vgs - vds) - vp;
        double i = 0.0, fu = 0.0, fv = 0.0;
        if (u > 0.0)
        {
            const double l = 1.0 + lambda * v;
            if (v < u)
            {
                i = beta * (2.0 * u * v - v * v) * l;
                fu = beta * 2.0 * v * l;
                fv = beta * ((2.0 * u - 2.0 * v) * l + (2.0 * u * v - v * v) * lambda);
            }
            else
            {
                i = beta * u * u * l;
                fu = beta * 2.0 * u * l;
                fv = beta * u * u * lambda;
            }
        }
        if (fwd) { dVgs = fu; dVds = fv; return i; }
        dVgs = -fu;
        dVds = fu + fv;
        return -i;
    }
};

// T5/T6 precision rectifier with ideal opamps and ideal diodes (their forward drops are supplied by
// the opamp outputs and never reach a node the rest of the card sees). With a = t5_minus held at 0,
// r = rect_half, p = t6_plus = m = t6_minus, q = rectified, the stages reduce to two regimes each
// (docs/validation/engine.md, "Detector"). Returns the current the rectifier delivers into
// C14 (node E) as max (a1 - b1 E, a2 - b2 E).
struct Detector
{
    double g40 = 0.0, vz = 0.0;
    // T6 (741 macro) output limit: its output stops at rail - 1.675 V (the macro's headroom) behind R0 = 50 ohm; the 1S44 D6
    // adds about 0.684 V + 7 ohm (linearised between 5 and 14 mA). Beyond it the delivered current is
    // (limit6 - vd0 - E) / (R33 + R0 + rD6), a third line that caps the D6 branch.
    double limit6 = 1e9;
    static constexpr double kVd6 = 0.684, kR0 = 50.0, kRd6 = 7.0, kHead6 = 1.675;
    static constexpr double R33 = 1000.0, R35 = 4700.0, R38 = 4700.0, R39 = 4700.0, R41 = 4700.0, R42 = 4700.0;

    void coefficients (double u, double ePrev, double& a1, double& b1, double& a2, double& b2) const noexcept
    {
        // T5 stage: D4 on (A) or D5 on (B)
        const double pA = -kIb741 * R39;
        const double rA = R38 * (kIbBig - u / R35 - pA / R39);
        double r, p;
        if (-(rA / R38 + (rA - pA) / R41) >= 0.0)
        {
            r = rA;
            p = pA;
        }
        else
        {
            const double c = 1.0 / (R38 + R41) + 1.0 / R39;
            p = (kIbBig - u / R35) / c;
            r = p * R38 / (R38 + R41);
        }
        // T6 stage: D6 on (q1 behind R33) or D7 on (m behind R33 + R42)
        const double q1 = p + R42 * (kIb741 - (r - p) / R41 - (vz - p) * g40);
        a1 = q1 / R33;
        b1 = 1.0 / R33;
        if (q1 > limit6 - kVd6 - 1.0)   // near or above the T6 limit: pick the lower of the two D6 lines at the last E
        {
            const double a3 = (limit6 - kVd6) / (R33 + kR0 + kRd6), b3 = 1.0 / (R33 + kR0 + kRd6);
            if (a3 - b3 * ePrev < a1 - b1 * ePrev)
            {
                a1 = a3;
                b1 = b3;
            }
        }
        a2 = p / (R33 + R42);
        b2 = 1.0 / (R33 + R42);
    }
};
} // namespace

struct Card::Impl
{
    // settings
    Revision rev = Revision::Early;
    double bias = 0.5, gain = 1.0, inV = 1.0, outV = 1.0;
    int osRequest = 1;
    bool dirty = true;

    Oversampler os;
    int osN = 1;
    double latency = 0.0;
    double fs = 48000.0;

    // constants of the revision
    RevData rd = kEarly;
    double vz = 0.0, clipHi = 0.0, clipLo = 0.0;
    Revision vzRev = Revision::Early;
    Jfet jfet { lmc::device::JfetParameters {} };
    Detector det;

    // blocks
    TrapNet n1a, n1b, n1bf, n1bDc, n2, n2f, n2dc;
    int c1aU = 0, o1a = 0, c1bU = 0, o1b = 0, o1bCap[2] {}, c1bfU = 0, o1bf = 0;
    int c2Pre = 0, c2Ch = 0, c2E = 0, c2T4 = 0, o2Cap[kStates] {};
    const TrapNet* act = nullptr;   // the card block of this sample: n2, or n2f while T4 clips
    double t4Forced = 0.0;
    int c2dCh = 0, c2dE = 0;

    // state
    double h1a[TrapNet::kMaxS] {}, h1b[TrapNet::kMaxS] {}, h2[TrapNet::kMaxS] {};
    double iCh = 0.0, iE = 0.0;
    double gate = 0.0, env = 0.0, sb = 0.0, o4 = 0.0, pre1 = 0.0;
    double preDc = 0.0;
    bool grTrack = false;
    double grSum = 0.0;
    long grCount = 0;

    // ---------------------------------------------------------------- builders
    void solveZener()
    {
        // KCL at z10: (V+ - Vz)/R11 = Iz(Vz) + Vz/(R9 + R10) + Vz/(10k + R28) + Vz/R40
        const lmc::device::ZenerParameters zp;
        auto f = [&] (double v)
        {
            const double iz = -lmc::device::zener (-v, zp).current;
            return (rd.railP - v) / 2200.0 - iz - v / (8200.0 + 2200.0) - v / (10000.0 + 2700.0) - (rd.r40 > 0 ? v / rd.r40 : 0.0);
        };
        double lo = 5.0, hi = 12.0;
        for (int it = 0; it < 80; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            (f (mid) > 0 ? lo : hi) = mid;
        }
        vz = 0.5 * (lo + hi);
    }

    void buildN1a (double T)
    {
        // Differential half-circuit of the input: 200 ohm source, R16 + R17, and the ideal 4:1
        // transformer's secondary load (R18, C6) reflected by 1/16.
        n1a.clear();
        const int s = n1a.node(), p = n1a.node();
        const int u = n1a.source (s);
        n1a.resistor (s, p, 200.0);
        n1a.resistor (p, 0, 1.0 / (1.0 / 13600.0 + 16.0 / 100000.0));
        n1a.capacitor (p, 0, 16.0 * 680e-12);
        o1a = n1a.outVoltage (p);
        n1a.build (T);
        c1aU = n1a.column (u);
    }

    void buildN1b (TrapNet& n, double T, bool dc)
    {
        n.clear();
        const int P = n.node(), M = n.node(), J = n.node(), Cg = n.node(), O = n.node(), Of = n.node();
        const int u = n.source (P);
        const int ib = n.inject (M, 0);
        n.constant (ib, kIbBig);
        if (dc) n.constant (u, 0.0);
        n.resistor (M, J, 1000.0);
        n.resistor (J, O, std::max (250000.0 * gain, 1.0));
        n.resistor (M, Cg, 1000.0);
        n.capacitor (Cg, 0, 470e-9);                         // C4
        n.nullor (P, M, O);
        n.resistor (O, Of, 1000.0);
        n.capacitor (Of, 0, 1.0 / (2.0 * kPi * kOpampPoleHz * 1000.0));
        o1b = n.outVoltage (Of);
        o1bCap[0] = n.outCapVoltage (0);
        o1bCap[1] = n.outCapVoltage (1);
        n.build (T);
        if (! dc) c1bU = n.column (u);
    }

    /** T2 while it clips: its output is a voltage source at the clamp (the macro clamps after its output-stage lag, so the pole's
        capacitor sits across the source and the output has no further smoothing) and the inverting input hangs on the feedback
        network (R13/C4 and R15 + rheostat from the clamped output), so C4 and the pole follow the clamped output instead of
        the linear response (same two states and the same companion conductances as buildN1b). */
    void buildN1bForced (TrapNet& n, double T)
    {
        n.clear();
        const int M = n.node(), J = n.node(), Cg = n.node(), O = n.node();
        const int u = n.source (O);
        const int ib = n.inject (M, 0);
        n.constant (ib, kIbBig);
        n.resistor (M, J, 1000.0);
        n.resistor (J, O, std::max (250000.0 * gain, 1.0));
        n.resistor (M, Cg, 1000.0);
        n.capacitor (Cg, 0, 470e-9);                         // C4
        n.capacitor (O, 0, 1.0 / (2.0 * kPi * kOpampPoleHz * 1000.0));   // the pole's capacitor follows the clamped output
        o1bf = n.outVoltage (O);
        n.build (T);
        c1bfU = n.column (u);
    }

    /** forceT4: T4 clips, its output O4 is a voltage source (input c2T4) instead of a nullor. The three variable inputs keep their columns. */
    void buildN2 (TrapNet& n, double T, bool dc, bool forceT4 = false)
    {
        n.clear();
        const int pre1n = n.node(), mix = n.node(), A = n.node(), Tn = n.node(), F = n.node(), G = n.node(), S = n.node(),
                  E = n.node(), M4 = n.node(), O4 = n.node(), Cg4 = n.node(), AC = n.node(), P15 = n.node(), BB = n.node(),
                  Z = n.node(), pre2 = n.node();
        const int uPre = n.source (pre1n), uPre2 = n.source (pre2), uZ = n.source (Z);
        const int xCh = n.inject (A, S), xE = n.inject (0, E), ibT = n.inject (Tn, 0), ibM = n.inject (M4, 0);
        n.constant (uPre2, preDc);
        n.constant (uZ, vz);
        n.constant (ibT, kIbBig);
        n.constant (ibM, kIbBig);
        if (dc) n.constant (uPre, preDc);
        const double rUp = std::max (10000.0 * (1.0 - bias), 0.01), rLo = std::max (10000.0 * bias, 0.01);
        n.resistor (pre1n, mix, 22000.0);                      // R25
        n.resistor (mix, pre2, 22000.0);                       // R26 (T3 idles: a DC source)
        n.resistor (A, S, 1e6);                                // R21
        n.resistor (F, G, 1e6);                                // R29
        n.resistor (G, E, 1e6);                                // R30
        n.resistor (Z, S, rUp);                                // BIAS_TRIM, upper
        n.resistor (S, BB, rLo);                               // BIAS_TRIM, lower
        n.resistor (BB, 0, 2700.0);                            // R28
        n.resistor (Tn, 0, 100000.0);                          // R32
        n.resistor (M4, Cg4, 1000.0);                          // R31
        n.resistor (M4, O4, 10000.0);                          // R34
        n.resistor (AC, 0, 2200.0);                            // R36
        n.resistor (AC, P15, 10.0);                            // R7
        n.resistor (P15, 0, 10000.0);                          // load
        if (dc) n.resistor (E, 0, kRx);
        // capacitor order is the state order (T7's gate capacitances and Cds are dropped, see engine.md)
        n.capacitor (mix, A, rd.c10);
        n.capacitor (A, F, rd.c17);
        n.capacitor (E, 0, rd.c14);
        n.capacitor (S, 0, rd.c13);
        n.capacitor (A, Tn, 1.5e-6);                           // C11
        n.capacitor (Tn, 0, 4.7e-9);                           // C15
        n.capacitor (Cg4, 0, rd.c12);
        n.capacitor (O4, AC, rd.c16);
        int uT4 = -1;
        if (forceT4) uT4 = n.source (O4);
        else n.nullor (Tn, M4, O4);
        // outputs: 0 vgs, 1 vds, 2 E, 3 t4_out, 4 pin15, 5 gate, 6 source, then capacitor voltages
        n.outVoltage (G, S);
        n.outVoltage (A, S);
        n.outVoltage (E);
        n.outVoltage (O4);
        n.outVoltage (P15);
        n.outVoltage (G);
        n.outVoltage (S);
        for (int k = 0; k < kStates; ++k) o2Cap[k] = n.outCapVoltage (k);
        n.build (T);
        if (dc) { c2dCh = n.column (xCh); c2dE = n.column (xE); }
        else { c2Pre = n.column (uPre); c2Ch = n.column (xCh); c2E = n.column (xE); if (forceT4) c2T4 = n.column (uT4); }
    }

    void build()
    {
        rd = rev == Revision::Early ? kEarly : kRev6a;
        clipHi = rd.railP - kHeadroom;
        clipLo = rd.railN + kHeadroom;
        if (vz == 0.0 || vzRev != rev) { solveZener(); vzRev = rev; }   // the 80-step bisection is for prepare / a revision change
        det.g40 = rd.r40 > 0.0 ? 1.0 / rd.r40 : 0.0;
        det.vz = vz;
        det.limit6 = rd.railP - Detector::kHead6;
        const double T = 1.0 / fs;
        buildN1a (T);
        buildN1b (n1b, T, false);
        buildN1bForced (n1bf, T);
    }

    void buildN2s (double T)
    {
        buildN2 (n2, T, false);
        buildN2 (n2f, T, false, true);
        act = &n2;
    }

    // ---------------------------------------------------------------- nonlinear ports
    /** What the rectifier and the C14 node look like for one instant: the delivered current is
        max (a1 - b1 E, a2 - b2 E) and E = e0 + k i_E, with i_E the total current into node E. */
    struct Port
    {
        double a1 = 0.0, b1 = 0.0, a2 = 0.0, b2 = 0.0, e0 = 0.0, k = 1.0;
    };

    /** Solves the T7 channel current for one instant. `lin` holds rows 0..3 (vgs, vds, E, t4_out) of
        the network with both port currents zero. */
    void solveJfet (const TrapNet& n, const double* lin, int cCh, int cE) noexcept
    {
        const double kgs = n.D[0][cCh], kds = n.D[1][cCh];
        const double vgs0 = lin[0] + n.D[0][cE] * iE, vds0 = lin[1] + n.D[1][cE] * iE;
        double i = iCh;
        for (int it = 0; it < 16; ++it)
        {
            double gm, gds;
            const double f = jfet.eval (vgs0 + kgs * i, vds0 + kds * i, gm, gds);
            const double dg = 1.0 - (gm * kgs + gds * kds);
            const double step = (i - f) / dg;
            i -= step;
            if (std::abs (step) < 1e-13 + 1e-10 * std::abs (i)) break;
        }
        iCh = i;
    }

    /** The rectifier and the E node at the solved channel current. */
    void portOf (const TrapNet& n, const double* lin, int cCh, int cE, double invRx, Port& p) noexcept
    {
        const double u = lin[3] + n.D[3][cCh] * iCh + n.D[3][cE] * iE;
        det.coefficients (u, env, p.a1, p.b1, p.a2, p.b2);
        p.b1 -= invRx;
        p.b2 -= invRx;
        p.e0 = lin[2] + n.D[2][cCh] * iCh;
        p.k = n.D[2][cE];
    }

    /** E alone: the current into C14 that the rectifier and the node agree on. */
    static double currentOf (const Port& p) noexcept
    {
        const double e1 = (p.e0 + p.k * p.a1) / (1.0 + p.k * p.b1), e2 = (p.e0 + p.k * p.a2) / (1.0 + p.k * p.b2);
        return (std::max (e1, e2) - p.e0) / p.k;
    }

    void solvePorts (const TrapNet& n, const double* lin, int cCh, int cE, double invRx) noexcept
    {
        solveJfet (n, lin, cCh, cE);
        Port p;
        portOf (n, lin, cCh, cE, invRx, p);
        iE = currentOf (p);
    }

    // ---------------------------------------------------------------- DC operating point
    void settle()
    {
        buildN1b (n1bDc, 1e12, true);
        preDc = n1bDc.y0[o1b];
        std::fill (std::begin (h1a), std::end (h1a), 0.0);
        for (int k = 0; k < 2; ++k) h1b[k] = -n1b.capConductance (k) * n1bDc.y0[o1bCap[k]];
        buildN2 (n2dc, 1e12, true);
        buildN2s (1.0 / fs);
        iCh = iE = 0.0;
        double lin[4];
        double prevE = 1e9;
        for (int it = 0; it < 200; ++it)
        {
            for (int o = 0; o < 4; ++o) lin[o] = n2dc.y0[o];
            solvePorts (n2dc, lin, c2dCh, c2dE, 1.0 / kRx);
            const double e = lin[2] + n2dc.D[2][c2dCh] * iCh + n2dc.D[2][c2dE] * iE;
            if (std::abs (e - prevE) < 1e-14 && it > 2) break;
            prevE = e;
        }
        for (int k = 0; k < kStates; ++k)
        {
            const double v = n2dc.y0[o2Cap[k]] + n2dc.D[o2Cap[k]][c2dCh] * iCh + n2dc.D[o2Cap[k]][c2dE] * iE;
            h2[k] = -n2.capConductance (k) * v;
        }
        // observables at the DC point; the rectifier's true current into E is zero there (no DC path
        // out of E except the rectifier), the helper resistor's share is gone with the injection
        gate = n2dc.y0[5] + n2dc.D[5][c2dCh] * iCh + n2dc.D[5][c2dE] * iE;
        env = n2dc.y0[2] + n2dc.D[2][c2dCh] * iCh + n2dc.D[2][c2dE] * iE;
        sb = n2dc.y0[6] + n2dc.D[6][c2dCh] * iCh + n2dc.D[6][c2dE] * iE;
        o4 = n2dc.y0[3] + n2dc.D[3][c2dCh] * iCh + n2dc.D[3][c2dE] * iE;
        pre1 = preDc;
        iE = 0.0;
    }

    // ---------------------------------------------------------------- one internal sample
    double lin[7] {};
    Port port;

    /** The input network, T2 and the linear part of the card; solves the JFET and reads the rectifier. */
    void beginTick (double emf) noexcept
    {
        // input network and T2
        const double vp = n1a.C[0][0] * h1a[0] + n1a.D[0][c1aU] * emf;
        h1a[0] = n1a.A[0][0] * h1a[0] + n1a.B[0][c1aU] * emf;
        const double vin = 4.0 * vp;
        // linear (nullor) response; the macro clamps its lagged output, so the decision is on the post-pole node. While the
        // response is beyond the clamp, T2 is a voltage source at the clamp (n1bf) and leaves it as soon as the linear
        // response is back inside (a hard exit: the macro's integrator windup was modelled and removed, see engine.md section 7).
        double y1 = n1b.y0[o1b] + n1b.C[o1b][0] * h1b[0] + n1b.C[o1b][1] * h1b[1] + n1b.D[o1b][c1bU] * vin;
        double nh0, nh1;
        if (y1 > clipHi || y1 < clipLo)
        {
            const double uO = y1 > clipHi ? clipHi : clipLo;
            y1 = uO;
            nh0 = n1bf.b0[0] + n1bf.A[0][0] * h1b[0] + n1bf.A[0][1] * h1b[1] + n1bf.B[0][c1bfU] * uO;
            nh1 = n1bf.b0[1] + n1bf.A[1][0] * h1b[0] + n1bf.A[1][1] * h1b[1] + n1bf.B[1][c1bfU] * uO;
        }
        else
        {
            nh0 = n1b.b0[0] + n1b.A[0][0] * h1b[0] + n1b.A[0][1] * h1b[1] + n1b.B[0][c1bU] * vin;
            nh1 = n1b.b0[1] + n1b.A[1][0] * h1b[0] + n1b.A[1][1] * h1b[1] + n1b.B[1][c1bU] * vin;
        }
        h1b[0] = nh0;
        h1b[1] = nh1;
        const double vpre = std::min (std::max (y1, clipLo), clipHi);

        // card; T4 is a nullor until its output would pass the clamp, then a voltage source at the clamp (n2f)
        act = &n2;
        for (int pass = 0; pass < 2; ++pass)
        {
            const TrapNet& n = *act;
            for (int o = 0; o < 7; ++o)
            {
                double s = n.y0[o] + n.D[o][c2Pre] * vpre;
                for (int k = 0; k < kStates; ++k) s += n.C[o][k] * h2[k];
                if (pass == 1) s += n.D[o][c2T4] * t4Forced;
                lin[o] = s;
            }
            solveJfet (n, lin, c2Ch, c2E);
            portOf (n, lin, c2Ch, c2E, 0.0, port);
            if (pass == 1) break;
            const double u4 = lin[3] + n.D[3][c2Ch] * iCh + n.D[3][c2E] * iE;
            if (u4 <= clipHi && u4 >= clipLo) break;
            t4Forced = u4 > clipHi ? clipHi : clipLo;
            act = &n2f;
        }
        pre1 = vpre;
    }

    /** With iE set (alone, or by the tie): the state update, the observables and pin15. */
    double finishTick() noexcept
    {
        const TrapNet& n = *act;
        const double fT4 = act == &n2f ? t4Forced : 0.0;
        double hn[kStates];
        for (int s = 0; s < kStates; ++s)
        {
            double a = n.b0[s] + n.B[s][c2Pre] * pre1 + n.B[s][c2Ch] * iCh + n.B[s][c2E] * iE;
            if (act == &n2f) a += n.B[s][c2T4] * fT4;
            for (int k = 0; k < kStates; ++k) a += n.A[s][k] * h2[k];
            hn[s] = a;
        }
        for (int s = 0; s < kStates; ++s) h2[s] = hn[s];
        gate = lin[5] + n.D[5][c2Ch] * iCh + n.D[5][c2E] * iE;
        env = lin[2] + n.D[2][c2Ch] * iCh + n.D[2][c2E] * iE;
        sb = lin[6] + n.D[6][c2Ch] * iCh + n.D[6][c2E] * iE;
        o4 = lin[3] + n.D[3][c2Ch] * iCh + n.D[3][c2E] * iE;
        return lin[4] + n.D[4][c2Ch] * iCh + n.D[4][c2E] * iE;
    }

    double tick (double emf) noexcept
    {
        beginTick (emf);
        iE = currentOf (port);
        return finishTick();
    }

    /** The two cards' C14 nodes joined through a tie of conductance g siemens (g <= 0: ideal wire). Each card receives
        the current that makes both E agree with its own rectifier and network; the nodes' sum is solved in this sample. */
    static void tieNodes (Impl& a, Impl& b, double g) noexcept
    {
        const Port& pa = a.port;
        const Port& pb = b.port;
        const double ia = 1.0 / pa.k, ib = 1.0 / pb.k;
        const double Aa[2] { pa.a1, pa.a2 }, Ba[2] { pa.b1, pa.b2 }, Ab[2] { pb.a1, pb.a2 }, Bb[2] { pb.b1, pb.b2 };
        double ea, eb;
        if (g <= 0.0)
        {
            // one node: sum of the injected currents = sum of the rectifier currents; the root of the min of increasing lines is the max of the roots
            double e = -1e300;
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j)
                    e = std::max (e, (pa.e0 * ia + pb.e0 * ib + Aa[i] + Ab[j]) / (ia + ib + Ba[i] + Bb[j]));
            ea = eb = e;
        }
        else
        {
            // two nodes with a conductance between them: four regime combinations, take the one that is consistent
            double best = 1e300;
            ea = eb = 0.0;
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j)
                {
                    const double ca = ia + Ba[i], cb = ib + Bb[j];
                    const double ra = Aa[i] + pa.e0 * ia, rb = Ab[j] + pb.e0 * ib;
                    const double det = ca * cb + g * (ca + cb);
                    const double xa = (ra * (cb + g) + g * rb) / det, xb = ((ca + g) * rb + g * ra) / det;
                    const double i1a = pa.a1 - pa.b1 * xa, i2a = pa.a2 - pa.b2 * xa, i1b = pb.a1 - pb.b1 * xb, i2b = pb.a2 - pb.b2 * xb;
                    const double va = (i == 0 ? i2a - i1a : i1a - i2a), vb = (j == 0 ? i2b - i1b : i1b - i2b);   // > 0: the other branch is higher
                    const double violation = std::max (va, 0.0) + std::max (vb, 0.0);
                    if (violation < best)
                    {
                        best = violation;
                        ea = xa;
                        eb = xb;
                    }
                }
        }
        a.iE = (ea - pa.e0) / pa.k;
        b.iE = (eb - pb.e0) / pb.k;
    }

    /** The attenuator's gain factor at the present gate voltage (Card::grEstimateDb is 20 log10 of it). */
    double attenuationRatio() const noexcept
    {
        const double u = gate - sb - jfet.vp;
        const double g = u > 0.0 ? 2.0 * jfet.beta * u : 0.0;
        const double gNode = 1.0 / 11000.0 + 1.0 / 100000.0 + 1.0 / 1e6;
        return 1.0 + g / gNode;
    }

    void trackSample() noexcept
    {
        if (grTrack)
        {
            grSum += std::log (attenuationRatio());
            ++grCount;
        }
    }

    /** Applies pending setting changes: the matrices of the present revision, bias and gain. */
    void applyDirty()
    {
        if (dirty)
        {
            // matrices only; states are kept (companion histories carry over)
            build();
            buildN2s (1.0 / fs);
            dirty = false;
        }
    }

    double toInternal (float x) const noexcept
    {
        double e = static_cast<double> (x);
        e = std::isfinite (e) ? std::min (std::max (e * inV, -1e4), 1e4) : 0.0;
        return e + 1e-18;   // keeps the idle filter states out of the denormal range
    }
};

Card::Card() : impl (new Impl) {}
Card::~Card() = default;

void Card::prepare (double hostRate)
{
    auto& s = *impl;
    s.osN = s.osRequest == 8 ? 8 : s.osRequest == 4 ? 4 : s.osRequest == 2 ? 2 : 1;
    s.fs = hostRate * s.osN;
    s.os.design (s.osN);
    {   // group delay: the position of the peak of the cascade's impulse response, in host samples
        double buf[8], best = 0.0;
        int at = 0;
        for (int n = 0; n < 200; ++n)
        {
            s.os.up (n == 0 ? 1.0 : 0.0, buf);
            const double y = s.os.down (buf);
            if (std::abs (y) > best) { best = std::abs (y); at = n; }
        }
        s.latency = s.osN == 1 ? 0.0 : at;
        s.os.reset();
    }
    s.build();
    s.dirty = false;
    s.settle();
}

void Card::reset()
{
    auto& s = *impl;
    if (s.dirty) { s.build(); s.dirty = false; }
    s.os.reset();
    s.settle();
}

void Card::setRevision (Revision r) noexcept { impl->rev = r; impl->dirty = true; }
void Card::setBias (double f) noexcept { impl->bias = std::min (std::max (f, 0.0), 1.0); impl->dirty = true; }
void Card::setGain (double f) noexcept { impl->gain = std::min (std::max (f, 0.0), 1.0); impl->dirty = true; }
void Card::setInputVoltsPerFullScale (double v) noexcept { impl->inV = v; }
void Card::setOutputVoltsPerFullScale (double v) noexcept { impl->outV = v; }
void Card::setOversampling (int f) noexcept { impl->osRequest = f; }

void Card::process (float* x, int n) noexcept
{
    auto& s = *impl;
    s.applyDirty();
    const double outScale = 1.0 / s.outV;
    const int N = s.osN;
    double buf[8];
    for (int i = 0; i < n; ++i)
    {
        const double e = s.toInternal (x[i]);
        double y;
        if (N == 1)
            y = s.tick (e);
        else
        {
            s.os.up (e, buf);
            for (int k = 0; k < N; ++k) buf[k] = s.tick (buf[k]);
            y = s.os.down (buf);
        }
        if (! std::isfinite (y))
        {
            s.settle();
            y = 0.0;
        }
        x[i] = static_cast<float> (y * outScale);
        s.trackSample();
    }
}

void Card::processTied (Card& a, Card& b, float* l, float* r, int n, double& tie, double tieTarget, double tieStep) noexcept
{
    auto& sa = *a.impl;
    auto& sb = *b.impl;
    sa.applyDirty();
    sb.applyDirty();
    const double outA = 1.0 / sa.outV, outB = 1.0 / sb.outV;
    const int N = sa.osN;
    double bufA[8], bufB[8];
    for (int i = 0; i < n; ++i)
    {
        // the tie resistance runs from 1 Mohm (amount 0) down to 1 ohm in decades; at amount 1 it is a wire
        tie = tie < tieTarget ? std::min (tie + tieStep, tieTarget) : std::max (tie - tieStep, tieTarget);
        const double g = tie >= 1.0 ? 0.0 : (tie <= 0.0 ? 1e-9 : std::pow (10.0, 6.0 * tie - 6.0));
        const double ea = sa.toInternal (l[i]), eb = sb.toInternal (r[i]);
        double ya, yb;
        if (N == 1)
        {
            sa.beginTick (ea);
            sb.beginTick (eb);
            Impl::tieNodes (sa, sb, g);
            ya = sa.finishTick();
            yb = sb.finishTick();
        }
        else
        {
            sa.os.up (ea, bufA);
            sb.os.up (eb, bufB);
            for (int k = 0; k < N; ++k)
            {
                sa.beginTick (bufA[k]);
                sb.beginTick (bufB[k]);
                Impl::tieNodes (sa, sb, g);
                bufA[k] = sa.finishTick();
                bufB[k] = sb.finishTick();
            }
            ya = sa.os.down (bufA);
            yb = sb.os.down (bufB);
        }
        if (! (std::isfinite (ya) && std::isfinite (yb)))
        {
            sa.settle();
            sb.settle();
            ya = yb = 0.0;
        }
        l[i] = static_cast<float> (ya * outA);
        r[i] = static_cast<float> (yb * outB);
        sa.trackSample();
        sb.trackSample();
    }
}

double Card::gateVolts() const noexcept { return impl->gate; }
double Card::envelopeVolts() const noexcept { return impl->env; }
double Card::sourceBiasVolts() const noexcept { return impl->sb; }
double Card::t4OutVolts() const noexcept { return impl->o4; }
double Card::pre1OutVolts() const noexcept { return impl->pre1; }
double Card::latencySamples() const noexcept { return impl->latency; }
int Card::oversampling() const noexcept { return impl->osN; }

double Card::grEstimateDb() const noexcept
{
    return 20.0 * std::log10 (impl->attenuationRatio());
}

void Card::trackGainReduction (bool on) noexcept
{
    impl->grTrack = on;
    impl->grSum = 0.0;
    impl->grCount = 0;
}

double Card::takeGainReductionDb() noexcept
{
    auto& s = *impl;
    const double db = s.grCount > 0 ? (20.0 / std::log (10.0)) * (s.grSum / static_cast<double> (s.grCount)) : 0.0;
    s.grSum = 0.0;
    s.grCount = 0;
    return db;
}
} // namespace lmc

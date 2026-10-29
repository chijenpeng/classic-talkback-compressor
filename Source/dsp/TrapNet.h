#pragma once

// A small dense linear network, discretised by the trapezoidal rule and reduced to a discrete
// state-space system (the method of 1167's FastNet.h, extended with nullors and constant inputs):
//
//     h' = A h + B u + b0        y = C h + D u + y0
//
// The states h are the capacitors' companion history currents (i = g v + h, h' = -h - 2 g v), so
// n capacitors give n states and no algebraic node remains at run time. Inputs are ideal voltage
// sources at nodes and injected currents (the nonlinear devices' ports); an input given a
// constant is folded into b0 / y0 and costs nothing per sample. A nullor (ideal opamp) forces
// v(p) = v(m) and supplies whatever current its output node needs.
//
// build() is for prepare() and for control changes (a few thousand flops, no allocation).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace lmc::dsp
{
class TrapNet
{
public:
    static constexpr int kMaxM = 24;   // MNA dimension (nodes + sources + nullors)
    static constexpr int kMaxS = 12;   // states (capacitors)
    static constexpr int kMaxI = 12;   // inputs
    static constexpr int kMaxV = 6;    // variable inputs kept at run time
    static constexpr int kMaxO = 20;   // outputs
    static constexpr int kMaxEl = 40;  // elements of each kind

    void clear() noexcept
    {
        nNodes = nSrc = nInj = nNul = nOut = nR = nC = nIn = 0;
        std::fill (std::begin (isConst), std::end (isConst), false);
    }
    int node() noexcept { return ++nNodes; }

    void resistor (int a, int b, double ohms) noexcept { r[nR++] = { a, b, 1.0 / ohms }; }
    int capacitor (int a, int b, double farads) noexcept { c[nC] = { a, b, farads }; return nC++; }
    /** Ideal voltage source from ground to `node`; returns the input index. */
    int source (int n) noexcept { src[nSrc] = n; kind[nIn] = 0; slot[nIn] = nSrc; ++nSrc; return nIn++; }
    /** A current of value u flowing from a to b through a device (it leaves a). */
    int inject (int a, int b) noexcept { inj[nInj] = { a, b }; kind[nIn] = 1; slot[nIn] = nInj; ++nInj; return nIn++; }
    /** Ideal opamp: v(p) = v(m); the output node o receives the current needed. */
    void nullor (int p, int m, int o) noexcept { nul[nNul++] = { p, m, o }; }
    void constant (int input, double value) noexcept { isConst[input] = true; constVal[input] = value; }
    int outVoltage (int a, int b = 0) noexcept { out[nOut] = { a, b }; return nOut++; }
    int outCapVoltage (int k) noexcept { return outVoltage (c[k].a, c[k].b); }

    bool build (double T) noexcept
    {
        const int nn = nNodes;
        m = nn + nSrc + nNul;
        ns = nC;
        nv = 0;
        for (int k = 0; k < nIn; ++k)
            varCol[k] = isConst[k] ? -1 : nv++;
        if (m > kMaxM || ns > kMaxS || nIn > kMaxI || nv > kMaxV || nOut > kMaxO) return false;
        std::memset (M, 0, sizeof (M));
        std::memset (Bu, 0, sizeof (Bu));
        std::memset (Bh, 0, sizeof (Bh));
        auto ix = [] (int n) { return n - 1; };   // node 0 (ground) has no row
        auto stampG = [&] (int a, int b, double gg)
        {
            if (a > 0) M[ix (a)][ix (a)] += gg;
            if (b > 0) M[ix (b)][ix (b)] += gg;
            if (a > 0 && b > 0) { M[ix (a)][ix (b)] -= gg; M[ix (b)][ix (a)] -= gg; }
        };
        for (int k = 0; k < nR; ++k) stampG (r[k].a, r[k].b, r[k].g);
        for (int k = 0; k < nC; ++k)
        {
            const double gg = 2.0 * c[k].f / T;
            capG[k] = gg;
            stampG (c[k].a, c[k].b, gg);
            if (c[k].a > 0) Bh[ix (c[k].a)][k] -= 1.0;
            if (c[k].b > 0) Bh[ix (c[k].b)][k] += 1.0;
        }
        for (int k = 0; k < nSrc; ++k)
        {
            const int row = nn + k;
            M[row][ix (src[k])] += 1.0;
            M[ix (src[k])][row] -= 1.0;
        }
        for (int k = 0; k < nNul; ++k)
        {
            const int row = nn + nSrc + k;
            if (nul[k].p > 0) M[row][ix (nul[k].p)] += 1.0;
            if (nul[k].m > 0) M[row][ix (nul[k].m)] -= 1.0;
            M[ix (nul[k].o)][row] -= 1.0;
        }
        for (int k = 0; k < nIn; ++k)
        {
            if (kind[k] == 0)
                Bu[nn + slot[k]][k] = 1.0;
            else
            {
                const auto& e = inj[slot[k]];
                if (e.a > 0) Bu[ix (e.a)][k] -= 1.0;
                if (e.b > 0) Bu[ix (e.b)][k] += 1.0;
            }
        }
        if (! invert (M, m)) return false;
        for (int i = 0; i < m; ++i)
        {
            for (int j = 0; j < nIn; ++j)
            {
                double s = 0.0;
                for (int k = 0; k < m; ++k) s += M[i][k] * Bu[k][j];
                Xu[i][j] = s;
            }
            for (int j = 0; j < ns; ++j)
            {
                double s = 0.0;
                for (int k = 0; k < m; ++k) s += M[i][k] * Bh[k][j];
                Xh[i][j] = s;
            }
        }
        // outputs and state update, with constants folded
        auto fold = [&] (const double* xuRow, double* dRow, double& y0v)
        {
            y0v = 0.0;
            for (int j = 0; j < nIn; ++j)
            {
                if (isConst[j]) y0v += xuRow[j] * constVal[j];
                else dRow[varCol[j]] = xuRow[j];
            }
        };
        // x = the unit response of v(a) - v(b) to the inputs (xu) and to the capacitor histories (xh)
        auto voltageResponse = [&] (int a, int b, double* xu, double* xh)
        {
            auto add = [&] (int node, double w)
            {
                if (node <= 0) return;
                for (int j = 0; j < nIn; ++j) xu[j] += w * Xu[ix (node)][j];
                for (int j = 0; j < ns; ++j) xh[j] += w * Xh[ix (node)][j];
            };
            add (a, 1.0);
            add (b, -1.0);
        };
        for (int o = 0; o < nOut; ++o)
        {
            double xu[kMaxI] = {}, xh[kMaxS] = {};
            voltageResponse (out[o].a, out[o].b, xu, xh);
            fold (xu, D[o], y0[o]);
            for (int j = 0; j < ns; ++j) C[o][j] = xh[j];
        }
        for (int s = 0; s < ns; ++s)
        {
            double xu[kMaxI] = {}, xh[kMaxS] = {};
            voltageResponse (c[s].a, c[s].b, xu, xh);   // v_c = x
            double bRow[kMaxV] = {};
            double b0v;
            fold (xu, bRow, b0v);
            const double w = -2.0 * capG[s];            // h' = -h - 2 g v
            for (int j = 0; j < nv; ++j) B[s][j] = w * bRow[j];
            b0[s] = w * b0v;
            for (int j = 0; j < ns; ++j) A[s][j] = w * xh[j] + (s == j ? -1.0 : 0.0);
        }
        return true;
    }

    int numStates() const noexcept { return ns; }
    int numOutputs() const noexcept { return nOut; }
    /** Compact column of an input given as variable at build time (-1 for a constant). */
    int column (int input) const noexcept { return varCol[input]; }
    double capConductance (int k) const noexcept { return capG[k]; }

    double A[kMaxS][kMaxS] {}, B[kMaxS][kMaxV] {}, b0[kMaxS] {};
    double C[kMaxO][kMaxS] {}, D[kMaxO][kMaxV] {}, y0[kMaxO] {};

private:
    struct Res { int a, b; double g; };
    struct Cap { int a, b; double f; };
    struct Inj { int a, b; };
    struct Nul { int p, m, o; };
    struct Out { int a, b; };

    static bool invert (double (&a)[kMaxM][kMaxM], int n) noexcept
    {
        double inv[kMaxM][kMaxM];
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                inv[i][j] = (i == j ? 1.0 : 0.0);
        for (int col = 0; col < n; ++col)
        {
            int p = col;
            double best = std::abs (a[col][col]);
            for (int i = col + 1; i < n; ++i)
                if (std::abs (a[i][col]) > best) { best = std::abs (a[i][col]); p = i; }
            if (best < 1.0e-300) return false;
            if (p != col)
                for (int j = 0; j < n; ++j) { std::swap (a[p][j], a[col][j]); std::swap (inv[p][j], inv[col][j]); }
            const double d = 1.0 / a[col][col];
            for (int j = 0; j < n; ++j) { a[col][j] *= d; inv[col][j] *= d; }
            for (int i = 0; i < n; ++i)
            {
                if (i == col) continue;
                const double f = a[i][col];
                if (f == 0.0) continue;
                for (int j = 0; j < n; ++j) { a[i][j] -= f * a[col][j]; inv[i][j] -= f * inv[col][j]; }
            }
        }
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                a[i][j] = inv[i][j];
        return true;
    }

    int nNodes = 0, nSrc = 0, nInj = 0, nNul = 0, nOut = 0, nR = 0, nC = 0, nIn = 0;
    int ns = 0, m = 0, nv = 0;
    Res r[kMaxEl] {}; Cap c[kMaxEl] {}; Inj inj[kMaxI] {}; Nul nul[4] {}; Out out[kMaxO] {};
    int src[kMaxI] {}, kind[kMaxI] {}, slot[kMaxI] {}, varCol[kMaxI] {};
    bool isConst[kMaxI] {};
    double constVal[kMaxI] {}, capG[kMaxS] {};
    double M[kMaxM][kMaxM] {}, Bu[kMaxM][kMaxI] {}, Bh[kMaxM][kMaxS] {};
    double Xu[kMaxM][kMaxI] {}, Xh[kMaxM][kMaxS] {};
};
} // namespace lmc::dsp

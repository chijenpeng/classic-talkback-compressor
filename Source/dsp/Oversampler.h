#pragma once

// Cascade of linear-phase halfband stages for 1x / 2x / 4x / 8x oversampling of a mono signal.
// Stage 0 (host rate to 2x) is the sharp one, later stages are shorter. Allocation-free after
// construction; design() runs in prepare().

#include <algorithm>
#include <cmath>
#include <iterator>

namespace lmc::dsp
{
class Oversampler
{
public:
    static constexpr int kMaxStages = 3;
    static constexpr int kMaxK = 31;

    /** factor is 1, 2, 4 or 8. */
    void design (int factor) noexcept
    {
        stages = factor >= 8 ? 3 : factor >= 4 ? 2 : factor >= 2 ? 1 : 0;
        static constexpr int kHalf[kMaxStages] = { 31, 11, 7 };
        for (int s = 0; s < stages; ++s)
        {
            st[s].K = kHalf[s];
            const double beta = 9.0, i0b = bessel0 (beta);
            for (int k = 1; k <= st[s].K; k += 2)
            {
                const double w = bessel0 (beta * std::sqrt (1.0 - double (k * k) / double ((st[s].K + 1) * (st[s].K + 1)))) / i0b;
                const double h = std::sin (3.14159265358979323846 * k / 2.0) / (3.14159265358979323846 * k);
                st[s].h[(k - 1) / 2] = 2.0 * h * w;     // pair coefficient, already doubled for the zero-stuffing gain
            }
        }
        reset();
    }
    void reset() noexcept
    {
        for (auto& x : st)
        {
            std::fill (std::begin (x.up), std::end (x.up), 0.0);
            std::fill (std::begin (x.dn), std::end (x.dn), 0.0);
            x.upPos = x.dnPos = 0;
        }
    }
    int stageCount() const noexcept { return stages; }
    int factor() const noexcept { return 1 << stages; }

    /** One host sample in; writes `factor()` samples to y. */
    void up (double x, double* y) noexcept
    {
        y[0] = x;
        int n = 1;
        for (int s = 0; s < stages; ++s)
        {
            double tmp[8];
            for (int i = 0; i < n; ++i)
            {
                double a, b;
                st[s].upStep (y[i], a, b);
                tmp[2 * i] = a;
                tmp[2 * i + 1] = b;
            }
            n *= 2;
            std::copy (tmp, tmp + n, y);
        }
    }
    /** `factor()` oversampled samples in (destroyed), one host sample out. */
    double down (double* y) noexcept
    {
        int n = 1 << stages;
        for (int s = stages - 1; s >= 0; --s)
        {
            n /= 2;
            for (int i = 0; i < n; ++i)
                y[i] = st[s].downStep (y[2 * i], y[2 * i + 1]);
        }
        return y[0];
    }

private:
    static double bessel0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
        return sum;
    }
    struct Stage
    {
        int K = 7;
        double h[(kMaxK + 1) / 2] {};          // odd-tap pair coefficients 2 h[1], 2 h[3], ...
        // histories, newest first at [pos]; every sample is written twice (pos and pos + length) so that a
        // window is always contiguous and no index wraps in the inner loops
        double up[2 * (kMaxK + 1)] {}, dn[2 * (2 * kMaxK + 2)] {};
        int upPos = 0, dnPos = 0;
        // x[n] in; a = even output (centre tap, delayed (K+1)/2 samples), b = odd output (interpolated)
        void upStep (double x, double& a, double& b) noexcept
        {
            const int M = (K + 1) / 2, L = 2 * M;
            upPos = upPos == 0 ? L - 1 : upPos - 1;
            up[upPos] = up[upPos + L] = x;
            const double* w = up + upPos;                      // w[age]
            a = w[M];
            double acc = 0.0;
            for (int i = 0; i < M; ++i)
                acc += h[i] * (w[M + i] + w[M - 1 - i]);       // x[n-M-i] and x[n-M+1+i]
            b = acc;
        }
        double downStep (double x0, double x1) noexcept
        {
            const int L = 2 * K + 2;
            dnPos = dnPos == 0 ? L - 1 : dnPos - 1;
            dn[dnPos] = dn[dnPos + L] = x0;
            dnPos = dnPos == 0 ? L - 1 : dnPos - 1;
            dn[dnPos] = dn[dnPos + L] = x1;                    // newest at dnPos
            const double* w = dn + dnPos;                      // w[age]; the output is centred on age K
            double acc = 0.5 * w[K];
            for (int i = 0, k = 1; k <= K; ++i, k += 2)
                acc += 0.5 * h[i] * (w[K - k] + w[K + k]);     // h[i] = 2 h[k]
            return acc;
        }
    };
    int stages = 0;
    Stage st[kMaxStages];
};
} // namespace lmc::dsp

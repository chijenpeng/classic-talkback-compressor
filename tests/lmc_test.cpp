// Fast regression test of the card engine against the S1 fixtures (reference/fixtures) and realtime
// stress cases. Fixture numbers are quoted from the file named in the comment next to each.
// The full pass/MISS comparison is tools/compare_engine.py; this one runs in a few seconds.

#include "Card.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
int failures = 0;
void check (bool ok, const char* what, double got, double want, double tol)
{
    std::printf ("%s %-58s got %-12.6g want %-12.6g tol %g\n", ok ? "pass" : "FAIL", what, got, want, tol);
    if (! ok) ++failures;
}
#define NEAR(what, got, want, tol) check (std::abs ((got) - (want)) <= (tol), what, got, want, tol)

const double kPi = 3.14159265358979323846;
const char* kRevName[] = { "early", "rev6a" };

struct Run
{
    std::vector<double> y, gate;
};

lmc::Card* makeCard (lmc::Revision rev, double sr, int os)
{
    auto* c = new lmc::Card;
    c->setRevision (rev);
    c->setOversampling (os);
    c->prepare (sr);
    return c;
}

/** Tone in dBV rms of EMF, 20 ms linear ramp, optional amplitude step (the run_spice.tone() source). */
double toneSample (double t, double dbv, double f, double k = 1.0, double t1 = 0, double t2 = 0, double edge = 1e-3)
{
    const double a = std::sqrt (2.0) * std::pow (10.0, dbv / 20.0);
    double env = std::min (t / 0.02, 1.0);
    if (k != 1.0)
    {
        const double r1 = std::min (std::max ((t - t1) / edge, 0.0), 1.0), r2 = std::min (std::max ((t - t2) / edge, 0.0), 1.0);
        env *= 1.0 + (k - 1.0) * (r1 - r2);
    }
    return a * env * std::sin (2.0 * kPi * f * t);
}

Run render (lmc::Revision rev, double sr, double dbv, double f, double T, double k = 1.0, double t1 = 0, double t2 = 0, int os = 1)
{
    std::unique_ptr<lmc::Card> c (makeCard (rev, sr, os));
    Run r;
    const int n = int (T * sr);
    for (int i = 0; i < n; ++i)
    {
        float x = float (toneSample (i / sr, dbv, f, k, t1, t2));
        c->process (&x, 1);
        r.y.push_back (x);
        r.gate.push_back (c->gateVolts());
    }
    return r;
}

/** Peak amplitude of the component at f over the last `window` seconds (integer cycles). */
double amplitude (const std::vector<double>& y, double sr, double f, double window = 0.1)
{
    const int cycles = int (std::lround (window * f));
    const int n = int (std::lround (cycles * sr / f));
    double re = 0, im = 0, mean = 0;
    const size_t start = y.size() - size_t (n);
    for (int i = 0; i < n; ++i) mean += y[start + size_t (i)];
    mean /= n;
    for (int i = 0; i < n; ++i)
    {
        const double ph = 2 * kPi * f * double (i) / sr;
        re += (y[start + size_t (i)] - mean) * std::cos (ph);
        im += (y[start + size_t (i)] - mean) * std::sin (ph);
    }
    return 2.0 * std::sqrt (re * re + im * im) / n;
}
double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-30)); }

/** (static_trim0.5.csv: lvl_dBV, GR_dB) */
std::vector<std::pair<double, double>> staticCurve (const char* rev)
{
    std::ifstream f (std::string (LMC_FIXTURE_DIR) + "/" + rev + "/static_trim0.5.csv");
    std::string line;
    std::getline (f, line);
    std::vector<std::pair<double, double>> v;
    while (std::getline (f, line))
    {
        std::stringstream ss (line);
        std::string c;
        std::vector<double> col;
        while (std::getline (ss, c, ',')) col.push_back (std::atof (c.c_str()));
        if (col.size() > 13 && col[13] == 0 && std::isfinite (col[3])) v.push_back ({ col[0], col[3] });
    }
    return v;
}

/** Time of the first crossing of the 63 % point of the change between pre and hold values. */
double t63 (const std::vector<double>& raw, double sr, double a, double b, double hold, double end, bool up)
{
    // 1 ms boxcar (one carrier cycle), centred, as run_spice.py smooths the gate before timing it
    const int w = int (std::lround (1e-3 * sr)) | 1, half = w / 2;
    std::vector<double> g (raw.size(), 0.0);
    double acc = 0;
    for (size_t i = 0; i < raw.size() + size_t (half); ++i)
    {
        if (i < raw.size()) acc += raw[i];
        if (i >= size_t (w)) acc -= raw[i - size_t (w)];
        if (i >= size_t (half)) g[i - size_t (half)] = acc / w;
    }
    const size_t i0 = size_t (a * sr), i1 = size_t (b * sr);
    double pre = 0, h = 0, fin = 0;
    int np = 0, nh = 0, nf = 0;
    for (size_t i = size_t ((0.4 - 0.1) * sr); i < size_t ((0.4 - 0.01) * sr); ++i) { pre += g[i]; ++np; }
    for (size_t i = size_t ((1.4 - 0.05) * sr); i < size_t ((1.4 - 0.005) * sr); ++i) { h += g[i]; ++nh; }
    for (size_t i = g.size() - size_t (0.1 * sr); i < g.size(); ++i) { fin += g[i]; ++nf; }
    pre /= np; h /= nh; fin /= nf;
    (void) hold; (void) end;
    const double from = up ? pre : h, to = up ? h : fin, target = from + 0.63 * (to - from);
    for (size_t i = i0; i < i1; ++i)
        if ((to > from) ? g[i] >= target : g[i] <= target) return (double (i) - double (i0)) / sr * 1e3;
    return -1;
}
} // namespace

int main()
{
    // idle point, S1 dc.json (nodes_V gate / source_bias): 10 mV
    const double dcGate[] = { 0.03165715, 0.04780914 }, dcBias[] = { 6.045535, 6.035189 };
    // gain at 1 kHz, small signal (static_trim0.5.csv, -88 dBV row)
    const double g1k = 72.6109;
    // +20 dB step from -76.7 dBV, 1 ms edge: gate t63 attack / release, ms (attack_release.json)
    const double stepAttack[] = { 4.0, 4.4 }, stepRelease[] = { 57.2, 41.8 };
    // 1 kHz at 10 dB GR (harmonics.json): level dBV, H2, H3 dBc
    const double hLevel[] = { -56.7, -56.8 }, hH2[] = { -14.8, -42.1 }, hH3[] = { -27.5, -45.2 };

    for (int r = 0; r < 2; ++r)
    {
        const auto rev = r == 0 ? lmc::Revision::Early : lmc::Revision::Rev6a;
        std::printf ("---- %s\n", kRevName[r]);
        {
            std::unique_ptr<lmc::Card> c (makeCard (rev, 192000, 1));
            NEAR ("idle gate (V)", c->gateVolts(), dcGate[r], 0.010);
            NEAR ("idle envelope (V)", c->envelopeVolts(), dcGate[r], 0.010);
            NEAR ("idle source bias (V)", c->sourceBiasVolts(), dcBias[r], 0.010);
        }
        const auto curve = staticCurve (kRevName[r]);
        for (double lvl : { -62.0, -56.0, -47.0, -38.0 })
        {
            double want = -1;
            for (auto& p : curve) if (std::abs (p.first - lvl) < 0.01) want = p.second;
            const Run run = render (rev, 192000, lvl, 1000, 0.8);
            const double gr = g1k - db (amplitude (run.y, 192000, 1000) / (std::sqrt (2.0) * std::pow (10.0, lvl / 20)));
            char name[80];
            std::snprintf (name, sizeof name, "static GR at %+.0f dBV (dB)", lvl);
            NEAR (name, gr, want, 0.25);
        }
        {
            const Run run = render (rev, 192000, -76.7, 1000, 0.4 + 1.0 + 0.6, std::pow (10.0, 20.0 / 20.0), 0.4, 1.4);
            const double a = t63 (run.gate, 192000, 0.4, 1.4, 1.0, 2.0, true);
            const double rl = t63 (run.gate, 192000, 1.4, 2.0, 1.0, 2.0, false);
            NEAR ("+20 dB step gate t63 attack (ms)", a, stepAttack[r], std::max (0.05 * stepAttack[r], 0.25));
            NEAR ("+20 dB step gate t63 release (ms)", rl, stepRelease[r], std::max (0.05 * stepRelease[r], 0.25));
        }
        {
            const Run run = render (rev, 384000, hLevel[r], 1000, 1.0);
            const double h1 = amplitude (run.y, 384000, 1000);
            NEAR ("H2 at 10 dB GR, 1 kHz (dBc)", db (amplitude (run.y, 384000, 2000) / h1), hH2[r], 1.0);
            NEAR ("H3 at 10 dB GR, 1 kHz (dBc)", db (amplitude (run.y, 384000, 3000) / h1), hH3[r], 1.0);
        }
        // ---- realtime stress, at 48 kHz with the oversampling a plug-in would use
        for (int os : { 1, 4 })
        {
            std::unique_ptr<lmc::Card> c (makeCard (rev, 48000, os));
            c->setInputVoltsPerFullScale (0.1);
            c->setOutputVoltsPerFullScale (10.0);
            c->reset();
            std::vector<float> x (48000);
            bool finite = true, bounded = true;
            double peak = 0;
            auto run = [&] (const char* what)
            {
                c->process (x.data(), int (x.size()));
                for (float v : x) { finite = finite && std::isfinite (v); peak = std::max (peak, double (std::abs (v))); }
                (void) what;
            };
            std::fill (x.begin(), x.end(), 0.0f);                                          run ("silence");
            std::fill (x.begin(), x.end(), 1.0f);                                          run ("full-scale DC");
            for (size_t i = 0; i < x.size(); ++i) x[i] = (i / 24 % 2) ? 1.0f : -1.0f;     run ("full-scale 1 kHz square");
            for (size_t i = 0; i < x.size(); ++i) x[i] = float (std::sin (2 * kPi * 5000.0 * double (i) / 48000.0));   run ("full-scale 5 kHz");
            std::fill (x.begin(), x.end(), 0.0f); x[100] = 1.0f;                           run ("impulse");
            std::fill (x.begin(), x.end(), 1e-40f);                                        run ("denormal");
            for (size_t i = 0; i < x.size(); ++i) x[i] = (i % 97 == 0) ? NAN : (i % 89 == 0 ? INFINITY : 0.001f);   run ("NaN/Inf");
            std::fill (x.begin(), x.end(), 0.0f);
            for (int k = 0; k < 3; ++k) run ("recovery");
            bounded = peak < 1e3;
            char name[80];
            std::snprintf (name, sizeof name, "stress os %d: all outputs finite", os);
            check (finite, name, finite, 1, 0);
            std::snprintf (name, sizeof name, "stress os %d: bounded (peak %.3g)", os, peak);
            check (bounded, name, peak, 0, 1e3);
            std::snprintf (name, sizeof name, "stress os %d: gate back at idle after 3 s", os);
            NEAR (name, c->gateVolts(), dcGate[r], 0.010);
        }
        {   // block size does not change the result; reset is repeatable
            std::vector<float> a (4800), b;
            for (size_t i = 0; i < a.size(); ++i) a[i] = float (0.003 * std::sin (2 * kPi * 700.0 * double (i) / 48000.0));
            b = a;
            std::unique_ptr<lmc::Card> c1 (makeCard (rev, 48000, 2)), c2 (makeCard (rev, 48000, 2));
            c1->process (a.data(), int (a.size()));
            for (size_t i = 0; i < b.size(); i += 37) c2->process (b.data() + i, int (std::min<size_t> (37, b.size() - i)));
            double d = 0;
            for (size_t i = 0; i < a.size(); ++i) d = std::max (d, double (std::abs (a[i] - b[i])));
            check (d == 0.0, "block size independent (bit-exact)", d, 0, 0);
        }
    }
    // ---- tied C14 nodes (Link): Card::processTied
    for (int r = 0; r < 2; ++r)
    {
        const lmc::Revision rev = r == 0 ? lmc::Revision::Early : lmc::Revision::Rev6a;
        const int N = 24000;
        auto make = [&] (std::vector<float>& l, std::vector<float>& rr, float gainR)
        {
            l.resize (size_t (N));
            rr.resize (size_t (N));
            for (int i = 0; i < N; ++i)
            {
                const float v = float (0.3 * std::sin (2 * kPi * 600.0 * i / 48000.0) * (i < 12000 ? 1.0 : 0.2));
                l[size_t (i)] = v;
                rr[size_t (i)] = v * gainR;
            }
        };
        auto setup = [&] (lmc::Card& c)
        {
            c.setRevision (rev);
            c.setOversampling (4);
            c.setInputVoltsPerFullScale (0.0117);
            c.setOutputVoltsPerFullScale (16.0);
            c.prepare (48000);
        };
        // identical inputs: tied equals independent (to float rounding), a wire and a part-way tie alike
        for (const double tieAmount : { 1.0, 0.6 })
        {
            std::vector<float> l, rr, l2, r2;
            make (l, rr, 1.0f);
            l2 = l;
            r2 = rr;
            lmc::Card a, b, c, d;
            setup (a); setup (b); setup (c); setup (d);
            double tie = tieAmount;
            lmc::Card::processTied (a, b, l.data(), rr.data(), N, tie, tieAmount, 0.0);
            c.process (l2.data(), N);
            d.process (r2.data(), N);
            double err = 0, peak = 0;
            for (int i = 0; i < N; ++i)
            {
                err = std::max ({ err, double (std::abs (l[size_t (i)] - l2[size_t (i)])), double (std::abs (rr[size_t (i)] - r2[size_t (i)])) });
                peak = std::max (peak, double (std::abs (l2[size_t (i)])));
            }
            char name[100];
            std::snprintf (name, sizeof name, "%s tie %.1f, L == R: equals two independent cards (rel. err)", kRevName[r], tieAmount);
            check (err / peak < 2e-6, name, err / peak, 0, 2e-6);
        }
        // tie amount 0 (nothing): equals independent for different inputs
        {
            std::vector<float> l, rr, l2, r2;
            make (l, rr, 0.1f);
            l2 = l;
            r2 = rr;
            lmc::Card a, b, c, d;
            setup (a); setup (b); setup (c); setup (d);
            double tie = 0.0;
            lmc::Card::processTied (a, b, l.data(), rr.data(), N, tie, 0.0, 0.0);
            c.process (l2.data(), N);
            d.process (r2.data(), N);
            double err = 0, peak = 0;
            for (int i = 0; i < N; ++i)
            {
                err = std::max ({ err, double (std::abs (l[size_t (i)] - l2[size_t (i)])), double (std::abs (rr[size_t (i)] - r2[size_t (i)])) });
                peak = std::max (peak, double (std::abs (l2[size_t (i)])));
            }
            char name[100];
            std::snprintf (name, sizeof name, "%s tie 0: equals two independent cards (rel. err)", kRevName[r]);
            check (err / peak < 1e-4, name, err / peak, 0, 1e-4);
        }
        // a wire: the envelope nodes agree, and the quiet side is reduced by the loud one
        {
            std::vector<float> l, rr, l2, r2;
            make (l, rr, 0.1f);
            l2 = l;
            r2 = rr;
            lmc::Card a, b, c, d;
            setup (a); setup (b); setup (c); setup (d);
            double tie = 1.0;
            lmc::Card::processTied (a, b, l.data(), rr.data(), 11000, tie, 1.0, 0.0);
            c.process (l2.data(), 11000);
            d.process (r2.data(), 11000);
            char name[100];
            std::snprintf (name, sizeof name, "%s wire: both envelope nodes equal (V)", kRevName[r]);
            NEAR (name, a.envelopeVolts() - b.envelopeVolts(), 0.0, 1e-9);
            const double quietTied = a.grEstimateDb() > 0 ? b.grEstimateDb() : 0.0, quietAlone = d.grEstimateDb();
            std::snprintf (name, sizeof name, "%s wire: the quiet side's GR (%.2f dB) exceeds its own (%.2f dB)", kRevName[r], quietTied, quietAlone);
            check (quietTied > quietAlone + 1.0, name, quietTied, quietAlone, 1.0);
            std::snprintf (name, sizeof name, "%s wire: the loud side's GR stays near its own (%.2f vs %.2f dB)", kRevName[r], a.grEstimateDb(), c.grEstimateDb());
            check (std::abs (a.grEstimateDb() - c.grEstimateDb()) < 3.0, name, a.grEstimateDb(), c.grEstimateDb(), 3.0);
        }
        // engaging while the sides differ has no step: the largest sample-to-sample change of the output stays within the signal's own
        {
            std::vector<float> l, rr;
            make (l, rr, 0.1f);
            lmc::Card a, b;
            setup (a); setup (b);
            double tie = 0.0;
            lmc::Card::processTied (a, b, l.data(), rr.data(), 9000, tie, 0.0, 0.0);
            std::vector<float> l1 (l.begin() + 9000, l.end()), r1 (rr.begin() + 9000, rr.end());
            // engage over 20 ms
            const int n1 = 15000 - 9000;
            lmc::Card::processTied (a, b, l.data() + 9000, rr.data() + 9000, n1, tie, 1.0, 1.0 / (0.02 * 48000.0));
            double step = 0, peak = 0;
            for (int i = 9001; i < 9000 + n1; ++i)
            {
                step = std::max (step, double (std::abs (rr[size_t (i)] - rr[size_t (i - 1)])));
                peak = std::max (peak, double (std::abs (rr[size_t (i)])));
            }
            char name[100];
            std::snprintf (name, sizeof name, "%s tie engaged over 20 ms: max output step / peak", kRevName[r]);
            check (step / peak < 0.2, name, step / peak, 0, 0.2);
        }
    }

    std::printf ("%s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}

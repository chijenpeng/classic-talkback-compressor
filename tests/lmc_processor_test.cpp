// Processor regression for the plug-in: parameter table, state and its version, layouts, reported latency against the measured
// delay, Mix, the stereo modes, circuit switch and bias transitions, allocations in processBlock, sample rates 44.1 to 192 kHz.
//
//   lmc_processor_test            (returns 1 if a check fails)

#include "../Source/PluginEditor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

//==============================================================================
// Allocation probe: counts operator new on the thread that switches it on (the audio thread of the tests).
namespace allocation_probe
{
namespace
{
thread_local bool active = false;
thread_local std::size_t count = 0;

void* allocate (std::size_t size, std::size_t alignment)
{
    if (active)
        ++count;
#if defined(_MSC_VER)
    if (void* p = _aligned_malloc (size == 0 ? 1 : size, alignment))
        return p;
#else
    if (alignment < sizeof (void*))
        alignment = sizeof (void*);
    void* p = nullptr;
    if (posix_memalign (&p, alignment, size == 0 ? 1 : size) == 0)
        return p;
#endif
    throw std::bad_alloc();
}

void release (void* p) noexcept
{
#if defined(_MSC_VER)
    _aligned_free (p);
#else
    std::free (p);
#endif
}
} // namespace
} // namespace allocation_probe

void* operator new (std::size_t size) { return allocation_probe::allocate (size, alignof (std::max_align_t)); }
void* operator new[] (std::size_t size) { return allocation_probe::allocate (size, alignof (std::max_align_t)); }
void* operator new (std::size_t size, std::align_val_t a) { return allocation_probe::allocate (size, (std::size_t) a); }
void* operator new[] (std::size_t size, std::align_val_t a) { return allocation_probe::allocate (size, (std::size_t) a); }
void operator delete (void* p) noexcept { allocation_probe::release (p); }
void operator delete[] (void* p) noexcept { allocation_probe::release (p); }
void operator delete (void* p, std::size_t) noexcept { allocation_probe::release (p); }
void operator delete[] (void* p, std::size_t) noexcept { allocation_probe::release (p); }
void operator delete (void* p, std::align_val_t) noexcept { allocation_probe::release (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { allocation_probe::release (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { allocation_probe::release (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { allocation_probe::release (p); }

//==============================================================================
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 512;
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    failures += ok ? 0 : 1;
}

double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-30)); }
/** -18 dBFS RMS: a sine of this peak amplitude (a full-scale sine is -3.01 dBFS RMS). */
const double kAlignPeak = std::pow (10.0, -18.0 / 20.0) * std::sqrt (2.0);

std::unique_ptr<ClassicTalkbackProcessor> make (double rate = 48000.0, int channels = 2, int block = kBlock)
{
    auto p = std::make_unique<ClassicTalkbackProcessor>();
    p->setPlayConfigDetails (channels, channels, rate, block);
    p->prepareToPlay (rate, block);
    return p;
}

void set (ClassicTalkbackProcessor& p, const char* id, float value)
{
    auto* parameter = p.apvts.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

float value (ClassicTalkbackProcessor& p, const char* id)
{
    auto& parameter = *p.apvts.getParameter (id);
    return parameter.convertFrom0to1 (parameter.getValue());
}

/** Plays `generator (n, channel)` through the processor from sample `start` for `count` samples; returns each channel's output. */
template <typename Generator>
std::vector<std::vector<float>> play (ClassicTalkbackProcessor& p, int channels, long start, long count, Generator generator, int block = kBlock)
{
    std::vector<std::vector<float>> out ((size_t) channels);
    juce::AudioBuffer<float> buffer (channels, block);
    juce::MidiBuffer midi;
    for (long done = 0; done < count; done += block)
    {
        const int n = (int) std::min<long> (block, count - done);
        buffer.setSize (channels, n, false, false, true);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
                buffer.setSample (ch, i, (float) generator (start + done + i, ch));
        p.processBlock (buffer, midi);
        for (int ch = 0; ch < channels; ++ch)
            out[(size_t) ch].insert (out[(size_t) ch].end(), buffer.getReadPointer (ch), buffer.getReadPointer (ch) + n);
    }
    return out;
}

auto sine (double rate, double amplitude, double frequency = 1000.0)
{
    return [=] (long n, int) { return amplitude * std::sin (2.0 * kPi * frequency * (double) n / rate); };
}

double worstConvergenceMs = 0.0;   // wall time of the slowest wait in runUntilSettled, for the report

/** Feeds the generator on, block by block, until the circuit parameter's request is the running circuit and no cross-over or settling is
    left (a pending request counts: the audio thread acts on it in the next block, once the worker has the other engine ready). Fails
    after 5 s of wall time. Returns the next sample index. */
template <typename Generator>
long runUntilSettled (ClassicTalkbackProcessor& p, int channels, long start, Generator generator, double rate)
{
    long at = start;
    const auto began = juce::Time::getMillisecondCounterHiRes();
    const auto wanted = [&] { return (int) std::lround (value (p, ParamID::circuit)); };
    while ((p.isSwitching() || (int) p.getRunningCircuit() != wanted()) && juce::Time::getMillisecondCounterHiRes() - began < 5000.0)
    {
        play (p, channels, at, kBlock, generator);
        at += kBlock;
        juce::Thread::sleep (1);
    }
    worstConvergenceMs = std::max (worstConvergenceMs, juce::Time::getMillisecondCounterHiRes() - began);
    (void) rate;
    return at;
}

double rms (const std::vector<float>& v, size_t from, size_t to)
{
    double s = 0.0;
    for (size_t i = from; i < to; ++i)
        s += (double) v[i] * v[i];
    return std::sqrt (s / (double) (to - from));
}

double maxStep (const std::vector<float>& v, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
        m = std::max (m, (double) std::abs (v[i] - v[i - 1]));
    return m;
}

/** Phase (radians) of the component at f over `cycles` whole cycles ending at the end of the vector. */
double phaseOf (const std::vector<float>& y, double rate, double f, int cycles)
{
    const int n = (int) std::lround (cycles * rate / f);
    double re = 0, im = 0;
    for (int i = 0; i < n; ++i)
    {
        const size_t k = y.size() - (size_t) n + (size_t) i;
        const double ph = 2.0 * kPi * f * (double) k / rate;
        re += y[k] * std::cos (ph);
        im += y[k] * std::sin (ph);
    }
    return std::atan2 (-im, re);
}

//==============================================================================
void checkParameters()
{
    auto p = make();
    const auto& all = p->getParameters();
    const char* ids[] { ParamID::circuit, ParamID::input, ParamID::bias, ParamID::output, ParamID::mix, ParamID::stereo };
    bool order = all.size() == 6;
    for (int i = 0; order && i < 6; ++i)
    {
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (all[i]);
        order = withId != nullptr && withId->paramID == ids[i] && withId->getVersionHint() == 1;
    }
    check (order, "parameter IDs in order: circuit, input, bias, output, mix, stereo (version hint 1)");

    auto* circuit = dynamic_cast<juce::AudioParameterChoice*> (p->apvts.getParameter (ParamID::circuit));
    auto* stereo = dynamic_cast<juce::AudioParameterChoice*> (p->apvts.getParameter (ParamID::stereo));
    check (circuit != nullptr && circuit->choices == juce::StringArray ({ "Early", "1984" }) && circuit->getIndex() == 0,
           "circuit: Early, 1984; default Early");
    check (stereo != nullptr && stereo->choices == juce::StringArray ({ "Dual", "Link", "M/S" }) && stereo->getIndex() == 0,
           "stereo: Dual, Link, M/S; default Dual");

    const auto range = [&] (const char* id) { return p->apvts.getParameter (id)->getNormalisableRange(); };
    const auto def = [&] (const char* id) { auto& q = *p->apvts.getParameter (id); return q.convertFrom0to1 (q.getDefaultValue()); };
    check (juce::exactlyEqual (range (ParamID::input).start, -24.0f) && juce::exactlyEqual (range (ParamID::input).end, 24.0f) && std::abs (def (ParamID::input)) < 1e-5f, "input: -24 to +24 dB, default 0");
    check (juce::exactlyEqual (range (ParamID::output).start, -24.0f) && juce::exactlyEqual (range (ParamID::output).end, 24.0f) && std::abs (def (ParamID::output)) < 1e-5f, "output: -24 to +24 dB, default 0");
    check (juce::exactlyEqual (range (ParamID::bias).start, 0.30f) && juce::exactlyEqual (range (ParamID::bias).end, 1.0f) && std::abs (def (ParamID::bias) - 0.5f) < 1e-6f
               && std::abs (p->apvts.getParameter (ParamID::bias)->getDefaultValue() - 0.5f) < 1e-3f,
           "bias: wiper 0.30 to 1.00, default 0.5 at 12 o'clock");
    check (juce::exactlyEqual (range (ParamID::mix).start, 0.0f) && juce::exactlyEqual (range (ParamID::mix).end, 100.0f) && juce::exactlyEqual (def (ParamID::mix), 100.0f), "mix: 0 to 100 %, default 100");

    auto text = [&] (const char* id, float v) { auto& q = *p->apvts.getParameter (id); return q.getText (q.convertTo0to1 (v), 0); };
    std::printf ("  (texts: '%s' '%s' '%s' '%s' '%s')\n", text (ParamID::input, 0.0f).toRawUTF8(), text (ParamID::input, 3.5f).toRawUTF8(),
                 text (ParamID::input, -24.0f).toRawUTF8(), text (ParamID::bias, 0.5f).toRawUTF8(), text (ParamID::mix, 100.0f).toRawUTF8());
    check (text (ParamID::input, 0.0f) == "0.0 dB" && text (ParamID::input, 3.5f) == "+3.5 dB" && text (ParamID::input, -24.0f) == "-24.0 dB"
               && text (ParamID::bias, 0.5f) == "0.50" && text (ParamID::mix, 100.0f) == "100 %",
           "value text: 0.0 dB, +3.5 dB, -24.0 dB, 0.50, 100 %");
    auto& in = *p->apvts.getParameter (ParamID::input);
    auto& bias = *p->apvts.getParameter (ParamID::bias);
    auto& mix = *p->apvts.getParameter (ParamID::mix);
    check (std::abs (in.convertFrom0to1 (in.getValueForText ("-3 dB")) + 3.0f) < 1e-4f && std::abs (in.convertFrom0to1 (in.getValueForText ("+2")) - 2.0f) < 1e-4f
               && std::abs (bias.convertFrom0to1 (bias.getValueForText ("0.45")) - 0.45f) < 1e-3f && std::abs (mix.convertFrom0to1 (mix.getValueForText ("35 %")) - 35.0f) < 1e-3f,
           "typed text in the parameter's units: -3 dB, +2, 0.45, 35 %");
    check (ParamText::numberIn ("abc") == std::nullopt, "text without a number is ignored");
}

//==============================================================================
void checkState()
{
    auto a = make();
    set (*a, ParamID::circuit, 1.0f);
    set (*a, ParamID::input, 7.5f);
    set (*a, ParamID::bias, 0.4f);
    set (*a, ParamID::output, -3.0f);
    set (*a, ParamID::mix, 40.0f);
    set (*a, ParamID::stereo, 2.0f);
    juce::MemoryBlock block;
    a->getStateInformation (block);

    auto b = make();
    b->setStateInformation (block.getData(), (int) block.getSize());
    check (value (*b, ParamID::circuit) == 1.0f && std::abs (value (*b, ParamID::input) - 7.5f) < 1e-4f && std::abs (value (*b, ParamID::bias) - 0.4f) < 2e-3f
               && std::abs (value (*b, ParamID::output) + 3.0f) < 1e-4f && std::abs (value (*b, ParamID::mix) - 40.0f) < 1e-3f && value (*b, ParamID::stereo) == 2.0f,
           "state round trip: all six parameters");

    const auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
    check (xml != nullptr && xml->getIntAttribute ("stateVersion") == lmc::alignment::kStateVersion && lmc::alignment::kStateVersion == 1,
           "state carries stateVersion 1");

    // A state that does not name some parameters: they take their defaults, whatever the live instance had.
    {
        auto reduced = *xml;
        for (const char* id : { ParamID::mix, ParamID::stereo, ParamID::circuit })
            if (auto* child = reduced.getChildByAttribute ("id", id))
                reduced.removeChildElement (child, true);
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (reduced, old);

        auto live = make();
        set (*live, ParamID::mix, 10.0f);
        set (*live, ParamID::stereo, 1.0f);
        set (*live, ParamID::circuit, 1.0f);
        set (*live, ParamID::input, -9.0f);
        live->setStateInformation (old.getData(), (int) old.getSize());
        check (value (*live, ParamID::mix) == 100.0f && value (*live, ParamID::stereo) == 0.0f && value (*live, ParamID::circuit) == 0.0f
                   && std::abs (value (*live, ParamID::input) - 7.5f) < 1e-4f,
               "state without mix, stereo and circuit: those take their defaults, the others load");
    }
    // A state from a later version (an unknown attribute and a parameter that does not exist) still loads what is known.
    {
        auto future = *xml;
        future.setAttribute ("stateVersion", 7);
        auto* extra = future.createNewChildElement ("PARAM");
        extra->setAttribute ("id", "hypothetical");
        extra->setAttribute ("value", 1.0);
        juce::MemoryBlock block2;
        juce::AudioProcessor::copyXmlToBinary (future, block2);
        auto c = make();
        c->setStateInformation (block2.getData(), (int) block2.getSize());
        check (value (*c, ParamID::circuit) == 1.0f && std::abs (value (*c, ParamID::input) - 7.5f) < 1e-4f, "state of a later version loads the parameters it knows");
        c->getStateInformation (block2);
        const auto saved = juce::AudioProcessor::getXmlFromBinary (block2.getData(), (int) block2.getSize());
        check (saved != nullptr && saved->getIntAttribute ("stateVersion") == 1, "...and is saved again as version 1");
    }
    // Garbage and another plug-in's state change nothing.
    {
        auto c = make();
        const char junk[] = "not a state";
        c->setStateInformation (junk, (int) sizeof junk);
        juce::XmlElement other ("SomethingElse");
        juce::MemoryBlock foreign;
        juce::AudioProcessor::copyXmlToBinary (other, foreign);
        c->setStateInformation (foreign.getData(), (int) foreign.getSize());
        check (value (*c, ParamID::mix) == 100.0f && value (*c, ParamID::circuit) == 0.0f, "a state that is not ours is ignored");
    }
}

//==============================================================================
void checkLayouts()
{
    auto p = make();
    const auto supported = [&] (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (in);
        layout.outputBuses.add (out);
        return p->checkBusesLayoutSupported (layout);
    };
    check (supported (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono()) && supported (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())
               && ! supported (juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo()) && ! supported (juce::AudioChannelSet::create5point1(), juce::AudioChannelSet::create5point1()),
           "layouts: mono and stereo in = out are supported, nothing else");

    // Mono: the stereo parameter is ignored (bit for bit), and equals channel 0 of a Dual stereo run.
    auto mono = make (48000.0, 1);
    auto reference = make (48000.0, 1);
    auto stereo = make (48000.0, 2);
    set (*mono, ParamID::stereo, 2.0f);
    set (*reference, ParamID::stereo, 0.0f);
    const auto gen = [] (long n, int ch) { return 0.2 * std::sin (0.05 * (double) n) * (ch == 0 ? 1.0 : 0.5); };
    const auto m = play (*mono, 1, 0, 48000, gen), r = play (*reference, 1, 0, 48000, gen), s = play (*stereo, 2, 0, 48000, gen);
    double e1 = 0.0, e2 = 0.0;
    for (size_t i = 0; i < m[0].size(); ++i)
    {
        e1 = std::max (e1, (double) std::abs (m[0][i] - r[0][i]));
        e2 = std::max (e2, (double) std::abs (m[0][i] - s[0][i]));
    }
    check (e1 == 0.0, "mono layout: the stereo parameter is ignored (M/S equals Dual, bit for bit)");
    check (e2 == 0.0, "mono layout equals channel 0 of the stereo Dual run, bit for bit");
}

//==============================================================================
/** Delay of the processor's wet path against a 1x card at the engine's own rate (the same discretisation): the only difference
    is the oversampling filters. A linear-level 500 Hz tone; the phase difference is the delay. */
double measuredDelay (double rate)
{
    auto p = make (rate, 1);
    set (*p, ParamID::input, -24.0f);
    const double f = 500.0, amp = 0.05;
    const long count = (long) std::lround (0.5 * rate / 50.0) * 50;   // a whole number of 10 ms
    p->prepareToPlay (rate, kBlock);
    const auto out = play (*p, 1, 0, count, sine (rate, amp, f))[0];

    const double inner = rate * 4.0;
    lmc::Card card;
    card.setRevision (lmc::Revision::Early);
    card.setBias (0.5);
    card.setOversampling (1);
    card.setInputVoltsPerFullScale (lmc::alignment::kInputVoltsPerFullScale * juce::Decibels::decibelsToGain (-24.0));
    card.setOutputVoltsPerFullScale (lmc::alignment::kOutputVoltsPerFullScale);
    card.prepare (inner);
    std::vector<float> ref ((size_t) (count * 4));
    for (size_t i = 0; i < ref.size(); ++i)
        ref[i] = (float) (amp * std::sin (2.0 * kPi * f * (double) i / inner));
    card.process (ref.data(), (int) ref.size());

    const int cycles = 100;
    double d = phaseOf (ref, inner, f, cycles) - phaseOf (out, rate, f, cycles);
    while (d < 0.0) d += 2.0 * kPi;
    while (d > 2.0 * kPi) d -= 2.0 * kPi;
    return d / (2.0 * kPi * f) * rate;
}

void checkLatencyAndMix()
{
    for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        auto p = make (rate, 2);
        const double measured = measuredDelay (rate);
        std::printf ("  (%.0f Hz: reported %d samples, measured %.3f)\n", rate, p->getLatencySamples(), measured);
        check (p->getLatencySamples() == 44 && std::abs (measured - (double) p->getLatencySamples()) < 0.05,
               "latency at " + juce::String (rate, 0) + " Hz: the reported 44 samples equals the measured delay (within 0.05)");
    }

    // Mix 0 nulls against the delayed dry input, bit for bit: from the start, and after automation has settled.
    for (const bool automated : { false, true })
    {
        auto p = make (48000.0, 2);
        if (! automated)
        {
            set (*p, ParamID::mix, 0.0f);
            p->prepareToPlay (48000.0, kBlock);
        }
        else
        {
            set (*p, ParamID::mix, 0.0f);
            play (*p, 2, 0, 4800, sine (48000.0, 0.3));
        }
        set (*p, ParamID::input, 9.0f);
        set (*p, ParamID::output, 5.0f);
        std::vector<float> noise (60000);
        std::srand (7);
        for (auto& v : noise) v = (float) (std::rand() % 20001 - 10000) * 1.0e-4f;
        const auto gen = [&] (long n, int ch) { return (double) noise[(size_t) (n + ch * 111) % noise.size()] * 0.5; };
        const long start = automated ? 4800 : 0;
        const auto out = play (*p, 2, start, 20000, gen);
        double worst = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (long i = 44; i < 20000; ++i)
                worst = std::max (worst, (double) std::abs (out[(size_t) ch][(size_t) i] - (float) gen (start + i - 44, ch)));
        check (worst == 0.0, juce::String ("Mix 0 equals the input delayed by the latency, bit for bit (") + (automated ? "after automation" : "from the start") + ")");
    }

    // Mix 100 is the wet path alone: independent of the dry signal; Mix 50 is the average of the two.
    {
        auto wet = make (48000.0, 1), half = make (48000.0, 1);
        set (*half, ParamID::mix, 50.0f);
        half->prepareToPlay (48000.0, kBlock);
        const auto g = sine (48000.0, 0.2, 700.0);
        const auto w = play (*wet, 1, 0, 24000, g)[0], h = play (*half, 1, 0, 24000, g)[0];
        double worst = 0.0;
        for (long i = 100; i < 24000; ++i)
            worst = std::max (worst, (double) std::abs (h[(size_t) i] - 0.5f * (w[(size_t) i] + (float) g (i - 44, 0))));
        check (worst < 1e-6, "Mix 50 is the mean of the wet path and the delayed dry input");
    }
}

//==============================================================================
void checkAlignment()
{
    std::vector<double> estimate, outputDb;
    for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        auto p = make (rate, 1);
        std::vector<float> out = play (*p, 1, 0, (long) (rate * 1.0), sine (rate, kAlignPeak))[0];
        double meter = 0.0;
        for (int b = 0; b < 40; ++b)   // the editor's needle averages over about 65 ms: the readings of the next 40 blocks
        {
            const auto more = play (*p, 1, (long) out.size(), kBlock, sine (rate, kAlignPeak))[0];
            out.insert (out.end(), more.begin(), more.end());
            meter += p->getGainReductionDb() / 40.0;
        }
        const size_t from = (size_t) (rate * 1.0);
        const double level = db (rms (out, from, out.size()) / (kAlignPeak / std::sqrt (2.0))) - 0.0;
        const double outDb = db (rms (out, from, out.size()));
        estimate.push_back (meter);
        outputDb.push_back (outDb);
        std::printf ("  (%.0f Hz: output %.3f dBFS RMS (gain %.3f dB), meter estimate %.3f dB, output peak %.3f)\n", rate, outDb, level,
                     meter, (double) *std::max_element (out.begin() + (long) from, out.end()));
    }
    check (std::abs (outputDb[1] + 18.0) < 0.05, "alignment: a -18 dBFS RMS 1 kHz sine comes out at -18.00 dBFS RMS on Early at the defaults (48 kHz)");
    double spreadGr = 0.0, spreadOut = 0.0;
    for (size_t i = 0; i < estimate.size(); ++i)
    {
        spreadGr = std::max (spreadGr, std::abs (estimate[i] - estimate[1]));
        spreadOut = std::max (spreadOut, std::abs (outputDb[i] - outputDb[1]));
    }
    check (spreadGr < 0.25 && spreadOut < 0.1, "44.1, 48, 96 and 192 kHz agree within 0.25 dB of reduction and 0.1 dB of level (spreads "
                                                + juce::String (spreadGr, 3) + " / " + juce::String (spreadOut, 3) + " dB)");
    check (estimate[1] > 9.5 && estimate[1] < 12.0, "the meter's estimate at the alignment is near 10 dB (" + juce::String (estimate[1], 2) + ")");
}

//==============================================================================
void checkStereo()
{
    const double rate = 48000.0;
    const long count = 48000;
    const auto same = [] (long n, int) { return kAlignPeak * std::sin (2.0 * kPi * 1000.0 * (double) n / 48000.0); };
    const auto render = [&] (int mode)
    {
        auto p = make (rate, 2);
        set (*p, ParamID::stereo, (float) mode);
        p->prepareToPlay (rate, kBlock);   // a mode set before playback needs no morph
        return play (*p, 2, 0, count, same);
    };
    const auto dual = render (0), link = render (1), ms = render (2);
    double linkErr = 0.0, msErr = 0.0, peak = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (size_t i = 0; i < (size_t) count; ++i)
        {
            linkErr = std::max (linkErr, (double) std::abs (dual[(size_t) ch][i] - link[(size_t) ch][i]));
            msErr = std::max (msErr, (double) std::abs (dual[(size_t) ch][i] - ms[(size_t) ch][i]));
            peak = std::max (peak, (double) std::abs (dual[(size_t) ch][i]));
        }
    std::printf ("  (L == R: Link differs from Dual by %.2e, M/S by %.2e of the peak)\n", linkErr / peak, msErr / peak);
    check (linkErr / peak < 2e-6, "Link with identical inputs equals Dual (to a float ulp)");
    check (msErr / peak < 2e-6, "M/S with identical inputs equals Dual on both channels");

    // M/S encode and decode are an identity: at a level far below the threshold the card is linear and the same for both sides, so M/S
    // output equals Dual output for different L and R.
    {
        const auto different = [] (long n, int ch)
        {
            return ch == 0 ? 0.003 * std::sin (2.0 * kPi * 600.0 * (double) n / 48000.0) : 0.002 * std::sin (2.0 * kPi * 1700.0 * (double) n / 48000.0 + 1.0);
        };
        auto d = make (rate, 2), m = make (rate, 2);
        set (*m, ParamID::stereo, 2.0f);
        m->prepareToPlay (rate, kBlock);
        const auto od = play (*d, 2, 0, 24000, different), om = play (*m, 2, 0, 24000, different);
        double err = 0.0, pk = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (size_t i = 2000; i < 24000; ++i)
            {
                err = std::max (err, (double) std::abs (od[(size_t) ch][i] - om[(size_t) ch][i]));
                pk = std::max (pk, (double) std::abs (od[(size_t) ch][i]));
            }
        check (err / pk < 3e-4, "M/S encode and decode are an identity for a linear card with L != R (" + juce::String (err / pk, 7) + " of the peak)");
    }

    // L != R: Link differs from Dual (the tie does something): the quiet side is reduced by the loud one.
    {
        const auto unequal = [] (long n, int ch) { return (ch == 0 ? 1.0 : 0.05) * kAlignPeak * std::sin (2.0 * kPi * 1000.0 * (double) n / 48000.0); };
        auto d = make (rate, 2), l = make (rate, 2);
        set (*l, ParamID::stereo, 1.0f);
        l->prepareToPlay (rate, kBlock);
        const auto od = play (*d, 2, 0, count, unequal), ol = play (*l, 2, 0, count, unequal);
        const double quietDual = rms (od[1], 40000, 48000), quietLink = rms (ol[1], 40000, 48000);
        check (db (quietLink / quietDual) < -3.0, "Link with a loud left and a quiet right: the right is reduced by the left ("
                                                      + juce::String (db (quietLink / quietDual), 2) + " dB against Dual)");
    }
}

//==============================================================================
void checkTransitions()
{
    const double rate = 48000.0;
    const auto tone = sine (rate, kAlignPeak);

    // The natural step of the steady-state output of each circuit, for the bound.
    double naturalStep[2];
    double finalRms[2];
    for (int c = 0; c < 2; ++c)
    {
        auto p = make (rate, 1);
        set (*p, ParamID::circuit, (float) c);
        p->prepareToPlay (rate, kBlock);
        const auto out = play (*p, 1, 0, 48000, tone)[0];
        naturalStep[c] = maxStep (out, 40000, out.size());
        finalRms[c] = rms (out, 40000, out.size());
    }
    const double stepBound = 1.15 * std::max (naturalStep[0], naturalStep[1]);

    // A circuit switch while running, both ways, mono and stereo-linked.
    for (const int mode : { 0, 1, 2 })
        for (const int from : { 0, 1 })
        {
            auto p = make (rate, 2);
            set (*p, ParamID::circuit, (float) from);
            set (*p, ParamID::stereo, (float) mode);
            p->prepareToPlay (rate, kBlock);
            auto out = play (*p, 2, 0, 24000, tone);
            set (*p, ParamID::circuit, (float) (1 - from));
            long at = 24000;
            auto more = play (*p, 2, at, 12000, tone);
            at += 12000;
            const long end = runUntilSettled (*p, 2, at, tone, rate);
            auto last = play (*p, 2, end, 24000, tone);
            for (int ch = 0; ch < 2; ++ch)
            {
                out[(size_t) ch].insert (out[(size_t) ch].end(), more[(size_t) ch].begin(), more[(size_t) ch].end());
                out[(size_t) ch].insert (out[(size_t) ch].end(), last[(size_t) ch].begin(), last[(size_t) ch].end());
            }
            // the step from just before the switch to the end of the cross-over, either channel
            double step = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                step = std::max (step, maxStep (out[(size_t) ch], 22000, 36000));
            const double settled = db (rms (last[0], 8000, last[0].size()) / finalRms[1 - from]);
            check (step <= stepBound && std::abs (settled) < 0.1 && p->getRunningCircuit() == (Circuit) (1 - from),
                   "circuit switch " + juce::String (from) + "->" + juce::String (1 - from) + ", stereo mode " + juce::String (mode) + ": max step "
                       + juce::String (step / naturalStep[0], 3) + " of the natural step (bound 1.15), settled level within " + juce::String (std::abs (settled), 3)
                       + " dB of a fresh instance");
        }

    // Switching back at once, before the retired engine is ready, still ends on the last request.
    {
        auto p = make (rate, 2);
        play (*p, 2, 0, 12000, tone);
        long at = 12000;
        for (int k = 0; k < 6; ++k)
        {
            set (*p, ParamID::circuit, (float) ((k + 1) % 2));
            play (*p, 2, at, 1024, tone);
            at += 1024;
        }
        const long end = runUntilSettled (*p, 2, at, tone, rate);
        check (p->getRunningCircuit() == Circuit::early, "rapid circuit toggling ends on the last request (Early)");
        set (*p, ParamID::circuit, 1.0f);
        play (*p, 2, end, 4 * kBlock, tone);
        runUntilSettled (*p, 2, end + 4 * kBlock, tone, rate);
        check (p->getRunningCircuit() == Circuit::rev1984, "...and follows a later request");
    }

    // Bias: a jump across the whole range and sweeps, no step beyond the signal's own.
    for (const double target : { 1.0, 0.3 })
    {
        auto p = make (rate, 1);
        set (*p, ParamID::bias, (float) (target == 1.0 ? 0.3 : 1.0));
        p->prepareToPlay (rate, kBlock);
        auto out = play (*p, 1, 0, 24000, tone)[0];
        const double before = maxStep (out, 20000, 24000);
        set (*p, ParamID::bias, (float) target);
        const auto after = play (*p, 1, 24000, 48000, tone)[0];
        const double natural = std::max (before, maxStep (after, 40000, after.size()));
        const double worst = maxStep (after, 0, after.size());
        check (worst <= 1.15 * natural, "bias jump 0.3 <-> 1.0: max step " + juce::String (worst / natural, 3) + " of the natural step (bound 1.15)");
    }
    {
        auto p = make (rate, 1);
        std::vector<float> all;
        long at = 0;
        for (int k = 0; k <= 70; ++k)
        {
            set (*p, ParamID::bias, 0.3f + 0.01f * (float) k);   // a staircase of automation, every 20 ms
            const auto part = play (*p, 1, at, 960, tone)[0];
            at += 960;
            all.insert (all.end(), part.begin(), part.end());
        }
        const double natural = maxStep (all, 0, all.size() / 4);
        const double worst = maxStep (all, 4000, all.size());
        check (worst <= 1.25 * natural, "bias staircase 0.30 to 1.00 in 0.01 steps: max step " + juce::String (worst / natural, 3) + " of the natural step (bound 1.25)");
    }
    // Input, Output and Mix jumps are smoothed.
    {
        auto p = make (rate, 1);
        auto out = play (*p, 1, 0, 24000, tone)[0];
        set (*p, ParamID::input, -24.0f);
        set (*p, ParamID::output, 12.0f);
        set (*p, ParamID::mix, 0.0f);
        const auto after = play (*p, 1, 24000, 24000, tone)[0];
        const double natural = maxStep (out, 20000, 24000);
        check (maxStep (after, 0, after.size()) <= 1.3 * natural, "input, output and mix jumps together: no step beyond the signal's own (" + juce::String (maxStep (after, 0, after.size()) / natural, 3) + ")");
    }
}

//==============================================================================
void checkAllocations()
{
    auto p = make (48000.0, 2);
    juce::AudioBuffer<float> buffer (2, kBlock);
    juce::MidiBuffer midi;
    const auto fill = [&] (long block)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float v = (float) (0.2 * std::sin (0.04 * (double) (block * kBlock + i)));
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, 0.7f * v);
        }
    };
    for (long b = 0; b < 20; ++b) { fill (b); p->processBlock (buffer, midi); }

    allocation_probe::count = 0;
    allocation_probe::active = true;
    long block = 20;
    for (int pass = 0; pass < 3; ++pass)
        for (int mode = 0; mode < 3; ++mode)
        {
            set (*p, ParamID::stereo, (float) mode);
            set (*p, ParamID::circuit, (float) ((pass + mode) % 2));
            set (*p, ParamID::bias, 0.3f + 0.3f * (float) mode);
            set (*p, ParamID::input, 6.0f * (float) (mode - 1));
            for (int k = 0; k < 40; ++k)
            {
                fill (block);
                p->processBlock (buffer, midi);
                ++block;
                if (k % 8 == 0) juce::Thread::sleep (1);   // let the worker settle; its allocations are on its own thread
            }
        }
    allocation_probe::active = false;
    check (allocation_probe::count == 0, "no allocation in processBlock across mode, circuit, bias and input changes (" + juce::String ((int) allocation_probe::count) + " counted)");
}

//==============================================================================
/** The audio path must not signal an event (WaitableEvent::signal() takes a mutex): the worker polls the atomic flag. */
void checkNoSignalOnAudioPath()
{
    const auto source = juce::File (__FILE__).getSiblingFile ("../Source/PluginProcessor.cpp").loadFileAsString();
    const int from = source.indexOf ("void ClassicTalkbackProcessor::requestStandbySettle");
    const int to = source.indexOf ("void ClassicTalkbackProcessor::getStateInformation");
    const auto path = source.substring (from, to);
    check (from > 0 && to > from && ! path.contains ("notify (") && ! path.contains (".signal (") && ! path.contains ("->notify"),
           "no notify() or signal() on the requestStandbySettle / processChunk / processBlock path");
}

void checkSilenceAndStress()
{
    auto p = make (48000.0, 2);
    juce::AudioBuffer<float> buffer (2, kBlock);
    juce::MidiBuffer midi;
    bool finite = true;
    const auto run = [&] (float v)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < kBlock; ++i)
                buffer.setSample (ch, i, v);
        p->processBlock (buffer, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < kBlock; ++i)
                finite = finite && std::isfinite (buffer.getSample (ch, i));
    };
    run (0.0f);
    for (int k = 0; k < 10; ++k) run (1.0f);
    run (NAN);
    run (INFINITY);
    for (int k = 0; k < 50; ++k) run (0.0f);
    check (finite, "silence, full-scale DC, NaN and Inf give finite output");
    set (*p, ParamID::input, 24.0f);
    set (*p, ParamID::output, 24.0f);
    for (int k = 0; k < 10; ++k) run (0.9f);
    check (finite, "Input and Output at +24 dB on full-scale input stay finite");
}
} // namespace

int main()
{
    const juce::ScopedJuceInitialiser_GUI gui;
    if (std::setvbuf (stdout, nullptr, _IONBF, 0) != 0)
        return 1;

    checkParameters();
    checkState();
    checkLayouts();
    checkLatencyAndMix();
    checkAlignment();
    checkStereo();
    checkTransitions();
    checkAllocations();
    checkNoSignalOnAudioPath();
    checkSilenceAndStress();

    std::printf ("  (slowest wait for a circuit request to converge: %.1f ms of wall time)\n", worstConvergenceMs);
    std::printf (failures == 0 ? "all checks passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

using namespace lmc::alignment;

//==============================================================================
juce::String ParamText::db (float value)
{
    return (juce::roundToInt (value * 10.0f) > 0 ? "+" : "") + juce::String (value, 1) + " dB";
}

juce::String ParamText::bias (float value)
{
    return juce::String (value, 2);
}

juce::String ParamText::percent (float value)
{
    return juce::String (juce::roundToInt (value)) + " %";
}

std::optional<float> ParamText::numberIn (const juce::String& text)
{
    const auto t = text.trim().replaceCharacter (',', '.');
    if (! t.containsAnyOf ("0123456789"))
        return std::nullopt;
    return t.retainCharacters ("0123456789.-+").getFloatValue();
}

//==============================================================================
std::array<float, kTrimTaps> ClassicTalkbackProcessor::makeTrimTaps()
{
    // Kaiser-windowed sinc centred between the two middle taps: a delay of (taps - 1) / 2 = 7.5 samples, unity gain at DC.
    const auto i0 = [] (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    std::array<double, kTrimTaps> h {};
    double total = 0.0;
    const double centre = 0.5 * (kTrimTaps - 1);
    for (int n = 0; n < kTrimTaps; ++n)
    {
        const double x = (double) n - centre;
        const double sinc = std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
        const double r = x / (0.5 * kTrimTaps);
        h[(size_t) n] = sinc * i0 (kTrimKaiserBeta * std::sqrt (1.0 - r * r)) / i0 (kTrimKaiserBeta);
        total += h[(size_t) n];
    }
    std::array<float, kTrimTaps> taps {};
    for (int n = 0; n < kTrimTaps; ++n)
        taps[(size_t) n] = (float) (h[(size_t) n] / total);
    return taps;
}

//==============================================================================
/** Settles the retired engine (or a standby whose bias has moved) off the audio thread: it takes a fraction of a millisecond and never
    allocates, but it is a rebuild of the card's matrices, which the audio callback does not do. */
class ClassicTalkbackProcessor::Settler final : public juce::Thread
{
public:
    explicit Settler (ClassicTalkbackProcessor& o) : juce::Thread ("ClassicTalkback settle"), owner (o) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            // Polled, not signalled: the audio thread only stores an atomic flag (WaitableEvent::signal() takes a mutex).
            wait (5);
            if (threadShouldExit())
                break;
            if (owner.standbyState.load() == 1)
                owner.settleStandby();
        }
    }

private:
    ClassicTalkbackProcessor& owner;
};

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout ClassicTalkbackProcessor::createLayout()
{
    using Float = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    using Attributes = juce::AudioParameterFloatAttributes;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<Choice> (juce::ParameterID { ParamID::circuit, 1 }, "Circuit", juce::StringArray { "Early", "1984" },
                                          (int) Circuit::early));

    const auto dbAttributes = [] {
        return Attributes().withLabel ("dB")
                           .withStringFromValueFunction ([] (float v, int) { return ParamText::db (v); })
                           .withValueFromStringFunction ([] (const juce::String& t) { return ParamText::numberIn (t).value_or (0.0f); });
    };
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::input, 1 }, "Input",
                                         juce::NormalisableRange<float> (kDbMin, kDbMax, 0.1f), 0.0f, dbAttributes()));

    // The trim wiper itself, 0.30 to 1.00 of the pot; 12 o'clock is 0.5.
    juce::NormalisableRange<float> biasRange (kBiasMin, kBiasMax, 0.001f);
    biasRange.setSkewForCentre (kBiasDefault);
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::bias, 1 }, "Bias", biasRange, kBiasDefault,
                                         Attributes().withStringFromValueFunction ([] (float v, int) { return ParamText::bias (v); })
                                                     .withValueFromStringFunction ([] (const juce::String& t)
                                                                                   { return ParamText::numberIn (t).value_or (kBiasDefault); })));

    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::output, 1 }, "Output",
                                         juce::NormalisableRange<float> (kDbMin, kDbMax, 0.1f), 0.0f, dbAttributes()));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::mix, 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                         Attributes().withLabel ("%")
                                                     .withStringFromValueFunction ([] (float v, int) { return ParamText::percent (v); })
                                                     .withValueFromStringFunction ([] (const juce::String& t)
                                                                                   { return ParamText::numberIn (t).value_or (100.0f); })));

    // In lmc::StereoMode's order.
    layout.add (std::make_unique<Choice> (juce::ParameterID { ParamID::stereo, 1 }, "Stereo", juce::StringArray { "Dual", "Link", "M/S" },
                                          (int) lmc::StereoMode::dual));
    return layout;
}

ClassicTalkbackProcessor::ClassicTalkbackProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ClassicTalkback", createLayout()),
      circuitParam (apvts.getRawParameterValue (ParamID::circuit)),
      inputParam (apvts.getRawParameterValue (ParamID::input)),
      biasParam (apvts.getRawParameterValue (ParamID::bias)),
      outputParam (apvts.getRawParameterValue (ParamID::output)),
      mixParam (apvts.getRawParameterValue (ParamID::mix)),
      stereoParam (apvts.getRawParameterValue (ParamID::stereo))
{
    apvts.state.setProperty ("stateVersion", kStateVersion, nullptr);
    settler = std::make_unique<Settler> (*this);
    settler->startThread();
}

ClassicTalkbackProcessor::~ClassicTalkbackProcessor()
{
    settler->signalThreadShouldExit();
    settler->notify();
    settler->stopThread (2000);
}

bool ClassicTalkbackProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == layouts.getMainInputChannelSet();
}

lmc::StereoMode ClassicTalkbackProcessor::currentStereoMode() const noexcept
{
    return (lmc::StereoMode) juce::jlimit (0, 2, juce::roundToInt (stereoParam->load (std::memory_order_relaxed)));
}

int ClassicTalkbackProcessor::desiredCircuit() const noexcept
{
    return juce::jlimit (0, 1, juce::roundToInt (circuitParam->load (std::memory_order_relaxed)));
}

float ClassicTalkbackProcessor::biasTarget() const noexcept
{
    return juce::jlimit (kBiasMin, kBiasMax, biasParam->load (std::memory_order_relaxed));
}

//==============================================================================
void ClassicTalkbackProcessor::configure (EngineSet& set, int circuit, double sampleRate)
{
    for (auto& card : set.card)
    {
        card.setRevision (circuit == 0 ? lmc::Revision::Early : lmc::Revision::Rev6a);
        card.setBias (biasTarget());
        card.setOversampling (kOversampling);
        card.setInputVoltsPerFullScale (kInputVoltsPerFullScale);
        card.setOutputVoltsPerFullScale (kOutputVoltsPerFullScale);
        card.trackGainReduction (true);
        card.prepare (sampleRate);
    }
    set.runner.prepare (sampleRate);
    set.runner.snapTo (currentStereoMode());
}

void ClassicTalkbackProcessor::prepareToPlay (double sampleRate, int)
{
    // The worker may still be settling an engine from before: wait for it (this is not the audio thread).
    while (standbyState.load() != 0)
        juce::Thread::sleep (1);

    // prepare() builds the cards and settles their operating points; it allocates, which is fine here and nowhere else.
    const int circuit = desiredCircuit();
    configure (sets[0], circuit, sampleRate);
    configure (sets[1], 1 - circuit, sampleRate);
    active = 0;
    activeIndex.store (0);
    activeCircuit.store (circuit);
    fading = false;
    standbyBias.store (biasTarget());
    biasCurrent = biasApplied = biasTarget();
    biasStep = 1.0 - std::exp (-(double) kChunk / (kBiasSmoothSeconds * sampleRate));

    setLatencySamples (kLatencySamples);
    trimTaps = makeTrimTaps();
    for (auto& history : trimHistory)
        history.fill (0.0f);
    trimPosition = 0;
    fadeLength = juce::jmax (1, juce::roundToInt (kCircuitFadeSeconds * sampleRate));

    inputGain.reset (sampleRate, kSmoothSeconds);
    outputGain.reset (sampleRate, kSmoothSeconds);
    mixAmount.reset (sampleRate, kSmoothSeconds);
    inputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inputParam->load()));
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));
    mixAmount.setCurrentAndTargetValue (mixParam->load() * 0.01f);
    for (auto& ring : dryRing)
        ring.fill (0.0f);
    ringPosition = 0;
    grDb.store (0.0f);
}

void ClassicTalkbackProcessor::reset()
{
    // A transport restart: the running cards go back to their idle point, the dry delay is emptied. A crossing-over is dropped; the
    // other engine is settled again in the background.
    for (auto& card : sets[(size_t) active].card)
        card.reset();
    sets[(size_t) active].runner.snapTo (currentStereoMode());
    for (auto& ring : dryRing)
        ring.fill (0.0f);
    for (auto& history : trimHistory)
        history.fill (0.0f);
    if (fading)
    {
        fading = false;
        requestStandbySettle();
    }
    grDb.store (0.0f);
}

void ClassicTalkbackProcessor::settleStandby()
{
    // Runs on the worker thread. The standby set belongs to it from the moment standbyState is 1 until it is 0 again.
    for (int pass = 0; pass < 8; ++pass)
    {
        const float bias = biasTarget();
        const int circuit = 1 - activeCircuit.load();
        for (auto& card : sets[(size_t) (1 - activeIndex.load())].card)
        {
            card.setRevision (circuit == 0 ? lmc::Revision::Early : lmc::Revision::Rev6a);
            card.setBias (bias);
            card.reset();
        }
        standbyBias.store (bias);
        if (std::abs (biasTarget() - bias) <= 0.002f)
            break;
    }
    standbyState.store (0);
}

void ClassicTalkbackProcessor::requestStandbySettle() noexcept
{
    // Called on the audio thread: no lock, no signal. The worker polls this flag.
    standbyState.store (1);
}

void ClassicTalkbackProcessor::applyBias (EngineSet& set, int numChannels, float bias) noexcept
{
    for (int ch = 0; ch < numChannels; ++ch)
        set.card[(size_t) ch].setBias (bias);
}

//==============================================================================
void ClassicTalkbackProcessor::processChunk (float* const* io, int numChannels, int n) noexcept
{
    // Bias: a bounded step towards the target; the matrices are rebuilt by the cards at the start of their next process().
    {
        const float target = biasTarget();
        const float next = std::abs (target - biasCurrent) < 2.0e-5f ? target : biasCurrent + (target - biasCurrent) * (float) biasStep;
        biasCurrent = next;
        if (! juce::exactlyEqual (biasCurrent, biasApplied))
        {
            biasApplied = biasCurrent;
            applyBias (sets[(size_t) active], numChannels, biasApplied);
            if (fading)
                applyBias (sets[(size_t) (1 - active)], numChannels, biasApplied);
        }
    }

    // The other engine follows the bias in the background, so that a circuit switch starts from a settled card at the present bias.
    if (! fading && standbyState.load() == 0 && std::abs (biasTarget() - standbyBias.load()) > 0.002f)
        requestStandbySettle();

    // A circuit switch: the settled engine of the other circuit runs beside this one and the two cross over.
    if (! fading && standbyState.load() == 0 && desiredCircuit() != activeCircuit.load())
    {
        auto& incoming = sets[(size_t) (1 - active)];
        applyBias (incoming, numChannels, biasApplied);
        incoming.runner.snapTo (currentStereoMode());
        fading = true;
        fadePosition = 0;
    }

    float inGain[kChunk], outGainV[kChunk], mixV[kChunk];
    for (int i = 0; i < n; ++i)
    {
        inGain[i] = inputGain.getNextValue();
        outGainV[i] = outputGain.getNextValue();
        mixV[i] = mixAmount.getNextValue();
    }

    float wet[kMaxChannels][kChunk], other[kMaxChannels][kChunk];
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < n; ++i)
        {
            const float x = std::isfinite (io[ch][i]) ? io[ch][i] : 0.0f;
            dryRing[(size_t) ch][(size_t) ((ringPosition + i) & (kRing - 1))] = x;
            wet[ch][i] = x * inGain[i];
            other[ch][i] = wet[ch][i];
        }

    const auto mode = currentStereoMode();
    const auto run = [&] (EngineSet& set, float (&buf)[kMaxChannels][kChunk])
    {
        if (numChannels == 2)
            set.runner.process (set.card[0], set.card[1], buf[0], buf[1], n, mode);
        else
            set.card[0].process (buf[0], n);
    };
    run (sets[(size_t) active], wet);

    // Which engine the meter follows: the one with the larger share of this chunk.
    const bool crossing = fading;
    int meterSet = active;
    if (fading)
    {
        run (sets[(size_t) (1 - active)], other);
        for (int i = 0; i < n; ++i)
        {
            const float w = juce::jmin (1.0f, (float) (fadePosition + i + 1) / (float) fadeLength);
            for (int ch = 0; ch < numChannels; ++ch)
                wet[ch][i] += (other[ch][i] - wet[ch][i]) * w;
        }
        fadePosition += n;
        if (2 * fadePosition > fadeLength)
            meterSet = 1 - active;
        if (fadePosition >= fadeLength)
        {
            active = 1 - active;
            activeIndex.store (active);
            activeCircuit.store (1 - activeCircuit.load());
            fading = false;
            requestStandbySettle();   // the retired engine, in the background
        }
    }

    // The engine's own gain-reduction observable (its mean over the chunk, per card; the larger card is shown).
    {
        float gr = 0.0f;
        for (int s = 0; s < 2; ++s)
        {
            if (! crossing && s != meterSet)
                continue;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float v = (float) sets[(size_t) s].card[(size_t) ch].takeGainReductionDb();
                if (s == meterSet)
                    gr = juce::jmax (gr, v);
            }
        }
        grAccumulated += gr * (float) n;
        grCount += n;
    }

    // The half-sample trim that makes the wet path a whole number of samples late (see Alignment.h).
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < n; ++i)
        {
            auto& history = trimHistory[(size_t) ch];
            const int at = (trimPosition + i) & (kTrimRing - 1);
            history[(size_t) at] = wet[ch][i];
            float y = 0.0f;
            for (int k = 0; k < kTrimTaps; ++k)
                y += trimTaps[(size_t) k] * history[(size_t) ((at - k) & (kTrimRing - 1))];
            wet[ch][i] = y;
        }
    trimPosition = (trimPosition + n) & (kTrimRing - 1);

    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < n; ++i)
        {
            const float dry = dryRing[(size_t) ch][(size_t) ((ringPosition + i - kLatencySamples) & (kRing - 1))];
            float w = wet[ch][i] * outGainV[i];
            if (! std::isfinite (w))
                w = 0.0f;
            io[ch][i] = dry * (1.0f - mixV[i]) + w * mixV[i];
        }
    ringPosition = (ringPosition + n) & (kRing - 1);
}

void ClassicTalkbackProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const int numChannels = juce::jmin (getTotalNumInputChannels(), getTotalNumOutputChannels(), kMaxChannels);
    if (numChannels < 1)
        return;

    inputGain.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (kDbMin, kDbMax, inputParam->load (std::memory_order_relaxed))));
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (kDbMin, kDbMax, outputParam->load (std::memory_order_relaxed))));
    mixAmount.setTargetValue (juce::jlimit (0.0f, 1.0f, mixParam->load (std::memory_order_relaxed) * 0.01f));

    grAccumulated = 0.0f;
    grCount = 0;
    float* pointers[kMaxChannels] {};
    for (int pos = 0; pos < numSamples; pos += kChunk)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            pointers[ch] = buffer.getWritePointer (ch) + pos;
        processChunk (pointers, numChannels, juce::jmin (kChunk, numSamples - pos));
    }
    if (grCount > 0)
        grDb.store (grAccumulated / (float) grCount, std::memory_order_relaxed);
}

//==============================================================================
void ClassicTalkbackProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    apvts.state.setProperty ("stateVersion", kStateVersion, nullptr);
    if (const auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void ClassicTalkbackProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    const auto loaded = juce::ValueTree::fromXml (*xml);
    juce::StringArray missing;
    for (const char* id : { ParamID::circuit, ParamID::input, ParamID::bias, ParamID::output, ParamID::mix, ParamID::stereo })
        if (! loaded.getChildWithProperty ("id", id).isValid())
            missing.add (id);
    apvts.replaceState (loaded);

    // replaceState leaves a parameter the state does not mention at its live value; a session that does not name one gets its default.
    for (const auto& id : missing)
        if (auto* parameter = apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->getDefaultValue());
    apvts.state.setProperty ("stateVersion", kStateVersion, nullptr);
}

juce::AudioProcessorEditor* ClassicTalkbackProcessor::createEditor()
{
    return new ClassicTalkbackEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ClassicTalkbackProcessor();
}

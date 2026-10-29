#pragma once

#include "Alignment.h"
#include "StereoModes.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>

/** Parameter IDs: stable API (sessions and automation refer to them). Permanent from 0.1.0; append, never reorder. */
namespace ParamID
{
    inline constexpr auto circuit = "circuit", input = "input", bias = "bias", output = "output", mix = "mix", stereo = "stereo";
}

/** Value text of the parameters, shared by the processor, the editor and the tests. */
namespace ParamText
{
    /** "+3.5 dB", "-24.0 dB" (a proper minus sign is not used: the text is typed back). */
    juce::String db (float value);
    /** "0.50": the trim wiper's position, 0 at the bottom of the pot, 1 at the top. */
    juce::String bias (float value);
    /** "100 %". */
    juce::String percent (float value);
    /** The number in typed text ("-3 dB", "+2", "5 %", "0,45"); nothing when there is none. */
    std::optional<float> numberIn (const juce::String& text);
}

enum class Circuit { early = 0, rev1984 = 1 };

class ClassicTalkbackProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int kMaxChannels = 2;

    ClassicTalkbackProcessor();
    ~ClassicTalkbackProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    /** Any thread: the gain reduction (dB, positive) the engine reports, averaged over the last block and over the cards. */
    float getGainReductionDb() const noexcept { return grDb.load (std::memory_order_relaxed); }
    /** Any thread: the circuit the audio is coming from (it follows the parameter once a switch has crossed over). */
    Circuit getRunningCircuit() const noexcept { return (Circuit) activeCircuit.load (std::memory_order_relaxed); }
    /** For tests: true while a circuit switch is crossing over or the retired engine is being settled. */
    bool isSwitching() const noexcept { return fading || standbyState.load() != 0; }

private:
    struct EngineSet
    {
        std::array<lmc::Card, kMaxChannels> card;
        lmc::StereoRunner runner;
    };
    class Settler;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    lmc::StereoMode currentStereoMode() const noexcept;
    int desiredCircuit() const noexcept;
    float biasTarget() const noexcept;
    void configure (EngineSet&, int circuit, double sampleRate);
    void settleStandby();
    void requestStandbySettle() noexcept;
    void applyBias (EngineSet&, int numChannels, float bias) noexcept;
    void processChunk (float* const* io, int numChannels, int n) noexcept;

    std::array<EngineSet, 2> sets;
    int active = 0;                           // audio thread only; the worker reads activeIndex
    std::atomic<int> activeIndex { 0 };       // mirror of `active`, stored before standbyState is set to 1
    std::atomic<int> activeCircuit { 0 };
    std::atomic<int> standbyState { 0 };      // 0 ready, 1 being settled by the worker
    std::atomic<float> standbyBias { 0.5f };
    bool fading = false;
    int fadePosition = 0, fadeLength = 1;

    float biasCurrent = 0.5f, biasApplied = 0.5f;
    double biasStep = 0.0;

    static constexpr int kRing = 128, kTrimRing = 32;
    std::array<float, lmc::alignment::kTrimTaps> trimTaps {};
    std::array<std::array<float, kTrimRing>, kMaxChannels> trimHistory {};
    int trimPosition = 0;
    std::array<std::array<float, kRing>, kMaxChannels> dryRing {};
    int ringPosition = 0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> inputGain, outputGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixAmount;
    std::atomic<float> grDb { 0.0f };
    float grAccumulated = 0.0f;
    int grCount = 0;
    static std::array<float, lmc::alignment::kTrimTaps> makeTrimTaps();

    std::atomic<float>* const circuitParam;
    std::atomic<float>* const inputParam;
    std::atomic<float>* const biasParam;
    std::atomic<float>* const outputParam;
    std::atomic<float>* const mixParam;
    std::atomic<float>* const stereoParam;

    std::unique_ptr<Settler> settler;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClassicTalkbackProcessor)
};

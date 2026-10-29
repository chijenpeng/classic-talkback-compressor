#pragma once

#include "PluginProcessor.h"

/** The embedded typefaces. Shared by all open editors and released with the last one, so
    nothing outlives the host's JUCE shutdown. */
struct EditorFonts
{
    EditorFonts();
    juce::Typeface::Ptr inter, mono;
};

/** Flat drawing for the knobs and the resize corner; everything else paints itself. */
class FlatLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float position,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
};

/** A flat disc with a dot, its name above and its value below. Double-click the disc for the
    default; double-click the value to type one. */
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState&, const char* paramId, const juce::String& name, const juce::String& help,
          int diameter, float sweepDegrees = 270.0f);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int kLabelHeight = 16, kGap = 8;
    /** Label, disc and value. */
    int preferredHeight() const noexcept { return 2 * (kLabelHeight + kGap) + diameter; }

private:
    void finishEditing (bool apply);

    juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::String name;
    int diameter;
    juce::TextEditor editor;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
};

/** "LABEL  OPTION  OPTION ..." as plain text; the selected option is lit. */
class OptionRow final : public juce::Component
{
public:
    OptionRow (juce::RangedAudioParameter&, const juce::String& label, juce::StringArray options, const juce::String& help);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** The label's own width; rows stacked together share the widest plus the label gap. */
    int labelTextWidth() const;
    void setLabelColumn (int width) { labelColumn = width; repaint(); }
    int preferredWidth() const;

private:
    std::vector<juce::Range<int>> optionRanges() const;
    int optionAt (int x) const;

    juce::String label;
    juce::StringArray options;
    int labelColumn = 0, selected = 0, hovered = -1;
    juce::ParameterAttachment attachment;
};

/** The gain-reduction needle on the 1176 VU face (as the Classic Channel Strip's): the engine's own reduction estimate, rest at 0,
    falling to the left, VU-style ballistics chosen for this plug-in. No numeric readout. */
class GrMeter final : public juce::Component
{
public:
    GrMeter();

    /** Called at the editor's timer rate (kRateHz) with the reduction in dB (positive). Repaints only when the needle moved visibly. */
    void push (float reductionDb);
    /** The needle's position in dB of reduction (what is drawn). */
    float shownReductionDb() const noexcept { return shownDb; }
    void paint (juce::Graphics&) override;

    static constexpr int kRateHz = 30;

private:
    static float angleFor (float meterDb) noexcept;

    float smoothed = 0.0f, shownDb = 0.0f;
};

/** Everything inside the window, laid out at the design size; the editor scales it. */
class EditorView final : public juce::Component
{
public:
    static constexpr int kWidth = 688, kHeight = 260;

    explicit EditorView (ClassicTalkbackProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Reads the processor's meter into the needle, and the running circuit into the subtitle. */
    void updateMeters();
    /** Shows the help of the control under the mouse (nullptr: the default text). */
    void showHelpFor (juce::Component*);
    /** Shows the help of the control for this parameter ID ("" is the meter). */
    void showHelpForParameter (const juce::String& paramId);

    /** The title and body that showHelpFor would show for this component. */
    static std::pair<juce::String, juce::String> helpFor (juce::Component*, const juce::Component* stopAt);
    static std::pair<juce::String, juce::String> defaultHelp();
    /** The lines the help body needs at the help block's width (the block holds three). */
    static int helpLines (const juce::String& body);
    juce::Component* controlFor (const juce::String& id);
    const GrMeter& meter() const noexcept { return reduction; }

private:
    ClassicTalkbackProcessor& processor;
    Knob input, bias, output, mix;
    OptionRow circuitRow, stereoRow;
    GrMeter reduction;

    int shownCircuit = 0;
    juce::String helpTitle, helpText;
    juce::Rectangle<int> helpArea;
};

class ClassicTalkbackEditor final : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    explicit ClassicTalkbackEditor (ClassicTalkbackProcessor&);
    ~ClassicTalkbackEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** For the offline snapshot tool, which has no host message loop or mouse. */
    void updateMeters() { view.updateMeters(); }
    void showHelpForParameter (const juce::String& paramId) { view.showHelpForParameter (paramId); }
    EditorView& getView() noexcept { return view; }

private:
    void timerCallback() override;

    ClassicTalkbackProcessor& owner;
    juce::SharedResourcePointer<EditorFonts> fonts;
    FlatLookAndFeel lookAndFeel;
    EditorView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClassicTalkbackEditor)
};

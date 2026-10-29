// Renders the editor to docs/editor.png without a host or a screen, after running a 1 kHz tone through the plug-in so the needle has
// something to show, and checks the hover help:
//
//   editor.png             1x, Early, the default help, the processor's real idle state (needle at 0: no signal has run)
//   editor-0.75x.png       the minimum window scale
//   editor-2x.png          the maximum window scale
//   editor-1984-link.png   1984, Link, with the Bias help showing, after the real engine has run a 1 kHz tone at -18 dBFS RMS
//                          (the needle shows what the processor measured; no value is injected anywhere)
//
//   lmc_snapshot [output.png [editorScale pixelScale]]
//
// (default: docs/editor.png at editorScale 1, pixelScale 2; the variants are written beside it; returns 1 if a check fails.)

#include "../Source/PluginEditor.h"

#include <cstdio>

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    failures += ok ? 0 : 1;
}

std::unique_ptr<ClassicTalkbackProcessor> makeProcessor()
{
    auto p = std::make_unique<ClassicTalkbackProcessor>();
    p->setPlayConfigDetails (2, 2, kRate, kBlock);
    p->prepareToPlay (kRate, kBlock);
    return p;
}

void set (ClassicTalkbackProcessor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

/** A 1 kHz sine on both channels at -18 dBFS RMS, so the needle reads about 10 dB at the defaults. */
void runTone (ClassicTalkbackProcessor& p, double seconds)
{
    static double phase = 0.0;
    const float amplitude = (float) (std::pow (10.0, -18.0 / 20.0) * std::sqrt (2.0));
    juce::AudioBuffer<float> buffer (2, kBlock);
    juce::MidiBuffer midi;
    for (int done = 0; done < (int) (seconds * kRate); done += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float v = amplitude * (float) std::sin (phase);
            phase += juce::MathConstants<double>::twoPi * 1000.0 / kRate;
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }
        p.processBlock (buffer, midi);
    }
}

/** The needle follows at the editor's timer rate; with no message loop here, push it by hand for the same time. */
void settleNeedle (ClassicTalkbackProcessor& p, ClassicTalkbackEditor& editor, double seconds)
{
    for (int k = 0; k < (int) (seconds * GrMeter::kRateHz); ++k)
    {
        runTone (p, 1.0 / GrMeter::kRateHz);
        editor.updateMeters();
    }
}

bool writeSnapshot (ClassicTalkbackEditor& editor, const juce::File& file, float pixelScale, const juce::String& helpFor = {})
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);   // let the editor's timer run too
    if (helpFor.isNotEmpty())
        editor.showHelpForParameter (helpFor);
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, pixelScale);
    file.deleteFile();
    juce::FileOutputStream out (file);
    const bool ok = out.openedOk() && juce::PNGImageFormat().writeImageToStream (image, out);
    std::printf ("%s %s (%d x %d)\n", ok ? "wrote" : "could not write", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    return ok;
}

void checkHelp (ClassicTalkbackProcessor& p, ClassicTalkbackEditor& editor)
{
    auto& view = editor.getView();
    const auto [defaultTitle, defaultText] = EditorView::defaultHelp();
    check (defaultTitle.isNotEmpty() && defaultText.contains ("double-click a knob for its default"), "default help names the plug-in and the gestures");
    check (EditorView::helpLines (defaultText) <= 3, "default help fits the three lines of the block (" + juce::String (EditorView::helpLines (defaultText)) + ")");

    // every parameter has a control with help of its own, which fits the block
    for (auto* parameter : p.getParameters())
    {
        const auto id = static_cast<juce::AudioProcessorParameterWithID*> (parameter)->paramID;
        auto* control = view.controlFor (id);
        check (control != nullptr, "parameter " + id + " has a control");
        if (control == nullptr)
            continue;
        const auto [title, text] = EditorView::helpFor (control, &view);
        check (title.isNotEmpty() && title == title.toUpperCase() && text.isNotEmpty() && text != defaultText, "control " + id + " has its own help, with an upper-case title");
        check (EditorView::helpLines (text) <= 3, "help of " + id + " fits three lines (" + juce::String (EditorView::helpLines (text)) + ")");

        // the knobs: the inner slider and the value editor resolve to the knob's help
        for (auto* child : control->getChildren())
        {
            const auto [childTitle, childText] = EditorView::helpFor (child, &view);
            check (childText == text && childTitle == title, "the inner component of " + id + " shows the same help");
        }
    }
    // the needle
    {
        auto* meter = view.controlFor ("meter");
        const auto [title, text] = EditorView::helpFor (meter, &view);
        check (meter != nullptr && title == "GAIN REDUCTION" && text != defaultText && EditorView::helpLines (text) <= 3, "the gain-reduction needle has help that fits (" + juce::String (EditorView::helpLines (text)) + " lines)");
    }
    // leaving restores the default
    view.showHelpFor (view.controlFor (ParamID::bias));
    view.showHelpFor (nullptr);
    const auto [afterTitle, afterText] = EditorView::helpFor (nullptr, &view);
    check (afterTitle == defaultTitle && afterText == defaultText, "no component under the mouse: the default text returns");

    // every direct child of the view is a control with help (nothing interactive without it)
    int children = 0;
    for (auto* child : view.getChildren())
    {
        ++children;
        check (EditorView::helpFor (child, &view).second != defaultText, "child '" + child->getComponentID() + "' carries help");
    }
    check (children == 7, "the view holds six controls and the needle (" + juce::String (children) + ")");
}
} // namespace

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    if (std::setvbuf (stdout, nullptr, _IONBF, 0) != 0)
        return 1;
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/editor.png");
    const double editorScale = argc > 2 ? juce::String (argv[2]).getDoubleValue() : 1.0;
    const float pixelScale = argc > 3 ? (float) juce::String (argv[3]).getDoubleValue() : 2.0f;
    file.getParentDirectory().createDirectory();
    const auto sibling = [&] (const juce::String& name) { return file.getSiblingFile (name); };
    const auto resize = [] (ClassicTalkbackEditor& e, double scale)
    {
        e.setSize (juce::roundToInt (EditorView::kWidth * scale), juce::roundToInt (EditorView::kHeight * scale));
    };

    bool ok = true;
    auto p = makeProcessor();
    {
        ClassicTalkbackEditor editor (*p);
        checkHelp (*p, editor);
        check (editor.getView().meter().shownReductionDb() < 0.05f, "idle: the needle rests at 0 (" + juce::String (editor.getView().meter().shownReductionDb(), 3) + " dB)");
        resize (editor, editorScale);
        ok = writeSnapshot (editor, file, pixelScale) && ok;
        if (argc <= 2)
        {
            resize (editor, 0.75);
            ok = writeSnapshot (editor, sibling ("editor-0.75x.png"), pixelScale) && ok;
            resize (editor, 2.0);
            ok = writeSnapshot (editor, sibling ("editor-2x.png"), pixelScale) && ok;
        }
    }
    {
        auto r = makeProcessor();
        ClassicTalkbackEditor editor (*r);
        runTone (*r, 1.0);
        settleNeedle (*r, editor, 1.5);
        std::printf ("needle at %.2f dB (the processor reports %.2f dB)\n", editor.getView().meter().shownReductionDb(), r->getGainReductionDb());
        check (editor.getView().meter().shownReductionDb() > 8.0 && editor.getView().meter().shownReductionDb() < 13.0, "the needle follows the engine's reduction");
    }
    if (argc <= 2)
    {
        auto q = makeProcessor();
        set (*q, ParamID::circuit, 1.0f);
        set (*q, ParamID::stereo, 1.0f);
        q->prepareToPlay (kRate, kBlock);
        ClassicTalkbackEditor editor (*q);
        settleNeedle (*q, editor, 1.5);
        ok = writeSnapshot (editor, sibling ("editor-1984-link.png"), pixelScale, ParamID::bias) && ok;
    }

    std::printf (failures == 0 ? "all checks passed\n" : "%d check(s) failed\n", failures);
    return ok && failures == 0 ? 0 : 1;
}

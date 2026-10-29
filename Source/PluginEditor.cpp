#include "PluginEditor.h"

#include <BinaryData.h>

namespace
{
    // The family's flat language on a light palette: a pale blue window, dark blue discs are the things you turn, dark blue type is
    // what is selected and the quieter slate names things. Chosen for this plug-in, not taken from the hardware (docs/ui-design.md).
    namespace Theme
    {
        const juce::Colour background { 0xffd3e5f7 }, panel { 0xffc3d9ef }, text { 0xff0a1a30 },
                           label { 0xff304a69 }, accent { 0xff0f6c9c }, pale { 0xff1a3a70 },
                           clipping { 0xffb3261e }, field { 0xffb2cbe6 };

        // The needle's face: lighter than the window, as a paper dial; its hood is the panel.
        const juce::Colour face { 0xffe8f1fb }, hood = panel;
        // The red zone of the scale, quieter than the clipping red it comes from.
        const juce::Colour redZone = clipping.withAlpha (0.8f);

        juce::Font make (const juce::Typeface::Ptr& typeface, float height, float tracking)
        {
            return juce::Font (juce::FontOptions (typeface).withHeight (height)).withExtraKerningFactor (tracking);
        }

        /** Labels are upper-case and tracked out; pass upper-case text. */
        juce::Font labelFont (float height = 11.0f)  { return make (juce::SharedResourcePointer<EditorFonts>()->inter, height, 0.09f); }
        juce::Font titleFont (float height)          { return make (juce::SharedResourcePointer<EditorFonts>()->inter, height, 0.18f); }
        juce::Font bodyFont (float height = 12.0f)   { return make (juce::SharedResourcePointer<EditorFonts>()->inter, height, 0.01f); }
        juce::Font valueFont (float height = 12.0f)  { return make (juce::SharedResourcePointer<EditorFonts>()->mono, height, 0.0f); }

        int widthOf (const juce::Font& font, const juce::String& s)
        {
            return juce::roundToInt (std::ceil (juce::GlyphArrangement::getStringWidth (font, s)));
        }
    }

    // Spacing grammar of an option row: sibling options closest, the label further from its first option.
    constexpr int kOptionGap = 10, kLabelGap = 18, kRowHeight = 18;

    const juce::Identifier helpId ("help"), helpTitleId ("helpTitle"), editorScaleId ("editorScale");

    constexpr auto kDefaultHelpTitle = "Classic Talkback Compressor";
    constexpr auto kDefaultHelp =
        "The SSL CF82B33 talkback (listen mic) compressor card, modelled from its two factory drawings. Hover over a control to see "
        "what it does; double-click a knob for its default, or its value to type one.";

    void setHelp (juce::Component& c, const juce::String& title, const juce::String& help)
    {
        c.getProperties().set (helpTitleId, title.toUpperCase());
        c.getProperties().set (helpId, help);
    }

    juce::String minusSign() { return juce::String::fromUTF8 ("\xE2\x88\x92"); }
    juce::String middleDot() { return juce::String::fromUTF8 ("\xC2\xB7"); }
}

//==============================================================================
EditorFonts::EditorFonts()
    : inter (juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize)),
      mono (juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoMedium_ttf, BinaryData::JetBrainsMonoMedium_ttfSize))
{
}

//==============================================================================
void FlatLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                                        float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 1.0f;
    const float angle = startAngle + position * (endAngle - startAngle);
    const float dot = juce::jmax (2.5f, radius * 0.11f);

    // On this light palette the disc is the dark blue, so a hovered disc goes lighter.
    g.setColour (slider.isMouseOverOrDragging() ? Theme::pale.brighter (0.25f) : Theme::pale);
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));

    g.setColour (Theme::background);
    g.fillEllipse (juce::Rectangle<float> (dot * 2.0f, dot * 2.0f).withCentre (centre.getPointOnCircumference (radius * 0.72f, angle)));
}

void FlatLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    // Three short diagonals in the label colour, quiet until touched.
    g.setColour (Theme::label.withAlpha (isMouseOver || isMouseDragging ? 0.8f : 0.3f));
    const float size = (float) juce::jmin (w, h);
    for (float i = 0.3f; i < 1.0f; i += 0.3f)
        g.drawLine ((float) w - size * i, (float) h - 2.0f, (float) w - 2.0f, (float) h - size * i, 1.0f);
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const char* paramId, const juce::String& nameToUse, const juce::String& help,
            int diameterToUse, float sweepDegrees)
    : name (nameToUse.toUpperCase()), diameter (diameterToUse)
{
    const float halfSweep = juce::degreesToRadians (sweepDegrees * 0.5f), twoPi = juce::MathConstants<float>::twoPi;
    slider.setRotaryParameters (twoPi - halfSweep, twoPi + halfSweep, true);
    slider.setVelocityModeParameters (0.15, 1, 0.0, true, juce::ModifierKeys::shiftModifier);   // Shift: fine

    attachment = std::make_unique<juce::SliderParameterAttachment> (*state.getParameter (paramId), slider);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);

    editor.setJustification (juce::Justification::centred);
    editor.setFont (Theme::valueFont (12.0f));
    editor.setIndents (0, 0);
    editor.setColour (juce::TextEditor::backgroundColourId, Theme::field);
    editor.setColour (juce::TextEditor::textColourId, Theme::pale);
    editor.setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.35f));
    editor.setColour (juce::TextEditor::highlightedTextColourId, Theme::text);
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::CaretComponent::caretColourId, Theme::pale);
    editor.onReturnKey = [this] { finishEditing (true); };
    editor.onEscapeKey = [this] { finishEditing (false); };
    editor.onFocusLost = [this] { finishEditing (true); };
    addChildComponent (editor);

    setHelp (*this, name, help);
}

void Knob::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (kLabelHeight + kGap - 1);
    slider.setBounds (area.removeFromTop (diameter + 2).withSizeKeepingCentre (diameter + 2, diameter + 2));
    editor.setBounds (getLocalBounds().removeFromBottom (kLabelHeight).withSizeKeepingCentre (72, kLabelHeight));
}

void Knob::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();

    g.setColour (Theme::label);
    g.setFont (Theme::labelFont());
    g.drawText (name, area.removeFromTop (kLabelHeight), juce::Justification::centred, false);

    g.setFont (Theme::valueFont (12.0f));
    g.setColour (Theme::text);
    g.drawText (slider.getTextFromValue (slider.getValue()), area.removeFromBottom (kLabelHeight), juce::Justification::centred, false);
}

void Knob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < getHeight() - kLabelHeight - 2)
        return;

    editor.setText (slider.getTextFromValue (slider.getValue()), false);
    editor.setVisible (true);
    editor.selectAll();
    editor.grabKeyboardFocus();
}

void Knob::finishEditing (bool apply)
{
    if (! editor.isVisible())
        return;

    editor.setVisible (false);
    const auto text = editor.getText().trim();

    // Only text that names a value; anything else is ignored, never turned into a default.
    if (apply && text.containsAnyOf ("0123456789"))
        slider.setValue (slider.getValueFromText (text), juce::sendNotificationSync);
    repaint();
}

//==============================================================================
OptionRow::OptionRow (juce::RangedAudioParameter& param, const juce::String& labelToUse, juce::StringArray optionsToUse, const juce::String& help)
    : label (labelToUse.toUpperCase()), options (std::move (optionsToUse)),
      attachment (param, [this] (float v) { selected = juce::roundToInt (v); repaint(); })
{
    labelColumn = labelTextWidth() + kLabelGap;
    attachment.sendInitialUpdate();
    setHelp (*this, label, help);
}

int OptionRow::labelTextWidth() const
{
    return Theme::widthOf (Theme::labelFont(), label);
}

std::vector<juce::Range<int>> OptionRow::optionRanges() const
{
    std::vector<juce::Range<int>> ranges;
    const auto font = Theme::labelFont();
    int x = labelColumn;

    for (const auto& option : options)
    {
        const int width = Theme::widthOf (font, option.toUpperCase());
        ranges.push_back ({ x, x + width });
        x += width + kOptionGap;
    }
    return ranges;
}

int OptionRow::preferredWidth() const
{
    const auto ranges = optionRanges();
    return ranges.empty() ? labelColumn : ranges.back().getEnd();
}

int OptionRow::optionAt (int x) const
{
    const auto ranges = optionRanges();
    for (int i = 0; i < (int) ranges.size(); ++i)
        if (ranges[(size_t) i].expanded (kOptionGap / 2).contains (x))
            return i;
    return -1;
}

void OptionRow::paint (juce::Graphics& g)
{
    g.setFont (Theme::labelFont());
    g.setColour (Theme::label);
    g.drawText (label, 0, 0, labelColumn, getHeight(), juce::Justification::centredLeft, false);

    const auto ranges = optionRanges();
    for (int i = 0; i < options.size(); ++i)
    {
        const auto range = ranges[(size_t) i];
        g.setColour (i == selected ? Theme::pale : (i == hovered ? Theme::text : Theme::label.withAlpha (0.7f)));
        g.drawText (options[i].toUpperCase(), range.getStart(), 0, range.getLength() + 4, getHeight(),
                    juce::Justification::centredLeft, false);
    }
}

void OptionRow::mouseDown (const juce::MouseEvent& e)
{
    const int option = optionAt (e.x);
    if (option >= 0)
        attachment.setValueAsCompleteGesture ((float) option);
}

void OptionRow::mouseMove (const juce::MouseEvent& e)   { hovered = optionAt (e.x); repaint(); }
void OptionRow::mouseExit (const juce::MouseEvent&)     { hovered = -1; repaint(); }

//==============================================================================
namespace
{
    // The VU face geometry of the 1176 and the Classic Channel Strip: deflection proportional to the rectified voltage, the scale
    // from -20 to +3. Here the needle reads reduction, so it rests at 0 and falls to the left.
    constexpr float kHalfSweep = 0.85f;   // radians either side of 12 o'clock
    constexpr float kArcTop = 25.0f;      // from the top of the face to the top of the arc
    constexpr float kHoodHeight = 19.0f, kCorner = 4.0f;
    constexpr float kMeterMin = -20.0f, kMeterMax = 3.0f;
    constexpr float kMarks[] { -20.0f, -10.0f, -7.0f, -5.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f };
}

GrMeter::GrMeter()
{
    setHelp (*this, "Gain reduction",
             "The reduction the card's JFET attenuator reports, from its gate voltage (a small-signal estimate; near 10 dB it can sit about "
             "1 dB from the level change you measure). The needle rests at 0, falls to the left and pins at the " + minusSign()
             + "20 end beyond 20 dB (the card reaches 25). The VU face and its 65 ms ballistics are chosen for this plug-in.");
}

float GrMeter::angleFor (float db) noexcept
{
    const auto deflection = [] (float d) { return std::pow (10.0f, d / 20.0f); };
    const float lo = deflection (kMeterMin), hi = deflection (kMeterMax);
    const float clamped = juce::jlimit (kMeterMin, kMeterMax, db);
    return kHalfSweep * (2.0f * (deflection (clamped) - lo) / (hi - lo) - 1.0f);
}

void GrMeter::push (float reductionDb)
{
    // VU-style ballistics chosen for this plug-in: a first-order follow with a 65 ms time constant, the same up and down.
    constexpr float kTimeConstantSeconds = 0.065f;
    const float k = 1.0f - std::exp (-(1.0f / (float) kRateHz) / kTimeConstantSeconds);
    smoothed += (juce::jlimit (0.0f, 40.0f, reductionDb) - smoothed) * k;
    // Repaint when the needle would move visibly, compared with what is on screen.
    if (std::abs (smoothed - shownDb) > 0.05f)
    {
        shownDb = smoothed;
        repaint();
    }
}

void GrMeter::paint (juce::Graphics& g)
{
    const auto faceArea = getLocalBounds().toFloat();
    const auto hood = faceArea.withTop (faceArea.getBottom() - kHoodHeight);
    const float radius = faceArea.getWidth() * 0.42f / std::sin (kHalfSweep);
    const juce::Point<float> pivot { faceArea.getCentreX(), faceArea.getY() + kArcTop + radius };

    g.setColour (Theme::face);
    g.fillRoundedRectangle (faceArea, kCorner);
    juce::Path hoodShape;
    hoodShape.addRoundedRectangle (hood.getX(), hood.getY(), hood.getWidth(), hood.getHeight(), kCorner, kCorner, false, false, true, true);
    g.setColour (Theme::hood);
    g.fillPath (hoodShape);

    juce::Path line, red;
    line.addCentredArc (pivot.x, pivot.y, radius, radius, 0.0f, angleFor (kMeterMin), angleFor (0.0f), true);
    red.addCentredArc (pivot.x, pivot.y, radius + 1.0f, radius + 1.0f, 0.0f, angleFor (0.0f), angleFor (kMeterMax), true);
    g.setColour (Theme::label.withAlpha (0.7f));
    g.strokePath (line, juce::PathStrokeType (1.0f));
    g.setColour (Theme::redZone);
    g.strokePath (red, juce::PathStrokeType (3.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));

    g.setFont (Theme::valueFont (8.0f));
    for (const float mark : kMarks)
    {
        const float a = angleFor (mark);
        const bool plus = mark > 0.0f;
        g.setColour (plus ? Theme::redZone : Theme::label);
        g.drawLine ({ pivot.getPointOnCircumference (radius, a), pivot.getPointOnCircumference (radius + 5.0f, a) }, mark == 0.0f ? 1.5f : 1.0f);

        const auto text = mark < 0.0f ? minusSign() + juce::String ((int) -mark) : (plus ? "+" : "") + juce::String ((int) mark);
        const auto at = pivot.getPointOnCircumference (radius + 11.0f, a);
        g.drawText (text, juce::Rectangle<float> (22.0f, 10.0f).withCentre (at), juce::Justification::centred, false);
    }

    g.setColour (Theme::label.withAlpha (0.7f));
    g.setFont (Theme::titleFont (10.0f));
    g.drawText ("GR", juce::Rectangle<float> (faceArea.getX(), pivot.y - radius + 35.0f, faceArea.getWidth(), 12.0f),
                juce::Justification::centred, false);

    // The needle, from its pivot under the hood: reduction falls to the left.
    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (faceArea.withBottom (hood.getY()).toNearestInt());
    g.setColour (Theme::pale);
    g.drawLine ({ pivot, pivot.getPointOnCircumference (radius + 2.0f, angleFor (-shownDb)) }, 1.25f);
}

//==============================================================================
EditorView::EditorView (ClassicTalkbackProcessor& p)
    : processor (p),
      input (p.apvts, ParamID::input, "Input",
             "How hard the signal drives the card. 0 dB: a 1 kHz sine at " + minusSign() + "18 dBFS RMS gives 10 dB of reduction on EARLY at "
             "Bias 0.5; an alignment chosen for this plug-in. The preamp clips about 20 dB above that; at Input +15, peaks above about "
             + minusSign() + "10 dBFS. Checked against circuit simulation up to " + minusSign() + "16 dBV at the card input; hotter is "
             "extrapolated.",
             64, 320.0f),
      bias (p.apvts, ParamID::bias, "Bias",
            "The card's BIAS_TRIM, as the wiper position (0 bottom, 1 top). Lower starts reducing earlier and, at the same reduction, is "
            "softer, slower and cleaner: at 10 dB on EARLY, 0.30 gives 6 % THD and a 21 ms release, 0.5 gives 19 % and 9 ms, 1.0 gives "
            "30 % and 4 ms (simulated). Range and default are plug-in choices.",
            64),
      output (p.apvts, ParamID::output, "Output",
              "The level after the card, before Mix. 0 dB: the same " + minusSign() + "18 dBFS RMS sine comes out at " + minusSign()
              + "18 dBFS RMS on EARLY at the default settings; an alignment chosen for this plug-in. It does not change how hard the "
              "card works.",
              64, 320.0f),
      mix (p.apvts, ParamID::mix, "Mix",
           "Blends the dry input with the processed signal: 100 % is the card alone, 0 % the dry input only. The dry signal is delayed "
           "by the oversampling latency (36 samples, reported to the host) so the two line up. The card has no mix control; this is a "
           "plug-in addition.",
           64),
      circuitRow (*p.apvts.getParameter (ParamID::circuit), "Circuit", { "Early", "1984" },
                  "EARLY: the CF82B33 drawing. 1984: the T82033.71 rev 6 sheet of the SSL 4000 G manual. They differ mainly in C17 "
                  "(100 pF against 4.7 nF): 1984 distorts far less (about 1 % against 19 % THD at 10 dB of reduction) and releases "
                  "sooner. A switch crosses over in 60 ms."),
      stereoRow (*p.apvts.getParameter (ParamID::stereo), "Stereo", { "Dual", "Link", "M/S" },
                 "DUAL: two independent cards. LINK: the two cards' detector capacitors (C14) are tied, so the louder side sets both; a "
                 "software extension, by analogy with the 1176SA tie, since the card itself is mono. M/S: the cards see mid and side, "
                 "M = (L + R) / 2 and S = (L " + minusSign() + " R) / 2. Mono tracks ignore this.")
{
    const std::pair<juce::Component*, const char*> controls[] {
        { &input, ParamID::input }, { &bias, ParamID::bias }, { &output, ParamID::output }, { &mix, ParamID::mix },
        { &circuitRow, ParamID::circuit }, { &stereoRow, ParamID::stereo }
    };
    for (const auto& [component, id] : controls)
    {
        component->setComponentID (id);
        addAndMakeVisible (component);
    }
    reduction.setComponentID ("meter");
    addAndMakeVisible (reduction);

    showHelpFor (nullptr);
    updateMeters();
    setSize (kWidth, kHeight);
}

juce::Component* EditorView::controlFor (const juce::String& id)
{
    for (auto* child : getChildren())
        if (child->getComponentID() == id)
            return child;
    return nullptr;
}

void EditorView::updateMeters()
{
    reduction.push (processor.getGainReductionDb());
    const int circuit = (int) processor.getRunningCircuit();
    if (circuit != shownCircuit)
    {
        shownCircuit = circuit;
        repaint (0, 0, kWidth, 50);
    }
}

std::pair<juce::String, juce::String> EditorView::defaultHelp()
{
    return { juce::String (kDefaultHelpTitle).toUpperCase(), kDefaultHelp };
}

std::pair<juce::String, juce::String> EditorView::helpFor (juce::Component* component, const juce::Component* stopAt)
{
    // The help belongs to the nearest ancestor that has any (a knob's slider has none itself).
    for (auto* c = component; c != nullptr && c != stopAt; c = c->getParentComponent())
        if (c->getProperties().contains (helpId))
            return { c->getProperties()[helpTitleId].toString(), c->getProperties()[helpId].toString() };
    return defaultHelp();
}

int EditorView::helpLines (const juce::String& body)
{
    juce::AttributedString s;
    s.append (body, Theme::bodyFont(), juce::Colours::black);
    juce::TextLayout layout;
    layout.createLayout (s, (float) (kWidth - 48));
    return layout.getNumLines();
}

void EditorView::showHelpFor (juce::Component* component)
{
    const auto [title, text] = helpFor (component, this);
    if (title != helpTitle || text != helpText)
    {
        helpTitle = title;
        helpText = text;
        repaint (helpArea);
    }
}

void EditorView::showHelpForParameter (const juce::String& paramId)
{
    if (auto* c = controlFor (paramId.isEmpty() ? juce::String ("meter") : paramId))
        showHelpFor (c);
}

void EditorView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    // Title: the product, and quieter, the circuit that is running.
    const auto title = Theme::titleFont (15.0f), subtitle = Theme::titleFont (11.0f);
    const float baseline = 33.0f;
    const juce::String name = "CLASSIC TALKBACK COMPRESSOR";
    g.setColour (Theme::pale);
    g.setFont (title);
    g.drawSingleLineText (name, 24, (int) baseline);
    g.setColour (Theme::label);
    g.setFont (subtitle);
    g.drawSingleLineText (shownCircuit == (int) Circuit::early ? "CF82B33 " + middleDot() + " EARLY" : "T82033.71 REV 6A " + middleDot() + " 1984",
                          24 + Theme::widthOf (title, name) + 12, (int) baseline);

    // Help, bottom left: what the control under the mouse does.
    auto help = helpArea;
    g.setColour (Theme::pale);
    g.setFont (Theme::labelFont());
    g.drawText (helpTitle, help.removeFromTop (16), juce::Justification::topLeft, false);
    g.setColour (Theme::label);
    g.setFont (Theme::bodyFont());
    g.drawFittedText (helpText, help, juce::Justification::topLeft, 3, 1.0f);
}

void EditorView::resized()
{
    constexpr int margin = 24, header = 50, knobCell = 104, meterWidth = 200, meterHeight = 104, laneToKnobs = 4, meterGap = 24;

    auto area = getLocalBounds().reduced (margin, 0);
    area.removeFromTop (header);

    // Bottom: what the control under the mouse does.
    area.removeFromBottom (10);
    helpArea = area.removeFromBottom (54);

    auto unit = area;
    auto knobArea = unit.removeFromLeft (4 * knobCell);
    const int laneHeight = kRowHeight, groupGap = 24;

    // One option row at the left margin, under the title: the Circuit group, then the Stereo group after the guide's 24 px.
    circuitRow.setBounds (knobArea.getX(), knobArea.getY(), circuitRow.preferredWidth(), kRowHeight);
    stereoRow.setBounds (circuitRow.getRight() + groupGap, knobArea.getY(), stereoRow.preferredWidth(), kRowHeight);

    knobArea.removeFromTop (laneHeight + laneToKnobs);
    const int bankHeight = bias.preferredHeight();
    auto bank = knobArea.removeFromTop (bankHeight);
    for (auto* knob : { &input, &bias, &output, &mix })
        knob->setBounds (bank.removeFromLeft (knobCell).withSizeKeepingCentre (knobCell, knob->preferredHeight()));

    // The needle sits right of the bank, centred vertically on the rows and the bank together.
    unit.removeFromLeft (meterGap);
    const int totalHeight = laneHeight + laneToKnobs + bankHeight;
    reduction.setBounds (unit.removeFromLeft (meterWidth).withHeight (totalHeight).withSizeKeepingCentre (meterWidth, meterHeight));
}

//==============================================================================
ClassicTalkbackEditor::ClassicTalkbackEditor (ClassicTalkbackProcessor& p)
    : AudioProcessorEditor (p), owner (p), view (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (view);

    // Scales as a whole, 0.75x to 2x, keeping the design's aspect ratio.
    constexpr int w = EditorView::kWidth, h = EditorView::kHeight;
    setResizable (true, true);
    setResizeLimits (w * 3 / 4, h * 3 / 4, w * 2, h * 2);
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);

    const double scale = juce::jlimit (0.75, 2.0, (double) p.apvts.state.getProperty (editorScaleId, 1.0));
    setSize (juce::roundToInt (w * scale), juce::roundToInt (h * scale));
    startTimerHz (GrMeter::kRateHz);
}

ClassicTalkbackEditor::~ClassicTalkbackEditor()
{
    setLookAndFeel (nullptr);
}

void ClassicTalkbackEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
}

void ClassicTalkbackEditor::resized()
{
    const float scale = (float) getWidth() / (float) EditorView::kWidth;
    view.setTransform (juce::AffineTransform::scale (scale));
    view.setBounds (0, 0, EditorView::kWidth, EditorView::kHeight);

    // Remember a size the user chose (a window on screen), not the ones a host passes through while opening or closing it.
    if (isShowing() && getPeer() != nullptr)
        owner.apvts.state.setProperty (editorScaleId, (double) scale, nullptr);
}

void ClassicTalkbackEditor::timerCallback()
{
    view.updateMeters();

    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    view.showHelpFor (under != nullptr && view.isParentOf (under) ? under : nullptr);
}

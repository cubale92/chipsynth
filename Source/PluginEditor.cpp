#include "PluginEditor.h"

namespace col
{
    const juce::Colour bg       { 0xffd6d5de };
    const juce::Colour card     { 0xffe9e8ef };
    const juce::Colour edge     { 0xffb6b4c4 };
    const juce::Colour dark     { 0xff1e1b26 };
    const juce::Colour darkEdge { 0xff3a3548 };
    const juce::Colour ink      { 0xff2b2836 };
    const juce::Colour purple   { 0xff4b3f8c };
    const juce::Colour lavender { 0xffb8aef0 };
    const juce::Colour red      { 0xffd0453f };
    const juce::Colour yellow   { 0xffd9a62a };
    const juce::Colour green    { 0xff3e9e5a };
    const juce::Colour blue     { 0xff3f63c7 };
}

//==============================================================================
SfcLookAndFeel::SfcLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, col::ink);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, col::ink);
    setColour (juce::TextButton::textColourOffId, col::ink);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::ComboBox::backgroundColourId, col::card);
    setColour (juce::ComboBox::outlineColourId, col::edge);
    setColour (juce::ComboBox::textColourId, col::ink);
    setColour (juce::ComboBox::arrowColourId, col::purple);
    setColour (juce::PopupMenu::backgroundColourId, col::card);
    setColour (juce::PopupMenu::textColourId, col::ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, col::purple);
    setColour (juce::ListBox::backgroundColourId, col::dark);
    setColour (juce::ScrollBar::thumbColourId, col::lavender.withAlpha (0.5f));
}

void SfcLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                       float startAngle, float endAngle, juce::Slider& s)
{
    const auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float r = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto c = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);

    // track
    juce::Path track;
    track.addCentredArc (c.x, c.y, r - 2, r - 2, 0, startAngle, endAngle, true);
    g.setColour (col::edge);
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path val;
    val.addCentredArc (c.x, c.y, r - 2, r - 2, 0, startAngle, angle, true);
    g.setColour (accent);
    g.strokePath (val, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // knob body
    const float kr = r - 7;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4558), c.x, c.y - kr,
                                             juce::Colour (0xff24212d), c.x, c.y + kr, false));
    g.fillEllipse (c.x - kr, c.y - kr, kr * 2, kr * 2);
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.drawEllipse (c.x - kr, c.y - kr, kr * 2, kr * 2, 1.0f);

    juce::Path ptr;
    ptr.addRoundedRectangle (-1.5f, -kr + 3, 3.0f, kr * 0.55f, 1.5f);
    g.setColour (juce::Colours::white);
    g.fillPath (ptr, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
}

void SfcLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                           bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    auto fill = on ? col::purple : col::card;
    if (highlighted) fill = fill.brighter (0.08f);
    if (down) fill = fill.darker (0.15f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (on ? col::purple.darker (0.3f) : col::edge);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
}

void SfcLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat();
    const auto accent = b.findColour (juce::ToggleButton::tickColourId);
    auto led = r.removeFromLeft (18.0f).withSizeKeepingCentre (11.0f, 11.0f);
    g.setColour (b.getToggleState() ? accent : col::edge.darker (highlighted ? 0.1f : 0.0f));
    g.fillEllipse (led);
    g.setColour (col::ink.withAlpha (0.4f));
    g.drawEllipse (led, 1.0f);
    g.setColour (col::ink);
    g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    g.drawText (b.getButtonText(), r.withTrimmedLeft (4.0f), juce::Justification::centredLeft);
}

juce::Font SfcLookAndFeel::getTextButtonFont (juce::TextButton&, int h)
{
    return juce::Font (juce::FontOptions (juce::jmin (13.0f, (float) h * 0.45f), juce::Font::bold));
}

//==============================================================================
void WaveformView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (col::dark);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (col::darkEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

    auto area = r.reduced (10.0f, 12.0f);
    g.setColour (col::lavender.withAlpha (0.12f));
    g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

    if (sample == nullptr || sample->pcm.empty())
    {
        g.setColour (col::lavender.withAlpha (0.6f));
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText ("Drop a .spc / .sfc / .brr here or press LOAD", r, juce::Justification::centred);
        return;
    }

    const auto& pcm = sample->pcm;
    const int len = (int) pcm.size();
    const int W = (int) area.getWidth();

    if (sample->loops())
    {
        const float lx = area.getX() + area.getWidth() * (float) sample->loopStart / (float) len;
        g.setColour (col::lavender.withAlpha (0.10f));
        g.fillRect (juce::Rectangle<float> (lx, area.getY(), area.getRight() - lx, area.getHeight()));
        g.setColour (col::yellow);
        g.drawVerticalLine ((int) lx, area.getY(), area.getBottom());
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText ("LOOP", (int) lx + 4, (int) area.getY(), 40, 12, juce::Justification::left);
    }

    juce::Path p;
    const float mid = area.getCentreY(), half = area.getHeight() / 2.0f;
    for (int x = 0; x < W; ++x)
    {
        const int a = (int) ((int64_t) x * len / W);
        const int b = juce::jmax (a + 1, (int) ((int64_t) (x + 1) * len / W));
        int lo = 32767, hi = -32768;
        for (int i = a; i < b && i < len; ++i) { lo = juce::jmin (lo, (int) pcm[(size_t) i]); hi = juce::jmax (hi, (int) pcm[(size_t) i]); }
        const float px = area.getX() + (float) x;
        p.addLineSegment ({ px, mid - half * (float) hi / 32768.0f, px, mid - half * (float) lo / 32768.0f + 0.5f }, 1.0f);
    }
    g.setColour (col::lavender);
    g.fillPath (p);

    g.setColour (col::lavender.withAlpha (0.7f));
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    const double secs = len / 32000.0;
    g.drawText (juce::String (len) + " samples @ 32 kHz  (" + juce::String (secs, 2) + " s)"
                    + (sample->loops() ? "   loop " + juce::String (sample->loopStart) + "-" + juce::String (len) : "   one-shot"),
                r.reduced (12.0f, 6.0f), juce::Justification::bottomRight);
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId,
            const juce::String& label, juce::Colour a)
    : text (label), accent (a)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    addAndMakeVisible (slider);
    att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, paramId, slider);
}

void Knob::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (16));
}

void Knob::paint (juce::Graphics& g)
{
    g.setColour (col::ink);
    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    g.drawText (text, getLocalBounds().removeFromTop (16), juce::Justification::centred);
}

//==============================================================================
SfcEditor::SfcEditor (SfcProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (loadButton);
    loadButton.onClick = [this] { openFileChooser(); };

    for (int i = 0; i < 8; ++i)
    {
        auto& b = trackButtons[(size_t) i];
        b.setClickingTogglesState (false);
        b.onClick = [this, i] { proc.selectTrack (i); };
        addAndMakeVisible (b);
    }

    sampleList.setRowHeight (20);
    sampleList.setColour (juce::ListBox::outlineColourId, col::darkEdge);
    sampleList.setOutlineThickness (1);
    addAndMakeVisible (sampleList);
    addAndMakeVisible (waveform);

    fileLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    voicesLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    voicesLabel.setJustificationType (juce::Justification::centredRight);
    for (auto* l : { &fileLabel, &statusLabel, &voicesLabel }) addAndMakeVisible (*l);

    auto addKnob = [this] (const char* id, const char* label, juce::Colour c)
    {
        knobs.push_back (std::make_unique<Knob> (proc.apvts, id, label, c));
        addAndMakeVisible (*knobs.back());
    };
    // row 1: tone + envelope
    addKnob ("root", "ROOT", col::red);
    addKnob ("fine", "FINE", col::red);
    addKnob ("bend", "BEND", col::red);
    addKnob ("vibrato", "VIBRATO", col::red);
    addKnob ("attack", "AR", col::yellow);
    addKnob ("decay", "DR", col::yellow);
    addKnob ("sustainLevel", "SL", col::yellow);
    addKnob ("sustainRate", "SR", col::yellow);
    addKnob ("release", "RELEASE", col::yellow);
    // row 2: echo + play
    addKnob ("echoDelay", "EDL", col::green);
    addKnob ("echoFeedback", "EFB", col::green);
    addKnob ("echoVolume", "EVOL", col::green);
    addKnob ("velocity", "VELOCITY", col::blue);
    addKnob ("glideTime", "GLIDE", col::blue);
    addKnob ("pan", "PAN", col::blue);
    addKnob ("volume", "VOLUME", col::blue);

    // root knob shows note names
    knobs[0]->slider.textFromValueFunction = [] (double v)
    { return juce::MidiMessage::getMidiNoteName ((int) v, true, true, 4); };
    knobs[0]->slider.updateText();

    // velocity as %, pan as L/C/R
    knobs[12]->slider.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    knobs[12]->slider.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue() / 100.0; };
    knobs[14]->slider.textFromValueFunction = [] (double v)
    {
        const int p = juce::roundToInt (v * 100.0);
        return p == 0 ? juce::String ("C") : (p < 0 ? "L" + juce::String (-p) : "R" + juce::String (p));
    };
    knobs[14]->slider.valueFromTextFunction = [] (const juce::String& t)
    {
        const double n = t.retainCharacters ("0123456789.").getDoubleValue() / 100.0;
        return t.startsWithIgnoreCase ("L") ? -n : n;
    };
    for (int i : { 12, 14 }) knobs[(size_t) i]->slider.updateText();

    polyBox.addItemList (proc.apvts.getParameter ("poly")->getAllValueStrings(), 1);
    firBox.addItemList (SfcProcessor::firNames, 1);
    addAndMakeVisible (polyBox);
    addAndMakeVisible (firBox);
    polyAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "poly", polyBox);
    firAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "fir", firBox);

    echoToggle.setColour (juce::ToggleButton::tickColourId, col::green);
    limitToggle.setColour (juce::ToggleButton::tickColourId, col::blue);
    glideToggle.setColour (juce::ToggleButton::tickColourId, col::blue);
    for (auto* t : { &echoToggle, &limitToggle, &glideToggle }) addAndMakeVisible (*t);
    echoAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "echo", echoToggle);
    limitAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "pitchLimit", limitToggle);
    glideAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "glide", glideToggle);

    keyboard.setAvailableRange (24, 108);
    keyboard.setLowestVisibleKey (36);
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, col::purple.withAlpha (0.7f));
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, col::lavender.withAlpha (0.4f));
    addAndMakeVisible (keyboard);

    proc.addChangeListener (this);
    refreshFromProcessor();
    startTimerHz (10);
    setSize (1060, 650);
}

SfcEditor::~SfcEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
void SfcEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);

    // header bar
    auto header = getLocalBounds().removeFromTop (56);
    g.setColour (col::dark);
    g.fillRect (header);
    g.setColour (col::lavender);
    g.setFont (juce::Font (juce::FontOptions (28.0f, juce::Font::bold)));
    g.drawText ("SFC-8", 16, 6, 110, 44, juce::Justification::centredLeft);
    g.setColour (col::lavender.withAlpha (0.6f));
    g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    g.drawText ("8CEE  /  S-DSP SAMPLE SYNTH", 112, 6, 200, 44, juce::Justification::centredLeft);

    // four colour dots
    const juce::Colour dots[] = { col::red, col::yellow, col::green, col::blue };
    for (int i = 0; i < 4; ++i)
    {
        g.setColour (dots[i]);
        g.fillEllipse ((float) getWidth() - 96.0f + (float) i * 20.0f, 21.0f, 13.0f, 13.0f);
    }

    // left column labels
    g.setColour (col::purple);
    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    const bool spc = proc.getLoadResult().kind == snes::LoadResult::Kind::Spc;
    g.drawText (spc ? "TRACKS  (SPC VOICES 1-8)" : "TRACKS  1-8", 14, 64, 240, 16, juce::Justification::centredLeft);
    g.drawText ("SAMPLES  (" + juce::String ((int) proc.getLoadResult().samples.size()) + ")  -> CURRENT TRACK",
                14, sampleList.getY() - 18, 240, 16, juce::Justification::centredLeft);

    // section cards
    auto drawSection = [&] (juce::Rectangle<int> r, const juce::String& name, juce::Colour c)
    {
        g.setColour (col::card);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
        g.setColour (col::edge);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 8.0f, 1.0f);
        g.setColour (c);
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) r.getX() + 10.0f, (float) r.getY() + 8.0f, 4.0f, 12.0f), 2.0f);
        g.setColour (col::ink);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (name, r.getX() + 20, r.getY() + 6, 200, 16, juce::Justification::centredLeft);
    };
    drawSection (secTone, "TONE", col::red);
    drawSection (secEnv,  "ENVELOPE  (S-DSP ADSR)", col::yellow);
    drawSection (secEcho, "ECHO  (8-TAP FIR)", col::green);
    drawSection (secPlay, "PLAY", col::blue);
}

void SfcEditor::resized()
{
    loadButton.setBounds (330, 13, 80, 30);
    fileLabel.setBounds (420, 8, 400, 22);
    fileLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    voicesLabel.setBounds (getWidth() - 260, 30, 150, 20);
    voicesLabel.setColour (juce::Label::textColourId, col::lavender.withAlpha (0.7f));

    // left column
    for (int i = 0; i < 8; ++i)
        trackButtons[(size_t) i].setBounds (12 + (i % 2) * 122, 84 + (i / 2) * 38, 118, 34);
    sampleList.setBounds (12, 260, 240, 220);

    // right column
    const int rx = 266, rw = getWidth() - rx - 12;
    waveform.setBounds (rx, 64, rw, 150);
    statusLabel.setBounds (rx, 216, rw, 20);

    const int row1 = 240, row2 = 364, rowH = 118;
    const int knobW = 66, knobH = 92;

    secTone = { rx, row1, 4 * knobW + 24, rowH };
    secEnv  = { secTone.getRight() + 8, row1, rw - secTone.getWidth() - 8, rowH };
    secEcho = { rx, row2, 3 * knobW + 148, rowH };
    secPlay = { secEcho.getRight() + 8, row2, rw - secEcho.getWidth() - 8, rowH };

    auto place = [&] (int first, int count, juce::Rectangle<int> sec, int leftPad)
    {
        int x = sec.getX() + leftPad;
        for (int i = first; i < first + count; ++i)
        {
            knobs[(size_t) i]->setBounds (x, sec.getY() + 22, knobW, knobH);
            x += knobW + 4;
        }
        return x;
    };

    place (0, 4, secTone, 10);
    {
        const int gap = (secEnv.getWidth() - 20 - 5 * knobW) / 4;
        int x = secEnv.getX() + 10;
        for (int i = 4; i < 9; ++i) { knobs[(size_t) i]->setBounds (x, secEnv.getY() + 22, knobW, knobH); x += knobW + gap; }
    }

    // echo: toggle + FIR column, then knobs
    echoToggle.setBounds (secEcho.getX() + 12, secEcho.getY() + 34, 110, 22);
    firBox.setBounds (secEcho.getX() + 12, secEcho.getY() + 66, 116, 24);
    place (9, 3, secEcho, 136);

    // play: poly + limit column, then knobs
    polyBox.setBounds (secPlay.getX() + 12, secPlay.getY() + 30, 120, 24);
    limitToggle.setBounds (secPlay.getX() + 12, secPlay.getY() + 60, 120, 22);
    glideToggle.setBounds (secPlay.getX() + 12, secPlay.getY() + 84, 120, 22);
    {
        const int start = secPlay.getX() + 140;
        const int avail = secPlay.getRight() - 8 - start;
        const int gap = juce::jmax (0, (avail - 4 * knobW) / 3);
        int x = start;
        for (int i = 12; i < 16; ++i) { knobs[(size_t) i]->setBounds (x, secPlay.getY() + 22, knobW, knobH); x += knobW + gap; }
    }

    keyboard.setBounds (12, 492, getWidth() - 24, getHeight() - 492 - 12);
    keyboard.setKeyWidth ((float) (getWidth() - 24) / 43.0f);
}

//==============================================================================
void SfcEditor::refreshFromProcessor()
{
    const auto& r = proc.getLoadResult();
    const bool spc = r.kind == snes::LoadResult::Kind::Spc;

    juce::String title = proc.getFileName();
    if (! r.gameTitle.empty()) title << "   " << juce::String (r.gameTitle);
    if (! r.songTitle.empty()) title << "  -  " << juce::String (r.songTitle);
    fileLabel.setText (title.isEmpty() ? "No file loaded" : title, juce::dontSendNotification);
    statusLabel.setText (proc.getStatusText(), juce::dontSendNotification);

    const bool loaded = ! r.samples.empty();
    for (int i = 0; i < 8; ++i)
    {
        auto& b = trackButtons[(size_t) i];
        const auto& vi = r.voices[(size_t) i];
        const int idx = proc.getTrackSample (i);
        juce::String t = juce::String (i + 1);
        juce::String tip;
        if (idx >= 0 && idx < (int) r.samples.size())
        {
            const auto& s = *r.samples[(size_t) idx];
            if (s.srcn >= 0) t << "  SRC " << juce::String::toHexString (s.srcn).toUpperCase().paddedLeft ('0', 2);
            else             t << "  #" << juce::String (idx + 1);
            if (spc && vi.valid && vi.sampleIndex == idx)
            {
                if (! vi.looksActive) t << "  -";
                tip = vi.looksActive ? "Voice was playing in the snapshot" : "Voice looked silent in the snapshot";
            }
            else
            {
                tip = "Click to play. Then pick any sample in the list to put it on this track.";
            }
        }
        else if (loaded)
        {
            t << "  empty";
            tip = "Select this track, then pick a sample in the list";
        }
        b.setButtonText (t);
        b.setEnabled (loaded);
        b.setToggleState (loaded && proc.getSelectedTrack() == i, juce::dontSendNotification);
        b.setTooltip (tip);
    }

    updatingList = true;
    sampleList.updateContent();
    if (proc.getSelectedListIndex() >= 0)
    {
        sampleList.selectRow (proc.getSelectedListIndex(), true, true);
        sampleList.scrollToEnsureRowIsOnscreen (proc.getSelectedListIndex());
    }
    else
    {
        sampleList.deselectAllRows();
    }
    updatingList = false;

    waveform.setSample (proc.getCurrentSample());
    repaint();
}

void SfcEditor::changeListenerCallback (juce::ChangeBroadcaster*) { refreshFromProcessor(); }

void SfcEditor::timerCallback()
{
    voicesLabel.setText ("voices " + juce::String (proc.getActiveVoices()), juce::dontSendNotification);
}

int SfcEditor::getNumRows() { return (int) proc.getLoadResult().samples.size(); }

void SfcEditor::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    const auto& s = proc.getLoadResult().samples;
    if (row < 0 || row >= (int) s.size()) return;
    if (selected)
    {
        g.setColour (col::purple);
        g.fillRect (0, 0, w, h);
    }
    // mark samples used by the 8 voices
    bool used = false;
    for (auto& vi : proc.getLoadResult().voices) used |= (vi.valid && vi.sampleIndex == row);
    g.setColour (used ? col::yellow : col::lavender.withAlpha (selected ? 1.0f : 0.8f));
    g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));
    g.drawText (juce::String (s[(size_t) row]->name), 8, 0, w - 10, h, juce::Justification::centredLeft);
}

void SfcEditor::selectedRowsChanged (int lastRow)
{
    if (updatingList || lastRow < 0) return;
    if (lastRow != proc.getSelectedListIndex())
        proc.selectSample (lastRow);
}

//==============================================================================
bool SfcEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (f.endsWithIgnoreCase (".spc") || f.endsWithIgnoreCase (".sfc") || f.endsWithIgnoreCase (".smc")
            || f.endsWithIgnoreCase (".brr") || f.endsWithIgnoreCase (".fig") || f.endsWithIgnoreCase (".swc"))
            return true;
    return false;
}

void SfcEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (! files.isEmpty()) proc.loadFile (juce::File (files[0]));
}

void SfcEditor::openFileChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Load SNES file", juce::File(), "*.spc;*.sfc;*.smc;*.brr;*.fig;*.swc");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) proc.loadFile (f);
                          });
}

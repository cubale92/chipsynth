#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

//==============================================================================
class SfcLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SfcLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& bg,
                               bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
};

//==============================================================================
class WaveformView : public juce::Component
{
public:
    void setSample (snes::SamplePtr s) { sample = std::move (s); repaint(); }
    void paint (juce::Graphics&) override;
private:
    snes::SamplePtr sample;
};

//==============================================================================
class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId,
          const juce::String& label, juce::Colour accent);
    void resized() override;
    void paint (juce::Graphics&) override;
    juce::Slider slider;
private:
    juce::String text;
    juce::Colour accent;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
};

//==============================================================================
class SfcEditor : public juce::AudioProcessorEditor,
                  public juce::FileDragAndDropTarget,
                  private juce::ChangeListener,
                  private juce::ListBoxModel,
                  private juce::Timer
{
public:
    explicit SfcEditor (SfcProcessor&);
    ~SfcEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    int  getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int lastRow) override;

    void refreshFromProcessor();
    void openFileChooser();

    SfcProcessor& proc;
    SfcLookAndFeel lnf;

    juce::TextButton loadButton { "LOAD" };
    std::array<juce::TextButton, 8> trackButtons;
    juce::ListBox sampleList { "samples", this };
    WaveformView waveform;
    juce::Label fileLabel, statusLabel, voicesLabel;

    std::vector<std::unique_ptr<Knob>> knobs;
    juce::ComboBox polyBox, firBox;
    juce::ToggleButton echoToggle { "ECHO" }, limitToggle { "PITCH LIMIT" }, glideToggle { "GLIDE" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> polyAtt, firAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> echoAtt, limitAtt, glideAtt;

    juce::MidiKeyboardComponent keyboard;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 500 };
    bool updatingList = false;

    juce::Rectangle<int> secTone, secEnv, secEcho, secPlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SfcEditor)
};

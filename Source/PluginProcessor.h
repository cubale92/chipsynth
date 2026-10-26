#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <deque>
#include "Snes/Engine.h"
#include "Snes/Loaders.h"

class SfcProcessor : public juce::AudioProcessor,
                     public juce::ChangeBroadcaster
{
public:
    SfcProcessor();
    ~SfcProcessor() override = default;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Message-thread API used by the editor
    bool loadFile (const juce::File& f, bool autoSelect = true);
    void selectTrack (int track);          // 0..7: play that track's sample
    void selectSample (int listIndex);     // assign a list entry to the current track and play it

    const snes::LoadResult& getLoadResult() const { return load; }
    juce::String getFileName() const       { return currentFile.getFileName(); }
    juce::String getStatusText() const     { return status; }
    int getSelectedTrack() const           { return currentTrack; }
    int getTrackSample (int t) const       { return trackSample[(size_t) t]; }
    int getSelectedListIndex() const       { return trackSample[(size_t) currentTrack]; }
    snes::SamplePtr getCurrentSample() const { return std::atomic_load (&published); }
    int getActiveVoices() const            { return activeVoices.load(); }

    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;

    static const juce::StringArray firNames;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void publish (std::shared_ptr<snes::Sample> s, bool applyHints, int voice);
    snes::EngineParams readParams() const;
    void setParam (const juce::String& id, float plainValue);

    snes::Engine engine;
    snes::LoadResult load;
    juce::File currentFile;
    juce::String status { "Drop a .spc, .sfc/.smc or .brr file" };
    int currentTrack = 0;
    std::array<int, 8> trackSample { -1, -1, -1, -1, -1, -1, -1, -1 };  // list index per track
    void publishTrack();

    snes::SamplePtr published;                // read with std::atomic_load
    std::deque<snes::SamplePtr> keepAlive;    // freed on the message thread only
    std::array<std::atomic<int>, 8> fileFir;
    std::atomic<int> activeVoices { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SfcProcessor)
};

#include "PluginProcessor.h"
#include "PluginEditor.h"

const juce::StringArray SfcProcessor::firNames { "From file", "Flat", "Lowpass", "Mellow", "Band" };

namespace
{
    const std::array<std::array<int, 8>, 4> kFirPresets {{
        {{ 127, 0, 0, 0, 0, 0, 0, 0 }},
        {{ -1, 8, 23, 36, 36, 23, 8, -1 }},
        {{ 12, 33, 43, 43, 19, -2, -13, -7 }},
        {{ 52, 51, 0, -39, -27, 1, -4, -21 }},
    }};

    juce::String noteName (float midi)
    {
        return juce::MidiMessage::getMidiNoteName (juce::roundToInt (midi), true, true, 4);
    }
}

SfcProcessor::SfcProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    for (auto& f : fileFir) f.store (0);
    fileFir[0].store (127);
}

juce::AudioProcessorValueTreeState::ParameterLayout SfcProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    p.push_back (std::make_unique<AudioParameterInt>   (ParameterID { "root", 1 }, "Root Note", 0, 127, 60));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "fine", 1 }, "Fine Tune",
                                                        NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f,
                                                        AudioParameterFloatAttributes().withLabel ("ct")));
    p.push_back (std::make_unique<AudioParameterInt>   (ParameterID { "attack", 1 }, "Attack (AR)", 0, 15, 15));
    p.push_back (std::make_unique<AudioParameterInt>   (ParameterID { "decay", 1 }, "Decay (DR)", 0, 7, 7));
    p.push_back (std::make_unique<AudioParameterInt>   (ParameterID { "sustainLevel", 1 }, "Sustain Level (SL)", 0, 7, 7));
    p.push_back (std::make_unique<AudioParameterInt>   (ParameterID { "sustainRate", 1 }, "Sustain Rate (SR)", 0, 31, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "release", 1 }, "Release",
                                                        NormalisableRange<float> (0.0f, 5000.0f, 1.0f, 0.35f), 250.0f,
                                                        AudioParameterFloatAttributes().withLabel ("ms")));
    p.push_back (std::make_unique<AudioParameterChoice>(ParameterID { "poly", 1 }, "Polyphony",
                                                        StringArray { "8 (hardware)", "16", "32" }, 1));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "pitchLimit", 1 }, "Hardware Pitch Limit", true));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "echo", 1 }, "Echo", false));
    p.push_back (std::make_unique<AudioParameterInt>   (ParameterID { "echoDelay", 1 }, "Echo Delay (EDL)", 0, 15, 4));
    p.push_back (std::make_unique<AudioParameterInt>   (ParameterID { "echoFeedback", 1 }, "Echo Feedback (EFB)", -128, 127, 64));
    p.push_back (std::make_unique<AudioParameterInt>   (ParameterID { "echoVolume", 1 }, "Echo Volume (EVOL)", -128, 127, 48));
    p.push_back (std::make_unique<AudioParameterChoice>(ParameterID { "fir", 1 }, "Echo FIR", firNames, 1));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "velocity", 1 }, "Velocity Sens",
                                                        NormalisableRange<float> (0.0f, 1.0f), 0.7f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "vibrato", 1 }, "Mod Wheel Vibrato",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 30.0f,
                                                        AudioParameterFloatAttributes().withLabel ("ct")));
    p.push_back (std::make_unique<AudioParameterInt>   (ParameterID { "bend", 1 }, "Bend Range", 0, 24, 2));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "glide", 1 }, "Glide", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "glideTime", 1 }, "Glide Time",
                                                        NormalisableRange<float> (1.0f, 1000.0f, 1.0f, 0.4f), 80.0f,
                                                        AudioParameterFloatAttributes().withLabel ("ms")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "pan", 1 }, "Pan",
                                                        NormalisableRange<float> (-1.0f, 1.0f), 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "volume", 1 }, "Volume",
                                                        NormalisableRange<float> (-36.0f, 12.0f, 0.1f), -3.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));
    return { p.begin(), p.end() };
}

//==============================================================================
bool SfcProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void SfcProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
    keyboardState.reset();
}

snes::EngineParams SfcProcessor::readParams() const
{
    auto v = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    snes::EngineParams p;
    p.rootNote     = v ("root");
    p.fineCents    = v ("fine");
    p.a            = (int) v ("attack");
    p.d            = (int) v ("decay");
    p.sl           = (int) v ("sustainLevel");
    p.sr           = (int) v ("sustainRate");
    p.releaseMs    = v ("release");
    const int poly = (int) v ("poly");
    p.polyphony    = poly == 0 ? 8 : (poly == 1 ? 16 : 32);
    p.hwPitchLimit = v ("pitchLimit") > 0.5f;
    p.echoOn       = v ("echo") > 0.5f;
    p.echoDelay    = (int) v ("echoDelay");
    p.echoFeedback = (int) v ("echoFeedback");
    p.echoVolume   = (int) v ("echoVolume");
    const int fir  = (int) v ("fir");
    if (fir == 0) for (int i = 0; i < 8; ++i) p.fir[(size_t) i] = fileFir[(size_t) i].load();
    else          p.fir = kFirPresets[(size_t) (fir - 1)];
    p.velocitySens = v ("velocity");
    p.vibratoCents = v ("vibrato");
    p.bendRange    = v ("bend");
    p.glide        = v ("glide") > 0.5f;
    p.glideMs      = v ("glideTime");
    p.pan          = v ("pan");
    p.gain         = juce::Decibels::decibelsToGain (v ("volume"), -36.0f);
    return p;
}

void SfcProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    keyboardState.processNextMidiBuffer (midi, 0, n, true);

    auto current = std::atomic_load (&published);
    engine.setSample (current.get());
    engine.setParams (readParams());

    buffer.clear();
    const int numCh = buffer.getNumChannels();
    float* L = buffer.getWritePointer (0);
    float* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    // mono output: render right into a scratch area on the stack in chunks
    float scratch[256];

    auto renderRange = [&] (int start, int end)
    {
        if (R != nullptr)
        {
            engine.render (L + start, R + start, end - start);
            return;
        }
        for (int pos = start; pos < end;)
        {
            const int cnt = std::min (256, end - pos);
            engine.render (L + pos, scratch, cnt);
            for (int i = 0; i < cnt; ++i) L[pos + i] = 0.5f * (L[pos + i] + scratch[i]);
            pos += cnt;
        }
    };

    int cursor = 0;
    for (const auto meta : midi)
    {
        const int pos = juce::jlimit (0, n, meta.samplePosition);
        if (pos > cursor) { renderRange (cursor, pos); cursor = pos; }

        const auto m = meta.getMessage();
        if (m.isNoteOn())                     engine.noteOn (m.getNoteNumber(), m.getVelocity());
        else if (m.isNoteOff())               engine.noteOff (m.getNoteNumber());
        else if (m.isPitchWheel())            engine.pitchBend (m.getPitchWheelValue());
        else if (m.isControllerOfType (1))    engine.modWheel ((float) m.getControllerValue() / 127.0f);
        else if (m.isSustainPedalOn())        engine.sustainPedal (true);
        else if (m.isSustainPedalOff())       engine.sustainPedal (false);
        else if (m.isAllNotesOff())           engine.allNotesOff (false);
        else if (m.isAllSoundOff())           engine.allNotesOff (true);
    }
    if (cursor < n) renderRange (cursor, n);

    activeVoices.store (engine.activeVoiceCount());
}

//==============================================================================
void SfcProcessor::setParam (const juce::String& id, float plainValue)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        p->endChangeGesture();
    }
}

void SfcProcessor::publish (std::shared_ptr<snes::Sample> s, bool applyHints, int voice)
{
    snes::prepareSample (*s);

    if (applyHints)
    {
        const float root = s->rootNote;
        const int rootInt = juce::roundToInt (root);
        setParam ("root", (float) juce::jlimit (0, 127, rootInt));
        setParam ("fine", juce::jlimit (-100.0f, 100.0f, (root - (float) rootInt) * 100.0f));

        if (s->hasEnvelopeHint)
        {
            setParam ("attack", (float) s->adsrA);
            setParam ("decay", (float) s->adsrD);
            setParam ("sustainLevel", (float) s->adsrSL);
            setParam ("sustainRate", (float) s->adsrSR);
        }

        if (load.kind == snes::LoadResult::Kind::Spc && voice >= 0)
        {
            const auto& vi = load.voices[(size_t) voice];
            const bool echo = load.echo.present && vi.echoOn;
            setParam ("echo", echo ? 1.0f : 0.0f);
            if (echo)
            {
                setParam ("echoDelay", (float) load.echo.delay);
                setParam ("echoFeedback", (float) load.echo.feedback);
                const int ev = (std::abs (load.echo.volL) + std::abs (load.echo.volR)) / 2;
                setParam ("echoVolume", (float) juce::jlimit (-128, 127, ev));
                setParam ("fir", 0.0f);
            }
        }
    }

    snes::SamplePtr sp = std::move (s);
    keepAlive.push_back (sp);
    while (keepAlive.size() > 4) keepAlive.pop_front();  // old ones die here, never on the audio thread
    std::atomic_store (&published, sp);

    status = juce::String (sp->name) + "   root " + noteName (sp->rootNote)
             + (sp->rootDetected ? "" : " (not detected - set ROOT by ear)");
    sendChangeMessage();
}

bool SfcProcessor::loadFile (const juce::File& f, bool autoSelect)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb))
    {
        status = "Could not read " + f.getFileName();
        sendChangeMessage();
        return false;
    }
    std::vector<uint8_t> bytes ((const uint8_t*) mb.getData(), (const uint8_t*) mb.getData() + mb.getSize());

    const auto ext = f.getFileExtension().toLowerCase();
    snes::LoadResult r;
    if (ext == ".spc")                                   r = snes::loadSpc (bytes);
    else if (ext == ".brr")                              r = snes::loadRawBrr (bytes);
    else if (ext == ".sfc" || ext == ".smc" || ext == ".fig" || ext == ".swc") r = snes::scanRom (bytes);
    else
    {
        // sniff
        r = snes::loadSpc (bytes);
        if (r.kind == snes::LoadResult::Kind::None) r = snes::scanRom (bytes);
    }

    if (r.samples.empty())
    {
        status = f.getFileName() + ": " + juce::String (r.error.empty() ? "nothing found" : r.error);
        sendChangeMessage();
        return false;
    }

    load = std::move (r);
    currentFile = f;
    trackSample.fill (-1);
    currentTrack = 0;

    if (load.kind == snes::LoadResult::Kind::Spc)
    {
        // the 8 tracks are the 8 hardware voices of the snapshot
        for (int v = 0; v < 8; ++v)
            if (load.voices[(size_t) v].valid) trackSample[(size_t) v] = load.voices[(size_t) v].sampleIndex;
        for (int i = 0; i < 8; ++i) fileFir[(size_t) i].store (load.echo.fir[(size_t) i]);

        currentTrack = -1;
        for (int v = 0; v < 8 && currentTrack < 0; ++v)
            if (load.voices[(size_t) v].valid && load.voices[(size_t) v].looksActive) currentTrack = v;
        if (currentTrack < 0) currentTrack = 0;
    }
    else
    {
        // ROM / BRR: pre-fill the 8 tracks with the first samples found; re-assign from the list
        for (int t = 0; t < 8 && t < (int) load.samples.size(); ++t) trackSample[(size_t) t] = t;
    }

    if (autoSelect) selectTrack (currentTrack);
    else            sendChangeMessage();
    return true;
}

void SfcProcessor::publishTrack()
{
    const int idx = trackSample[(size_t) currentTrack];
    if (idx < 0 || idx >= (int) load.samples.size())
    {
        status = "Track " + juce::String (currentTrack + 1) + " is empty - pick a sample from the list";
        sendChangeMessage();
        return;
    }

    // SPC: when the track still holds its original voice sample, apply that voice's ADSR/echo
    int voice = -1;
    if (load.kind == snes::LoadResult::Kind::Spc)
    {
        const auto& vi = load.voices[(size_t) currentTrack];
        if (vi.valid && vi.sampleIndex == idx) voice = currentTrack;
    }

    auto s = std::make_shared<snes::Sample> (*load.samples[(size_t) idx]);
    s->name = "Track " + std::to_string (currentTrack + 1) + " - " + s->name;
    publish (s, true, voice);
}

void SfcProcessor::selectTrack (int track)
{
    if (track < 0 || track > 7) return;
    currentTrack = track;
    publishTrack();
}

void SfcProcessor::selectSample (int listIndex)
{
    if (listIndex < 0 || listIndex >= (int) load.samples.size()) return;
    trackSample[(size_t) currentTrack] = listIndex;
    publishTrack();
}

//==============================================================================
void SfcProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    juce::ValueTree smp ("SAMPLE");

    if (auto s = std::atomic_load (&published))
    {
        juce::MemoryBlock pcm (s->pcm.data(), s->pcm.size() * sizeof (int16_t));
        smp.setProperty ("pcm", pcm.toBase64Encoding(), nullptr);
        smp.setProperty ("loopStart", s->loopStart, nullptr);
        smp.setProperty ("name", juce::String (s->name), nullptr);
        smp.setProperty ("rootNote", s->rootNote, nullptr);
        smp.setProperty ("rootDetected", s->rootDetected, nullptr);
    }
    smp.setProperty ("file", currentFile.getFullPathName(), nullptr);
    smp.setProperty ("track", currentTrack, nullptr);
    juce::String slots;
    for (int t : trackSample) slots << t << " ";
    smp.setProperty ("slots", slots.trim(), nullptr);
    juce::String fir;
    for (auto& c : fileFir) fir << c.load() << " ";
    smp.setProperty ("fileFir", fir.trim(), nullptr);

    state.removeChild (state.getChildWithName ("SAMPLE"), nullptr);
    state.appendChild (smp, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void SfcProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr) return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid()) return;

    auto smp = state.getChildWithName ("SAMPLE");
    state.removeChild (smp, nullptr);
    apvts.replaceState (state);

    if (! smp.isValid()) return;

    auto firTokens = juce::StringArray::fromTokens (smp.getProperty ("fileFir").toString(), " ", "");
    for (int i = 0; i < 8 && i < firTokens.size(); ++i) fileFir[(size_t) i].store (firTokens[i].getIntValue());

    // Reload the file list if the file is still there (keeps current params)
    const juce::File f (smp.getProperty ("file").toString());
    if (f.existsAsFile() && loadFile (f, false))
    {
        auto slotTokens = juce::StringArray::fromTokens (smp.getProperty ("slots").toString(), " ", "");
        for (int t = 0; t < 8 && t < slotTokens.size(); ++t)
            trackSample[(size_t) t] = juce::jlimit (-1, (int) load.samples.size() - 1, slotTokens[t].getIntValue());
    }
    currentTrack = juce::jlimit (0, 7, (int) smp.getProperty ("track", 0));

    // The sample itself is restored from the saved PCM so projects work without the file
    juce::MemoryBlock pcm;
    if (pcm.fromBase64Encoding (smp.getProperty ("pcm").toString()) && pcm.getSize() >= 2)
    {
        auto s = std::make_shared<snes::Sample>();
        s->pcm.resize (pcm.getSize() / sizeof (int16_t));
        std::memcpy (s->pcm.data(), pcm.getData(), s->pcm.size() * sizeof (int16_t));
        s->loopStart = smp.getProperty ("loopStart", -1);
        s->name = smp.getProperty ("name").toString().toStdString();
        s->rootNote = (float) (double) smp.getProperty ("rootNote", 60.0);
        s->rootDetected = smp.getProperty ("rootDetected", false);

        snes::SamplePtr sp = s;
        keepAlive.push_back (sp);
        while (keepAlive.size() > 4) keepAlive.pop_front();
        std::atomic_store (&published, sp);
        status = juce::String (s->name);
    }
    sendChangeMessage();
}

//==============================================================================
juce::AudioProcessorEditor* SfcProcessor::createEditor() { return new SfcEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SfcProcessor(); }

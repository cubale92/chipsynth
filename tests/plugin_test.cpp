// Drives the real SfcProcessor/SfcEditor headlessly:
// load synthetic .spc + .sfc, play a chord through processBlock, save/restore state,
// render the editor to PNG.
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <cmath>

static void encodeBlock (const int16_t* x, uint8_t* out, bool last, bool loop)
{
    int maxAbs = 0;
    for (int i = 0; i < 16; ++i) maxAbs = std::max (maxAbs, std::abs ((int) x[i]));
    int shift = 0;
    while (shift < 12 && maxAbs > 7 * (1 << shift)) ++shift;
    out[0] = (uint8_t) ((shift << 4) | (loop ? 2 : 0) | (last ? 1 : 0));
    for (int i = 0; i < 16; ++i)
    {
        int nib = juce::jlimit (-8, 7, (int) std::lround ((double) x[i] / (double) (1 << shift)));
        if (i & 1) out[1 + i / 2] |= (uint8_t) (nib & 0xF);
        else       out[1 + i / 2] = (uint8_t) ((nib & 0xF) << 4);
    }
}

// writes a 64-block looping waveform at addr; shape 0 = sine, 1 = saw, 2 = square
static void writeWave (uint8_t* mem, int addr, int period, int shape)
{
    std::vector<int16_t> w (1024);
    for (int i = 0; i < 1024; ++i)
    {
        const double ph = (double) (i % period) / period;
        double v = shape == 0 ? std::sin (2 * juce::MathConstants<double>::pi * ph)
                 : shape == 1 ? 2 * ph - 1 : (ph < 0.5 ? 0.8 : -0.8);
        w[(size_t) i] = (int16_t) (11000 * v * (0.6 + 0.4 * std::sin (i * 0.003)));
    }
    for (int b = 0; b < 64; ++b) encodeBlock (&w[(size_t) b * 16], mem + addr + b * 9, b == 63, true);
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sfc8test");
    dir.createDirectory();

    // ---- synthetic SPC: 3 voices with different samples ----
    std::vector<uint8_t> spc (0x10200, 0);
    std::memcpy (spc.data(), "SNES-SPC700 Sound File Data v0.30", 33);
    std::memcpy (spc.data() + 0x2E, "Overworld", 9);
    std::memcpy (spc.data() + 0x4E, "Test Quest", 10);
    uint8_t* ram = spc.data() + 0x100;
    uint8_t* dsp = spc.data() + 0x10100;
    const int addrs[3] = { 0x1000, 0x1400, 0x1800 };
    for (int k = 0; k < 3; ++k)
    {
        writeWave (ram, addrs[k], 64 + 32 * k, k);
        ram[0x200 + k * 4 + 0] = addrs[k] & 0xFF; ram[0x200 + k * 4 + 1] = (uint8_t) (addrs[k] >> 8);
        const int lp = addrs[k] + 32 * 9;
        ram[0x200 + k * 4 + 2] = lp & 0xFF;  ram[0x200 + k * 4 + 3] = (uint8_t) (lp >> 8);
        dsp[k * 0x10 + 4] = (uint8_t) k;
        dsp[k * 0x10 + 5] = 0x8E; dsp[k * 0x10 + 6] = 0xC4;
        dsp[k * 0x10 + 0] = 90; dsp[k * 0x10 + 1] = 90;
    }
    dsp[0x5D] = 0x02;
    dsp[0x4D] = 0x01; dsp[0x2C] = 40; dsp[0x3C] = 40; dsp[0x7D] = 5; dsp[0x0D] = 70;
    auto spcFile = dir.getChildFile ("test.spc");
    spcFile.replaceWithData (spc.data(), spc.size());

    // ---- synthetic ROM: 512 KB of noise with 3 BRR samples embedded ----
    std::vector<uint8_t> rom (512 * 1024);
    juce::Random rng (42);
    for (auto& b : rom) b = (uint8_t) rng.nextInt (256);
    std::memcpy (rom.data() + 0x7FC0, "TEST QUEST ROM       ", 21);
    writeWave (rom.data(), 0x20000, 100, 0);
    writeWave (rom.data(), 0x30000, 80, 1);
    writeWave (rom.data(), 0x41000, 120, 2);
    auto romFile = dir.getChildFile ("test.sfc");
    romFile.replaceWithData (rom.data(), rom.size());

    SfcProcessor proc;
    proc.setRateAndBufferSizeDetails (48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    auto playChord = [&] (const char* label)
    {
        juce::AudioBuffer<float> buf (2, 512);
        double sumSq = 0; float peak = 0; int maxVoices = 0;
        for (int blk = 0; blk < 94; ++blk) // ~1 s
        {
            juce::MidiBuffer midi;
            if (blk == 0) for (int n : { 60, 64, 67, 71 }) midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 100), 10);
            if (blk == 60) for (int n : { 60, 64, 67, 71 }) midi.addEvent (juce::MidiMessage::noteOff (1, n), 0);
            proc.processBlock (buf, midi);
            maxVoices = std::max (maxVoices, proc.getActiveVoices());
            for (int i = 0; i < 512; ++i) { const float v = buf.getSample (0, i); sumSq += v * v; peak = std::max (peak, std::abs (v)); }
        }
        std::printf ("  [%s] chord: max voices=%d peak=%.3f rms=%.3f voices at end=%d\n",
                     label, maxVoices, peak, std::sqrt (sumSq / (94 * 512)), proc.getActiveVoices());
    };

    std::printf ("SPC load: %d\n", proc.loadFile (spcFile));
    std::printf ("  status: %s\n", proc.getStatusText().toRawUTF8());
    for (int t = 0; t < 8; ++t) std::printf ("  track %d -> sample %d\n", t + 1, proc.getTrackSample (t));
    std::printf ("  AR=%g SL=%g echo=%g EDL=%g\n", proc.apvts.getRawParameterValue ("attack")->load(),
                 proc.apvts.getRawParameterValue ("sustainLevel")->load(), proc.apvts.getRawParameterValue ("echo")->load(),
                 proc.apvts.getRawParameterValue ("echoDelay")->load());
    playChord ("SPC track 1");
    proc.selectTrack (2);
    std::printf ("  track 3 status: %s\n", proc.getStatusText().toRawUTF8());
    playChord ("SPC track 3");

    // render editor
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png;
        juce::FileOutputStream os (dir.getChildFile ("editor_spc.png"));
        os.setPosition (0); os.truncate();
        png.writeImageToStream (img, os);
    }

    std::printf ("ROM load: %d\n", proc.loadFile (romFile));
    std::printf ("  title='%s' samples=%zu\n", proc.getLoadResult().gameTitle.c_str(), proc.getLoadResult().samples.size());
    for (auto& s : proc.getLoadResult().samples) std::printf ("    %s\n", s->name.c_str());
    std::printf ("  status: %s\n", proc.getStatusText().toRawUTF8());
    playChord ("ROM track 1");
    proc.selectTrack (5);
    std::printf ("  empty track: %s\n", proc.getStatusText().toRawUTF8());
    proc.selectSample (1);
    std::printf ("  track 6 <- sample 2: track6=%d status: %s\n", proc.getTrackSample (5), proc.getStatusText().toRawUTF8());
    playChord ("ROM track 6");

    // state round trip
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    SfcProcessor proc2;
    proc2.setRateAndBufferSizeDetails (48000.0, 512);
    proc2.prepareToPlay (48000.0, 512);
    proc2.setStateInformation (state.getData(), (int) state.getSize());
    std::printf ("state: %zu bytes, restored track=%d track6=%d sample=%s\n", state.getSize(),
                 proc2.getSelectedTrack() + 1, proc2.getTrackSample (5),
                 proc2.getCurrentSample() ? proc2.getCurrentSample()->name.c_str() : "none");

    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png;
        juce::FileOutputStream os (dir.getChildFile ("editor_rom.png"));
        os.setPosition (0); os.truncate();
        png.writeImageToStream (img, os);
    }
    std::printf ("images in %s\n", dir.getFullPathName().toRawUTF8());
    return 0;
}

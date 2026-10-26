// Standalone test for the SNES core: builds a synthetic .spc, loads it,
// scans it as a "ROM", detects root, renders a chord.
#include "../Source/Snes/Loaders.h"
#include "../Source/Snes/Engine.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

static void encodeBlock (const int16_t* x, uint8_t* out, bool last, bool loop)
{
    int maxAbs = 0;
    for (int i = 0; i < 16; ++i) maxAbs = std::max (maxAbs, std::abs ((int) x[i]));
    int shift = 0;
    while (shift < 12 && maxAbs > 7 * (1 << shift)) ++shift;
    out[0] = (uint8_t) ((shift << 4) | (loop ? 2 : 0) | (last ? 1 : 0));
    for (int i = 0; i < 16; ++i)
    {
        int nib = (int) std::lround ((double) x[i] / (double) (1 << shift));
        nib = std::clamp (nib, -8, 7);
        if (i & 1) out[1 + i / 2] |= (uint8_t) (nib & 0xF);
        else       out[1 + i / 2] = (uint8_t) ((nib & 0xF) << 4);
    }
}

int main()
{
    std::vector<uint8_t> spc (0x10200, 0);
    std::memcpy (spc.data(), "SNES-SPC700 Sound File Data v0.30", 33);
    std::memcpy (spc.data() + 0x2E, "Test Song", 9);
    std::memcpy (spc.data() + 0x4E, "Test Game", 9);
    uint8_t* ram = spc.data() + 0x100;
    uint8_t* dsp = spc.data() + 0x10100;

    // sine, period 128 samples (250 Hz at 32 kHz -> MIDI ~59.2), 1024 samples, loop at 512
    std::vector<int16_t> sine (1024);
    for (int i = 0; i < 1024; ++i) sine[(size_t) i] = (int16_t) (12000 * std::sin (2 * M_PI * i / 128.0));
    const int start = 0x1000;
    for (int b = 0; b < 64; ++b)
        encodeBlock (&sine[(size_t) b * 16], ram + start + b * 9, b == 63, true);

    ram[0x200] = start & 0xFF; ram[0x201] = start >> 8;
    const int loop = start + 32 * 9;
    ram[0x202] = loop & 0xFF; ram[0x203] = loop >> 8;
    dsp[0x5D] = 0x02;
    dsp[0x04] = 0;    // voice 0 srcn
    dsp[0x05] = 0x8F; // ADSR on, A=15 D=0
    dsp[0x06] = 0xE0; // SL=7 SR=0
    dsp[0x00] = 100; dsp[0x01] = 100;

    auto r = snes::loadSpc (spc);
    std::printf ("SPC: err='%s' game='%s' song='%s' samples=%zu voice0 valid=%d idx=%d adsr=%d\n",
                 r.error.c_str(), r.gameTitle.c_str(), r.songTitle.c_str(), r.samples.size(),
                 r.voices[0].valid, r.voices[0].sampleIndex, r.voices[0].adsrOn);
    if (r.samples.empty()) return 1;

    snes::Sample s = *r.samples[0];
    std::printf ("decoded %zu samples loopStart=%d name='%s'\n", s.pcm.size(), s.loopStart, s.name.c_str());
    double err = 0;
    for (size_t i = 0; i < 1024; ++i) err = std::max (err, std::abs ((double) s.pcm[i] - sine[i]));
    std::printf ("max decode error vs source: %.0f\n", err);

    snes::prepareSample (s);
    std::printf ("root detected=%d note=%.2f (expected ~59.21)\n", s.rootDetected, s.rootNote);

    // ROM scan on the raw ARAM image
    std::vector<uint8_t> rom (ram, ram + 0x10000);
    auto rr = snes::scanRom (rom);
    std::printf ("ROM scan: %zu hits, first at $%X err='%s'\n", rr.samples.size(),
                 rr.samples.empty() ? 0 : rr.samples[0]->address, rr.error.c_str());
    if (! rr.samples.empty())
        std::printf ("  guessed loop start=%d (true 512, any multiple of 128 is seamless)\n", rr.samples[0]->loopStart);

    // Render a C major chord
    snes::Engine e;
    e.prepare (48000.0);
    snes::EngineParams p;
    p.rootNote = s.rootNote;
    p.echoOn = true;
    e.setParams (p);
    e.setSample (&s);
    e.noteOn (60, 100); e.noteOn (64, 100); e.noteOn (67, 100);
    std::vector<float> L (48000), R (48000);
    e.render (L.data(), R.data(), 24000);
    std::printf ("voices active during chord: %d\n", e.activeVoiceCount());
    e.noteOff (60); e.noteOff (64); e.noteOff (67);
    e.render (L.data() + 24000, R.data() + 24000, 24000);

    double peak = 0, rmsHold = 0; bool bad = false;
    for (int i = 0; i < 48000; ++i)
    {
        if (! std::isfinite (L[(size_t) i])) bad = true;
        peak = std::max (peak, (double) std::abs (L[(size_t) i]));
        if (i > 4000 && i < 20000) rmsHold += L[(size_t) i] * L[(size_t) i];
    }
    std::printf ("peak=%.3f holdRMS=%.3f finite=%d voices after release=%d\n",
                 peak, std::sqrt (rmsHold / 16000), ! bad, e.activeVoiceCount());

    // frequency check: single note at root should give 250 Hz
    e.reset(); e.setSample (&s);
    p.echoOn = false; e.setParams (p);
    e.noteOn ((int) std::lround (s.rootNote), 127);
    std::vector<float> M (48000), M2 (48000);
    e.render (M.data(), M2.data(), 48000);
    int zc = 0;
    for (int i = 8001; i < 40000; ++i) if (M[(size_t) i - 1] < 0 && M[(size_t) i] >= 0) ++zc;
    std::printf ("note %ld -> measured %.1f Hz (expect %.1f)\n", std::lround (s.rootNote), zc / (32000.0 / 48000.0),
                 250.0 * std::pow (2.0, (std::lround (s.rootNote) - s.rootNote) / 12.0));
    return bad ? 1 : 0;
}

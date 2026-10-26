#pragma once
// File loaders: .spc sound snapshots, .sfc/.smc ROM scanning, raw .brr files.

#include "Brr.h"
#include <array>
#include <string>
#include <vector>

namespace snes
{

struct EchoInfo
{
    bool   present = false;
    int    delay = 0;          // EDL 0..15 (x16 ms)
    int    feedback = 0;       // EFB -128..127
    int    volL = 0, volR = 0; // EVOL
    std::array<int, 8> fir { 127, 0, 0, 0, 0, 0, 0, 0 };
};

struct VoiceInfo
{
    bool valid = false;
    int  srcn = 0;
    int  pitchReg = 0;
    int  volL = 0, volR = 0;
    bool adsrOn = false;
    int  a = 15, d = 7, sl = 7, sr = 0;
    bool echoOn = false;
    bool looksActive = false;
    int  sampleIndex = -1;     // index into LoadResult::samples
};

struct LoadResult
{
    enum class Kind { None, Spc, Rom, Brr };
    Kind kind = Kind::None;
    std::string error;
    std::string gameTitle, songTitle;
    std::array<VoiceInfo, 8> voices {};
    std::vector<SamplePtr> samples;   // browsable list
    EchoInfo echo;
};

LoadResult loadSpc (const std::vector<uint8_t>& data);
LoadResult scanRom (const std::vector<uint8_t>& data);
LoadResult loadRawBrr (const std::vector<uint8_t>& data);

// Detect root pitch (MIDI note, fractional) of a decoded sample played at 32 kHz.
bool detectRootNote (const Sample& s, float& midiOut);

// Guess a loop start for a sample whose loop address is unknown.
int guessLoopStart (const std::vector<int16_t>& pcm);

// Fills rootNote/rootDetected. Call before publishing a sample to the audio thread.
void prepareSample (Sample& s);

} // namespace snes

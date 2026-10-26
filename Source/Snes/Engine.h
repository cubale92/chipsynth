#pragma once
// Polyphonic S-DSP style engine.
// Voices run at the native 32 kHz rate with the 14-bit pitch counter,
// 4-tap gaussian interpolation, hardware ADSR rate table and the
// 8-tap FIR echo, then get resampled to the host rate.

#include "Brr.h"
#include <array>
#include <cstdint>
#include <vector>

namespace snes
{

struct EngineParams
{
    float rootNote = 60.0f;
    float fineCents = 0.0f;
    int   a = 15, d = 7, sl = 7, sr = 0;
    float releaseMs = 0.0f;        // 0 = hardware release (~8 ms)
    int   polyphony = 16;
    bool  hwPitchLimit = true;     // clamp pitch register to $3FFF like the S-DSP
    bool  echoOn = false;
    int   echoDelay = 4;           // EDL 0..15
    int   echoFeedback = 64;       // EFB
    int   echoVolume = 48;         // EVOL
    std::array<int, 8> fir { 127, 0, 0, 0, 0, 0, 0, 0 };
    float velocitySens = 0.7f;
    float vibratoCents = 30.0f;    // depth at full mod wheel
    float pan = 0.0f;
    float gain = 1.0f;
    float bendRange = 2.0f;
    bool  glide = false;
    float glideMs = 80.0f;
};

class Engine
{
public:
    static constexpr int maxVoices = 32;
    static constexpr double nativeRate = 32000.0;

    Engine();

    void prepare (double hostSampleRate);
    void reset();

    void setSample (const Sample* s);          // audio thread only
    void setParams (const EngineParams& p);    // audio thread only

    void noteOn (int note, int velocity);
    void noteOff (int note);
    void allNotesOff (bool hard);
    void pitchBend (int value14);
    void modWheel (float v01) { modWheel01 = v01; }
    void sustainPedal (bool down);

    void render (float* left, float* right, int numSamples);

    int activeVoiceCount() const;

private:
    enum class EnvMode { Attack, Decay, Sustain, Release, Off };

    struct Voice
    {
        bool     active = false;
        bool     keyDown = false;
        bool     sustained = false;
        int      note = 60;
        float    velocity = 1.0f;
        uint64_t pos = 0;              // 12-bit fractional sample position
        uint32_t step = 0x1000;
        float    currentNote = 60.0f;  // for glide
        int      env = 0;
        float    envF = 0.0f;          // release (smooth mode)
        EnvMode  mode = EnvMode::Off;
        uint32_t tick = 0;
        uint64_t age = 0;
    };

    void renderNativeFrame (float& outL, float& outR);
    void updateEnvelope (Voice& v);
    void updatePitches();
    int  sampleAt (int64_t i) const;
    void startRelease (Voice& v);

    std::array<Voice, maxVoices> voices {};
    const Sample* sample = nullptr;
    EngineParams params;

    double hostRate = 44100.0;
    double ratio = 32000.0 / 44100.0;
    double phase = 1.0;
    std::array<float, 4> histL {}, histR {};

    float bendSemis = 0.0f;
    float modWheel01 = 0.0f;
    bool  pedalDown = false;
    double lfoPhase = 0.0;
    int   pitchUpdateCounter = 0;
    uint64_t ageCounter = 0;
    int   lastNote = -1;

    // echo
    std::vector<float> echoBufL, echoBufR;
    int echoPos = 0;
    std::array<float, 8> firHistL {}, firHistR {};
    int firIdx = 0;

    // analog output stage
    float lpL = 0.0f, lpR = 0.0f, lpCoef = 0.5f;

    std::array<int16_t, 512> gauss {};
};

} // namespace snes

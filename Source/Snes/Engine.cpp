#include "Engine.h"
#include <cmath>
#include <algorithm>

namespace snes
{

namespace
{
    // S-DSP envelope rate table (samples between steps at 32 kHz); 0 = never
    constexpr uint32_t kRates[32] = {
        0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
        64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1
    };

    constexpr double kPi = 3.14159265358979323846;
}

Engine::Engine()
{
    // 4-tap gaussian interpolation table modelled on the S-DSP's 512-entry table.
    // g[j] = C * exp(-k * x^2), x = distance in samples = (512 - j) / 256.
    const double k = 1.2606;
    const double C = 2046.0 / (1.0 + 2.0 * std::exp (-k) + std::exp (-4.0 * k));
    for (int j = 0; j < 512; ++j)
    {
        const double x = (512.0 - j) / 256.0;
        gauss[(size_t) j] = (int16_t) std::lround (C * std::exp (-k * x * x));
    }
}

void Engine::prepare (double sr)
{
    hostRate = sr > 0 ? sr : 44100.0;
    ratio = nativeRate / hostRate;
    // ~14 kHz one-pole: soft analog output stage
    const double fc = std::min (14000.0, hostRate * 0.45);
    lpCoef = (float) (1.0 - std::exp (-2.0 * kPi * fc / hostRate));
    echoBufL.assign (15 * 512 + 1, 0.0f);
    echoBufR.assign (15 * 512 + 1, 0.0f);
    reset();
}

void Engine::reset()
{
    for (auto& v : voices) v = Voice();
    phase = 1.0;
    histL.fill (0); histR.fill (0);
    std::fill (echoBufL.begin(), echoBufL.end(), 0.0f);
    std::fill (echoBufR.begin(), echoBufR.end(), 0.0f);
    firHistL.fill (0); firHistR.fill (0);
    echoPos = 0; firIdx = 0;
    lpL = lpR = 0.0f;
}

void Engine::setSample (const Sample* s)
{
    if (s != sample)
    {
        for (auto& v : voices) v.active = false;
        sample = s;
    }
}

void Engine::setParams (const EngineParams& p)
{
    const bool pitchChanged = p.rootNote != params.rootNote || p.fineCents != params.fineCents
                              || p.hwPitchLimit != params.hwPitchLimit;
    params = p;
    params.polyphony = std::clamp (params.polyphony, 1, maxVoices);
    for (int i = params.polyphony; i < maxVoices; ++i)
        voices[(size_t) i].active = false;
    if (pitchChanged) updatePitches();
}

//==============================================================================
void Engine::noteOn (int note, int velocity)
{
    if (sample == nullptr || sample->pcm.empty()) return;

    const int poly = params.polyphony;
    Voice* target = nullptr;

    for (int i = 0; i < poly && ! target; ++i)
        if (! voices[(size_t) i].active) target = &voices[(size_t) i];

    if (! target)
    {
        // steal: released voices first, then the oldest
        uint64_t bestAge = UINT64_MAX;
        bool bestReleased = false;
        for (int i = 0; i < poly; ++i)
        {
            auto& v = voices[(size_t) i];
            const bool rel = v.mode == EnvMode::Release;
            if ((rel && ! bestReleased) || (rel == bestReleased && v.age < bestAge))
            {
                target = &v; bestAge = v.age; bestReleased = rel;
            }
        }
    }
    if (! target) return;

    const float vel01 = (float) velocity / 127.0f;
    auto& v = *target;
    v = Voice();
    v.active = true;
    v.keyDown = true;
    v.note = note;
    v.velocity = (1.0f - params.velocitySens) + params.velocitySens * vel01 * vel01;
    v.currentNote = (params.glide && lastNote >= 0) ? (float) lastNote : (float) note;
    v.pos = 0;
    v.env = 0;
    v.mode = EnvMode::Attack;
    v.tick = 0;
    v.age = ++ageCounter;
    lastNote = note;
    updatePitches();
}

void Engine::noteOff (int note)
{
    for (auto& v : voices)
    {
        if (v.active && v.keyDown && v.note == note)
        {
            v.keyDown = false;
            if (pedalDown) v.sustained = true;
            else           startRelease (v);
        }
    }
}

void Engine::sustainPedal (bool down)
{
    pedalDown = down;
    if (! down)
        for (auto& v : voices)
            if (v.active && v.sustained && ! v.keyDown) { v.sustained = false; startRelease (v); }
}

void Engine::allNotesOff (bool hard)
{
    for (auto& v : voices)
    {
        if (hard) v.active = false;
        else if (v.active) startRelease (v);
    }
}

void Engine::pitchBend (int value14)
{
    bendSemis = (float) (value14 - 8192) / 8192.0f * params.bendRange;
}

void Engine::startRelease (Voice& v)
{
    if (v.mode == EnvMode::Release || v.mode == EnvMode::Off) return;
    v.mode = EnvMode::Release;
    v.envF = (float) v.env;
}

int Engine::activeVoiceCount() const
{
    int n = 0;
    for (auto& v : voices) n += v.active ? 1 : 0;
    return n;
}

//==============================================================================
void Engine::updatePitches()
{
    const float vib = (float) std::sin (lfoPhase * 2.0 * kPi) * params.vibratoCents * modWheel01 / 100.0f;
    for (auto& v : voices)
    {
        if (! v.active) continue;
        const float semis = v.currentNote - params.rootNote + params.fineCents / 100.0f + bendSemis + vib;
        double st = 4096.0 * std::pow (2.0, semis / 12.0);
        if (params.hwPitchLimit) st = std::min (st, (double) 0x3FFF);
        v.step = (uint32_t) std::max (1.0, st);
    }
}

int Engine::sampleAt (int64_t i) const
{
    const auto& pcm = sample->pcm;
    const int64_t len = (int64_t) pcm.size();
    if (i < 0) return 0;
    if (i >= len)
    {
        if (! sample->loops()) return 0;
        const int64_t ls = sample->loopStart;
        i = ls + (i - ls) % (len - ls);
    }
    return pcm[(size_t) i];
}

void Engine::updateEnvelope (Voice& v)
{
    ++v.tick;
    auto due = [&] (int rate) { const uint32_t p = kRates[rate]; return p != 0 && (v.tick % p) == 0; };

    switch (v.mode)
    {
        case EnvMode::Attack:
        {
            const int rate = params.a * 2 + 1;
            if (due (rate))
            {
                v.env += (rate == 31) ? 1024 : 32;
                if (v.env >= 0x7FF) { v.env = 0x7FF; v.mode = EnvMode::Decay; }
            }
            break;
        }
        case EnvMode::Decay:
        {
            if ((v.env >> 8) <= params.sl) { v.mode = EnvMode::Sustain; break; }
            if (due (params.d * 2 + 16))
            {
                v.env -= ((v.env - 1) >> 8) + 1;
                if ((v.env >> 8) <= params.sl) v.mode = EnvMode::Sustain;
            }
            break;
        }
        case EnvMode::Sustain:
            if (due (params.sr))
                v.env -= ((v.env - 1) >> 8) + 1;
            break;

        case EnvMode::Release:
            if (params.releaseMs <= 0.0f)
            {
                v.env -= 8; // hardware key-off release
            }
            else
            {
                // smooth exponential release to -48 dB in releaseMs
                const float coef = (float) std::exp (std::log (0.004) / (params.releaseMs * 32.0));
                v.envF *= coef;
                v.env = (int) v.envF;
            }
            break;

        case EnvMode::Off:
            break;
    }

    if (v.env <= 0)
    {
        v.env = 0;
        if (v.mode == EnvMode::Release || v.mode == EnvMode::Sustain)
        {
            v.mode = EnvMode::Off;
            v.active = false;
        }
    }
}

void Engine::renderNativeFrame (float& outL, float& outR)
{
    float dryL = 0.0f, dryR = 0.0f;

    if (++pitchUpdateCounter >= 32) // ~1 ms control rate
    {
        pitchUpdateCounter = 0;
        lfoPhase += 32.0 * 5.5 / nativeRate;
        if (lfoPhase >= 1.0) lfoPhase -= 1.0;

        if (params.glide)
        {
            // exponential approach, ~95% of the way in glideMs (1 control tick = 1 ms)
            const float k = 1.0f - std::exp (-3.0f / std::max (1.0f, params.glideMs));
            for (auto& v : voices)
            {
                if (! v.active) continue;
                const float diff = (float) v.note - v.currentNote;
                if (std::abs (diff) < 0.001f) v.currentNote = (float) v.note;
                else                          v.currentNote += diff * k;
            }
        }
        else
        {
            for (auto& v : voices) v.currentNote = (float) v.note;
        }
        updatePitches();
    }

    const float panL = std::sqrt (0.5f * (1.0f - params.pan));
    const float panR = std::sqrt (0.5f * (1.0f + params.pan));

    if (sample != nullptr && ! sample->pcm.empty())
    {
        const int64_t len = (int64_t) sample->pcm.size();
        const bool loops = sample->loops();

        for (int vi = 0; vi < params.polyphony; ++vi)
        {
            auto& v = voices[(size_t) vi];
            if (! v.active) continue;

            const int64_t idx = (int64_t) (v.pos >> 12);
            if (! loops && idx >= len) { v.active = false; continue; }

            const int f = (int) ((v.pos >> 4) & 0xFF);
            int out = (gauss[(size_t) (255 - f)] * sampleAt (idx - 1)) >> 11;
            out    += (gauss[(size_t) (511 - f)] * sampleAt (idx))     >> 11;
            out    += (gauss[(size_t) (256 + f)] * sampleAt (idx + 1)) >> 11;
            out     = (int16_t) out; // 16-bit wrap like the hardware's first stage
            out    += (gauss[(size_t) f]         * sampleAt (idx + 2)) >> 11;
            out     = clamp16 (out) & ~1;

            updateEnvelope (v);
            const int vo = (out * v.env) >> 11;

            const float s = (float) vo * v.velocity;
            dryL += s * panL;
            dryR += s * panR;

            // wrap position inside the loop to keep the counter small
            v.pos += v.step;
            if (loops)
            {
                const uint64_t lenFx = (uint64_t) len << 12;
                if (v.pos >= lenFx)
                {
                    const uint64_t loopFx = (uint64_t) sample->loopStart << 12;
                    v.pos = loopFx + (v.pos - lenFx) % (lenFx - loopFx);
                }
            }
        }
    }

    // ---- echo (S-DSP style: delayed buffer -> 8-tap FIR -> feedback) ----
    float wetL = 0.0f, wetR = 0.0f;
    if (params.echoOn && ! echoBufL.empty())
    {
        const int len = std::max (1, params.echoDelay * 512);
        if (echoPos >= len) echoPos = 0;

        firHistL[(size_t) firIdx] = echoBufL[(size_t) echoPos];
        firHistR[(size_t) firIdx] = echoBufR[(size_t) echoPos];

        float firL = 0.0f, firR = 0.0f;
        for (int t = 0; t < 8; ++t)
        {
            // fir[0] applies to the oldest of the 8 taps, fir[7] to the newest
            const int h = (firIdx + 1 + t) & 7;
            firL += firHistL[(size_t) h] * (float) params.fir[(size_t) t];
            firR += firHistR[(size_t) h] * (float) params.fir[(size_t) t];
        }
        firL /= 128.0f; firR /= 128.0f;
        firIdx = (firIdx + 1) & 7;

        const float fb = (float) params.echoFeedback / 128.0f;
        echoBufL[(size_t) echoPos] = (float) clamp16 ((int) (dryL + firL * fb));
        echoBufR[(size_t) echoPos] = (float) clamp16 ((int) (dryR + firR * fb));
        echoPos = (echoPos + 1) % len;

        const float ev = (float) params.echoVolume / 128.0f;
        wetL = firL * ev;
        wetR = firR * ev;
    }

    outL = (dryL + wetL) / 32768.0f;
    outR = (dryR + wetR) / 32768.0f;
}

//==============================================================================
void Engine::render (float* left, float* right, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        while (phase >= 1.0)
        {
            phase -= 1.0;
            float l, r;
            renderNativeFrame (l, r);
            histL[0] = histL[1]; histL[1] = histL[2]; histL[2] = histL[3]; histL[3] = l;
            histR[0] = histR[1]; histR[1] = histR[2]; histR[2] = histR[3]; histR[3] = r;
        }

        const float t = (float) phase;
        auto hermite = [t] (const std::array<float, 4>& y)
        {
            const float c0 = y[1];
            const float c1 = 0.5f * (y[2] - y[0]);
            const float c2 = y[0] - 2.5f * y[1] + 2.0f * y[2] - 0.5f * y[3];
            const float c3 = 0.5f * (y[3] - y[0]) + 1.5f * (y[1] - y[2]);
            return ((c3 * t + c2) * t + c1) * t + c0;
        };

        float l = hermite (histL), r = hermite (histR);
        lpL += lpCoef * (l - lpL);
        lpR += lpCoef * (r - lpR);

        left[i]  = lpL * params.gain;
        right[i] = lpR * params.gain;
        phase += ratio;
    }
}

} // namespace snes

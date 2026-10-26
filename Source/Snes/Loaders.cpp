#include "Loaders.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <set>
#include <cctype>

namespace snes
{

namespace
{
    std::string trimText (const uint8_t* p, size_t n)
    {
        std::string s;
        for (size_t i = 0; i < n && p[i] != 0; ++i)
            s += (p[i] >= 32 && p[i] < 127) ? (char) p[i] : ' ';
        while (! s.empty() && s.back() == ' ') s.pop_back();
        while (! s.empty() && s.front() == ' ') s.erase (s.begin());
        return s;
    }

    std::string hex (int v, int digits)
    {
        char buf[16];
        std::snprintf (buf, sizeof (buf), "%0*X", digits, v);
        return buf;
    }

    struct Quality { double rms = 0, diffRatio = 0, clipFrac = 0; };

    Quality measure (const std::vector<int16_t>& pcm, int clips)
    {
        Quality q;
        if (pcm.empty()) return q;
        double sumSq = 0, sumAbs = 0, sumDiff = 0;
        for (size_t i = 0; i < pcm.size(); ++i)
        {
            const double x = pcm[i];
            sumSq += x * x;
            sumAbs += std::abs (x);
            if (i) sumDiff += std::abs (x - (double) pcm[i - 1]);
        }
        q.rms = std::sqrt (sumSq / (double) pcm.size());
        q.diffRatio = sumAbs > 0 ? sumDiff / sumAbs : 10.0;
        q.clipFrac = (double) clips / (double) pcm.size();
        return q;
    }

    bool plausible (const Quality& q, double maxClip, double maxDiff)
    {
        return q.rms >= 250.0 && q.clipFrac <= maxClip && q.diffRatio <= maxDiff;
    }
}

//==============================================================================
LoadResult loadSpc (const std::vector<uint8_t>& data)
{
    LoadResult r;
    static const char* magic = "SNES-SPC700 Sound File Data";
    if (data.size() < 0x10180 || std::memcmp (data.data(), magic, std::strlen (magic)) != 0)
    {
        r.error = "Not a valid .spc file";
        return r;
    }
    r.kind = LoadResult::Kind::Spc;

    const uint8_t* ram = data.data() + 0x100;
    const uint8_t* dsp = data.data() + 0x10100;

    // ID666 text tag (most dumps use text format)
    r.songTitle = trimText (data.data() + 0x2E, 32);
    r.gameTitle = trimText (data.data() + 0x4E, 32);

    const int dir = dsp[0x5D] << 8;

    // Echo
    r.echo.delay    = dsp[0x7D] & 0x0F;
    r.echo.feedback = (int8_t) dsp[0x0D];
    r.echo.volL     = (int8_t) dsp[0x2C];
    r.echo.volR     = (int8_t) dsp[0x3C];
    for (int i = 0; i < 8; ++i) r.echo.fir[(size_t) i] = (int8_t) dsp[0x0F + i * 0x10];
    const int eon = dsp[0x4D];
    const bool echoWritesEnabled = (dsp[0x6C] & 0x20) == 0;
    r.echo.present = eon != 0 && echoWritesEnabled && (r.echo.volL != 0 || r.echo.volR != 0);

    std::vector<int> startToIndex; // parallel search helper
    std::vector<uint16_t> starts;

    auto addFromDir = [&] (int srcn, bool force) -> int
    {
        const int e = (dir + srcn * 4) & 0xFFFF;
        const uint16_t start = (uint16_t) (ram[e] | (ram[(e + 1) & 0xFFFF] << 8));
        const uint16_t loop  = (uint16_t) (ram[(e + 2) & 0xFFFF] | (ram[(e + 3) & 0xFFFF] << 8));

        for (size_t i = 0; i < starts.size(); ++i)
            if (starts[i] == start) return startToIndex[i];

        if (! force && (start == 0 || start == 0xFFFF)) return -1;

        auto s = std::make_shared<Sample>();
        if (! decodeBrr (ram, 0x10000, start, loop, *s, true, 7300)) return -1;

        if (! force)
        {
            // re-decode to count clips for plausibility of unused directory slots
            std::vector<int16_t> tmp;
            int p1 = 0, p2 = 0, clips = 0;
            uint32_t a = start;
            for (size_t b = 0; b < s->pcm.size() / 16 && b < 7300; ++b)
            {
                uint8_t blk[9];
                for (int k = 0; k < 9; ++k) blk[k] = ram[(a + (uint32_t) k) & 0xFFFF];
                decodeBrrBlock (blk, tmp, p1, p2, &clips);
                if (blk[0] & 1) break;
                a = (a + 9) & 0xFFFF;
            }
            if (! plausible (measure (tmp, clips), 0.03, 1.2) || s->pcm.size() < 32) return -1;
        }

        s->srcn = srcn;
        s->address = start;
        s->name = "SRC " + hex (srcn, 2) + "  @$" + hex (start, 4) + "  " + std::to_string (s->pcm.size()) + " smp"
                  + (s->loops() ? "  loop" : "");
        r.samples.push_back (s);
        starts.push_back (start);
        startToIndex.push_back ((int) r.samples.size() - 1);
        return (int) r.samples.size() - 1;
    };

    // The 8 hardware voices first
    for (int v = 0; v < 8; ++v)
    {
        const int b = v << 4;
        auto& vi = r.voices[(size_t) v];
        vi.volL    = (int8_t) dsp[b + 0];
        vi.volR    = (int8_t) dsp[b + 1];
        vi.pitchReg = (dsp[b + 2] | (dsp[b + 3] << 8)) & 0x3FFF;
        vi.srcn    = dsp[b + 4];
        const int adsr1 = dsp[b + 5], adsr2 = dsp[b + 6];
        vi.adsrOn = (adsr1 & 0x80) != 0;
        vi.a  = adsr1 & 0x0F;
        vi.d  = (adsr1 >> 4) & 0x07;
        vi.sl = adsr2 >> 5;
        vi.sr = adsr2 & 0x1F;
        vi.echoOn = (eon >> v) & 1;
        vi.looksActive = dsp[b + 8] != 0 || dsp[b + 9] != 0 || vi.volL != 0 || vi.volR != 0;
        vi.sampleIndex = addFromDir (vi.srcn, true);
        vi.valid = vi.sampleIndex >= 0;
    }

    // Copy envelope hints into each voice's sample (first voice using it wins)
    for (auto& vi : r.voices)
    {
        if (! vi.valid || ! vi.adsrOn) continue;
        auto* s = const_cast<Sample*> (r.samples[(size_t) vi.sampleIndex].get());
        if (s->hasEnvelopeHint) continue;
        s->adsrA = vi.a; s->adsrD = vi.d; s->adsrSL = vi.sl; s->adsrSR = vi.sr;
        s->hasEnvelopeHint = true;
    }

    // Then every other plausible sample in the directory
    for (int srcn = 0; srcn < 256; ++srcn)
        addFromDir (srcn, false);

    if (r.samples.empty()) r.error = "No samples found in this .spc";
    return r;
}

//==============================================================================
LoadResult scanRom (const std::vector<uint8_t>& raw)
{
    LoadResult r;
    r.kind = LoadResult::Kind::Rom;

    size_t off0 = (raw.size() % 1024 == 512) ? 512 : 0; // copier header
    const uint8_t* d = raw.data() + off0;
    const size_t n = raw.size() - off0;

    // Internal ROM title (LoROM $7FC0 / HiROM $FFC0)
    for (size_t hdr : { (size_t) 0x7FC0, (size_t) 0xFFC0, (size_t) 0x40FFC0 })
    {
        if (hdr + 21 <= n)
        {
            auto t = trimText (d + hdr, 21);
            int printable = 0;
            for (char c : t) if (std::isalnum ((unsigned char) c)) ++printable;
            if (printable >= 4) { r.gameTitle = t; break; }
        }
    }

    const int minBlocks = 10, maxBlocks = 4000;
    size_t pos = 0;

    while (pos + 9 * (size_t) minBlocks <= n && r.samples.size() < 1500)
    {
        // structural pass
        if ((d[pos] & 0x0C) != 0 || (d[pos] >> 4) > 12) { ++pos; continue; }

        int blocks = 0;
        bool ok = false;
        size_t p = pos;
        while (p + 9 <= n && blocks < maxBlocks)
        {
            const uint8_t h = d[p];
            if ((h >> 4) > 12) break;
            ++blocks;
            if (h & 1) { ok = true; break; }
            p += 9;
        }

        if (! ok || blocks < minBlocks) { ++pos; continue; }

        // reject runs of empty blocks (padding / zero-filled ROM areas)
        auto emptyBlock = [&] (size_t at)
        {
            for (int k = 1; k < 9; ++k) if (d[at + (size_t) k] != 0) return false;
            return true;
        };
        if (emptyBlock (pos)) { ++pos; continue; }
        int empties = 0;
        for (int b = 0; b < blocks; ++b) empties += emptyBlock (pos + (size_t) b * 9) ? 1 : 0;
        if (empties * 4 > blocks) { ++pos; continue; }

        // Real audio changes loudness gradually, so the per-block shift (range) drifts slowly.
        // Code/graphics/random data jumps around (average step ~3-4).
        int shiftJumps = 0;
        for (int b = 1; b < blocks; ++b)
            shiftJumps += std::abs ((d[pos + (size_t) b * 9] >> 4) - (d[pos + (size_t) (b - 1) * 9] >> 4));
        if (shiftJumps > 2 * (blocks - 1)) { ++pos; continue; }

        // audio pass
        auto s = std::make_shared<Sample>();
        int p1 = 0, p2 = 0, clips = 0;
        uint8_t lastHeader = 0;
        for (int b = 0; b < blocks; ++b)
            lastHeader = decodeBrrBlock (d + pos + (size_t) b * 9, s->pcm, p1, p2, &clips);

        const auto q = measure (s->pcm, clips);
        if (! plausible (q, 0.01, 0.9)) { ++pos; continue; }

        if (lastHeader & 2)
            s->loopStart = guessLoopStart (s->pcm);

        s->address = (int) pos;
        s->name = "ROM $" + hex ((int) pos, 6) + "  " + std::to_string (s->pcm.size()) + " smp"
                  + (s->loops() ? "  loop" : "");
        r.samples.push_back (s);
        pos += (size_t) blocks * 9;
    }

    if (r.samples.empty())
        r.error = "No BRR samples found (this game may compress its sound data - try an .spc of it)";
    return r;
}

//==============================================================================
LoadResult loadRawBrr (const std::vector<uint8_t>& data)
{
    LoadResult r;
    r.kind = LoadResult::Kind::Brr;

    size_t off = 0;
    int64_t loopAddr = -1;
    if (data.size() % 9 == 2 && data.size() > 2)
    {
        off = 2;
        loopAddr = (int64_t) (data[0] | (data[1] << 8)); // loop offset in bytes from data start
    }
    if (data.size() - off < 9) { r.error = "File too small"; return r; }

    auto s = std::make_shared<Sample>();
    if (! decodeBrr (data.data() + off, data.size() - off, 0, loopAddr, *s, false, 100000))
    {
        // file may lack an end flag: decode everything
        s->pcm.clear();
        int p1 = 0, p2 = 0;
        for (size_t b = 0; b + 9 <= data.size() - off; b += 9)
            decodeBrrBlock (data.data() + off + b, s->pcm, p1, p2);
    }
    if (s->loopStart < 0 && loopAddr < 0 && ! s->pcm.empty())
    {
        const size_t lastBlk = off + ((data.size() - off) / 9 - 1) * 9;
        if (data[lastBlk] & 2) s->loopStart = guessLoopStart (s->pcm);
    }
    s->name = "BRR  " + std::to_string (s->pcm.size()) + " smp" + (s->loops() ? "  loop" : "");
    r.samples.push_back (s);
    return r;
}

//==============================================================================
int guessLoopStart (const std::vector<int16_t>& pcm)
{
    const int len = (int) pcm.size();
    const int M = 32;
    if (len < 3 * M) return 0;

    double best = 1e300;
    int bestK = 0;
    for (int k = M; k <= len - 16; k += 16)
    {
        double e = 0;
        for (int j = 0; j < M; ++j)
        {
            const double dlt = (double) pcm[(size_t) (len - M + j)] - (double) pcm[(size_t) (k - M + j)];
            e += dlt * dlt;
        }
        // mild preference for longer loops
        e *= 1.0 + 0.25 * (double) k / (double) len;
        if (e < best) { best = e; bestK = k; }
    }
    return bestK;
}

bool detectRootNote (const Sample& s, float& midiOut)
{
    const int N = 4096;
    std::vector<double> x;
    x.reserve (N);

    const int len = (int) s.pcm.size();
    if (s.loops() && len - s.loopStart >= 8)
    {
        const int L = len - s.loopStart;
        for (int i = 0; i < N; ++i) x.push_back (s.pcm[(size_t) (s.loopStart + i % L)]);
    }
    else
    {
        if (len < 512) return false;
        const int start = std::min (len / 5, len - 512);
        const int cnt = std::min (N, len - start);
        for (int i = 0; i < cnt; ++i) x.push_back (s.pcm[(size_t) (start + i)]);
    }

    const int n = (int) x.size();
    double mean = 0;
    for (double v : x) mean += v;
    mean /= n;
    for (double& v : x) v -= mean;

    const int W = n / 2;
    const int maxTau = std::min (W, 2000);
    std::vector<double> dfn ((size_t) maxTau + 1, 0.0), cmnd ((size_t) maxTau + 1, 1.0);
    for (int tau = 1; tau <= maxTau; ++tau)
    {
        double sum = 0;
        for (int i = 0; i < W; ++i)
        {
            const double dd = x[(size_t) i] - x[(size_t) (i + tau)];
            sum += dd * dd;
        }
        dfn[(size_t) tau] = sum;
    }
    double running = 0;
    for (int tau = 1; tau <= maxTau; ++tau)
    {
        running += dfn[(size_t) tau];
        cmnd[(size_t) tau] = running > 0 ? dfn[(size_t) tau] * tau / running : 1.0;
    }

    int tauEst = -1;
    for (int tau = 4; tau < maxTau; ++tau)
    {
        if (cmnd[(size_t) tau] < 0.15)
        {
            while (tau + 1 < maxTau && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau]) ++tau;
            tauEst = tau;
            break;
        }
    }
    if (tauEst < 0) return false;

    double t = tauEst;
    if (tauEst > 1 && tauEst < maxTau)
    {
        const double a = cmnd[(size_t) tauEst - 1], b = cmnd[(size_t) tauEst], c = cmnd[(size_t) tauEst + 1];
        const double den = a - 2 * b + c;
        if (std::abs (den) > 1e-12) t += 0.5 * (a - c) / den;
    }

    const double freq = 32000.0 / t;
    const double midi = 69.0 + 12.0 * std::log2 (freq / 440.0);
    if (midi < 12 || midi > 120) return false;
    midiOut = (float) midi;
    return true;
}

void prepareSample (Sample& s)
{
    float m;
    if (detectRootNote (s, m)) { s.rootNote = m; s.rootDetected = true; }
    else                       { s.rootNote = 60.0f; s.rootDetected = false; }
}

} // namespace snes

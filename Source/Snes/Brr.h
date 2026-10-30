#pragma once
// SNES BRR (Bit Rate Reduction) sample decoder.
// Mirrors the S-DSP decode path: 9-byte blocks, 16 nibbles each,
// 4 prediction filters, 15-bit output stored with LSB = 0.

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace snes
{

struct Sample
{
    std::vector<int16_t> pcm;      // decoded, 32 kHz at pitch 0x1000
    int  loopStart = -1;           // sample index, -1 = one-shot
    bool loops() const { return loopStart >= 0 && loopStart < (int) pcm.size(); }

    // metadata
    std::string name;
    int   srcn        = -1;        // directory index (SPC) or -1
    int   address     = 0;         // start address in ARAM / ROM
    float rootNote    = 60.0f;     // detected root (MIDI, fractional)
    bool  rootDetected = false;

    // envelope / echo hints taken from an SPC voice (or defaults)
    int  adsrA = 15, adsrD = 7, adsrSL = 7, adsrSR = 0;
    bool hasEnvelopeHint = false;
};

using SamplePtr = std::shared_ptr<const Sample>;

inline int clamp16 (int v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }

// Decodes one BRR block (9 bytes) appending 16 samples. p1/p2 = filter history.
// Returns header byte.
inline uint8_t decodeBrrBlock (const uint8_t* blk, std::vector<int16_t>& out, int& p1, int& p2, int* clipCount = nullptr)
{
    const uint8_t header = blk[0];
    const int shift  = header >> 4;
    const int filter = header & 0x0C;

    for (int i = 0; i < 16; ++i)
    {
        const uint8_t byte = blk[1 + (i >> 1)];
        int nib = (i & 1) ? (byte & 0x0F) : (byte >> 4);
        nib = (nib ^ 8) - 8; // sign-extend 4 bit

        int s;
        if (shift <= 12) s = (nib * (1 << shift)) >> 1;   // multiply: shifting a negative is UB
        else             s = nib < 0 ? -2048 : 0;

        const int a = p1;          // previous output (already *2 scaled)
        const int b = p2 >> 1;     // output before that, halved

        if (filter >= 8)
        {
            s += a;
            s -= b;
            if (filter == 8) { s += b >> 4;  s += (a * -3) >> 6; }      // filter 2
            else             { s += (a * -13) >> 7; s += (b * 3) >> 4; } // filter 3
        }
        else if (filter)
        {
            s += a >> 1;                                                  // filter 1
            s += (-a) >> 5;
        }

        const int c = clamp16 (s);
        if (clipCount && c != s) ++*clipCount;
        s = (int16_t) (c * 2);

        p2 = p1;
        p1 = s;
        out.push_back ((int16_t) s);
    }
    return header;
}

// Decode a BRR chain from a memory image.
// wrap: addresses wrap at 64K (ARAM). maxBlocks guards against garbage.
inline bool decodeBrr (const uint8_t* mem, size_t memSize, uint32_t start, int64_t loopAddr,
                       Sample& out, bool wrap, int maxBlocks = 8000)
{
    out.pcm.clear();
    out.loopStart = -1;
    int p1 = 0, p2 = 0;

    auto blockAt = [&] (uint32_t addr, uint8_t* tmp) -> bool
    {
        for (int i = 0; i < 9; ++i)
        {
            uint32_t a = addr + (uint32_t) i;
            if (wrap) a &= 0xFFFF;
            if (a >= memSize) return false;
            tmp[i] = mem[a];
        }
        return true;
    };

    uint8_t blk[9];
    uint32_t addr = start;
    bool ended = false, loopFlag = false;
    int blocks = 0;

    while (blocks < maxBlocks)
    {
        if (! blockAt (addr, blk)) return false;
        const uint8_t h = decodeBrrBlock (blk, out.pcm, p1, p2);
        ++blocks;
        if (h & 1) { ended = true; loopFlag = (h & 2) != 0; break; }
        addr += 9;
        if (wrap) addr &= 0xFFFF;
    }

    if (! ended) return false;
    const uint32_t endAddr = addr + 9; // one past last block (unwrapped)

    if (loopFlag && loopAddr >= 0)
    {
        const int64_t rel = loopAddr - (int64_t) start;
        if (rel >= 0 && rel < (int64_t) (endAddr - start) && rel % 9 == 0)
        {
            out.loopStart = (int) (rel / 9) * 16;
        }
        else
        {
            // Loop points into a different chain: decode it and append.
            const int base = (int) out.pcm.size();
            uint32_t la = (uint32_t) loopAddr;
            int extra = 0;
            bool ok = false;
            while (extra < maxBlocks)
            {
                if (! blockAt (la, blk)) break;
                const uint8_t h = decodeBrrBlock (blk, out.pcm, p1, p2);
                ++extra;
                if (h & 1) { ok = true; break; }
                la += 9;
                if (wrap) la &= 0xFFFF;
            }
            if (ok) out.loopStart = base;
            else    out.pcm.resize ((size_t) base);
        }
    }
    return ! out.pcm.empty();
}

} // namespace snes

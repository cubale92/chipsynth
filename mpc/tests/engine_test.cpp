// Drives the MPC engine (mpc_engine()) the way the wrapper does, with real files in a data folder:
// scan, pick the .spc, chord, track switch + reported hints, pick the ROM, sample reassign, state restore.
//   g++ -std=gnu++17 -fsanitize=address,undefined -I<mpc-vst-plugins>/wrapper mpc/tests/engine_test.cpp
//       mpc/src/sfc8_mpc.cpp Source/Snes/Engine.cpp Source/Snes/Loaders.cpp -lpthread
extern "C" {
#include "engine.h"
}
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, ...) do { std::printf ((c) ? "ok   " : "FAIL "); std::printf (__VA_ARGS__); std::printf ("\n"); if (! (c)) ++fails; } while (0)

static void encodeBlock (const int16_t* x, uint8_t* out, bool last, bool loop)
{
    int maxAbs = 0;
    for (int i = 0; i < 16; ++i) maxAbs = std::max (maxAbs, std::abs ((int) x[i]));
    int shift = 0;
    while (shift < 12 && maxAbs > 7 * (1 << shift)) ++shift;
    out[0] = (uint8_t) ((shift << 4) | (loop ? 2 : 0) | (last ? 1 : 0));
    for (int i = 0; i < 16; ++i)
    {
        int nib = std::max (-8, std::min (7, (int) std::lround ((double) x[i] / (double) (1 << shift))));
        if (i & 1) out[1 + i / 2] |= (uint8_t) (nib & 0xF);
        else       out[1 + i / 2] = (uint8_t) ((nib & 0xF) << 4);
    }
}

static void writeWave (uint8_t* mem, int addr, int period, int shape)
{
    std::vector<int16_t> w (1024);
    for (int i = 0; i < 1024; ++i)
    {
        const double ph = (double) (i % period) / period;
        const double v = shape == 0 ? std::sin (2 * M_PI * ph) : shape == 1 ? 2 * ph - 1 : (ph < 0.5 ? 0.8 : -0.8);
        w[(size_t) i] = (int16_t) (11000 * v);
    }
    for (int b = 0; b < 64; ++b) encodeBlock (&w[(size_t) b * 16], mem + addr + b * 9, b == 63, true);
}

static void save (const std::string& path, const std::vector<uint8_t>& d)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    std::fwrite (d.data(), 1, d.size(), f);
    std::fclose (f);
}

static std::string get (const mpc_engine_t* e, void* in, const char* key)
{
    char buf[8192];
    return e->get_param (in, key, buf, sizeof buf) > 0 ? std::string (buf) : std::string ("<none>");
}

static double play (const mpc_engine_t* e, void* in, std::initializer_list<int> notes, int blocks)
{
    int16_t out[256];
    for (int n : notes) { uint8_t m[3] = { 0x90, (uint8_t) n, 100 }; e->midi (in, m, 3); }
    double sum = 0;
    for (int b = 0; b < blocks; ++b)
    {
        e->render (in, out, 128);
        for (int i = 0; i < 256; ++i) sum += (double) out[i] * out[i];
    }
    for (int n : notes) { uint8_t m[3] = { 0x80, (uint8_t) n, 0 }; e->midi (in, m, 3); }
    return std::sqrt (sum / (blocks * 256.0)) / 32768.0;
}

// Render (that's where finished work is picked up) until a key's text satisfies pred, up to ~3 s.
template <typename F> static bool waitFor (const mpc_engine_t* e, void* in, F pred)
{
    int16_t out[256];
    for (int i = 0; i < 300; ++i)
    {
        e->render (in, out, 128);
        if (pred()) return true;
        usleep (10000);
    }
    return false;
}

static std::vector<std::string> drainChanged (void* in)
{
    std::vector<std::string> keys;
    char k[64];
    while (mpc_engine_changed (in, k, sizeof k)) keys.push_back (k);
    return keys;
}

int main (int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "/tmp/sfc8_mpc_test";
    mkdir (dir.c_str(), 0755);
    mkdir ((dir + "/Game").c_str(), 0755);

    // .spc: voices 1-3 play three different samples, voice 1 has ADSR + echo
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
        dsp[k * 0x10 + 5] = (uint8_t) (0x80 | (k == 0 ? 0x0B : 0x0E)); // ADSR on; voice 1: AR 11
        dsp[k * 0x10 + 6] = 0xC4;
        dsp[k * 0x10 + 0] = 90; dsp[k * 0x10 + 1] = 90;
    }
    dsp[0x5D] = 0x02;
    dsp[0x4D] = 0x01; dsp[0x2C] = 40; dsp[0x3C] = 40; dsp[0x7D] = 5; dsp[0x0D] = 70;
    save (dir + "/Game/b_song.spc", spc);

    // ROM: noise with three BRR samples
    std::vector<uint8_t> rom (512 * 1024);
    uint32_t seed = 1;
    for (auto& b : rom) { seed = seed * 1103515245u + 12345u; b = (uint8_t) (seed >> 16); }
    std::memcpy (rom.data() + 0x7FC0, "TEST QUEST ROM       ", 21);
    writeWave (rom.data(), 0x20000, 100, 0);
    writeWave (rom.data(), 0x30000, 80, 1);
    writeWave (rom.data(), 0x41000, 120, 2);
    save (dir + "/a_game.sfc", rom);
    save (dir + "/notes.txt", { 'h', 'i' });

    const mpc_engine_t* e = mpc_engine();
    void* in = e->create (dir.c_str());

    CHECK (waitFor (e, in, [&] { return get (e, in, "file_name").rfind ("Pick a file (2)", 0) == 0; }),
           "scan finds the 2 SNES files, not the .txt: '%s'", get (e, in, "file_name").c_str());
    CHECK (play (e, in, { 60 }, 20) == 0.0, "silent with no file");

    // pick file 2 = Game/b_song.spc (folder sorts after the root's files? sorted folder then name)
    e->set_param (in, "file", "2");
    CHECK (waitFor (e, in, [&] { return get (e, in, "game").find ("Test Quest") != std::string::npos; }),
           "spc loads: game='%s' file='%s'", get (e, in, "game").c_str(), get (e, in, "file_name").c_str());
    auto ch = drainChanged (in);
    std::string chs; for (auto& k : ch) chs += k + " ";
    CHECK (chs.find ("attack") != std::string::npos && chs.find ("echo") != std::string::npos && chs.find ("root") != std::string::npos,
           "load reports hint keys to the host: %s", chs.c_str());
    CHECK (get (e, in, "attack") == "11" && get (e, in, "echo") == "1" && get (e, in, "edl") == "5" && get (e, in, "fir") == "0",
           "track 1 takes the voice's ADSR + echo (AR=%s echo=%s EDL=%s FIR=%s)", get (e, in, "attack").c_str(),
           get (e, in, "echo").c_str(), get (e, in, "edl").c_str(), get (e, in, "fir").c_str());
    for (int t = 1; t <= 4; ++t)
    {
        char k[16]; std::snprintf (k, sizeof k, "trk_%d", t);
        std::printf ("     %s = '%s'  on=%s\n", k, get (e, in, k).c_str(), get (e, in, (std::string (k) + "_on").c_str()).c_str());
    }
    std::printf ("     sample_name = '%s'  root=%s (%s)\n", get (e, in, "sample_name").c_str(), get (e, in, "root").c_str(),
                 get (e, in, "root_display").c_str());

    const double chord = play (e, in, { 60, 64, 67, 71 }, 200);
    CHECK (chord > 0.02, "4-note chord sounds (rms %.3f)", chord);

    // pitch: sample 1 period 64 -> 500 Hz; play its root and count zero crossings
    {
        e->set_param (in, "echo", "0");
        const int root = std::atoi (get (e, in, "root").c_str());
        uint8_t on[3] = { 0x90, (uint8_t) root, 127 }; e->midi (in, on, 3);
        int16_t out[256]; std::vector<int16_t> L;
        for (int b = 0; b < 400; ++b) { e->render (in, out, 128); for (int i = 0; i < 128; ++i) L.push_back (out[2 * i]); }
        uint8_t off[3] = { 0x80, (uint8_t) root, 0 }; e->midi (in, off, 3);
        int zc = 0;
        for (size_t i = 8001; i < 48101; ++i) if (L[i - 1] < 0 && L[i] >= 0) ++zc;
        const double hz = zc / (40100.0 / 44100.0);
        const double expect = 440.0 * std::pow (2.0, (root - 69) / 12.0);   // in tune: root + fine correct the sample's detune
        CHECK (std::fabs (hz - expect) < 6, "root key plays in tune with A440: %.1f Hz (expect %.1f)", hz, expect);
        e->set_param (in, "echo", "1");
    }

    // tap track 3 tile
    e->set_param (in, "trk_3", "1");
    CHECK (get (e, in, "track") == "3" && get (e, in, "trk_3_on") == "1" && get (e, in, "trk_1_on") == "0",
           "tapping track 3 selects it (track=%s)", get (e, in, "track").c_str());
    CHECK (get (e, in, "attack") == "14" && get (e, in, "echo") == "0", "track 3 takes its own voice settings (AR=%s echo=%s)",
           get (e, in, "attack").c_str(), get (e, in, "echo").c_str());
    CHECK (play (e, in, { 60, 63, 67 }, 100) > 0.02, "track 3 chord sounds");

    // state of an SPC project, then a fresh instance restores it from the chunk
    e->set_param (in, "release", "1234");
    const std::string state = get (e, in, "state");
    std::printf ("     state:\n%s", state.c_str());

    // switch to the ROM
    e->set_param (in, "file", "1");
    CHECK (waitFor (e, in, [&] { return get (e, in, "game") == "TEST QUEST ROM"; }), "ROM loads: '%s'", get (e, in, "game").c_str());
    CHECK (get (e, in, "trk_1").find ("#1") != std::string::npos && get (e, in, "trk_4") == "4  -",
           "ROM fills tracks with found samples (1='%s' 4='%s')", get (e, in, "trk_1").c_str(), get (e, in, "trk_4").c_str());
    e->set_param (in, "track", "4");
    e->set_param (in, "sample", "2");
    CHECK (get (e, in, "trk_4").find ("#2") != std::string::npos, "sample 2 assigned to empty track 4: '%s'", get (e, in, "trk_4").c_str());
    CHECK (play (e, in, { 48, 55, 60 }, 100) > 0.02, "ROM track 4 chord sounds");
    e->set_param (in, "sample", "1499");
    CHECK (get (e, in, "trk_4").find ("#3") != std::string::npos, "sample past the end clamps to the last: '%s'", get (e, in, "trk_4").c_str());

    // restore the SPC state into a second instance
    void* b = e->create (dir.c_str());
    e->set_param (b, "state", state.c_str());
    CHECK (waitFor (e, b, [&] { return get (e, b, "game").find ("Test Quest") != std::string::npos; }),
           "restored instance reloads the .spc by name: '%s'", get (e, b, "file_name").c_str());
    CHECK (get (e, b, "track") == "3" && get (e, b, "release") == "1234" && get (e, b, "attack") == "14",
           "restored track/release/AR = %s/%s/%s", get (e, b, "track").c_str(), get (e, b, "release").c_str(), get (e, b, "attack").c_str());
    CHECK (play (e, b, { 60, 64, 67 }, 100) > 0.02, "restored instance plays");

    // a project whose file is gone keeps running
    void* c = e->create (dir.c_str());
    std::string gone = state;
    gone.replace (gone.find ("b_song.spc"), 10, "missing.spc");
    e->set_param (c, "state", gone.c_str());
    CHECK (waitFor (e, c, [&] { return get (e, c, "status").rfind ("Not found", 0) == 0; }), "missing file: status '%s'", get (e, c, "status").c_str());
    CHECK (play (e, c, { 60 }, 20) == 0.0, "missing file: silent, no crash");

    e->set_param (in, "rescan", "1");
    CHECK (waitFor (e, in, [&] { return get (e, in, "status").find ("Scanning") == std::string::npos; }) && get (e, in, "file") == "1",
           "rescan keeps the loaded ROM selected (file=%s)", get (e, in, "file").c_str());

    e->destroy (c);
    e->destroy (b);
    e->destroy (in);
    std::printf (fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}

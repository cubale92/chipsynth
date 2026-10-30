// SFC-8 for MPC OS: the S-DSP sample synth as a block-rendering engine for the
// mpc-vst-plugins VST2 wrapper (wrapper/engine.h).
//
// Files (.spc, .sfc/.smc, .brr) live in the plugin's "SNES" folder (MODULE_SUBDIR), or in an
// "SFC-8" folder on a card (/media/<card>/SFC-8). Scanning and loading run on a worker thread;
// render() picks up finished work with a trylock, so the audio thread never waits for the disk.
//
// Locks: the wrapper serialises create/set/get/midi/render per instance (its own lock). `mu`
// below only guards what the worker shares with them, and is never held around I/O.

extern "C" {
#include "engine.h"
}
#include "../../Source/Snes/Engine.h"
#include "../../Source/Snes/Loaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <pthread.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{
constexpr int kBlock = 128;
constexpr int kMaxFiles = 1023;
constexpr int kScanDepth = 3;

const char* const kFirNames[] = { "From file", "Flat", "Lowpass", "Mellow", "Band" };
const int kFirPresets[4][8] = {
    { 127, 0, 0, 0, 0, 0, 0, 0 },
    { -1, 8, 23, 36, 36, 23, 8, -1 },
    { 12, 33, 43, 43, 19, -2, -13, -7 },
    { 52, 51, 0, -39, -27, 1, -4, -21 },
};

// Engine-side copies of the plain (non-text) parameters, in the units params.json uses.
struct Values
{
    float volume = -3, root = 60, fine = 0;
    int attack = 15, decay = 7, sl = 7, sr = 0;
    float release = 250;
    int echo = 0, edl = 4, efb = 64, evol = 48, fir = 1;
    int poly = 1, pitchLimit = 1, glide = 0;
    float glideTime = 80, velocity = 70;
    int bend = 2;
    float vibrato = 30, pan = 0;
};

// Keys the engine may change by itself (a track's SPC hints); reported through mpc_engine_changed().
const char* const kHintKeys[] = { "root", "fine", "attack", "decay", "sl", "sr", "echo", "edl", "efb", "evol", "fir",
                                  "track", "sample", "file" };
constexpr int kNumHintKeys = (int) (sizeof (kHintKeys) / sizeof (kHintKeys[0]));

bool hasExt (const std::string& name, std::initializer_list<const char*> exts)
{
    const auto dot = name.find_last_of ('.');
    if (dot == std::string::npos) return false;
    const std::string e = name.substr (dot);
    for (auto* x : exts) if (! strcasecmp (e.c_str(), x)) return true;
    return false;
}

bool isSnesFile (const std::string& n) { return hasExt (n, { ".spc", ".sfc", ".smc", ".brr", ".fig", ".swc" }); }

std::string baseName (const std::string& p)
{
    const auto s = p.find_last_of ('/');
    return s == std::string::npos ? p : p.substr (s + 1);
}

void walk (const std::string& dir, int depth, std::vector<std::string>& out)
{
    if (depth < 0 || (int) out.size() >= kMaxFiles) return;
    DIR* d = opendir (dir.c_str());
    if (! d) return;
    std::vector<std::string> subdirs;
    while (dirent* de = readdir (d))
    {
        if (de->d_name[0] == '.') continue;
        const std::string path = dir + "/" + de->d_name;
        struct stat st;
        if (lstat (path.c_str(), &st) != 0 || S_ISLNK (st.st_mode)) continue;   // never follow links
        if (S_ISREG (st.st_mode) && isSnesFile (de->d_name)) out.push_back (path);
        else if (S_ISDIR (st.st_mode)) subdirs.push_back (path);
    }
    closedir (d);
    for (auto& s : subdirs) walk (s, depth - 1, out);
}

// Sorted folder-then-name, so a Q-Link position always means the same file.
bool fileOrder (const std::string& a, const std::string& b)
{
    const auto da = a.substr (0, a.find_last_of ('/')), db = b.substr (0, b.find_last_of ('/'));
    if (da != db) return strcasecmp (da.c_str(), db.c_str()) < 0;
    return strcasecmp (baseName (a).c_str(), baseName (b).c_str()) < 0;
}

int put (char* buf, int n, const std::string& s)
{
    if (! buf || n <= 0) return 0;
    std::snprintf (buf, (size_t) n, "%s", s.c_str());
    return (int) std::strlen (buf) > 0 ? (int) std::strlen (buf) : 1;   // an empty text still answers
}

std::string noteName (float midi)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int n = (int) std::lround (midi);
    if (n < 0 || n > 127) return "-";
    return std::string (names[n % 12]) + std::to_string (n / 12 - 1);   // MIDI 60 = C4
}

std::string fmt (const char* f, double v)
{
    char b[64];
    std::snprintf (b, sizeof b, f, v);
    return b;
}

//==============================================================================
struct Loaded
{
    snes::LoadResult result;
    std::string path;
};

struct Inst
{
    // ---- shared with the worker (guarded by mu) ----
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread {};
    bool threadOn = false;
    bool quit = false;
    bool scanReq = true;                      // scan when the plugin opens
    std::string loadReq;                      // a path to load, empty = none
    bool loadReqRestore = false;
    std::vector<std::string> sharedFiles;     // a finished scan, handed over in render()
    bool filesReady = false;
    std::unique_ptr<Loaded> pending;          // a finished load
    bool pendingRestore = false;
    std::string loadError;
    bool busyScan = false, busyLoad = false;
    std::vector<std::unique_ptr<Loaded>> trash;   // old results, freed by the worker

    // ---- engine side (wrapper lock only) ----
    std::string dataDir;
    snes::Engine engine;
    Values v;
    std::vector<std::string> files;           // what the "file" param indexes (1-based)
    bool scanned = false;
    int fileIdx = 0;
    std::unique_ptr<Loaded> cur;
    int track = 0;                            // 0..7
    int slots[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    int sampleRaw[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };   // the host's value; clamped when used
    std::array<int, 8> fileFir {{ 127, 0, 0, 0, 0, 0, 0, 0 }};
    std::string status = "Scanning...";
    unsigned rev = 1;
    uint32_t changed = 0;                     // bit per kHintKeys entry

    // restore (from the state chunk) until the file is found and loaded
    std::string wantFile;
    int wantSlots[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    bool restoring = false;

    std::vector<float> L, R;
};

void markChanged (Inst* s, const char* key)
{
    for (int i = 0; i < kNumHintKeys; ++i)
        if (! std::strcmp (kHintKeys[i], key)) s->changed |= 1u << i;
}

void bump (Inst* s) { ++s->rev; }

//==============================================================================
void* workerMain (void* arg)
{
    auto* s = (Inst*) arg;
    pthread_mutex_lock (&s->mu);
    for (;;)
    {
        while (! s->quit && ! s->scanReq && s->loadReq.empty() && s->trash.empty())
            pthread_cond_wait (&s->cv, &s->mu);
        if (s->quit) break;

        // free old results outside the lock
        std::vector<std::unique_ptr<Loaded>> toFree;
        toFree.swap (s->trash);

        const bool doScan = s->scanReq;
        s->scanReq = false;
        const std::string path = s->loadReq;
        const bool restore = s->loadReqRestore;
        s->loadReq.clear();
        s->busyScan = doScan;
        s->busyLoad = ! path.empty();
        pthread_mutex_unlock (&s->mu);

        toFree.clear();

        if (doScan)
        {
            std::vector<std::string> found;
            walk (s->dataDir, kScanDepth, found);
            if (DIR* m = opendir ("/media"))            // collections on a card or stick
            {
                while (dirent* de = readdir (m))
                    if (de->d_name[0] != '.')
                        walk (std::string ("/media/") + de->d_name + "/SFC-8", kScanDepth, found);
                closedir (m);
            }
            std::sort (found.begin(), found.end(), fileOrder);
            found.erase (std::unique (found.begin(), found.end()), found.end());
            if ((int) found.size() > kMaxFiles) found.resize (kMaxFiles);

            pthread_mutex_lock (&s->mu);
            s->sharedFiles.swap (found);
            s->filesReady = true;
            s->busyScan = false;
            pthread_mutex_unlock (&s->mu);
        }

        if (! path.empty())
        {
            auto loaded = std::make_unique<Loaded>();
            loaded->path = path;
            std::string err;

            std::ifstream f (path, std::ios::binary);
            std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
            if (bytes.empty()) err = "Could not read " + baseName (path);
            else
            {
                if (hasExt (path, { ".spc" }))      loaded->result = snes::loadSpc (bytes);
                else if (hasExt (path, { ".brr" })) loaded->result = snes::loadRawBrr (bytes);
                else                                 loaded->result = snes::scanRom (bytes);
                if (loaded->result.samples.empty())
                    err = baseName (path) + ": " + (loaded->result.error.empty() ? "nothing found" : loaded->result.error);
                else
                    for (auto& sp : loaded->result.samples)          // root detection, before anyone plays them
                        snes::prepareSample (*const_cast<snes::Sample*> (sp.get()));
            }

            pthread_mutex_lock (&s->mu);
            if (err.empty()) { s->pending = std::move (loaded); s->pendingRestore = restore; s->loadError.clear(); }
            else             s->loadError = err;
            s->busyLoad = false;
            pthread_mutex_unlock (&s->mu);
        }
        pthread_mutex_lock (&s->mu);
    }
    pthread_mutex_unlock (&s->mu);
    return nullptr;
}

void requestLoad (Inst* s, const std::string& path, bool restore)
{
    pthread_mutex_lock (&s->mu);
    s->loadReq = path;
    s->loadReqRestore = restore;
    pthread_cond_signal (&s->cv);
    pthread_mutex_unlock (&s->mu);
}

void requestScan (Inst* s)
{
    pthread_mutex_lock (&s->mu);
    s->scanReq = true;
    pthread_cond_signal (&s->cv);
    pthread_mutex_unlock (&s->mu);
}

//==============================================================================
const snes::Sample* trackSample (const Inst* s, int t)
{
    if (! s->cur) return nullptr;
    const int idx = s->slots[t];
    if (idx < 0 || idx >= (int) s->cur->result.samples.size()) return nullptr;
    return s->cur->result.samples[(size_t) idx].get();
}

// Play the current track's sample; with hints, take its root and (SPC) the voice's ADSR / echo.
void applyTrack (Inst* s, bool hints)
{
    const snes::Sample* smp = trackSample (s, s->track);
    s->engine.setSample (smp);
    markChanged (s, "track");
    markChanged (s, "sample");
    bump (s);
    if (! smp || ! hints) return;

    const auto& r = s->cur->result;
    const int rootInt = std::max (0, std::min (127, (int) std::lround (smp->rootNote)));
    s->v.root = (float) rootInt;
    s->v.fine = std::max (-100.0f, std::min (100.0f, ((float) rootInt - smp->rootNote) * 100.0f));   // corrects a sharp/flat sample
    markChanged (s, "root"); markChanged (s, "fine");

    if (smp->hasEnvelopeHint)
    {
        s->v.attack = smp->adsrA; s->v.decay = smp->adsrD; s->v.sl = smp->adsrSL; s->v.sr = smp->adsrSR;
        markChanged (s, "attack"); markChanged (s, "decay"); markChanged (s, "sl"); markChanged (s, "sr");
    }

    if (r.kind == snes::LoadResult::Kind::Spc)
    {
        const auto& vi = r.voices[(size_t) s->track];
        if (vi.valid && vi.sampleIndex == s->slots[s->track])
        {
            const bool echo = r.echo.present && vi.echoOn;
            s->v.echo = echo ? 1 : 0;
            markChanged (s, "echo");
            if (echo)
            {
                s->v.edl = r.echo.delay;
                s->v.efb = r.echo.feedback;
                s->v.evol = std::max (-128, std::min (127, (std::abs (r.echo.volL) + std::abs (r.echo.volR)) / 2));
                s->v.fir = 0;
                markChanged (s, "edl"); markChanged (s, "efb"); markChanged (s, "evol"); markChanged (s, "fir");
            }
        }
    }
}

// A finished load arrives (render thread, wrapper lock held).
void takeLoaded (Inst* s, std::unique_ptr<Loaded> next, bool restore)
{
    std::unique_ptr<Loaded> old = std::move (s->cur);
    s->cur = std::move (next);
    const auto& r = s->cur->result;
    const int n = (int) r.samples.size();

    if (restore)
    {
        for (int t = 0; t < 8; ++t) s->slots[t] = (s->wantSlots[t] >= 0 && s->wantSlots[t] < n) ? s->wantSlots[t] : -1;
    }
    else
    {
        for (int t = 0; t < 8; ++t) s->slots[t] = -1;
        if (r.kind == snes::LoadResult::Kind::Spc)
        {
            for (int t = 0; t < 8; ++t)
                if (r.voices[(size_t) t].valid) s->slots[t] = r.voices[(size_t) t].sampleIndex;
            s->track = 0;
            for (int t = 0; t < 8; ++t)
                if (r.voices[(size_t) t].valid && r.voices[(size_t) t].looksActive) { s->track = t; break; }
        }
        else
        {
            for (int t = 0; t < 8 && t < n; ++t) s->slots[t] = t;
            s->track = 0;
        }
    }
    for (int i = 0; i < 8; ++i) s->fileFir[(size_t) i] = r.echo.fir[(size_t) i];
    for (int t = 0; t < 8; ++t) s->sampleRaw[t] = s->slots[t] + 1;

    applyTrack (s, ! restore);
    s->restoring = false;
    s->status = baseName (s->cur->path) + ": " + std::to_string (n) + " samples";
    bump (s);

    if (old)   // the worker frees it, not the audio thread
    {
        pthread_mutex_lock (&s->mu);
        s->trash.push_back (std::move (old));
        pthread_cond_signal (&s->cv);
        pthread_mutex_unlock (&s->mu);
    }
}

void pickUp (Inst* s)
{
    if (pthread_mutex_trylock (&s->mu) != 0) return;
    std::unique_ptr<Loaded> next;
    bool restore = false;
    bool gotFiles = false;
    std::string err;
    if (s->pending) { next = std::move (s->pending); restore = s->pendingRestore; }
    if (s->filesReady) { s->files.swap (s->sharedFiles); s->filesReady = false; gotFiles = true; }
    if (! s->loadError.empty()) { err.swap (s->loadError); }
    const bool busy = s->busyLoad || ! s->loadReq.empty();
    pthread_mutex_unlock (&s->mu);

    if (gotFiles)
    {
        s->scanned = true;
        // keep the same file selected by name after a rescan / find the one a project asked for
        const std::string want = ! s->wantFile.empty() ? s->wantFile : (s->cur ? baseName (s->cur->path) : std::string());
        s->fileIdx = 0;
        for (size_t i = 0; i < s->files.size() && ! want.empty(); ++i)
            if (baseName (s->files[i]) == want) { s->fileIdx = (int) i + 1; break; }
        if (! s->wantFile.empty())
        {
            if (s->fileIdx > 0) requestLoad (s, s->files[(size_t) s->fileIdx - 1], true);
            else                s->status = "Not found: " + s->wantFile;
            s->wantFile.clear();
        }
        else if (s->cur)
        {
            s->status = baseName (s->cur->path) + ": " + std::to_string (s->cur->result.samples.size()) + " samples";
        }
        else
        {
            s->status = s->files.empty() ? "No files: put .spc/.sfc in the SNES folder"
                                         : std::to_string (s->files.size()) + " files - pick one";
        }
        markChanged (s, "file");
        bump (s);
    }
    if (next) takeLoaded (s, std::move (next), restore);
    if (! err.empty()) { s->status = err; s->restoring = false; bump (s); }
    (void) busy;
}

//==============================================================================
snes::EngineParams engineParams (const Inst* s)
{
    const Values& v = s->v;
    snes::EngineParams p;
    p.rootNote = v.root;
    p.fineCents = v.fine;
    p.a = v.attack; p.d = v.decay; p.sl = v.sl; p.sr = v.sr;
    p.releaseMs = v.release;
    p.polyphony = v.poly == 0 ? 8 : (v.poly == 1 ? 16 : 32);
    p.hwPitchLimit = v.pitchLimit != 0;
    p.echoOn = v.echo != 0;
    p.echoDelay = v.edl; p.echoFeedback = v.efb; p.echoVolume = v.evol;
    if (v.fir <= 0) for (int i = 0; i < 8; ++i) p.fir[(size_t) i] = s->fileFir[(size_t) i];
    else            for (int i = 0; i < 8; ++i) p.fir[(size_t) i] = kFirPresets[std::min (v.fir, 4) - 1][i];
    p.velocitySens = v.velocity / 100.0f;
    p.vibratoCents = v.vibrato;
    p.bendRange = (float) v.bend;
    p.glide = v.glide != 0;
    p.glideMs = v.glideTime;
    p.pan = v.pan / 100.0f;
    p.gain = std::pow (10.0f, v.volume / 20.0f);
    return p;
}

int clampi (const char* val, int lo, int hi) { return std::max (lo, std::min (hi, (int) std::lround (std::atof (val)))); }
float clampf (const char* val, float lo, float hi) { return std::max (lo, std::min (hi, (float) std::atof (val))); }

// --- plain parameter table: key <-> Values field ------------------------------------------------
struct NumParam { const char* key; float lo, hi; bool isInt; float Values::* f; int Values::* i; };
const NumParam kNum[] = {
    { "volume",     -36, 12,   false, &Values::volume,    nullptr },
    { "root",         0, 127,  true,  &Values::root,      nullptr },
    { "fine",      -100, 100,  false, &Values::fine,      nullptr },
    { "attack",       0, 15,   true,  nullptr, &Values::attack },
    { "decay",        0, 7,    true,  nullptr, &Values::decay },
    { "sl",           0, 7,    true,  nullptr, &Values::sl },
    { "sr",           0, 31,   true,  nullptr, &Values::sr },
    { "release",      0, 5000, false, &Values::release,   nullptr },
    { "echo",         0, 1,    true,  nullptr, &Values::echo },
    { "edl",          0, 15,   true,  nullptr, &Values::edl },
    { "efb",       -128, 127,  true,  nullptr, &Values::efb },
    { "evol",      -128, 127,  true,  nullptr, &Values::evol },
    { "fir",          0, 4,    true,  nullptr, &Values::fir },
    { "poly",         0, 2,    true,  nullptr, &Values::poly },
    { "pitch_limit",  0, 1,    true,  nullptr, &Values::pitchLimit },
    { "glide",        0, 1,    true,  nullptr, &Values::glide },
    { "glide_time",   1, 1000, false, &Values::glideTime, nullptr },
    { "velocity",     0, 100,  false, &Values::velocity,  nullptr },
    { "bend",         0, 24,   true,  nullptr, &Values::bend },
    { "vibrato",      0, 100,  false, &Values::vibrato,   nullptr },
    { "pan",       -100, 100,  false, &Values::pan,       nullptr },
};

const NumParam* findNum (const char* key)
{
    for (auto& p : kNum) if (! std::strcmp (p.key, key)) return &p;
    return nullptr;
}

std::string numValue (const Inst* s, const NumParam& p)
{
    if (p.isInt) return std::to_string (p.i ? s->v.*(p.i) : (int) std::lround (s->v.*(p.f)));
    return fmt ("%g", s->v.*(p.f));
}

void setNum (Inst* s, const NumParam& p, const char* val)
{
    if (p.i) s->v.*(p.i) = clampi (val, (int) p.lo, (int) p.hi);
    else if (p.isInt) s->v.*(p.f) = (float) clampi (val, (int) p.lo, (int) p.hi);
    else s->v.*(p.f) = clampf (val, p.lo, p.hi);
}

// --- state chunk: "SFC8 1" then key=value lines ----------------------------------------------------
std::string makeState (const Inst* s)
{
    std::string out = "SFC8 1\n";
    std::string file = s->cur ? baseName (s->cur->path) : s->wantFile;
    out += "file=" + file + "\n";
    out += "track=" + std::to_string (s->track) + "\n";
    out += "slots=";
    for (int t = 0; t < 8; ++t) out += std::to_string (s->cur ? s->slots[t] : s->wantSlots[t]) + (t < 7 ? "," : "\n");
    for (auto& p : kNum) out += std::string (p.key) + "=" + numValue (s, p) + "\n";
    return out;
}

void applyState (Inst* s, const char* text)
{
    if (std::strncmp (text, "SFC8 1\n", 7) != 0) return;
    const char* p = text + 7;
    std::string file;
    while (*p)
    {
        const char* nl = std::strchr (p, '\n');
        std::string line (p, nl ? (size_t) (nl - p) : std::strlen (p));
        p = nl ? nl + 1 : p + line.size();
        const auto eq = line.find ('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr (0, eq), val = line.substr (eq + 1);
        if (k == "file") file = val;
        else if (k == "track") s->track = clampi (val.c_str(), 0, 7);
        else if (k == "slots")
        {
            int t = 0;
            for (const char* q = val.c_str(); *q && t < 8; ++t)
            {
                s->wantSlots[t] = std::atoi (q);
                const char* c = std::strchr (q, ',');
                if (! c) break;
                q = c + 1;
            }
        }
        else if (auto* np = findNum (k.c_str())) setNum (s, *np, val.c_str());
    }

    s->engine.setSample (nullptr);
    if (file.empty()) return;
    s->restoring = true;
    if (s->scanned)
    {
        s->fileIdx = 0;
        for (size_t i = 0; i < s->files.size(); ++i)
            if (baseName (s->files[i]) == file) { s->fileIdx = (int) i + 1; break; }
        if (s->fileIdx > 0) requestLoad (s, s->files[(size_t) s->fileIdx - 1], true);
        else { s->status = "Not found: " + file; s->restoring = false; }
    }
    else
    {
        s->wantFile = file;   // picked up when the scan finishes
    }
    s->status = "Loading " + file + "...";
    bump (s);
}

//==============================================================================
std::string trackLabel (const Inst* s, int t)
{
    std::string label = std::to_string (t + 1);
    const snes::Sample* smp = trackSample (s, t);
    if (! smp) return label + "  -";
    if (smp->srcn >= 0)
    {
        char b[12];
        std::snprintf (b, sizeof b, "%02X", smp->srcn & 0xFF);
        label += std::string ("  SRC ") + b;
    }
    else
    {
        label += "  #" + std::to_string (s->slots[t] + 1);
    }
    if (s->cur && s->cur->result.kind == snes::LoadResult::Kind::Spc)
    {
        const auto& vi = s->cur->result.voices[(size_t) t];
        if (vi.valid && vi.sampleIndex == s->slots[t] && ! vi.looksActive) label += " (off)";
    }
    return label;
}

std::string sampleName (const Inst* s)
{
    const snes::Sample* smp = trackSample (s, s->track);
    if (! smp) return s->cur ? "Track " + std::to_string (s->track + 1) + ": empty" : "-";
    std::string n = "#" + std::to_string (s->slots[s->track] + 1) + "/" + std::to_string (s->cur->result.samples.size())
                    + "  " + smp->name;
    n += "  root " + noteName (smp->rootNote);
    return n;
}

//==============================================================================
void* create (const char* dataDir)
{
    auto* s = new Inst();
    pthread_mutex_init (&s->mu, nullptr);
    pthread_cond_init (&s->cv, nullptr);
    s->dataDir = dataDir ? dataDir : ".";
    s->engine.prepare (44100.0);
    s->L.assign (kBlock, 0.0f);
    s->R.assign (kBlock, 0.0f);
    s->threadOn = pthread_create (&s->thread, nullptr, workerMain, s) == 0;
    return s;
}

void destroy (void* inst)
{
    auto* s = (Inst*) inst;
    pthread_mutex_lock (&s->mu);
    s->quit = true;
    pthread_cond_signal (&s->cv);
    pthread_mutex_unlock (&s->mu);
    if (s->threadOn) pthread_join (s->thread, nullptr);
    s->engine.setSample (nullptr);
    pthread_cond_destroy (&s->cv);
    pthread_mutex_destroy (&s->mu);
    delete s;
}

void midi (void* inst, const uint8_t* m, int len)
{
    auto* s = (Inst*) inst;
    if (len < 1) return;
    const int st = m[0] & 0xF0;
    const int d1 = len > 1 ? m[1] & 0x7F : 0, d2 = len > 2 ? m[2] & 0x7F : 0;
    switch (st)
    {
        case 0x90: if (d2 > 0) s->engine.noteOn (d1, d2); else s->engine.noteOff (d1); break;
        case 0x80: s->engine.noteOff (d1); break;
        case 0xE0: s->engine.pitchBend (d1 | (d2 << 7)); break;
        case 0xB0:
            if (d1 == 1)        s->engine.modWheel ((float) d2 / 127.0f);
            else if (d1 == 64)  s->engine.sustainPedal (d2 >= 64);
            else if (d1 == 120) s->engine.allNotesOff (true);
            else if (d1 == 123) s->engine.allNotesOff (false);
            break;
        default: break;
    }
}

void selectTrack (Inst* s, int t)
{
    t = std::max (0, std::min (7, t));
    if (t == s->track) return;
    s->track = t;
    s->engine.allNotesOff (false);
    applyTrack (s, true);
}

void setSampleIdx (Inst* s, int oneBased)
{
    s->sampleRaw[s->track] = std::max (0, std::min (1499, oneBased));
    if (! s->cur) return;
    const int n = (int) s->cur->result.samples.size();
    const int idx = std::max (1, std::min (n, s->sampleRaw[s->track])) - 1;
    if (idx == s->slots[s->track]) return;
    s->slots[s->track] = idx;
    s->engine.allNotesOff (true);
    applyTrack (s, true);
}

void setParam (void* inst, const char* key, const char* val)
{
    auto* s = (Inst*) inst;
    if (! key || ! val) return;

    if (auto* np = findNum (key)) { setNum (s, *np, val); return; }

    if (! std::strcmp (key, "file"))
    {
        const int before = std::min (s->fileIdx, (int) s->files.size());
        s->fileIdx = std::max (0, std::min (kMaxFiles, (int) std::lround (std::atof (val))));
        const int idx = std::min (s->fileIdx, (int) s->files.size());   // past the end = the last file
        if (idx == before) return;
        s->wantFile.clear();
        if (idx > 0)
        {
            requestLoad (s, s->files[(size_t) idx - 1], false);
            s->status = "Loading " + baseName (s->files[(size_t) idx - 1]) + "...";
        }
        bump (s);
    }
    else if (! std::strcmp (key, "track"))       selectTrack (s, (int) std::lround (std::atof (val)) - 1);
    else if (! std::strncmp (key, "trk_", 4) && ! std::strstr (key, "_on"))
    {
        if (std::atof (val) > 0.5) selectTrack (s, std::atoi (key + 4) - 1);   // a tap that turns the tile on
    }
    else if (! std::strcmp (key, "sample"))      setSampleIdx (s, (int) std::lround (std::atof (val)));
    else if (! std::strcmp (key, "rescan"))      { if (std::atof (val) > 0.5) { requestScan (s); s->status = "Scanning..."; bump (s); } }
    else if (! std::strcmp (key, "state"))       applyState (s, val);
}

int getParam (void* inst, const char* key, char* buf, int n)
{
    auto* s = (Inst*) inst;
    if (! key) return 0;

    if (auto* np = findNum (key)) return put (buf, n, numValue (s, *np));

    if (! std::strcmp (key, "display_rev")) return put (buf, n, std::to_string (s->rev));
    if (! std::strcmp (key, "state"))       return put (buf, n, makeState (s));
    if (! std::strcmp (key, "file"))        return put (buf, n, std::to_string (s->fileIdx));
    if (! std::strcmp (key, "track"))       return put (buf, n, std::to_string (s->track + 1));
    if (! std::strcmp (key, "sample"))      return put (buf, n, std::to_string (s->sampleRaw[s->track]));
    if (! std::strcmp (key, "rescan"))      return put (buf, n, "0");

    if (! std::strcmp (key, "file_name"))
    {
        if (! s->scanned) return put (buf, n, "Scanning...");
        if (s->files.empty()) return put (buf, n, "No .spc / .sfc files");
        const int idx = std::min (s->fileIdx, (int) s->files.size());
        if (idx <= 0) return put (buf, n, "Pick a file (" + std::to_string (s->files.size()) + ")");
        return put (buf, n, std::to_string (idx) + "/" + std::to_string (s->files.size()) + "  "
                                + baseName (s->files[(size_t) idx - 1]));
    }
    if (! std::strcmp (key, "game"))
    {
        if (! s->cur) return put (buf, n, "-");
        const auto& r = s->cur->result;
        std::string t = r.gameTitle.empty() ? baseName (s->cur->path) : r.gameTitle;
        if (! r.songTitle.empty()) t += " - " + r.songTitle;
        return put (buf, n, t);
    }
    if (! std::strcmp (key, "sample_name")) return put (buf, n, sampleName (s));
    if (! std::strcmp (key, "status"))      return put (buf, n, s->status);
    if (! std::strncmp (key, "trk_", 4))
    {
        const int t = std::atoi (key + 4) - 1;
        if (t < 0 || t > 7) return 0;
        if (std::strstr (key, "_on")) return put (buf, n, t == s->track ? "1" : "0");
        return put (buf, n, trackLabel (s, t));
    }
    if (! std::strcmp (key, "file_display"))   return getParam (inst, "file_name", buf, n);
    if (! std::strcmp (key, "sample_display")) return put (buf, n, sampleName (s));
    if (! std::strcmp (key, "track_display"))  return put (buf, n, "Track " + std::to_string (s->track + 1));
    if (! std::strcmp (key, "root_display"))   return put (buf, n, noteName (s->v.root));
    if (! std::strcmp (key, "volume_display")) return put (buf, n, fmt ("%.1f dB", s->v.volume));
    if (! std::strcmp (key, "pan_display"))
    {
        const int p = (int) std::lround (s->v.pan);
        return put (buf, n, p == 0 ? std::string ("C") : (p < 0 ? "L" + std::to_string (-p) : "R" + std::to_string (p)));
    }
    if (! std::strcmp (key, "release_display"))
        return put (buf, n, s->v.release < 0.5f ? std::string ("HW") : fmt ("%.0f ms", s->v.release));
    return 0;
}

void render (void* inst, int16_t* out, int frames)
{
    auto* s = (Inst*) inst;
    pickUp (s);
    s->engine.setParams (engineParams (s));

    frames = std::max (0, std::min (kBlock, frames));
    s->engine.render (s->L.data(), s->R.data(), frames);
    for (int i = 0; i < frames; ++i)
    {
        const float l = std::max (-1.0f, std::min (1.0f, s->L[(size_t) i]));
        const float r = std::max (-1.0f, std::min (1.0f, s->R[(size_t) i]));
        out[2 * i]     = (int16_t) std::lround (l * 32767.0f);
        out[2 * i + 1] = (int16_t) std::lround (r * 32767.0f);
    }
}

const mpc_engine_t kEngine = { create, destroy, midi, setParam, getParam, render, nullptr, nullptr };
} // namespace

extern "C" const mpc_engine_t* mpc_engine (void) { return &kEngine; }

// Report the values a track change set by itself, so MPC's screen and Q-Links follow.
extern "C" int mpc_engine_changed (void* inst, char* key, int len)
{
    auto* s = (Inst*) inst;
    for (int i = 0; i < kNumHintKeys; ++i)
    {
        if (s->changed & (1u << i))
        {
            s->changed &= ~(1u << i);
            std::snprintf (key, (size_t) len, "%s", kHintKeys[i]);
            return 1;
        }
    }
    return 0;
}

# SFC-8 — 8cee S-DSP sample synth

A polyphonic VST3 instrument (plus Standalone) that plays the instruments of SNES games, using the
sound of the SNES S-DSP chip: BRR samples at 32 kHz, the 14-bit pitch counter, 4-tap gaussian
interpolation, the hardware ADSR rate table and the 8-tap FIR echo. You can play chords on it like any
other software synth.

## Loading files

Drop a file on the window or press **LOAD**.

| File | What you get |
|---|---|
| `.spc` (sound snapshot of a song) | **Tracks 1–8 = the 8 SNES voices** at the moment of the snapshot, each with its exact sample, loop point, ADSR and echo settings. All other instruments in the song's sample directory appear in the list. |
| `.sfc` / `.smc` (ROM) | The ROM is scanned for uncompressed BRR samples. The first 8 found are put on tracks 1–8. Select a track and click any sample in the list to put it on that track. |
| `.brr` | A single raw BRR sample (2-byte loop header supported). |

**Why `.spc` is the exact one:** a ROM does not store "8 tracks". The 8 voices only exist while
the game's sound driver is running. An `.spc` is a snapshot of the sound chip mid-song, so it shows
which instrument each of the 8 voices was using. A ROM scan finds the samples, but it can't know which
voice uses which one, and some games compress their sound data so nothing is found. SPC sets for
almost every game can be found online.

## Controls

- **TONE** — Root (auto-detected from the sample's pitch), Fine, Bend range, mod-wheel Vibrato
- **ENVELOPE** — real S-DSP ADSR: AR 0–15, DR 0–7, SL 0–7, SR 0–31 (taken from the SPC voice), plus a smooth Release (0 = hardware 8 ms key-off)
- **ECHO** — EDL, EFB, EVOL and FIR coefficients, as on the chip (FIR "From file" uses the SPC's own filter)
- **PLAY** — Polyphony (8 like the hardware, 16 or 32), Pitch limit (S-DSP caps pitch at about 2 octaves above the root), Glide, Velocity, Pan, Volume
- Sustain pedal, pitch bend and mod wheel are supported.

The DAW project stores the current sample itself, so a project still opens if the file has moved.

## Builds

Every push builds Windows VST3 + Standalone on GitHub Actions (also macOS) and on GitLab CI.
Download the artifact, then copy `SFC-8.vst3` to `C:\Program Files\Common Files\VST3`.

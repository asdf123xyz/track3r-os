# Chip (module `chip`)

A chip-tune instrument for the Community OS, loaded from the SD card as `/modules/instruments/chip.tmod` (ADR 0002, OS-012c). **Written from scratch** against `module/sdk/o3module.h` (no third-party code, no licence gate:
`THIRD_PARTY.md`), MIT like the SDK. Status: new Community OS behaviour; tested on the host through the ABI (`modules/chip/tests/chip_test.cpp`); run on the unit (measured 2.8 % of a block per sounding voice on 0.4.9, `docs/architecture/cpu-budget.md`; the HIL scenarios bind and sound it). By-ear checks of each wave remain open.

## What it is

One mono voice an instance (the ABI runs one instance per instrument slot: play chords on several tracks).

- **Wave**: pulse with a **Duty** of 12.5 / 25 / 50 / 75 %, triangle, noise from a 15-bit LFSR, long (period 32767) or short (period 93, the metallic one). The pulse is band-limited with PolyBLEP; **Raw Edges** turns that
  off for the aliasing of a real chip. The pulse has its mean removed (a narrow pulse peaks at 1.75, hence the -12 dB default Volume); noise is scaled by 0.7. Noise is a sample-and-hold of the LFSR clocked at 16 x the note's
  frequency (never faster than the sample rate), so it follows the pitch.
- **Vol Steps**: the envelope's level is quantised to sixteen 4-bit steps (the staircase of a hardware volume register); off, the level is smooth.
- **Envelope**: Attack, Decay (the time of the fall from full scale to the sustain level), Sustain, Release, linear, per sample (no 60 Hz zipper). Attack and Release never go below 1 ms (a step would click); a new note over a
  sounding one starts from the level the old one is at. A sustain of 0 makes the note end by itself after the decay (a percussive sound), and the voice then says it is idle.
- **Arp 1 / Arp 2 / Arp Speed**: the pitch steps through 0, Arp 1, Arp 2 semitones at Arp Speed steps a second (two steps when Arp 2 is 0, none when both are 0).
- **Sweep / Sweep Time**: the note starts Sweep semitones away and glides to its pitch over Sweep Time (linearly in semitones; 0 ms turns it off): a kick, a laser.
- **Vib Depth / Vib Rate / Vib Delay**: a triangle vibrato of +-Depth cents at Rate Hz, fading in over Delay.
- Pitch (arpeggio, sweep, vibrato) updates every 32 samples; a live legato key retriggers like any other note, but a note the host ramps over the one that sounds (the Glide and Slide FX of a pattern, ABI 1.8)
  keeps the envelope, the arpeggio, the sweep and the vibrato and only moves the pitch. A note off before the first sample of a note ends the voice at once.

## Views

Two views on the Instrument page, drawn from the parameter values the page hands over: **Wave** (two periods of the pulse with its duty, the triangle, or 48 steps of the noise LFSR, named) and **Envelope** (the level against
time for the key held and released, as the parameters say, in 4-bit steps when Vol Steps is on). About 35 to 120 (the pulse to the noise) and 340 estimated display-list words (the pair fits the page's 700 with room for the page: the test holds 500).

## Parameters (a preset is twenty little-endian float32 values in this order; four pages: TONE 0-7, ENV 8-11, PITCH 12-16, ARP 17-19)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Wave | 0..3 | 0 | PULSE, TRIANGLE, NOISE LONG, NOISE SHORT (labelled; learnable) |
| 1 | Duty | 0..3 | 1 | 12.5%, 25%, 50%, 75% (labelled; learnable) |
| 2 | Raw Edges | 0 / 1 | 0 | |
| 3 | Vol Steps | 0 / 1 | 0 | |
| 4 | Volume | -48..+6 dB | -12 | modulation target |
| 5 | Vel Sens | 0..100 % | 50 | 1 - sens x (1 - velocity / 127) |
| 6 | Transpose | -24..24 semitones | 0 | |
| 7 | Fine | -100..100 cents | 0 | |
| 8 | Attack | 0..500 ms | 0 | at least 1 ms |
| 9 | Decay | 0..2000 ms | 250 | |
| 10 | Sustain | 0..100 % | 60 | |
| 11 | Release | 0..2000 ms | 80 | at least 1 ms |
| 12 | Sweep | -48..48 semitones | 0 | modulation target |
| 13 | Sweep Time | 0..2000 ms | 120 | |
| 14 | Vib Depth | 0..200 cents | 0 | modulation target |
| 15 | Vib Rate | 1..16 Hz (log) | 6 | |
| 16 | Vib Delay | 0..1000 ms | 0 | |
| 17 | Arp 1 | -24..24 semitones | 0 | |
| 18 | Arp 2 | -24..24 semitones | 0 | |
| 19 | Arp Speed | 1..120 Hz (log) | 50 | |

## Sizes and cost (mk66, `arm-none-eabi-g++ -O2` through `tools/o3m_pack.py`)

Image 6,272 B: hot part 2,096 B (`render` with the three oscillator loops inlined; the SRAM_L slab is 14 KiB shared by all modules), the rest cold (descriptor, parameter table, set-up, the two views); instance 192 B
(the manifest reserves 208), as `o3m_pack.py` builds it at OS-SNDMOD S1. **Cost, measured on the unit: 2.8 % of a 64-frame block (about 7.3k cycles) per sounding voice on 0.4.9**
(`docs/architecture/cpu-budget.md`; the earlier estimate of 1.8 % from the instruction count was low); a silent voice costs nothing.

## Tests (`ctest -R o3m_chip`)

Duty cycles and the removed mean; pitch (six notes, two waves, transpose and fine); PolyBLEP against raw edges (inharmonic share of the spectrum, at least 6 dB lower); triangle shape; noise levels and flip rate; the LFSR's
periods (32767 and 93); envelope timing (attack, decay, sustain, release, the 1 ms floor, retrigger continuity, sustain 0 ending the note); 4-bit steps (every sample a multiple of 1/15, sixteen levels); arpeggio (three, two and one
step, downwards); sweep (linear in semitones, repeats at every note); vibrato (depth, rate, delay); velocity and Volume; **block-size independence** (bit for bit over five block sizes); hostile parameters (NaN, 1e30, out-of-range
ids, note and velocity): no NaN, bounded; the packed `.tmod` through the loader's checks (header, CRC, relocation, descriptor); both views against formulae and the draw API's budgets for every wave, duty and corner of the envelope.

## Hardware check (by ear still open)

The cost is measured (above). Copy `chip.tmod` to `/modules/instruments/` on a test card, Instrument page F1 ENGINE, pick Chip, play: each wave and duty, the envelope, the arpeggio and the sweep by ear; read the `diag mod` timing for a held note.

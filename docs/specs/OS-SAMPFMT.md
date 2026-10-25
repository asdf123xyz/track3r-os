# OS-SAMPFMT — AIFF and IFF 8SVX samples in Sample Load

Status: implemented 2026-10-14 (maintainer: AIFF and IFF would be good; deviations and measurements below) · Level B (two pure readers + the Sample Load list; no persistent-format change: the result is an ordinary sample instrument) · **New Community OS behaviour**.

## Formats

- **IFF 8SVX** — the Amiga's native sample format: 8-bit signed PCM; `VHDR` gives `oneShotHiSamples`, `repeatHiSamples` (the loop part) and `samplesPerSec` (often a Paula rate, 8,363 or 16,726 Hz). Read: the first octave's one-shot + repeat → start / loop / end and play mode (ONE SHOT or LOOP FWD); the **native rate is kept** (`Instrument::sample_rate`, nothing resampled), so a Paula-rate one-shot or forward loop picks the Amiga engine by the existing automatic rule (`kInstEngineAuto`). Fibonacci-delta compression (`sCompression = 1`) is decoded; other compressions refused. Multi-octave instruments (`ctOctave > 1`): the first octave, reported. `NAME` → the instrument name.
- **AIFF / AIFF-C** — Apple's format, common in sample packs: big-endian PCM 8 / 16 / 24-bit (24 → 16 as the WAV reader does), AIFF-C `NONE` and `sowt` (little-endian); mono or stereo (stereo → mono as the WAV reader does). `INST` sustain loop + `MARK` → loop points and LOOP FWD; the rate is kept (80-bit float `sampleRate` converted exactly).

## Rules

- Pure readers beside `formats/wav_sample.h`, same contract (bounded memory, streaming into the arena tail, untrusted input), host tests with fixtures (generated in the test, no copyrighted samples) and a fuzz pass.
- Sample Load lists `.wav`, `.aif` / `.aiff` / `.aifc` and `.8svx` / `.iff` / `.svx`; the type is decided by the header, not the extension.
- Record the flash delta (expected a few KB).

## Acceptance

Host tests: each layout (8SVX plain and delta, one-shot and looped; AIFF 8/16/24, mono / stereo, AIFF-C NONE / sowt, with and without a loop), malformed files refused with a reason; sim golden of the Sample Load list. HW: a classic Amiga 8SVX plays at its pitch on the Amiga engine.

## Implementation notes (2026-10-14)

One reader serves both formats: `formats/iff_sample.{h,cpp}` (`iff::SampleReader`, registry id `iff-sample`, names `.aif .aiff .aifc .8svx .svx .iff`). It reuses `wav::make_sample_instrument` (name, path, rate, loop, wavetable rule) and the WAV reader's TPDF dither (the 20 lines are copied, not shared: wav_importer.cpp was left untouched). Tests: `tests/iff_sample_test.cpp` (also the QEMU suite `qemu_iff_sample`), plus the Sample load, dir_scan and audition tests.

Choices where the spec was silent (new Community OS behaviour, reversible):

- **Format by header.** `SampleBank::open_wav` reads the first 12 bytes and seeks back: `RIFF` the WAV reader, `FORM` + AIFF / AIFC / 8SVX the IFF reader, `vawt` Surge's; anything else goes by the name (`.wt` Surge, else WAV, whose reader says why the file is bad). A registry `sniff` of fewer than 12 bytes does not claim `FORM` (the native project is a FORM too).
- **Loop end.** AIFF loop = [begin marker, end marker) in frames, end exclusive, held to the audio length; 8SVX loop = [oneShot, oneShot + repeat). Mode 1 forward, mode 2 (forward-backward) ping-pong. Not verified against files from a specific tool.
- **Markers.** At most 24 kept; a loop naming a later one is dropped and counted.
- **Fibonacci-delta** is read as the EA reference unpacker does (pad byte, initial value, two nibbles a byte high first, no output for the initial value, sum clamped to -128..127). No real compressed file was at hand: open question, to be held against one.
- **Rates.** 8SVX `samplesPerSec` 0 is refused. AIFF rates below 1 Hz (including 0.5) are refused; a rate that does not fit 32 bits too.
- **AIFF depths.** 1..24 bits (container 1, 2 or 3 bytes, left-justified); 32-bit PCM and AIFF-C float / ulaw / alaw / ima4 / other compressions are refused Unsupported with "AIFF-C compression other than NONE and sowt" (32-bit: "PCM deeper than 24 bits"). `sowt` is read as little-endian PCM at any of those depths.
- **Order.** COMM / VHDR must precede SSND / BODY (a streamed read cannot go back); INST and MARK may follow SSND. A chunk past the end of the form is Malformed (past the file's size: Truncated); trouble after the audio ends the read successfully and is counted in `FormatReport::sanitised`.
- **8SVX extras.** `CHAN` = stereo reads the left channel and is counted; multi-octave files read the first octave and are counted; `NAME` names the instrument.
- **Audition (F5)** does not read these formats: it says `NO PREVIEW OF THIS TYPE: LOAD IT` (as for `.wt`). Follow-up: a preview reader.
- **Pitch warning.** (Removed by OS-RATEPITCH: the Modern engine now plays a sample at the pitch of its recorded rate, so there is nothing to warn about. It was `SAMPLE RATE n HZ: PITCH IS OFF`, for every non-44,100 Hz file, Paula rates included.)
- **Sim.** The browser's picture is the new gallery state `sampleload_formats` (the scenario runner has no card).
- **Flash.** See the build report of the branch (recorded in the final report; the reader is about 4 KB of code).

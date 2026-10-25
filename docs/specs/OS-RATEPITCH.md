# OS-RATEPITCH: a sample plays at the pitch of its recorded rate

Status: done (host PASS; hardware check open). Level C (audio voices and the sample loader). New Community OS behaviour, not an OG parity claim.

## The bug

The Modern voice advanced the sample position by the note's increment alone, as if every sample were 44,100 Hz. A 16,726 Hz 8SVX, a 22,095 Hz AIFF or a module sample (S3M C2SPD) sounded 2.6 or 2 times too high, and the Amiga conversion started from the wrong ratio.

## The design

- `SampleDesc::rate_q28` (voices.h) is the sample's rate over 44,100 in Q28, made once when the descriptor is bound (`rate_factor_q28`, main loop). 0 is exactly 1.0: a 44,100 Hz sample, or one that records no rate, takes the increment untouched, bit for bit what it was.
- The factor enters the increment where a voice makes it: `VoicePool::pitch_inc` (note start; the Slide FX and the finetune modulator through `repitch`). It is in flash (`O3OS_AUDIO_COLD`, once per note and once per block of a voice that has a slide), so SRAM_L does not grow. Never in the per-sample loops.
- The rate is held to 1,000..384,000 Hz (below it a voice would stop, above it the Q28 would overflow). A wavetable's pitch is its window's and takes no factor. The Amiga engine's pitch is the period's and ignores it.
- The Amiga conversion converts from the rate the sample plays at (`amiga_step_q32(period, src_amiga, src_rate_hz)`).
- The "SAMPLE RATE n HZ: PITCH IS OFF" strip warning is gone: there is nothing left to warn about.
- No layout change: `sample_rate` was already in INST 1.1.

## Reload of a native project (fold round)

A native project's `samples/instrNN.wav` is written with the instrument's `sample_rate` in its header and is read back by `OgSampleLoader`. The loader now takes a WAV of the rate the instrument stores when `begin(..., native = true)` (the platform passes it for a native load). A native WAV of another rate than the instrument stores is Unusable (it is not the file that was saved). An OG folder load keeps this loader's earlier rule: 44,100 Hz only. The descriptor's factor comes from the file's rate.

## Release note: the pitch of non-44.1 samples changes

Modern-engine samples that are not 44,100 Hz now play at their true pitch. A module import (S3M C2SPD), an 8SVX or an AIFF that the user tuned by ear before this fix sounds different (usually lower) on the same note: its `TUNE` has to be set again once. Samples on the Amiga engine, and 44,100 Hz samples, are unchanged. Imports of a C2SPD under 1,000 Hz are held to 1,000 and counted in the import report (`rates_clamped`).

## Tests

`audio_voices_test` (`ratepitch:`: the factor, the pitch through the voice, the tune/fine/Slide/Glide/tracker FX, note 127 with tune +24 at 192 and 384 kHz, backward and ping-pong loops), `audio_granular_test` (a grain), `amiga_voices_test` (the Amiga engine ignores it), `amiga_convert_test`, `sample_edit_screen_test` (conversion from 22,050 Hz; undo and redo rebind the factor), `sample_load_test`, `save_app_test` (save and native reload at 16,726, 8,287, 48,000 and 96,000 Hz: rate, factor, pitch; OG folder refusal), `modimp_import_test` (C2SPD under 1,000).

## Hardware

`platform/mk66/tools/hil/qa_ratepitch.json`; QA rows QA-SLD-007 to QA-SLD-010.

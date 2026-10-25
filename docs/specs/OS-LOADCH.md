# OS-LOADCH: the channel of a stereo file in Sample load

Status: implemented 2026-10-14 (maintainer approved the spec) · Level B · new Community OS behaviour (no historical parity claimed) · no persistent format change.

## What

1. **CHANNEL option.** A stereo WAV or AIFF can be loaded as `MIX` (the default, today's (L + R) / 2), `LEFT` or `RIGHT`. The result is an ordinary mono sample: nothing in the project, instrument or sample formats changes.
   - Stereo files carry an **`ST`** tag in the Sample load list (the last two cells of the name column; a name that long is cut with `~` to leave them).
   - **Key: F8** (free on Sample load, ui-map rules 3 and 4: Sample load is not a document screen, so F8 has no UNDO to give up, as on Mixer, Perform and Record where it is an action; the cycle is 3 long and forward only). Counts after the change: Sample load uses F1..F8 (LOAD UP ENTER ADD PLAY RESCAN WT CH), one page, no key left. **Label: `CH MIX` / `CH LEFT` / `CH RIGHT`** (at most 8 characters, the F7 `WT` precedent: the label names the state). The label shows only while the file under the cursor is stereo; on a mono file, a folder or an empty list it reads `--` and F8 says `ONLY FOR A STEREO FILE`. Each press on a stereo file says `STEREO FILE LOADS: LEFT / RIGHT / MIX (L+R)/2`. Shift + F8 falls through to the plain key and keeps its label (OS-UIFIX).
   - The choice (`WavView::channel`) is remembered for the session (across files and screens), **not saved** (not in the project nor the settings). A mono file ignores it.
   - F1 LOAD, F4 ADD and the F5 audition all follow it.
2. **AIFF with SSND before COMM** stays refused (OS-SAMPFMT known limit: the readers are streamed and cannot go back), but the strip says `AIFF: SSND BEFORE COMM NOT SUPPORTED` instead of `IMPORT <NAME> FAILED: DAMAGED FILE`. That reason is longer than the strip leaves after a file name, so it is the whole line (any reason over 24 characters is; the others compose as before).

## How

- `formats::ReadParams::channel` (0 mix, 1 left, 2 right) and `wav::Importer::begin(..., channel)`. The channel is picked at the conversion, in the existing stereo-to-mono branch (`convert_downmix` for WAV, `convert_aiff` for AIFF): the left or right value at the same 24-bit precision as the mean, so 8/16-bit and exact 24-bit/float files come through bit for bit, and a 24-bit file with a low byte dithers as in a mono 24-bit file. **Not** the `keep_stereo` interleave followed by a de-interleave: that needs twice the arena room, so a file that fits as MIX would fail LEFT.
- The `ST` tag is read from the file's header by the folder scan: `formats::sample_channels` (a pure walker, `formats/sample_probe.h`) reads the RIFF `fmt ` or the AIFF `COMM` through a seek + read callback, walking at most 16 chunks (LIST, bext, ANNO in front are skipped by seeking) and giving up at the sound data. `DirScan` opens each listed `.wav`, `.aif`, `.aiff`, `.aifc` (`ListFilter::tag_stereo`, on `kWavFilter` only; one probe's cost, `kProbeUnits`) and sets `DirIndex::stereo` (flags bit 3). An unreadable file is simply not tagged. 8SVX is mono and is not opened; an AIFF with SSND first is not tagged (the load refuses it).
- `ImportJob::detail` words the SSND case from `formats::iff::kSsndBeforeComm`; the strip's long-reason rule is in `App::report_jobs`.
- `platform/host/FakeStorage` can now hold a file's bytes (`add_file(path, bytes)`), for the gallery state `sampleload_stereo`.

## Tests and QA

`sample_load_test` (probe, ST tag, F8 visibility and cycle, session memory, LEFT/RIGHT bit-exact for WAV and AIFF, MIX unchanged, mono ignores, SSND message), `wav_test` (every depth, split frames, the mix's room), `audition_test` (the audition follows the channel); gallery `sampleload_stereo` (3 themes) and `qa_sample_load/qa_sld_001`; HIL `qa_sample_load.json` steps `qa_sld_011..014`; catalogue rows QA-SLD-011..014.

## Cost and open points

- Scan: a probe per audio file listed, so a folder of 200 WAVs reads 200 headers (open + 2 to 4 small reads each) while the listing is already usable; PROVISIONAL cost model (`kProbeUnits` = 8 readdirs), to be measured on the unit.
- Open: does a very large folder of audio files make the first scan feel slow? HIL step `qa_sld_011`.
- Known limits (review, 2026-10-14; accepted): a probe that fails (no free file handle, a transient read error) leaves a stereo file untagged, so F8 says ONLY FOR A STEREO FILE and the file loads as MIX; a 2-channel `fmt ` the importer then refuses (ADPCM) is still tagged ST. One probe is not cut by the step budget (open + up to 16 chunk skips on FatFs): the HIL step `qa_sld_011` on a large `/WAV` folder is the check before scan responsiveness is claimed.
- The strip shows a failure reason alone only for `kSsndBeforeCommText`; every other reason keeps the file name before it (review fix).

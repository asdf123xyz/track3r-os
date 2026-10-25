# OS-STARTER content radar: redistributable starter pack

Status: research card, 2026-10-17. Nothing downloaded or copied; licences read from web pages and from the local test-card inventory (`SOURCES.txt`, kept outside the repository). Every size marked *estimate* is arithmetic or recollection, not a measurement.

Rules applied: CC0, public domain, CC-BY, CC-BY-SA, or GPL-3.0 (compatible with the GPL-3.0 release; keep the notice). No NC, ND, "royalty free", "free for personal use" or unknown terms. All candidates below allow commercial use.

## Facts: candidate table

Licence column: verbatim licence name, then the source of that statement. "Verified" = read on the page in this research; "inventory" = taken from the maintainer's local inventory, whose licence field was copied from the source site and not re-read here.

| Cat | Name | URL | Licence (verbatim) | Author / attribution line | Size | Why useful | State |
|---|---|---|---|---|---|---|---|
| 1 | AKWF Adventure Kid Waveforms (pick a subset) | https://www.adventurekid.se/akrt/waveforms/adventure-kid-waveforms/ ; zip http://www.adventurekid.se/AKRTfiles/AKWF/AKWF.zip ; mirror https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE | Page: "To the extent possible under law, Kristoffer Ekstrand has waived all copyright and related or neighboring rights to AKWF Waveforms." (CC0 1.0, http://creativecommons.org/publicdomain/zero/1.0/). GitHub repo lists "CC0-1.0 license" (LICENSE.md) | Kristoffer Ekstrand (Adventure Kid). No credit required; credit anyway | Full set about 4,300 files (a commenter on the page cites W0000 to W4358; not author-stated). Subset of 300-600 single cycles, 16-bit mono, about 0.5-1 MB *estimate* (600 samples x 2 B = 1.2 KB per file x 600 files = 0.7 MB) | The de-facto single-cycle library; fits wavetable oscillator tests | Verified |
| 1 | Surge XT wavetables, folders `Basic/` and `Generated/` only | https://github.com/surge-synthesizer/surge/tree/main/resources/data/wavetables | GPL-3.0 (Surge XT repository licence, per inventory) | Surge Synth Team. Keep copyright notice and ship the GPL-3.0 text and a source pointer | Inventory files 4 KB to about 512 KB (Sine To Square HQ: 262,144 x 2 B). 10 files about 1.5 MB *estimate* | Multi-frame (2048 x N) tables in `.wt` and plain WAV; exercises the frame loader | Inventory; other Surge subfolders may carry third-party terms, re-check per folder |
| 1 | Generated morphs (sine to saw, PWM, formant, harmonic series) | our script | CC0 (own synthetic data) | track3r-os contributors | 100 files x 4 KB = 0.4 MB *estimate* | Zero licence risk, exact frame counts we choose | Generate |
| 2 | VCSL, Versilian Community Sample Library (percussion and idiophone excerpts only) | https://github.com/sgossner/VCSL | README: "This collection is under a Creative Commons 0 license."; repo shows "CC0-1.0 license" | Versilian Studios / Sam Gossner and contributors. No credit required | Whole library is huge (each instrument "about 20-75 MB in raw .wav"). Trim 20-30 hits to 16-bit mono, 22-44 kHz, about 3 MB *estimate* (e.g. 30 hits x 0.5 s x 44,117 x 2 B = 1.3 MB) | Real acoustic percussion and mallet one-shots, safe licence | Verified licence; exact instrument folders not browsed; pick files by hand and check the folder for any contributor note |
| 2 | Freesound "Linn LM-1 Drum Computer Samples (FREE)", PJCmusic | https://freesound.org/people/PJCmusic/packs/37785/ | NOT VERIFIED: pack page shows no licence; per-sound pages must be read. Do not use until each sound reads CC0 | PJCmusic | 12 sounds, small | Classic drum-machine set | Lead only |
| 2 | Freesound "DIY Drum Kit", Hard3eat | https://freesound.org/people/Hard3eat/packs/21786/ | NOT VERIFIED: pack page shows no licence | Hard3eat | small | Synth kick/snare/hat/tom | Lead only |
| 2 | Katie Cadet samples bassdrum.st and LUVHORN.ST (from kc-dancinonamiga.mod) | https://modarchive.org/index.php?request=view_by_moduleid&query=175731 | "Public Domain" (Mod Archive licence field, inventory) | Katie Cadet | few KB | Real Amiga one-shots, also as 8SVX | Inventory |
| 3 | Mod Archive MOD: hawks_claw (Drozerix), neon_techno (Drozerix), kc_dancinonamiga (Katie Cadet), loituma_4chn (JAM), stranger_run (Stranger), thrones_32ch (Beyond, edge case) | modarchive.org module ids 197917, 178172, 175731, 195783, 177387, 172293 (same URL form as above) | "Public Domain" (Mod Archive licence field, inventory) | as named | 2-60 KB each; about 0.3 MB *estimate* | Classic 4-channel and edge-case MODs | Inventory |
| 3 | Mod Archive MOD, CC-BY 4.0: dammit_triton (M4kachu), tiny_chip (M4kachu), ges01_jungle_6chn (The Goofy-Mouse), pxf_protrck2_8chn (elPatrixF) | ids 212346, 213205, 212582, 181781 | "CC-BY 4.0" (Mod Archive licence field; https://creativecommons.org/licenses/by/4.0/) | Credit line needed: title, author, source URL, licence | 8 KB-60 KB each; about 0.2 MB *estimate* | 4/6/8-channel variety | Inventory; attribution required |
| 3 | Mod Archive MOD, CC-BY-SA 4.0: metallo8_6chn (CHiPs) | id 179445 | "CC-BY-SA 4.0" (https://creativecommons.org/licenses/by-sa/4.0/) | CHiPs, credit plus share-alike | small | 6CHN example | Inventory; see ShareAlike note |
| 3 | Mod Archive S3M, Public Domain: a_new_frontend (K. Jose), kc_techrockloop and speed_chip (Katie Cadet), simpsons16 and solarwar (SoDa7) | ids 178540, 175418, 172462, 63962, 63964 | "Public Domain" (Mod Archive licence field) | as named | simpsons16 355 KB, others under 100 KB; about 0.6 MB *estimate* | S3M, 16-channel limit | Inventory |
| 3 | Mod Archive XM, Public Domain: chemical16, building_energy_24ch (Drozerix), electronic_22ch (aw24), npo_anarchy_32ch (New Plum Order), pacmawax (JAM) | ids 177684, 185456, 192058, 173441, 174607 | "Public Domain" (Mod Archive licence field) | as named | 7-100 KB each; about 0.3 MB *estimate* | XM including over-16-channel imports | Inventory |
| 3 | Mod Archive IT, Public Domain: artemis, cherish_17ch, ghost_20ch (Beyond), coolchiptune (Katie Cadet) | ids 197646, 213289, 185462, 174990 | "Public Domain" (Mod Archive licence field) | as named | 3-100 KB each; about 0.3 MB *estimate* | IT, channel-count edge cases | Inventory |
| 3 | OpenGameArt "2 creepy background songs" (two XM) | https://opengameart.org/content/2-creepy-background-songs | "CC0" (page links CC0 1.0 deed) | sauer2 | about 3.1 KB zip | Tiny, clean extra XM pair | Verified |
| 3 | Big Mod Music Pack, used as an index only | https://computermouth.itch.io/big-mod-music-pack | Page: "700+ permissively-licensed songs" from Mod Archive (Public Domain, CC0, CC BY, CC BY-SA 4.0; NC/ND excluded); per-song terms only through MasterList.txt links | computermouth (compiler); original authors per song | 383 MB zip, far too big | MasterList.txt can name more PD/CC0 modules, including possibly MED. Do not redistribute the pack; re-check each chosen song at modarchive.org | Lead only |
| 3 | MED / OctaMED module | none found | none | none | none | All MED files in the local inventory are unknown (libxmp test suite, no licence). Mod Archive returned HTTP 503 during this research, so its format and licence filter could not be queried | Gap, see below |
| 4 | Katie Cadet 8SVX: kc_bassdrum_16726, kc_luvhorn_8363_loop | derived from Mod Archive id 175731 | "Public Domain" (Mod Archive licence field) | Katie Cadet; 8SVX wrapper by our script | few KB | Real 8SVX one-shot and looped | Inventory |
| 4 | Toisto AIFF Test Suite (karip), files `aiff-samplesize-16/-8`, `aiff-chunk-inst`, `aiff-chunk-ssnd-offset`, `aifc-type-sowt`, `aiff-chunk-ssnd-before-comm` | https://github.com/karip/toisto-aiff-test-suite | "CC0" (Toisto AIFF Test Suite, inventory) | karip | few KB each | AIFF and AIFF-C features, loops, markers | Inventory |
| 4 | Generated 8SVX, AIFF, Serum-style WAV with `clm ` chunk | our script | CC0 (own synthetic data) | track3r-os contributors | under 0.5 MB *estimate* | Fibonacci-delta 8SVX, looped 8SVX, 24-bit AIFF, 48 kHz AIFF, 16-bit wavetable | Generate (already exist locally) |

Excluded as unknown or non-conforming: ffmpeg `mensaje.8svx`, McGill AFsp AIFF files, OpenMPT player-test files, libxmp test data (MED, FLT, Gidion Graveland, zob), Echo Sound Works wavetables ("100% Royalty Free", terms not published), the German "Open Source Drum Kit" (download no longer offered), commercial and "royalty free" Gumroad kits.

## Facts: licence cautions

- **Mod Archive default is not open.** The forum thread https://modarchive.org/forums/index.php?topic=3613.0 states the default "Mod Archive Distribution license" is granted to the site and not passed on, and that Public Domain "would be a separate license which can be set by the artists themselves". Use only modules whose licence field was set to Public Domain or CC by the artist. The field is the uploader's assertion: samples inside a module may come from elsewhere (see https://modarchive.org/forums/index.php?topic=4579.0). Re-open each module page right before release and keep a screenshot-free text record of the licence line and date.
- **CC-BY-SA 4.0 in a GPL-3.0 zip.** The `.mod` is distributed unmodified as separate data in the same archive (aggregation), so the GPL does not apply to it and it stays CC-BY-SA 4.0. Do not alter the file and keep its attribution. If maintainers prefer zero doubt, drop metallo8_6chn. *This is a reading of the licences, not legal advice.*
- **Surge GPL-3.0 wavetables.** Compatible with the GPL-3.0 release. Ship the GPL-3.0 text, name "Surge Synth Team" and give the repository URL. Do not take wavetables from other Surge folders without reading their notices.
- **AKWF:** the CC0 waiver is stated by the author on the pack page. I read the licence block but not the whole 109 k-character page, and did not audit the provenance of individual waves.
- **Attribution file.** Ship `CREDITS.txt` (file, author, licence, source URL) next to the zip content. It is mandatory for the CC-BY, CC-BY-SA and GPL rows and good practice for the rest.

## Estimates: size of the pack

All *estimate*:

| Part | MB |
|---|---|
| AKWF subset (600 files) | 0.7 |
| Surge wavetables (10 files) | 1.5 |
| Generated wavetables | 0.4 |
| VCSL excerpts | 3.0 |
| Generated drum kit (see below) | 1.0 |
| Modules (MOD, S3M, XM, IT, plus generated MED) | 1.8 |
| 8SVX, AIFF examples | 0.3 |
| **Total** | **about 8.7** |

This sits inside the 5-15 MB target with headroom of about 6 MB.

## What we generate ourselves instead

Open items where no clean source was verified. Everything generated is CC0, authored by the project, with the generating script in `tools/` and the licence stated in `CREDITS.txt`.

1. **Drum one-shots (kick, snare, clap, closed/open hat, toms, rim, cowbell).** No CC0 kit could be verified: the Freesound candidates show no licence on the pack page, and the Meadowlark CC0 repository (https://github.com/BillyDM/meadowlark-factory-library, README: "All samples and presets found in this repo are licensed under the Creative Commons Zero (CC0) license.") is an almost empty fork. Synthesise with a script: sine sweep plus decay envelope for kicks, filtered noise for snare and hats. 16 hits x 0.4 s x 44,117 Hz x 2 B = 0.57 MB *estimate*; with a few pitched variants about 1 MB. Also yields known loop points and exact pitches for tests.
2. **A MED module.** No MED with a clean licence was found. Options in order: (a) the maintainer asks Mod Archive for MED files whose licence field is Public Domain or CC (when the site responds), (b) a small script that writes an MMD0 file with a few synthetic samples and a short pattern, released CC0. Option (b) is the safe default for the first release. A hand-written MMD0 writer is about 100 lines of stdlib Python *estimate*.
3. **Wavetables beyond AKWF and Surge.** Generate additive morphs (sine to saw, square to PWM, formant, spectral tilt), 2048 x N frames, plain 16-bit WAV and Serum-style with the `clm ` chunk, so the loader is tested with files we own.
4. **Own demo song** for each module format (MOD, S3M, XM, IT, MED), generated by script from a tiny score. Optional; the Public Domain songs above already cover the formats except MED.
5. **8SVX and AIFF examples** keep using the CC0 generated files already in the local test card.

## Open decisions for the maintainer

- Accept GPL-3.0 (Surge) and CC-BY-SA 4.0 (one MOD) in the zip, or restrict to CC0 plus Public Domain plus CC-BY.
- Choose the VCSL instruments (needs a hand listening pass; licence is repository-wide CC0).
- Confirm a Freesound drum pack per sound page, or accept the generated kit only.
- Accept a generated MED as the only MED example.

## Sources

- https://www.adventurekid.se/akrt/waveforms/adventure-kid-waveforms/
- https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE
- https://github.com/sgossner/VCSL
- https://github.com/BillyDM/meadowlark-factory-library
- https://freesound.org/people/PJCmusic/packs/37785/
- https://freesound.org/people/Hard3eat/packs/21786/
- https://opengameart.org/content/2-creepy-background-songs
- https://computermouth.itch.io/big-mod-music-pack
- https://modarchive.org/forums/index.php?topic=3613.0
- https://modarchive.org/forums/index.php?topic=4579.0
- https://en.wikipedia.org/wiki/Mod_Archive
- https://github.com/karip/toisto-aiff-test-suite
- https://github.com/surge-synthesizer/surge

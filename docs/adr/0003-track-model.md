# ADR 0003 — Track model: 12 typed tracks over a voice pool

Status: **Accepted** (maintainer, 2026-10-10) · Source: `docs/design/product-ux.md` §2, §8 Q1.

- **Decision.** 12 tracks, each typed **Audio**, **MIDI** or **External audio**, sharing one voice pool, which is the polyphony budget across instruments. The ceiling rises to 16 only after a `hw-session` measures CPU headroom.
- **USB stems.** They are separate stem buses (radar card `usb-multichannel-audio`). The USB channel limit does not cap the track count.
- **Consequences.**
  - Project format v1 of the new OS stores up to 16 track slots, so raising 12 → 16 needs no format bump.
  - OG/1.9 imports map their 8 (or 12, for 1.9.0 patterns) tracks one-to-one.
  - The voice-stealing policy and per-engine voice costs come from the audio engine design.
- **Validation.** Worst-case CPU with 12 tracks of the heaviest planned engines (rt-optimize budget ≤ 60 %).

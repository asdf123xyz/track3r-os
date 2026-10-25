# ADR 0011 — Diagnostics over USB, crash records, and lessons from the first hardware session

Status: **Accepted** (2026-10-11).

**Sources:**
- The maintainer's first hardware session, 2026-10-10 (findings summarised below).
- The maintainer's USB-debug priorities.

## Hardware lessons the new tree must design in
1. **Alignment is declared, never placed.**
   - Root cause: the `size` build never booted because `audio_block_t` had 2-byte natural alignment. With `-fdata-sections` the pool landed at 0x1FFF0606, and LDM/LDRD faulted with UNALIGNED.
   - Rule for the new tree: every buffer that may be accessed with word or multi-word loads gets `alignas` plus a `static_assert`, and DSP and display-list buffers are 4/8-byte aligned by type.
   - Placement must never decide alignment.
2. **Display orientation is a HAL setting.** This unit showed the image upside down with REG_ROTATE=1 in the first hardware session (the value the OS's own test image used). What the official firmware sets is an open research question.
   - `hal::Display` gets an orientation parameter, plus a hardware-revision probe if one can be found.
   - The parameter can be overridden by a device setting.
3. **FT812 init must survive a warm reset.** The first boot after the bootloader showed a corrupted display, cured by a power cycle.
   - The display bring-up must handle a chip that was already running: explicit reset, wait on the ID register, clear RAM_DL, and verify REG_CMD_READ/WRITE before the first frame.
4. **Flashing facts.**
   - A separate SD bootloader flashes the firmware. The app writes `Firmware/_fwinfo` and pulls the bootloader request pin (Teensy pin 81, PTA1) low (works on the unit, hw-notes §3).
   - The hidden reset button flashes the first `.ptf` in `Firmware/`.
   - The 1.9 menu lists `PolyendTracker*.ptf` (observed on the unit; `docs/hw/tracker-hardware-notes.md`).
   - Recovery with a single file in `Firmware/` is proven.
   - The OS must keep this recovery path intact.

## Diagnostics, in priority order (OS work items; HAL first, the simulator gets the same interfaces)
1. **`diag` CDC console** (D-OS-6). Commands:
   - `version`: with build hash;
   - `log`: a ring buffer;
   - `cpu`: audio cycles per object/engine;
   - `mem`: stack high-water, pools, audio blocks;
   - `stage`: boot/run stage;
   - `reset`.

   Commands are actions with string names (ADR 0009 rev 1), not a second registry.
2. **Persistent crash record.**
   - The fault handler takes the exception frame from the stack selected by EXC_RETURN; the old ad-hoc handler missed it by 8 bytes.
   - It saves PC/LR/CFSR/HFSR/BFAR, the current stage and the module ID into noinit RAM, then reboots. The ADR 0002 system register file can be used too.
   - A watchdog covers hangs.
   - The next boot shows a notification and `diag` prints the last crash.
3. **File transfer over serial** (`ls`/`get`/`put` on the SD) plus `update <file>`, which writes `_fwinfo` and pulls the bootloader pin.
   - A stdlib host tool, `o3console.py`, wraps them; `o3 flash x.ptf` uploads, updates, waits for the reboot and verifies the version.
   - `update` stays maintainer gated: the project rule is that firmware is flashed only by the maintainer.
4. **Input injection and display-list screenshot over USB**, in the same format as the simulator's scenario recorder. This allows scripted hardware tests and CMD_SNAPSHOT2-style calibration of the simulator goldens.
5. **USB audio capture** for automated A/B audio tests, with the same plumbing as USB multichannel (radar card).
6. **Unique USB serial number** (today 4294967295) and an always-on, cheap boot-stage trace.

## Consequences
- These become OS work items: `platform/mk66` bring-up, then OS-0xx diag, after the UI toolkit.
- Reference material: a boot-trace experiment (trace macros, heartbeat, fault dump) done on early test images, and `docs/hw/tracker-hardware-notes.md` (orientation, FT812 warm reset, bootloader/updater, USB identity, EEPROM, SD quirks, open board questions). All work goes to this tree.

## Amendment 1 — display platform driver sources and licences (2026-10-11; radar card `eve-driver-libraries`)
- **Adapt Rudolph Riedel's EmbeddedVideoEngine** (FT800-FT813, MIT, active). Take its init order, warm-start handling, DMA burst pattern and coprocessor fault recovery for `platform/mk66`, with attribution in `THIRD_PARTY.md`. It has no MK66 target, so this is an adaptation, not a vendored copy.
- **Bridgetek EVE-MCU-Dev is reference only.** The repository LICENSE says MIT, but the source headers point to the proprietary BRT Software Licence, which has a field-of-use limit and forbids derivative works.
- **Vendor sample drivers whose licence conflicts with GPL-3.0 must not enter `os/`.** FTDI's "FT81x simple library for PIC MCU" carries an FTDI-only-use clause (its published licence text).
  - `eve` is written from the FT81X Programmer Guide. The vendor headers were used only to cross-check bit layouts and register values, which are facts.
  - Reviews must keep it that way.
- **Orientation.** REG_ROTATE alone is enough; CMD_SETROTATE only adds the touch matrix.
  - The OS stores orientation as a device setting, initially REG_ROTATE = 0. The prediction is that this fixes this unit, which was upside down at 1 in the first hardware session (later superseded: OS-HW1 found the unit upright at 1; `docs/hw/tracker-hardware-notes.md`).
  - Provide a recovery gesture at boot to flip it, in case another unit differs.
  - Verify on hardware.
- **Hardware checks for the next `hw-session`:**
 - Actual F_BUS: 90 MHz per the K66 clock setup, 60 MHz per earlier architecture notes. The effective SPI clock may be 18 MHz, not 20.
  - Whether the FT812 PD# pin is wired, which allows a clean power-down reset for the warm-reset fix.

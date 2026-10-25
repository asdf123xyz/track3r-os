# qanet_latency.json (OS-QANET D4)

The latency budgets of `tests/qanet/budgets.cpp` measured on the unit. It gates nothing: every measured step is `soft`, so one run prints every number. The setup steps (the walk to a clip, the check that no voice rings before F5, the first file sounding before the cursor move) are hard: a failed setup stops the run, which is a setup error, not a finding. New Community OS behaviour (test tooling); the limits are open research questions until this run (spec section 12, Q1).

| Budget | Limit | Measured by |
|---|---|---|
| preview.first_sound | 150 ms | F5 held on a 1 s mono clip: the key, then `voices` polled every 10 ms until a voice is in use |
| preview.long_clip | 150 ms | the same on a 6 s stereo clip |
| preview.cursor_move | 150 ms | F5 held on a 210 Hz clip, one step down to an 882 Hz clip: the old voice stops, then a voice is in use again (the two steps' seconds added); the capture then checks the pitch is 882 Hz |
| screen.switch | 100 ms | each screen button to `state` showing the new screen |
| listing.file, listing.sample_load | 500 ms | the press to the first entry under the cursor (`sel`): the time to the first entry, not to the full listing |

## What the tester needs on the card

- The four clips in the card **root**: `python3 -I platform/mk66/tools/hil/qanet_clips.py <folder>`, then copy the four `qanet_*.wav` files themselves (not their folder; 3 MB in all) into the card root. Do not overwrite a file of the same name that is already there: the names all start with `qanet_`. They sort one after the other; the scenario walks to them by name.
- For the listings, the large card of the random user: `python3 -I tools/qanet_card.py --out <dir>/qanet_card --o3pj-tool <build>/o3pj_tool` (about 2000 files in 52 folders; the largest folder, the root, has about 220 entries and the next 51), copied to the card with the clips. The host test's listing numbers are for that card; a card with a much larger folder gives a different number, say which one was used.
- The unit on the USB AUDIO capture (`--audio-device`), `needs: card, audio`. It writes nothing to the card.

Run: `python3 -I platform/mk66/tools/o3hil.py run qanet_latency --port <port> --audio-device auto --report qanet_latency.json` (the tester runs it; `o3hil.py validate` checks the file without a unit).

## Reading the numbers

- The number is the step's `seconds` in the `--report`. It includes the console round trip of the key and of each poll (a few ms), and the 10 ms poll adds up to 10 ms, so it overstates the latency slightly; and a step that passes at, say, 0.16 s did so after `within` ran out and one more poll: read the seconds, not only pass or fail.
- `voices` in use is the engine starting the voice; the sound follows within one block (about 1.5 ms). The USB capture is a level and pitch check after the key, not a timestamp: an onset timestamp inside a capture needs the capture to start before the key, which `o3hil.py` does not do yet (follow-up).
- The screen budget is the app's `screen` in `state`; the frame that shows it follows within one frame (16 ms). `shot` is the check of the picture.
- A cursor move whose first step fails ("in use 0" not seen) and whose second passes at once means the old voice went on or the new one began inside the first poll: read it with the pitch capture.

## Where the values go

Write the measured table (budget | limit | measured | verdict) into `build-hil/<version>/qanet_latency.md` and the `--report` next to it; when it is merged into `docs/qa/qanet-ledger.md`, add the numbers under "Measuring the net" and say which card and which image. Then set D2's defaults from them (`platform/host/include/o3os/platform/host/slow_card.h`, PROVISIONAL until then) and rerun `ctest -R qanet_budgets`: the baseline's findings move with them.

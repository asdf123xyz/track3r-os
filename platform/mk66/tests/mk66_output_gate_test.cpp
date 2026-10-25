// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rule that lets the first Play open the outputs (output_gate.h, OS-007g): the audio path has to have run clean for a number of blocks, and what happened
// before that window does not count. The case that mattered: an underrun or a hardware fault anywhere in the uptime before the first Play (the SD card's first
// long read, USB, the display) used to keep the outputs shut for good, with the Play waiting for them for ever. Runs on the host and on QEMU's Cortex-M4F.
//
// OS-OUTON: the outputs open once at boot instead of at the first Play (the amplifier key's click belongs to power-on). open_wanted() is the main loop's question "may I try
// now?"; the cases at the end run the loop's own steps (observe, ready, the open, the retry after a failed unmute) over a timeline of boot, the project load, a fault, a Play and the
// firmware update's hand-over.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include <o3os/platform/mk66/output_gate.h>

#include <cstdint>

using o3os::platform::mk66::OutputGate;
using o3os::platform::mk66::open_wanted;
using o3os::platform::mk66::OpenRetry;

TEST_CASE("output gate: a clean run opens at the settle count, as the cumulative rule did") {
  OutputGate g(400);
  g.observe(70, 0, 0);
  CHECK_FALSE(g.ready(0));
  CHECK_FALSE(g.ready(399));
  CHECK(g.ready(400));
  g.observe(140, 0, 0);
  g.observe(210, 0, 0);
  CHECK(g.ready(400));
  CHECK(g.ready(100000));
}

TEST_CASE("output gate: no reading ever taken is the same as a clean run (the tone image's rule at its first look)") {
  const OutputGate g(400);
  CHECK_FALSE(g.ready(399));
  CHECK(g.ready(400));
}

TEST_CASE("output gate: an underrun moves the window, a clean stretch after it opens the gate again") {
  OutputGate g(400);
  g.observe(70, 0, 0);
  g.observe(140, 0, 0);
  g.observe(210, 3, 0);  // three late blocks somewhere in the last poll period: the clean run starts here
  CHECK_FALSE(g.ready(210));
  CHECK_FALSE(g.ready(609));
  CHECK(g.ready(610));
  // The cumulative rule would have refused for ever: the counter is still 3 and stays 3.
  g.observe(280, 3, 0);
  g.observe(1000, 3, 0);
  CHECK(g.ready(1000));
}

TEST_CASE("output gate: a hardware fault moves the window the same way, and a new one moves it again") {
  OutputGate g(400);
  g.observe(100, 0, 1);
  CHECK_FALSE(g.ready(499));
  CHECK(g.ready(500));
  g.observe(450, 0, 2);
  CHECK_FALSE(g.ready(500));
  CHECK_FALSE(g.ready(849));
  CHECK(g.ready(850));
  g.observe(900, 1, 2);
  CHECK_FALSE(g.ready(1299));
  CHECK(g.ready(1300));
}

TEST_CASE("output gate: faults that keep coming keep it shut") {
  OutputGate g(400);
  std::uint32_t faults = 0;
  for (std::uint32_t b = 70; b < 20000; b += 70) {
    g.observe(b, 0, ++faults);  // one at every poll: the path is never clean for 400 blocks
    CHECK_FALSE(g.ready(b));
  }
}

TEST_CASE("output gate: a reading with a smaller boundary count is a restart of the audio path") {
  OutputGate g(400);
  g.observe(5000, 7, 2);
  g.observe(5070, 7, 2);
  CHECK(g.ready(5470));
  g.observe(10, 0, 0);  // `diag audio start`: the status counters began again
  CHECK_FALSE(g.ready(10));
  CHECK_FALSE(g.ready(399));
  CHECK(g.ready(400));
  g.observe(80, 1, 0);  // and a fault after the restart counts
  CHECK_FALSE(g.ready(479));
  CHECK(g.ready(480));
}

// ---- OS-OUTON: when the main loop makes the attempt ----------------------------------------------------------------------------------------------------------------

TEST_CASE("open_wanted: at boot, with nobody asking, the attempt waits for the audio path and for the project load, and then it is made") {
  CHECK_FALSE(open_wanted(/*audio_up=*/false, /*opened=*/false, /*asked=*/false, /*loading=*/false, /*fw_busy=*/false, /*powering_off=*/false));  // the audio path did not come up: nothing to open
  CHECK_FALSE(open_wanted(true, false, false, /*loading=*/true, false, false));                                                  // the boot project load: the unmute must not sit on its slices
  CHECK(open_wanted(true, false, false, false, false, false));                                                            // loaded, or nothing to load (no card, no Workspace)
}

TEST_CASE("open_wanted: a Play, a Song Play or a live note that found the outputs closed does not wait for the load (the fallback)") {
  CHECK(open_wanted(true, false, /*asked=*/true, /*loading=*/true, false, false));
  CHECK(open_wanted(true, false, true, false, false, false));
  CHECK_FALSE(open_wanted(/*audio_up=*/false, false, true, false, false, false));  // no audio path: the request waits for `diag audio start`, it opens nothing
  CHECK_FALSE(open_wanted(false, false, true, true, false, false));
}

TEST_CASE("open_wanted: once opened, not again until whatever muted them clears the flag") {
  for (int asked = 0; asked < 2; ++asked) {
    for (int loading = 0; loading < 2; ++loading) {
      CHECK_FALSE(open_wanted(true, /*opened=*/true, asked != 0, loading != 0, false, false));
    }
  }
}

TEST_CASE("open_wanted: never while the firmware update path holds the card or waits for the bootloader, asked or not") {
  CHECK_FALSE(open_wanted(true, false, false, false, /*fw_busy=*/true, false));
  CHECK_FALSE(open_wanted(true, false, true, false, true, false));
  CHECK_FALSE(open_wanted(true, false, true, true, true, false));
  CHECK(open_wanted(true, false, false, false, false, false));  // and it comes back the moment the update path is idle
}

TEST_CASE("open retry: a first attempt is always allowed; a failure holds the next one back for the backoff") {
  OpenRetry r;
  CHECK(r.ready(false, 0));
  CHECK(r.ready(false, 123456));
  r.failed(10000);
  CHECK(r.failures() == 1);
  CHECK_FALSE(r.ready(false, 10000));
  CHECK_FALSE(r.ready(false, 11999));
  CHECK(r.ready(false, 12000));
  CHECK_FALSE(r.ready(true, 11999));  // an explicit request waits for the backoff as well: the unmute blocks for up to 0.2 s, not every pass
  CHECK(r.ready(true, 12000));
}

TEST_CASE("open retry: the boot's own attempts stop after three failures; a Play keeps its retries") {
  OpenRetry r;
  r.failed(0);
  r.failed(2000);
  CHECK_FALSE(r.gave_up());
  CHECK(r.ready(false, 4000));
  r.failed(4000);
  CHECK(r.gave_up());
  CHECK_FALSE(r.ready(false, 6000));       // the cap: nobody asked, so the outputs stay closed
  CHECK_FALSE(r.ready(false, 1000000));
  CHECK_FALSE(r.ready(true, 5999));        // a Play that found them closed: still behind the backoff ...
  CHECK(r.ready(true, 6000));              // ... and then it retries, as often as it asks
  r.failed(6000);
  CHECK(r.failures() == 4);
  CHECK_FALSE(r.ready(true, 7999));
  CHECK(r.ready(true, 8000));
  r.succeeded();                           // a success starts the count again
  CHECK(r.failures() == 0);
  CHECK_FALSE(r.gave_up());
  CHECK(r.ready(false, 8000));
}

TEST_CASE("open retry: the backoff survives the millisecond counter wrapping, and the failure count saturates") {
  OpenRetry r;
  r.failed(0xFFFFFFFFu - 500u);            // retry_at wraps to 1499
  CHECK_FALSE(r.ready(false, 0xFFFFFFFFu));
  CHECK_FALSE(r.ready(false, 1498));
  CHECK(r.ready(false, 1499));
  for (int i = 0; i < 400; ++i) {
    r.failed(5);
  }
  CHECK(r.failures() == 255);
}

namespace {

// main.cpp's loop body for the open, step for step: the attempt when open_wanted() and the backoff allow it, a fresh reading into the gate, the gate's verdict, then the
// unmute: a success opens (and takes the failure's LED code back), a failure is logged, shown and retried later; nothing in it ever stops the loop.
struct BootLoop {
  OutputGate gate{400};
  OpenRetry retry;
  bool opened = false;      // appimg::audio_outputs_open()
  bool unmute_ok = true;    // false: a codec write that is not acknowledged
  bool led_fault = false;   // fault_code == kAudioFailure, and the boot fault listed by `diag stage`
  int unmutes = 0;
  int failures_seen = 0;
  int passes = 0;           // the rest of the loop (display, console, input) ran this many times: a failed open does not stop it
  struct In {
    std::uint32_t ms = 0;
    bool audio_up = true;
    bool asked = false;
    bool loading = false;
    bool fw_busy = false;
    bool powering_off = false;  // OS-POWER P1: the shutdown's latch
    std::uint32_t boundaries = 0;
    std::uint32_t underruns = 0;
    std::uint32_t faults = 0;
  };
  // One pass; true when the unmute ran on it.
  bool pass(const In& in) {
    ++passes;
    if (!(open_wanted(in.audio_up, opened, in.asked, in.loading, in.fw_busy, in.powering_off) && retry.ready(in.asked, in.ms))) {
      return false;
    }
    gate.observe(in.boundaries, in.underruns, in.faults);
    if (!gate.ready(in.boundaries)) {
      return false;
    }
    ++unmutes;
    if (unmute_ok) {
      opened = true;
      retry.succeeded();
      led_fault = false;
    } else {
      opened = false;
      retry.failed(in.ms);
      led_fault = true;
      ++failures_seen;
    }
    return true;
  }
  // The poll that runs whether or not anyone wants the outputs (main.cpp, every 100 ms): the gate learns where the faults were.
  void poll(std::uint32_t boundaries, std::uint32_t underruns = 0, std::uint32_t faults = 0) { gate.observe(boundaries, underruns, faults); }
  // FwHost::quiesce(): the codec is muted and the app's flag follows.
  void quiesce() { opened = false; }
};

BootLoop::In at(std::uint32_t ms, std::uint32_t boundaries) {
  BootLoop::In in;
  in.ms = ms;
  in.boundaries = boundaries;
  return in;
}

}  // namespace

TEST_CASE("boot timeline: no Play, a long project load: the outputs open on the first pass after the load, not before, and once") {
  BootLoop loop;
  // The SD mount and the display took seconds: the first pass of the loop is far past boundary 400, the load has just started.
  BootLoop::In in = at(3000, 3000);
  in.loading = true;
  CHECK_FALSE(loop.pass(in));
  in = at(5000, 5000);
  in.loading = true;
  CHECK_FALSE(loop.pass(in));
  in = at(9000, 9000);
  in.loading = true;
  CHECK_FALSE(loop.pass(in));  // the gate alone would have opened long ago: the load is what holds it back
  CHECK(loop.gate.ready(9000));
  CHECK(loop.pass(at(9100, 9100)));
  CHECK(loop.opened);
  CHECK_FALSE(loop.pass(at(9200, 9200)));
  in = at(9300, 9300);
  in.asked = true;
  CHECK_FALSE(loop.pass(in));  // a Play now finds them open and starts at once; it asks for nothing
  CHECK(loop.unmutes == 1);
}

TEST_CASE("boot timeline: nothing to load (no card, no Workspace): the outputs open on the very first pass of the loop") {
  BootLoop loop;
  CHECK(loop.pass(at(2500, 2500)));
  CHECK(loop.opened);
  CHECK(loop.unmutes == 1);
}

TEST_CASE("boot timeline: an underrun in the middle of the load moves the gate's window, and the open waits for 400 clean blocks after it") {
  BootLoop loop;
  loop.poll(1000);
  loop.poll(1070, /*underruns=*/2);  // the SD card's long read: two late blocks
  loop.poll(1140, 2);
  BootLoop::In in = at(1200, 1200);
  in.underruns = 2;
  CHECK_FALSE(loop.pass(in));  // the load ended 130 blocks after the fault: still inside the window
  in = at(1469, 1469);
  in.underruns = 2;
  CHECK_FALSE(loop.pass(in));
  in = at(1470, 1470);
  in.underruns = 2;
  CHECK(loop.pass(in));  // 400 blocks after the boundary the fault was seen at (1070)
  CHECK(loop.opened);
}

TEST_CASE("boot timeline: the Play that comes before the outputs are open waits for the gate only, loading or not, and is the fallback's one open") {
  BootLoop loop;
  loop.poll(1000);
  loop.poll(1070, 1);  // a late block at 1070
  BootLoop::In in = at(1100, 1100);
  in.asked = true;
  in.loading = true;
  in.underruns = 1;
  CHECK_FALSE(loop.pass(in));  // asked during the load: the gate refuses (30 blocks after the fault)
  in = at(1470, 1470);
  in.asked = true;
  in.loading = true;
  in.underruns = 1;
  CHECK(loop.pass(in));  // and opens as soon as it is clean, without waiting for the load
  CHECK(loop.opened);
  in = at(1600, 1600);
  in.asked = true;
  in.underruns = 1;
  CHECK_FALSE(loop.pass(in));  // the boot attempt after the load finds them open
  CHECK(loop.unmutes == 1);
}

TEST_CASE("boot timeline: the audio path that failed at boot and is started by `diag audio start` goes through the same gate") {
  BootLoop loop;
  for (int i = 0; i < 20; ++i) {
    BootLoop::In in = at(1000 + i, 0);
    in.audio_up = false;
    in.asked = i % 2 == 0;  // nothing runs, a Play that asked waits too
    CHECK_FALSE(loop.pass(in));
  }
  CHECK(loop.unmutes == 0);
  // `diag audio start` worked: the counters begin again at 0 and the loop notices a pass later.
  CHECK_FALSE(loop.pass(at(2000, 1)));
  CHECK_FALSE(loop.pass(at(2400, 399)));
  CHECK(loop.pass(at(2401, 400)));  // the same 400 clean blocks
  CHECK(loop.opened);
}

TEST_CASE("failed unmute at boot: the loop goes on, the fault is shown, the retry waits for the backoff, and the boot's attempts end at three") {
  BootLoop loop;
  loop.unmute_ok = false;  // the codec's I2C bus NACKs (it is shared with the button scanner)
  CHECK(loop.pass(at(3000, 3000)));  // attempt 1 at 3000
  CHECK_FALSE(loop.opened);
  CHECK(loop.led_fault);             // the LED shows the audio code and `diag stage` lists the fault; the UI is up (below)
  CHECK(loop.failures_seen == 1);
  const int passes_before = loop.passes;
  for (std::uint32_t ms = 3001; ms < 5000; ++ms) {  // 2 s of ordinary passes: no attempt, no 0.2 s block on every pass
    CHECK_FALSE(loop.pass(at(ms, ms)));
  }
  CHECK(loop.passes == passes_before + 1999);       // the loop ran every one of them: nothing halted
  CHECK(loop.unmutes == 1);
  CHECK(loop.pass(at(5000, 5000)));                 // attempt 2 after the backoff
  CHECK(loop.failures_seen == 2);
  CHECK_FALSE(loop.pass(at(6999, 6999)));
  CHECK(loop.pass(at(7000, 7000)));                 // attempt 3
  CHECK(loop.failures_seen == 3);
  CHECK(loop.retry.gave_up());
  for (std::uint32_t ms = 7001; ms < 60000; ms += 250) {  // after the cap nobody asks: the outputs stay closed and the loop never blocks again
    CHECK_FALSE(loop.pass(at(ms, ms)));
  }
  CHECK(loop.unmutes == 3);
  CHECK_FALSE(loop.opened);
  CHECK(loop.led_fault);
}

TEST_CASE("failed unmute at boot: a Play after the cap is the lazy retry, and a success takes the fault code back") {
  BootLoop loop;
  loop.unmute_ok = false;
  CHECK(loop.pass(at(3000, 3000)));
  CHECK(loop.pass(at(5000, 5000)));
  CHECK(loop.pass(at(7000, 7000)));
  CHECK(loop.retry.gave_up());
  BootLoop::In in = at(20000, 20000);
  in.asked = true;                        // the Play (SeqTransport::play sets the pending request)
  loop.unmute_ok = true;                  // the bus is fine again
  CHECK(loop.pass(in));
  CHECK(loop.opened);
  CHECK_FALSE(loop.led_fault);            // the code is taken back, the boot fault cleared
  CHECK(loop.retry.failures() == 0);
  CHECK(loop.unmutes == 4);
}

TEST_CASE("failed unmute: a Play that keeps asking retries behind the backoff, never every pass") {
  BootLoop loop;
  loop.unmute_ok = false;
  BootLoop::In in = at(1000, 1000);
  in.asked = true;
  CHECK(loop.pass(in));
  for (std::uint32_t ms = 1001; ms < 3000; ++ms) {
    in = at(ms, ms);
    in.asked = true;
    CHECK_FALSE(loop.pass(in));
  }
  in = at(3000, 3000);
  in.asked = true;
  CHECK(loop.pass(in));
  CHECK(loop.unmutes == 2);
}

TEST_CASE("fw update hand-over: quiesce clears the open; the guard keeps the amplifier down during the wait; a failed or timed-out hand-over opens them again") {
  BootLoop loop;
  CHECK(loop.pass(at(3000, 3000)));
  CHECK(loop.opened);
  // `fw update`: the upload and the re-check hold the card, then quiesce() mutes the outputs and clears the app's flag.
  BootLoop::In in = at(10000, 10000);
  in.fw_busy = true;
  CHECK_FALSE(loop.pass(in));
  loop.quiesce();
  CHECK_FALSE(loop.opened);
  for (std::uint32_t ms = 10001; ms < 15000; ms += 100) {  // the 5 s wait for the bootloader: the update path is busy, nothing raises the amplifier
    in = at(ms, ms);
    in.fw_busy = true;
    CHECK_FALSE(loop.pass(in));
    in.asked = true;  // not even a Play that waits
    CHECK_FALSE(loop.pass(in));
  }
  CHECK(loop.unmutes == 1);
  CHECK(loop.pass(at(15100, 15100)));  // "the bootloader did not take over": the phase is idle again, the outputs open again
  CHECK(loop.opened);
  CHECK(loop.unmutes == 2);
}

TEST_CASE("open_wanted: never while the shutdown has the audio path (the power-off latch), whatever the other arguments are") {
  for (int audio_up = 0; audio_up < 2; ++audio_up) {
    for (int opened = 0; opened < 2; ++opened) {
      for (int asked = 0; asked < 2; ++asked) {
        for (int loading = 0; loading < 2; ++loading) {
          for (int fw_busy = 0; fw_busy < 2; ++fw_busy) {
            CHECK_FALSE(open_wanted(audio_up != 0, opened != 0, asked != 0, loading != 0, fw_busy != 0, /*powering_off=*/true));
          }
        }
      }
    }
  }
  // and without the latch the same inputs open as before
  CHECK(open_wanted(true, false, false, false, false, /*powering_off=*/false));
  CHECK(open_wanted(true, false, true, true, false, false));
}

TEST_CASE("boot timeline: after the shutdown's latch nothing reopens the outputs: not the mute's cleared flag, not a Play that asks, not a live note") {
  BootLoop loop;
  CHECK(loop.pass(at(3000, 3000)));
  NEED(loop.opened);
  // stage A of the shutdown: the latch is set, the engine stops; stage A2 mutes (the app's flag follows, as after the update path's quiesce)
  loop.quiesce();
  for (std::uint32_t ms = 3100; ms < 20000; ms += 50) {
    BootLoop::In in = at(ms, ms);
    in.powering_off = true;
    in.asked = (ms / 50) % 2 == 0;  // a Play or a live note waits for the outputs
    CHECK_FALSE(loop.pass(in));
  }
  CHECK(loop.unmutes == 1);
  CHECK_FALSE(loop.opened);
}

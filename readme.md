<p align="center">
  <img src="mixer%20screenshot.png" alt="Paula Mixer on Amiga Workbench" width="800">
</p>

# Paula Audio over HDMI — AI CODED

**Real Amiga Paula audio is now being reproduced through the Raspberry Pi HDMI output on PiStorm Classic.** The implementation runs alongside Emu68 on a Raspberry Pi 3A+ and requires no Linux, external ADC, HDMI audio injector, AHI driver, or application modification.

The current hardware-validated milestone adds **PaulaMixer V2 live controls** and a new **transactional Chip RAM cache recovery path** that removes the first-playback corruption previously caused by cold-cache sample misses. The physical Paula remains active, so the original analogue audio output continues to function alongside HDMI.

## How it works

The implementation combines the existing PiStorm Classic bus infrastructure with a software Paula renderer:

* **CPU0** captures CPU-originated writes to Paula's custom registers.
* **CPU1** replays those register writes into the software emulator and renders stereo PCM at 48 kHz.
* **CPU0 alone** performs physical Chip RAM reads for a read-only, asynchronous 2 MiB shadow cache.
* **CPU1 never reads the physical Amiga bus directly.**
* The PCM stream is sent through the Raspberry Pi's HDMI MAI audio backend in the existing 48 kHz / 24-bit output path.

The software emulation is based on **[amiga-paula-8364-emulator](https://github.com/akustikrausch/amiga-paula-8364-emulator)** by Akustikrausch (Andreas Wendorf), an MIT-licensed Paula 8364 emulator. The core has been adapted for Emu68's bare-metal environment, replacing its standard C++ runtime dependencies with lightweight callbacks and static storage while preserving the original implementation and license.

## Live audio, cache and recovery

The audio path includes an 8,192-entry register event queue, timestamp-ordered replay, and a 48 kHz audio-frame clock. Chip RAM sample data is mirrored through an asynchronous page cache divided into 256-byte pages, with request deduplication, invalidation generations and rejection of stale publications.

The original implementation could return a synthetic zero when CPU1 reached a sample page that CPU0 had not fetched yet. Because the Paula source cursor was still allowed to advance, the first playback of a newly loaded module could differ audibly from later warm-cache playback.

The current **CACHE-FIX2** milestone changes that behavior:

* Active channels receive bounded, nonblocking lookahead of up to 1024 bytes using the actual software DMA cursor.
* Lookahead also anticipates the next loop when a one-shot sample is approaching its end.
* Each PCM frame is rendered speculatively from a saved Paula-core state.
* If rendering encounters a missing valid sample page, the Paula core state and master-gain state are restored instead of committing the incomplete frame.
* Speculative virtual-IRQ accounting is discarded together with the failed frame.
* Register events continue to be processed normally and are not rolled back.
* HDMI output never blocks on the Amiga bus. During a cache wait, the last valid output fades toward silence over roughly 2 ms; playback fades back in once rendering can continue.

This preserves the architectural rule that CPU0 owns physical Chip RAM access while CPU1 remains dedicated to rendering. The trade-off is that recovery can introduce a small virtual-audio delay relative to the physical Paula; the implementation is not intended to be cycle-exact.

## PaulaMixer V2

The milestone includes the native AmigaOS **PaulaMixer V2** GUI and a live atomic settings interface between AmigaOS and the bare-metal audio renderer. Controls are applied while Paula playback is running:

* **Master volume:** 0–100%, with a short output gain ramp.
* **Stereo separation:** 0–100%.
* **Interpolation:** nearest or linear.
* **Channel mute:** independent mute controls for all four Paula audio channels.

The settings bank uses an atomic request/applied protocol so the GUI can update the renderer without stopping audio. The existing V2 GUI and runtime ABI are preserved unchanged by CACHE-FIX2.

## Current scope and limitations

This is a hardware-validated experimental implementation and a stable development baseline, not a claim of cycle-accurate Paula emulation.

The current core does not yet provide direct `AUDxDAT` playback, audio modulation, or the analogue filter. Copper-originated Paula register writes and blitter modifications to sample memory are not fully covered by CPU bus interception. CACHE-FIX2 is deliberately a bounded, nonblocking recovery mechanism rather than a synchronous bus-access path.

The current milestone also deliberately does **not** add a limiter or alter the validated HDMI clock, MAI FIFO or IEC958 output sequence.

## Source and milestone

The current archive contains the complete PaulaMixer V2 source overlay required on top of the matching clean Emu68 tree, together with PaulaProbe, host regression tests, source patches and the before/after hardware validation captures. Unrelated Emu68 files are not included.

**Base commit:** `9b4379a5c5dbf7f12f5e10cfe81a96b872e2426f`  
**Target:** `raspi64` / `pistorm-classic`  

The implementation remains entirely bare-metal. Existing Amiga software continues to program the real Paula normally while Emu68 mirrors the resulting audio path to HDMI in parallel.

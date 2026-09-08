<p align="center">
  <img src="images/paula-mixer-workbench.png" alt="Paula Mixer on Amiga Workbench" width="500">
</p>

# Paula Audio over HDMI — Hardware-Validated

**Real Amiga Paula audio is now being reproduced through the Raspberry Pi HDMI output on PiStorm Classic.** The implementation runs alongside Emu68 on a Raspberry Pi 3A+ and requires no Linux, external ADC, or HDMI audio injector.

The current milestone has been tested on real hardware, with excellent audio playback reported. The physical Paula remains active, so the original analogue audio output continues to function alongside HDMI.

## How it works

The implementation combines the existing PiStorm Classic bus infrastructure with a software Paula renderer:

* **CPU0** captures CPU-originated writes to Paula's custom registers.
* **CPU1** replays those register writes into the software emulator and renders stereo PCM at 48 kHz.
* A read-only, asynchronous cache provides the renderer with sample data from the Amiga's Chip RAM.
* The PCM stream is sent through the Raspberry Pi's HDMI MAI audio backend.

The software emulation is based on **[amiga-paula-8364-emulator](https://github.com/akustikrausch/amiga-paula-8364-emulator)** by Akustikrausch (Andreas Wendorf), an MIT-licensed Paula 8364 emulator. The core has been adapted for Emu68's bare-metal environment, replacing its standard C++ runtime dependencies with lightweight callbacks and static storage while preserving the original implementation and license.

## Live audio and recovery

The audio path includes an 8,192-entry register event queue, timestamp-ordered replay, and a 48 kHz audio-frame clock. Sample data is prefetched in small windows and subsequent pages are loaded asynchronously as playback advances.

When the event history becomes incomplete or the replay falls too far behind, the renderer can reconstruct the latest CPU-observed register state and resume playback. This keeps the HDMI audio service operational without interfering with the physical Paula.

A companion AmigaOS utility, **PaulaProbe**, provides live diagnostics for captured and applied register writes, DMA state, per-channel parameters, sample-cache activity, PCM output, and recovery statistics.

## Hardware validation

The successful hardware test demonstrated live music playback through HDMI while the original analogue Paula output remained functional. The accompanying diagnostic snapshot recorded:

| Metric                   | Observed value |
| ------------------------ | -------------: |
| Captured register writes |        331,374 |
| Dropped events           |              0 |
| Sample reads             |      4,856,488 |
| Non-silent PCM frames    |      1,421,902 |
| HDMI MAI errors          |              0 |

These measurements confirm that the register capture, sample-memory access, software rendering, and HDMI output are operating together on the real PiStorm Classic hardware.

## Current scope

This is a hardware-validated working implementation, with further compatibility and accuracy work planned. The current core does not yet provide cycle-exact Paula behavior, direct `AUDxDAT` playback, audio modulation, or the analogue filter. Copper-originated register writes and blitter modifications to sample memory are not fully covered by CPU bus interception.

The milestone is preserved as a stable development baseline so that future improvements can be made without losing the verified audio path.

## Source and milestone

The archive contains the complete final source overlay, PaulaProbe, the original MIT-licensed Paula implementation, host tests, source patches, and the successful hardware validation log.

**Base commit:** `9b4379a`
**Target:** `raspi64` / `pistorm-classic`
**Milestone:** `Emu68-9b4379a-HDMI-AUDIO-FIX1-HARDWARE-VALIDATED-MILESTONE.zip`

The implementation is entirely bare-metal and requires no AmigaOS audio driver or application modifications. Existing software can continue to use Paula normally while the HDMI mirror operates in parallel.

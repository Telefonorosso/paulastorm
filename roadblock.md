## Framethrower → Emu68 HDMI Audio — Current Technical Status

The current goal is to transport **real Paula audio activity** from Framethrower Denise to Emu68 and output it through HDMI, while keeping **CPU1 completely free** and avoiding the previous software-Paula / Chip-RAM mirror architecture.

### Target architecture

`Real Paula AUDxDAT`
→ `Framethrower passive snoop`
→ `CSI-2`
→ `Emu68 UNICAM data/metadata DMA`
→ `Core0 receiver / renderer`
→ `HDMI MAI`

No Paula emulator is involved in the intended final path, and no duplicate Chip RAM sample fetching is required.

## Framethrower side

The capture side is currently considered stable.

Framethrower uses **PIO2 with four passive snoopers**, one for each Paula audio data register:

* `AUD0DAT $DFF0AA`
* `AUD1DAT $DFF0BA`
* `AUD2DAT $DFF0CA`
* `AUD3DAT $DFF0DA`

PIO0 remains dedicated to video, while PIO1 remains unchanged for the existing updater / RGA path.

The MIPI sender was extended to support:

* dynamic CSI-2 Data ID
* dynamic Word Count
* dynamic CSI-2 header ECC

The normal video stream remains `DT 0x22` RGB565.

### Current stable CSI transport

The only audio transport tested so far that does **not** corrupt video is:

`FS VC0`
→ `DT 0x12 audio packet`
→ `DT 0x22 RGB565 video lines`
→ `FE VC0`

Attempts using VC1 or separate CSI frames caused immediate video corruption.

The current audio packet begins with the `FTA1` signature and carries one real captured Paula event.

This STEP-E transport is therefore currently frozen and should not be modified without new evidence.

## Emu68 side

Emu68 configures UNICAM with `DT 0x22` as the image format.

The working assumption is that non-matching `DT 0x12` traffic can be received through the UNICAM alternate/data DMA path without entering the image framebuffer.

A Core0-only receiver scans the alternate buffer for `FTA1` packets and forwards the captured events to a new HDMI audio backend.

CPU1 remains on the normal Emu68 path and is **not used for audio**.

## Current blocker

The new Framethrower → Core0 → HDMI path is still **silent**.

A recent apparent success was invalid: `config.txt` was still loading the old hardware-validated `emu68.img.gz`, which contains the previous software-Paula HDMI path.

When the newly-built Framethrower image was actually loaded, HDMI audio remained silent.

This also explains why PaulaMixer did not detect its expected backend during that test.

## What has already been ruled out

Moving `hdmi_audio_ft_init()` to the final boot gate immediately before:

`M68K_StartEmu(0, NULL)`

did **not** restore HDMI audio.

A direct synthetic 10-second tone written through the new Core0 backend also remained silent.

The known-good older monolithic Emu68 image still produces HDMI audio correctly on exactly the same hardware.

Therefore the HDMI sink, cable and display are known-good.

## Current known-good facts

* Passive `AUDxDAT` snooping does not disturb Framethrower video.
* PIO2 capture is stable.
* Dynamic CSI-2 headers and ECC are stable.
* `VC0 + DT0x12` coexists cleanly with `VC0 + DT0x22` video.
* VC1 and separate audio CSI frames are currently known-bad for video.
* CPU1 is intentionally not used by the new architecture.
* The old software-Paula HDMI implementation remains hardware-validated.
* The new Core0 HDMI implementation remains silent.

## Most useful next step

The next useful investigation is a **strict source-level comparison** between the exact hardware-validated HDMI implementation and the current `hdmi_audio_ft` backend.

In particular:

* HDMI clock setup
* HSM / pixel clock dependencies
* MAI clock ratio
* MAI reset and enable sequence
* IEC958 configuration
* HDMI Audio InfoFrame programming
* FIFO initialization
* register write ordering
* initialization timing
* possible HDMI/video reinitialization after audio setup
* differences between the old CPU1 execution context and the new Core0 execution context

At this point it would be preferable **not to change the Framethrower STEP-E transport**, since it is the first configuration that carries the experimental audio packet without causing any visible video regression.

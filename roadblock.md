```markdown
## Framethrower → Emu68 HDMI Audio: Current Technical Status

The current goal is to transport **real Paula audio activity** from Framethrower Denise to Emu68 and output it over HDMI, while keeping **CPU1 completely free** and avoiding the previous software-Paula/Chip-RAM mirror architecture.

The intended path is:

`Paula AUDxDAT → Framethrower passive snoop → CSI-2 → UNICAM metadata/data DMA → Emu68 Core0 → HDMI MAI`

### Framethrower side

The capture side is currently considered stable.

Framethrower uses **PIO2 with four passive snoopers**, one per `AUDxDAT` register:

- `AUD0DAT $DFF0AA`
- `AUD1DAT $DFF0BA`
- `AUD2DAT $DFF0CA`
- `AUD3DAT $DFF0DA`

PIO0 remains dedicated to video and PIO1 remains unchanged for the existing updater/RGA path.

The MIPI sender was modified to support a dynamic CSI-2 Data ID, Word Count and ECC. Video remains DT `0x22`, and the validated audio transport currently uses a small DT `0x12` packet on **VC0 inside the existing video CSI frame**.

The working transport format is:

`FS VC0 → DT0x12 audio packet → DT0x22 video lines → FE VC0`

This configuration is important because previous attempts using VC1 or separate CSI frames produced immediate video corruption, while the current VC0/DT0x12 arrangement is visually stable.

The audio payload currently starts with the `FTA1` signature and carries one real captured Paula event per packet.

### Emu68 side

Emu68 configures UNICAM with DT `0x22` as the image format, with the expectation that the non-matching DT `0x12` traffic is routed to the alternate/data DMA path.

A Core0-only receiver scans the metadata/data buffer for `FTA1` packets and feeds a new HDMI audio backend. No CPU1 audio loop is used, and the old Paula emulator is not part of this path.

The new backend reuses the same general MAI/IEC958 register programming model as the previously hardware-validated HDMI audio POC, but is serviced from Core0.

### Current blocker

At the moment, the new Framethrower path is still **silent on HDMI**.

A recent apparent success was invalid: `config.txt` was still loading the old hardware-validated `emu68.img.gz`, which uses the previous software-Paula routing. Once the correct newly-built image was loaded, HDMI audio was again silent.

This means that moving `hdmi_audio_ft_init()` to the final boot gate immediately before `M68K_StartEmu()` did **not** solve the problem.

The known-good old monolithic image still produces HDMI audio on the same hardware, so the sink, cable, monitor and basic HDMI capability are known-good.

### What is currently considered proven

- Passive Framethrower AUDxDAT snooping does not disturb video.
- Dynamic MIPI headers/ECC do not disturb video.
- VC0 + DT0x12 coexistence with DT0x22 video is stable.
- CPU1 is not required by design for the new path.
- The problem is now most likely on the Emu68 receive/output side rather than in the Framethrower video transport.

### Next useful comparison

The next step should be a strict source-level comparison between the **exact hardware-validated old HDMI audio implementation** and the current `hdmi_audio_ft` path, including:

- HDMI/MAI clock setup
- register initialization order
- InfoFrame / IEC958 setup
- FIFO enable/reset sequencing
- exact point in boot where the audio block is enabled
- whether some later video/platform init resets HDMI audio state
- differences between CPU1 execution context and the new Core0 execution context

Until that comparison is complete, the Framethrower STEP-E transport should remain frozen, because it is currently the only audio-packet arrangement that preserves clean video.
```

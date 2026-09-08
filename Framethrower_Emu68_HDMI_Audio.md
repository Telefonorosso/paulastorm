# Framethrower Denise Audio Capture → Emu68 HDMI Audio

## Goal

The proposed architecture is deliberately simpler than the current PaulaMixer experiment.

The Framethrower Denise developer confirmed:

> “Framethrower observes RGA bus. With that you ‘see’ every single byte going to audio channels.”

This gives us a fundamentally different path for HDMI audio:

```text
Amiga Chip RAM
      │
      ▼
Original Amiga / Paula DMA
      │
      ▼
RGA bus
      │
      ▼
Framethrower Denise
passive capture only
      │
      ▼
audio transport
      │
      ▼
Emu68
minimal formatting + buffering
      │
      ▼
BCM2837 DMA
      │
      ▼
VC4 HDMI MAI FIFO
      │
      ▼
HDMI
```

There must be **no second Paula DMA engine, no second Chip RAM fetch, no Paula emulator, and no dedicated CPU1 audio renderer**.

Framethrower observes the original hardware activity. Emu68's job is only to transform the captured stream into the representation required by the Raspberry Pi HDMI hardware and keep that hardware fed efficiently.

The existing PaulaMixer work remains useful as a proof of the HDMI path, but its Paula replay, Chip RAM reader, register mirror and CPU1 rendering architecture are no longer required. The current POC explicitly uses CPU1, a software Paula renderer and polling writes to the HDMI FIFO.

---

## 1. The HDMI side is already understood

On BCM2835/BCM2837, HDMI audio is implemented inside the VC4 HDMI hardware. The Linux VC4 driver describes the path very clearly: the DMA engine supplies SPDIF/IEC958 subframes to an HDMI register, and the MAI bus carries them internally to the HDMI encoder for insertion into the video blanking periods.

Therefore our final Emu68 implementation should stop doing this:

```text
CPU → wait for MAI FIFO → write sample → wait → write sample ...
```

and move to this:

```text
FT input
   │
   ▼
small ARM RAM ring buffer
   │
   ▼
BCM2837 DMA ──DREQ17──► HD_MAI_DATA
                           │
                           ▼
                         HDMI
```

That is also very close to the architecture used by the upstream Linux VC4 driver: its HDMI audio endpoint is configured as a 32-bit DMA destination with a maximum burst of two words.

At 48 kHz stereo, the final MAI traffic is tiny:

```text
48,000 frames/s
× 2 channels
× 4 bytes
= 384,000 bytes/s
```

So bandwidth is not the problem. Correct pacing, buffering, DMA addressing and cache coherency are.

---

## 2. VC4 HDMI register blocks

The already validated Emu68 POC uses the Pi 3 VC4 register layout. The current Raspberry Pi Linux VC4 driver uses the same offsets.

There are two relevant register blocks.

### HD / MAI block

Bus base:

```text
0x7e808000
```

Current Emu68 CPU mapping:

```text
0xf2808000
```

| Register | Offset | Purpose |
|---|---:|---|
| `HD_MAI_CTL` | `0x014` | MAI reset, flush, enable, channel count and FIFO status |
| `HD_MAI_THR` | `0x018` | FIFO DREQ and panic thresholds |
| `HD_MAI_FMT` | `0x01c` | Audio sample-rate code and data format |
| `HD_MAI_DATA` | `0x020` | Audio FIFO data port — **DMA destination** |
| `HD_MAI_SMP` | `0x02c` | MAI sample-rate N/M divider |

The crucial register is therefore:

```text
CPU MMIO address: 0xf2808020
DMA bus address:  0x7e808020
```

These addresses are **not interchangeable**. The Broadcom memory architecture distinguishes ARM virtual/physical addresses from system-bus addresses, and explicitly requires DMA peripherals to be addressed in bus-address space.

### HDMI core block

Bus base:

```text
0x7e902000
```

Current Emu68 CPU mapping:

```text
0xf2902000
```

Important registers include:

| Register | Offset | Purpose |
|---|---:|---|
| `HDMI_MAI_CHANNEL_MAP` | `0x090` | Maps MAI channels to HDMI channels |
| `HDMI_MAI_CONFIG` | `0x094` | Channel mask and MAI bit/format ordering |
| `HDMI_AUDIO_PACKET_CFG` | `0x09c` | HDMI audio packet behaviour |
| `HDMI_RAM_PACKET_CFG` | `0x0a0` | Enables packet RAM entries |
| `HDMI_RAM_PACKET_STATUS` | `0x0a4` | Packet status |
| `HDMI_CRP_CFG` | `0x0a8` | HDMI audio clock regeneration |
| `HDMI_CTS_0` | `0x0ac` | CTS |
| `HDMI_CTS_1` | `0x0b0` | CTS |

The Pi device tree also identifies the two HDMI regions as `0x7e902000` and `0x7e808000`.

---

## 3. HDMI should remain owned by the existing video setup

An important property of our validated POC is that it adds audio to an HDMI link that is **already running**.

We should preserve this behaviour upstream.

Emu68 must not:

- reset the HDMI video engine;
- change the video mode;
- reprogram PLLH;
- disturb HVS or framebuffer state.

Only MAI/audio state should be reset or configured.

The current POC already follows this rule: it resets and flushes MAI only and leaves the HDMI video state untouched. Linux similarly starts HDMI audio by resetting/flushing the MAI audio side.

---

## 4. Format expected by `HD_MAI_DATA`

`HD_MAI_DATA` does **not** consume raw Paula bytes.

For the normal PCM path, VC4 expects **32-bit IEC958 subframes**. Linux exposes this as `IEC958_SUBFRAME_LE`.

Our hardware-tested POC already generates exactly these words:

```c
uint32_t word = ((uint32_t)sample & 0x00ffffffu) << 4;
```

with IEC958 channel-status, parity and block preamble added before writing the result to `HD_MAI_DATA`.

The final DMA buffer should therefore contain:

```text
L0 IEC958
R0 IEC958
L1 IEC958
R1 IEC958
L2 IEC958
R2 IEC958
...
```

Each entry is 32 bits.

One stereo frame is therefore exactly:

```text
8 bytes
```

The format conversion should happen **while filling the next DMA period**, not while servicing the HDMI FIFO.

This is a major distinction from the current POC. The existing implementation executes two MMIO writes per frame and polls `MAI_CTL_FULL` between them. The final implementation should prepare a batch in RAM and then let DMA perform all MAI writes.

---

## 5. HDMI clocking

For our first upstream implementation I would retain the already validated:

```text
48 kHz
stereo
24-bit PCM carried in IEC958 subframes
```

The MAI clock is generated using `HD_MAI_SMP`, while HDMI Audio Clock Regeneration uses `N` and `CTS`.

Our current POC derives the MAI N/M ratio from the active HSM clock and derives CTS from the active pixel clock. At 48 kHz it uses:

```text
ACR N = 6144
```

and computes CTS from the current HDMI pixel clock.

Again, Emu68 should **read the existing clock state**, not take ownership of HDMI/video clock generation.

---

## 6. MAI FIFO thresholds for DMA

The old POC uses:

```c
HD_MAI_THR = 0x10101010;
```

because it was designed around polling rather than DMA.

For the final Pi 3 DMA path we should instead start from the values used by the current upstream VC4 driver for VC4 generation 4:

```text
PANICHIGH = 8
PANICLOW  = 8
DREQHIGH  = 6
DREQLOW   = 8
```

This is important because `DREQ` should become the hardware pacing mechanism.

Emu68 should not decide when another HDMI word can be written.

**MAI decides.**

---

## 7. HDMI DMA request

The Raspberry Pi device tree assigns:

```text
DREQ 17 = HDMI
```

to the HDMI audio interface.

This must not be confused with DMA channel 17.

There are multiple DMA engines/channels; **17 is the peripheral request number**, not the DMA channel number.

Emu68 must reserve an otherwise unused BCM2837 DMA channel and program its transfer information with HDMI as the peripheral DREQ source.

Conceptually:

```text
SOURCE_AD  = audio_ring DMA bus address
DEST_AD    = 0x7e808020          // HD_MAI_DATA
PERMAP     = 17                  // HDMI DREQ
SRC_INC    = 1
DEST_INC   = 0
DEST_DREQ  = 1
transfer width = 32 bit
```

Linux's BCM2835 DMA implementation does essentially the same thing for memory-to-device transfers: the source increments, the destination is DREQ-paced, and the peripheral destination width must be four bytes.

The VC4 HDMI driver additionally declares:

```text
DMA width = 4 bytes
maxburst  = 2
```

A maximum burst of two 32-bit words is particularly natural for our stereo stream because it corresponds to one complete L/R HDMI frame.

---

## 8. DMA buffer restrictions

This part should be treated as part of the hardware ABI, not as an implementation detail.

### Location

The HDMI audio ring should live in **Raspberry Pi ARM RAM**, not Amiga Chip RAM.

That gives us:

```text
FT → Emu68-owned ARM buffer → HDMI DMA
```

and guarantees that HDMI output never initiates another Amiga memory fetch.

The region must be:

- permanently allocated while audio is active;
- physically stable;
- visible to the BCM DMA engine;
- outside temporary stack/storage;
- protected against unrelated Emu68 allocations.

A small statically reserved region is probably preferable for the first implementation.

### Address translation

A CPU pointer must never be copied directly into a DMA control block.

Broadcom distinguishes:

```text
CPU virtual address
CPU physical address
DMA/system bus address
```

and DMA transactions use bus addresses. The BCM2835 documentation describes RAM DMA addresses in the bus alias beginning at `0xc0000000`.

For Emu68, I would therefore add an explicit helper such as:

```c
uintptr_t emu68_dma_bus_address(const void *ptr);
```

rather than spreading address-mask/alias assumptions throughout the HDMI code.

The exact Pi 3 mapping should be validated against Emu68's existing memory layout before upstreaming.

### Alignment

The PCM/IEC958 data itself is transferred using 32-bit accesses, so:

```text
audio buffer base: at least 4-byte aligned
transfer length:   multiple of 4 bytes
stereo periods:    multiple of 8 bytes
```

For cache management and cleaner ownership boundaries, the actual ring and individual periods should preferably begin on cache-line boundaries.

DMA **Control Blocks have a stronger hardware requirement**: each CB is eight 32-bit words, 32 bytes total, and must begin at a 32-byte-aligned address.

That requirement is mandatory.

---

## 9. Cyclic DMA instead of restarting transfers

The preferred design is a permanently running cyclic DMA chain:

```text
period 0 ─► period 1 ─► period 2 ─► period 3
   ▲                                      │
   └──────────────────────────────────────┘
```

The BCM DMA engine natively supports linked control blocks, allowing one transfer to load the next without software intervention.

Linux uses the same mechanism for cyclic audio DMA. It also explicitly warns that the complete buffer length should be an exact multiple of the period length, otherwise interrupt timing can become irregular enough to produce audible clicks.

Therefore:

```text
ring_size % period_size == 0
```

should be an invariant.

A reasonable starting configuration would be:

```text
4 periods
1536 bytes per period
6144 bytes total
```

Why 1536 bytes?

An IEC958 block contains 192 stereo frames:

```text
192 frames × 8 bytes = 1536 bytes
```

At 48 kHz:

```text
192 / 48000 = 4 ms
```

Therefore four periods provide:

```text
16 ms total ring capacity
4 ms producer granularity
```

This has another useful property: each DMA period corresponds exactly to one IEC958 channel-status block, making the `B` preamble and 192-frame channel-status sequence trivial to maintain.

This exact period size is a design recommendation, not a hardware requirement.

---

## 10. Cache coherency and ownership

The producer and DMA must never modify/read the same period simultaneously.

Each period has a simple ownership cycle:

```text
FREE
  │
  ▼
Emu68 fills IEC958 words
  │
  ▼
cache clean / memory barrier
  │
  ▼
READY FOR DMA
  │
  ▼
DMA consumes period
  │
  ▼
FREE
```

The DMA buffer must either use a mapping whose coherency semantics are suitable for DMA, or Emu68 must explicitly clean modified cache lines before transferring ownership to DMA.

Similarly, DMA descriptors must be fully written and made visible before the DMA channel is activated.

The Broadcom documentation specifically requires memory ordering precautions around peripheral accesses.

The upstream version should hide this behind small architecture helpers rather than having HDMI code issue arbitrary cache operations.

---

## 11. Interrupt rate

There is no reason to interrupt the ARM for every sample.

With a 1536-byte period:

```text
250 DMA periods/s
```

because each period represents 4 ms.

That is already extremely modest.

We can use one DMA completion interrupt per period to advance the consumer/producer state, or potentially reduce this further after validation.

Most importantly, this is **not a CPU1 workload**.

There is no permanently spinning audio core and no FIFO polling loop.

---

## 12. What happens when FT data is late?

HDMI must remain a continuous clocked audio stream.

If the next FT-derived period is not ready before its deadline, the HDMI DMA engine should not be stopped and restarted. The producer should submit valid IEC958 silence for the missing region and continue.

This gives predictable failure behaviour:

```text
FT temporary starvation
        │
        ▼
IEC958 silence
        │
        ▼
DMA continues
        │
        ▼
HDMI remains synchronized
```

Useful counters for development would be:

```text
ft_audio_packets
ft_audio_overruns
hdmi_dma_periods
hdmi_underruns
mai_fifo_errors
silence_periods
```

but diagnostics must remain outside the critical path.

---

## 13. FT → Emu68 interface

The FT protocol should be kept independent from the HDMI hardware.

Framethrower's responsibility is:

```text
observe original RGA audio activity
capture the required data
preserve ordering/timing information
deliver it efficiently to Emu68
```

It should not know about:

```text
HDMI registers
VC4 DMA control blocks
IEC958 packet RAM
HSM clocks
CTS/N
```

Likewise, the HDMI backend should know nothing about:

```text
Amiga Chip RAM addresses
Paula DMA pointers
Paula sample fetching
68000 bus transactions
```

The clean boundary is therefore:

```text
Framethrower capture
        │
        ▼
small well-defined audio stream/event interface
        │
        ▼
Emu68 HDMI formatter
        │
        ▼
cyclic DMA
```

The precise FT packet format should be designed only after the first RGA audio capture is validated. We should not bake assumptions about Paula internals into the HDMI DMA layer.

---

## 14. Proposed Emu68 implementation

The old `hdmi_audio_poc.c` has already validated register programming, clock calculation, HDMI packet setup and direct MAI writes. Those pieces can be extracted into a much smaller upstream-oriented backend.

A clean structure could be:

```text
hdmi_audio.c
    hdmi_audio_init()
    hdmi_audio_start()
    hdmi_audio_stop()

hdmi_audio_dma.c
    hdmi_dma_init()
    hdmi_dma_start()
    hdmi_dma_period_complete()

hdmi_audio_buffer.c
    HDMI DMA ring
    IEC958 packing
    producer/consumer ownership

framethrower_audio.c
    FT transport only
```

There should be **no** dependency on:

```text
paula_live
paula_mirror
paula_replay
paula Chip RAM cache
CPU1 secondary_boot audio loop
```

The final steady-state path becomes:

```text
FT produces captured audio
          │
          ▼
Emu68 fills one RAM period
          │
          ▼
cache ownership handoff
          │
          ▼
BCM DMA, DREQ 17
          │
          ▼
HD_MAI_DATA
          │
          ▼
HDMI
```

This is the architecture worth targeting for upstream: small, hardware-driven, bounded, and largely independent of Emu68's 68k emulation workload.

## Final design principle

**Framethrower captures. Emu68 buffers and formats. DMA feeds HDMI.**

No Paula re-emulation.  
No duplicated Chip RAM traffic.  
No CPU1 audio engine.  
No per-sample MMIO polling.

The CPU should only touch audio in batches when a DMA period needs to be prepared. Everything after that point should be paced by the existing BCM2837 DMA and VC4 MAI hardware.

---

## References

- [Framethrower Denise firmware](https://github.com/PiStorm/Framethrower_Denise/tree/main/Firmware)
- [Linux VC4 HDMI driver](https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/gpu/drm/vc4/vc4_hdmi.c)
- [Linux VC4 HDMI register definitions](https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/gpu/drm/vc4/vc4_hdmi_regs.h)
- [Linux BCM2835 DMA driver](https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/dma/bcm2835-dma.c)
- [Raspberry Pi BCM2835 common device tree](https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/arch/arm/boot/dts/broadcom/bcm2835-common.dtsi)
- [BCM2835 ARM Peripherals](https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf)
- Amiga Hardware Reference Manual, Audio Hardware chapter and custom register reference.
- Framethrower developer statement on Discord, quoted above.

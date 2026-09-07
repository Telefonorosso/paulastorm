# amiga paula 8364 emulator

**by Akustikrausch (Andreas Wendorf)**

<p>
  <img src="https://img.shields.io/badge/license-MIT-3da639" alt="license MIT">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599c" alt="C++17">
  <img src="https://img.shields.io/badge/deps-none-brightgreen" alt="no deps">
  <img src="https://img.shields.io/badge/just-1%20header%20%2B%201%20source-blueviolet" alt="1 header + 1 source">
  <img src="https://img.shields.io/badge/chip-Amiga%20Paula%208364-ff5e00" alt="Amiga Paula 8364">
  <a href="https://github.com/akustikrausch/FXChainPlayer-Releases"><img src="https://img.shields.io/badge/proven%20in-FXChainPlayer-6c7bff" alt="proven in FXChainPlayer"></a>
</p>

a tiny, clean-room emulator of **paula** — the 4-channel sample-dma sound chip
in every commodore amiga (ocs/ecs/aga). pure c++, no dependencies, one header
and one source file. MIT.

you feed it a read callback over your own "chip ram", poke the same `$dff0xx`
registers a real replayer pokes, and pull stereo float audio out. that's the
whole api.

```cpp
amiga::Paula paula;                       // pal colour-clock by default
paula.setReadByteCallback(myChipRamRead); // how paula reads samples

paula.writeRegister16(0xDFF0A0, hi);      // audxlch  } sample pointer
paula.writeRegister16(0xDFF0A2, lo);      // audxlcl  }
paula.writeRegister16(0xDFF0A4, len);     // audxlen  (in words)
paula.writeRegister16(0xDFF0A6, period);  // audxper  (colourclock / rate)
paula.writeRegister16(0xDFF0A8, vol);     // audxvol  (0..64)
paula.writeRegister16(0xDFF096, 0x8201);  // dmacon: master + ch0 on

paula.render(outL, outR, frames, 48000);  // stereo float, ch0+3 L, 1+2 R
```

## why this exists

if you want to play amiga music — tracker replayers, custom 68k players, your
own chiptune engine — you usually reach for **UADE**. uade is great. uade is
also **GPL**, and it drags in a whole UAE-derived amiga. that's a non-starter
for a lot of projects (anything permissively licensed, anything commercial,
anything that just wants the sound chip and nothing else).

this is the other option: just paula, **MIT**, ~400 lines, zero deps. drop the
two files in and go. no full machine, no graphics, no disk — the sound chip,
done properly, and out of your way.

it's deliberately *not* a cycle-accurate silicon model. it nails the behaviour
that actually matters for music: 8-bit sample dma, period→pitch, the audxlen
loop reload, the dmacon master + per-channel enables, and the ch0+3/1+2 stereo
split. it resamples up to whatever output rate you ask for — nearest-neighbour
by default for the bright, authentic amiga character, or switch to linear for a
softer image. note-ons get a ~2 ms anti-click gain glide so volume changes
don't zipper.

## proven, not a toy

this is the exact paula that ships inside **FXChainPlayer**, where it does real
work two different ways:

- it drives the amiga demoscene **PreTracker** (`.prt`) corpus — the original
  68k replayer runs on a 68000 core, writes `$dff0xx`, and *this* chip turns it
  into sound. plays the pouët / demozoo `.prt` catalogue.
- it's driven straight from c++ by a clean-room **musicline editor** engine,
  with no cpu emulation at all — same chip, fed by hand.

so the audio path has been hammered on by real material from both sides. see
it running in a shipping player here:

→ **https://github.com/akustikrausch/FXChainPlayer-Releases**

## build the example

```sh
g++ -std=c++17 -O2 -I. example/example.cpp paula.cpp -o paulatest && ./paulatest
```

writes `paula.wav` — a little 4-voice chord. that's all the integration there
is: a read callback, a few register writes, `render()`.

## the registers it understands

| reg            | offset    | what |
|----------------|-----------|------|
| `audxlch/lcl`  | `$a0/$a2` | sample start pointer (per channel, +$10 each) |
| `audxlen`      | `$a4`     | length in words (loop length) |
| `audxper`      | `$a6`     | period — pitch = colourclock / period |
| `audxvol`      | `$a8`     | volume 0..64 |
| `dmacon`       | `$96`     | bit 9 master, bits 0..3 per-channel enable |
| `intreq/ena`   | `$9c/$9a` | audio-finished irq bits (loop point) |
| `vhposr/vpos`  | `$06/$04` | synthetic beam counter for replayers that poll it |

period→hz uses the colour clock: pal `3.546895 MHz`, ntsc `3.579545 MHz`
(`paula.setClockHz()` to switch).

## a few extras

beyond poking raw registers, a handful of calls cover the things real music
needs that aren't a single register write:

```cpp
// two-stage loop: play a one-shot intro once, then loop a body region forever.
// latches the loop region the channel reloads on its NEXT block-wrap, without
// touching the live playhead — the amiga audxlc+audxlen-while-playing trick.
paula.setLoop(ch, loopByteAddr, loopLenWords);

// resampling: nearest (default, bright/authentic) or linear (softer).
paula.setInterpolation(amiga::Paula::Interp::Linear);

// stereo separation 0..1: 1.0 = the hard amiga ch0+3/1+2 ping-pong,
// lower blends toward centre for a natural image on headphones (default 0.85).
paula.setStereoSeparation(0.7f);

// mute a channel for stems / soloing — it keeps running, only the mix drops,
// so unmuting is glitch-free.
paula.setChannelMuted(2, true);
```

## what it doesn't do

audio modulation (adkcon amod), blitter-fed samples, and the analog
reconstruction filter — out of scope on purpose. feed `render()`'s output to
your own resampler / filter if you want the soft-amiga lowpass.

## license

MIT. do whatever, keep the notice. © 2026 akustikrausch / andreas wendorf.

clean-room from the amiga hardware reference manual + the usual public docs —
not a line from any other emulator.

---

*greetz to everyone still making the little chip sing.*

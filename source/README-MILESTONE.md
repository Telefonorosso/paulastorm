# Paulamixer v2 — Live R/W milestone

Date: 2026-09-08
Base: the clean Emu68-paula checkout supplied as Emu68-paula-base.tar.gz.
Target: Raspberry Pi 3A+ / PiStorm Classic.

This archive contains every production source file changed or added relative
to that clean checkout, including the complete HDMI Audio FIX1 implementation,
the Paulamixer v2 live backend, the native AmigaOS GUI and PaulaProbe CLI.
It also retains the MIT license for the Paula core.

It does not contain the unmodified Emu68 tree, historical transition patches,
upstream reference copies, tests, or generated build artifacts. The source
paths are preserved so the files can be copied into the matching clean tree.

The user successfully compiled the integrated firmware in Docker and
subsequently reported "PREFETTO!!!" after testing it. No source changes
were made after that report. The compiled Emu68.img was not uploaded and
is not included in this archive.

Build firmware in the existing cross-build environment:

    cmake -S . -B build -DTARGET=raspi64 -DVARIANT=pistorm-classic \
      -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-gnu.cmake
    cmake --build build -j$(nproc)

Build the AmigaOS GUI:

    m68k-amigaos-gcc -O2 -m68020 -noixemul -Iinclude PaulaMixer.c -o PaulaMixer

Preserve the known-good FIX1 image before replacing the boot image.
The matching v2 protocol header is include/paula_probe.h.

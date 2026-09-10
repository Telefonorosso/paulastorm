/*
    Minimal HDMI audio proof-of-concept for BCM2837 / Raspberry Pi 3.

    Goal: prove that Emu68 can add digital audio to the HDMI link already
    configured by the Raspberry Pi firmware, without touching the video mode.

    The POC is deliberately simple:
      - CPU1 feeds the HDMI MAI FIFO in polling mode.
      - 48 kHz, stereo, 24-bit IEC958 subframes.
      - 440 Hz square wave, about -18 dBFS.
      - no DMA, no interrupts, no Paula emulation.
      - no HDMI reset and no video register programming.

    Hardware register layout and semantics follow the public BCM2835/VC4 HDMI
    documentation and Linux VC4 register definitions.  This implementation is
    original code for Emu68 and does not incorporate Circle source code.

    SPDX-License-Identifier: MPL-2.0
*/

#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include "support.h"
#include "hdmi_audio_poc.h"
#include "paula/paula_live.h"
#include "paula/paula_mirror.h"
#include "paula_probe.h"
#include "paula/paula_replay.h"

#define ARM_IO_BASE            ((uintptr_t)0xf2000000ULL)
#define ARM_HDMI_BASE          (ARM_IO_BASE + 0x902000)
#define ARM_HD_BASE            (ARM_IO_BASE + 0x808000)
#define ARM_CM_BASE            (ARM_IO_BASE + 0x101000)

/* BCM2835/BCM2837 VC4 HDMI register offsets. */
#define HDMI_AUDIO_PACKET_CFG  (ARM_HDMI_BASE + 0x09c)
#define HDMI_MAI_CHANNEL_MAP   (ARM_HDMI_BASE + 0x090)
#define HDMI_MAI_CONFIG        (ARM_HDMI_BASE + 0x094)
#define HDMI_CRP_CFG           (ARM_HDMI_BASE + 0x0a8)
#define HDMI_CTS_0             (ARM_HDMI_BASE + 0x0ac)
#define HDMI_CTS_1             (ARM_HDMI_BASE + 0x0b0)
#define HDMI_RAM_PACKET_CFG    (ARM_HDMI_BASE + 0x0a0)
#define HDMI_RAM_PACKET_STATUS (ARM_HDMI_BASE + 0x0a4)
#define HDMI_RAM_AUDIO_0       (ARM_HDMI_BASE + 0x490)
#define HDMI_RAM_AUDIO_1       (ARM_HDMI_BASE + 0x494)
#define HDMI_RAM_AUDIO_2       (ARM_HDMI_BASE + 0x498)
#define HDMI_RAM_AUDIO_8       (ARM_HDMI_BASE + 0x4b0)
#define HDMI_TX_PHY_CTL_0      (ARM_HDMI_BASE + 0x2c4)

#define HD_MAI_CTL             (ARM_HD_BASE + 0x014)
#define HD_MAI_THR             (ARM_HD_BASE + 0x018)
#define HD_MAI_FMT             (ARM_HD_BASE + 0x01c)
#define HD_MAI_DATA            (ARM_HD_BASE + 0x020)
#define HD_MAI_SMP             (ARM_HD_BASE + 0x02c)

#define CM_HSMCTL              (ARM_CM_BASE + 0x088)
#define CM_HSMDIV              (ARM_CM_BASE + 0x08c)

#define RAM_PACKET_ENABLE      (1u << 16)
#define RAM_PACKET_AUDIO       (1u << 4)

#define MAI_CTL_RESET          (1u << 0)
#define MAI_CTL_ERROR_FULL     (1u << 1)
#define MAI_CTL_ERROR_EMPTY    (1u << 2)
#define MAI_CTL_ENABLE         (1u << 3)
#define MAI_CTL_CHANNELS_2     (2u << 4)
#define MAI_CTL_FLUSH          (1u << 9)
#define MAI_CTL_FULL           (1u << 11)
#define MAI_CTL_WHOLE_SAMPLE   (1u << 12)
#define MAI_CTL_CHANNEL_ALIGN  (1u << 13)
#define MAI_CTL_DELAYED        (1u << 15)

#define MAI_CFG_BIT_REVERSE    (1u << 26)
#define MAI_CFG_FMT_REVERSE    (1u << 27)

#define AUDIO_PKT_ZERO_INACTIVE (1u << 24)
#define AUDIO_PKT_ZERO_FLAT     (1u << 29)
#define AUDIO_PKT_B_PREAMBLE    (0x0fu << 10)
#define AUDIO_PKT_CEA_STEREO    0x03u

#define CRP_EXTERNAL_CTS       (1u << 24)
#define TX_PHY_RNG_POWER_DOWN  (1u << 25)

#define SAMPLE_RATE            48000u
#define IEC958_FRAMES_PER_BLOCK 192u
#define IEC958_B_PREAMBLE       0x0fu

/* Raspberry Pi firmware clock property ID: pixel clock. */

/* BCM2835/2837 PLLH pixel-clock registers, following Circle's
 * hdmisoundbasedevice.cpp. Read-only: no PLL or video-clock writes. */
#define A2W_PLLH_CTRLR          (ARM_CM_BASE + 0x1960)
#define A2W_PLLH_FRACR          (ARM_CM_BASE + 0x1a60)
#define A2W_PLLH_ANA0           (ARM_CM_BASE + 0x1070)
#define PLLH_FB_PREDIV_MASK     (1u << 11)
#define A2W_PLL_FRAC_BITS       20u
#define A2W_PLL_FRAC_MASK       ((1u << A2W_PLL_FRAC_BITS) - 1u)
#define A2W_PLL_CTRL_PDIV_MASK  0x7000u
#define A2W_PLL_CTRL_PDIV_SHIFT 12u
#define A2W_PLL_CTRL_NDIV_MASK  0x3ffu
#define PLLH_PIX_FIXED_DIVIDER  10u
#define RPI3_OSCILLATOR_HZ      19200000u


static inline uint64_t read_cntvct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void delay_ms(unsigned ms)
{
    const uint64_t start = read_cntvct();
    const uint64_t ticks = (read_cntfrq() * ms) / 1000u;
    while ((read_cntvct() - start) < ticks)
        __asm__ volatile("yield");
}


/* POC3 diagnostic mailbox: CPU1 produces, CPU0 renders.
 * The lock is held only for small RAM copies, never for MMIO or rendering. */
#define POC_LINES 12
#define POC_LINE_LENGTH 96

static struct {
    unsigned char lock;
    unsigned count;
    char lines[POC_LINES][POC_LINE_LENGTH];
    char status[POC_LINE_LENGTH];
} poc_diag;
static unsigned poc_start;

struct poc_buffer { char *p; unsigned len, cap; };

static void poc_format_char(void *ctx, char c)
{
    struct poc_buffer *b = (struct poc_buffer *)ctx;
    if (b->len + 1 < b->cap)
        b->p[b->len++] = c;
}

static int poc_try_lock(void)
{
    return !__atomic_test_and_set(&poc_diag.lock, __ATOMIC_ACQUIRE);
}

static void poc_unlock(void)
{
    __atomic_clear(&poc_diag.lock, __ATOMIC_RELEASE);
}

/* Fixed-size diagnostic copies: no libc fortify dependency. */
static void poc_copy(void *destination, const void *source, unsigned length)
{
    unsigned char *dst = (unsigned char *)destination;
    const unsigned char *src = (const unsigned char *)source;
    for (unsigned i = 0; i < length; ++i)
        dst[i] = src[i];
}

static void poc_message(int live, const char *format, ...)
{
    char text[POC_LINE_LENGTH];
    struct poc_buffer b = {text, 0, sizeof(text)};
    va_list ap;
    va_start(ap, format);
    vkprintf_pc(poc_format_char, &b, format, ap);
    va_end(ap);
    text[b.len] = 0;

    /* A stalled core must never make diagnostic logging block forever. */
    const uint64_t started = read_cntvct();
    const uint64_t timeout = read_cntfrq() / 100u; /* 10 ms */
    while (!poc_try_lock()) {
        if (read_cntvct() - started >= timeout)
            return;
        __asm__ volatile("yield");
    }

    char *dst;
    if (live) {
        dst = poc_diag.status;
    } else {
        unsigned n = poc_diag.count;
        if (n < POC_LINES) {
            poc_diag.count = n + 1;
        } else {
            for (unsigned i = 1; i < POC_LINES; ++i)
                poc_copy(poc_diag.lines[i - 1], poc_diag.lines[i],
                       POC_LINE_LENGTH);
            n = POC_LINES - 1;
        }
        dst = poc_diag.lines[n];
    }
    poc_copy(dst, text, b.len + 1);
    poc_unlock();
}

static int poc_snapshot(char lines[POC_LINES][POC_LINE_LENGTH],
                        char status[POC_LINE_LENGTH], unsigned *count)
{
    if (!poc_try_lock())
        return 0;
    *count = poc_diag.count;
    poc_copy(lines, poc_diag.lines, sizeof(poc_diag.lines));
    poc_copy(status, poc_diag.status, sizeof(poc_diag.status));
    poc_unlock();
    return 1;
}

/* CPU0 calls this once, after the normal PiStorm platform initialization. */
void hdmi_audio_poc_boot_test(void)
{
    char lines[POC_LINES][POC_LINE_LENGTH] = {{0}};
    char status[POC_LINE_LENGTH] = "Waiting for CPU1...";
    const char *ptrs[POC_LINES];
    unsigned count = 0;
    for (unsigned i = 0; i < POC_LINES; ++i)
        ptrs[i] = lines[i];

    const uint64_t freq = read_cntfrq();
    if (!freq)
        return;
    const uint64_t start = read_cntvct();
    const uint64_t duration = freq * 10u;

    __atomic_store_n(&poc_start, 1u, __ATOMIC_RELEASE);
    do {
        poc_snapshot(lines, status, &count);
        uint64_t elapsed = read_cntvct() - start;
        unsigned left = elapsed >= duration ? 0 :
            (unsigned)((duration - elapsed + freq - 1u) / freq);
        hdmi_audio_poc_splash(ptrs, count, status, left);
        delay_ms(100);
    } while (read_cntvct() - start < duration);

    poc_snapshot(lines, status, &count);
    hdmi_audio_poc_splash(ptrs, count, status, 0);
}

static int wait_packet_status(uint32_t mask, int want_set, unsigned timeout_ms)
{
    while (timeout_ms-- != 0)
    {
        const int is_set = (rd32le(HDMI_RAM_PACKET_STATUS) & mask) != 0;
        if (is_set == want_set)
            return 1;
        delay_ms(1);
    }
    return 0;
}

/*
 * HSM is normally sourced from PLLD_PER on Pi 3.  We deliberately support
 * only the two parent sources whose rate is unambiguous here.  If firmware
 * chose another parent, fail rather than guess and risk programming garbage.
 */
static uint32_t get_hsm_clock_rate(void)
{
    const uint32_t ctl = rd32le(CM_HSMCTL);
    const uint32_t src = ctl & 0x0fu;
    uint64_t parent_rate;

    if (src == 1u)             /* oscillator */
        parent_rate = 19200000ULL;
    else if (src == 6u)        /* PLLD_PER */
        parent_rate = 500000000ULL;
    else
    {
        kprintf("[HDMI-AUDIO] unsupported HSM parent source %u (CM_HSMCTL=%08x)\n",
                src, ctl);
        return 0;
    }

    /* HSM divider is 12.12 fixed point; this clock exposes 4.8 useful bits. */
    uint32_t div = rd32le(CM_HSMDIV);
    div = (div >> 4) & 0x0fffu;
    if (div == 0)
        return 0;

    return (uint32_t)((parent_rate << 8) / div);
}

/* Find N/M ~= clock/sample_rate, with N in 24 bits and M in 1..256. */

/* Direct PLLH readback. This is the Pi 1–3 method used by Circle.
 * The firmware remains the owner of all video and PLL configuration. */
static uint32_t get_pixel_clock_rate(void)
{
    const uint32_t ctrl = rd32le(A2W_PLLH_CTRLR);
    const uint32_t frac = rd32le(A2W_PLLH_FRACR) & A2W_PLL_FRAC_MASK;
    const uint32_t ana = rd32le(A2W_PLLH_ANA0 + 4u);
    uint32_t ndiv = ctrl & A2W_PLL_CTRL_NDIV_MASK;
    uint32_t pdiv = (ctrl & A2W_PLL_CTRL_PDIV_MASK) >>
                    A2W_PLL_CTRL_PDIV_SHIFT;
    uint64_t fdiv = frac;

    poc_message(0, "PLLH ctrl=%08x frac=%05x", ctrl, frac);
    poc_message(0, "PLLH ANA1=%08x NDIV=%u PDIV=%u", ana, ndiv, pdiv);

    if (ana & PLLH_FB_PREDIV_MASK) {
        ndiv *= 2u;
        fdiv *= 2u;
    }

    if (pdiv == 0 || ndiv == 0) {
        poc_message(0, "FAIL: invalid PLLH divider");
        return 0;
    }

    const uint64_t fixed = ((uint64_t)ndiv << A2W_PLL_FRAC_BITS) + fdiv;
    uint64_t rate = (uint64_t)RPI3_OSCILLATOR_HZ * fixed;
    rate /= pdiv;
    rate >>= A2W_PLL_FRAC_BITS;
    rate /= PLLH_PIX_FIXED_DIVIDER;

    if (rate == 0 || rate > 162000000u) {
        poc_message(0, "FAIL: PLLH pixel rate out of range: %u",
                    (unsigned)rate);
        return 0;
    }

    return (uint32_t)rate;
}

static int choose_mai_ratio(uint32_t clock_rate, uint32_t sample_rate,
                            uint32_t *best_n, uint32_t *best_m)
{
    uint64_t best_error = ~0ULL;
    uint32_t chosen_n = 0;
    uint32_t chosen_m = 0;

    for (uint32_t m = 1; m <= 256; ++m)
    {
        uint64_t n64 = ((uint64_t)clock_rate * m + sample_rate / 2u) / sample_rate;
        if (n64 == 0 || n64 > 0x00ffffffu)
            continue;

        uint64_t lhs = n64 * sample_rate;
        uint64_t rhs = (uint64_t)clock_rate * m;
        uint64_t err = lhs > rhs ? lhs - rhs : rhs - lhs;

        /* Compare err/m without division. */
        if (chosen_m == 0 || err * chosen_m < best_error * m)
        {
            best_error = err;
            chosen_n = (uint32_t)n64;
            chosen_m = m;
            if (err == 0)
                break;
        }
    }

    if (chosen_m == 0)
        return 0;

    *best_n = chosen_n;
    *best_m = chosen_m;
    return 1;
}

static uint32_t parity32(uint32_t x)
{
    return (uint32_t)__builtin_parity(x);
}

/* IEC60958 consumer status for 48 kHz / 24-bit PCM. */
static uint32_t iec958_word(int32_t sample, unsigned frame)
{
    static const uint8_t status[5] = {
        0x04,       /* consumer PCM, no pre-emphasis */
        0x00,
        0x00,
        0x02,       /* 48 kHz */
        0xdb        /* 24-bit, original sample frequency = 48 kHz */
    };

    uint32_t word = ((uint32_t)sample & 0x00ffffffu) << 4;

    if (frame < 40u && (status[frame >> 3] & (1u << (frame & 7u))))
        word |= 0x40000000u;

    if (parity32(word))
        word |= 0x80000000u;

    if (frame == 0)
        word |= IEC958_B_PREAMBLE;

    return word;
}

static int install_audio_infoframe(void)
{
    uint32_t cfg = rd32le(HDMI_RAM_PACKET_CFG);

    wr32le(HDMI_RAM_PACKET_CFG, cfg & ~RAM_PACKET_AUDIO);
    if (!wait_packet_status(RAM_PACKET_AUDIO, 0, 100))
        return 0;

    /* HDMI Audio InfoFrame: type 0x84, version 1, payload length 10, stereo. */
    wr32le(HDMI_RAM_AUDIO_0, 0x000a0184u);
    wr32le(HDMI_RAM_AUDIO_1, 0x00000170u);
    for (uintptr_t reg = HDMI_RAM_AUDIO_2; reg <= HDMI_RAM_AUDIO_8; reg += 4)
        wr32le(reg, 0);

    wr32le(HDMI_RAM_PACKET_CFG, cfg | RAM_PACKET_AUDIO);
    return wait_packet_status(RAM_PACKET_AUDIO, 1, 100);
}

static int hdmi_audio_init(void)
{
    poc_message(0, "Reading HDMI packet configuration");
    const uint32_t packet_cfg = rd32le(HDMI_RAM_PACKET_CFG);
    poc_message(0, "Packet cfg=%08x status=%08x", packet_cfg,
                rd32le(HDMI_RAM_PACKET_STATUS));
    if ((packet_cfg & RAM_PACKET_ENABLE) == 0)
    {
        poc_message(0, "FAIL: HDMI packet RAM disabled");
        kprintf("[HDMI-AUDIO] HDMI packet RAM is not enabled (%08x).\n", packet_cfg);
        kprintf("[HDMI-AUDIO] Sink may be DVI or firmware did not configure HDMI audio packets.\n");
        return 0;
    }

    poc_message(0, "Reading HSM clock state");
    const uint32_t hsm_rate = get_hsm_clock_rate();
    if (hsm_rate == 0)
    {
        poc_message(0, "FAIL: unsupported or invalid HSM clock");
        return 0;
    }

    poc_message(0, "HSM=%u Hz; reading PLLH pixel clock", hsm_rate);
    const uint32_t pixel_rate = get_pixel_clock_rate();
    if (pixel_rate == 0)
    {
        poc_message(0, "FAIL: PLLH pixel clock unavailable");
        return 0;
    }

    uint32_t ratio_n, ratio_m;
    if (!choose_mai_ratio(hsm_rate, SAMPLE_RATE, &ratio_n, &ratio_m))
    {
        poc_message(0, "FAIL: cannot derive MAI clock ratio");
        return 0;
    }

    const uint32_t acr_n = (SAMPLE_RATE * 128u) / 1000u;       /* 6144 @ 48 kHz */
    const uint32_t acr_cts = (uint32_t)(((uint64_t)pixel_rate * acr_n) /
                                        ((uint64_t)SAMPLE_RATE * 128u));

    kprintf("[HDMI-AUDIO] HSM=%u Hz pixel=%u Hz MAI N/M=%u/%u ACR N=%u CTS=%u\n",
            hsm_rate, pixel_rate, ratio_n, ratio_m, acr_n, acr_cts);

    poc_message(0, "HSM=%u pixel=%u N/M=%u/%u", hsm_rate, pixel_rate,
                ratio_n, ratio_m);
    poc_message(0, "ACR N=%u CTS=%u; programming audio", acr_n, acr_cts);
    /* Reset/flush MAI only.  Do not touch the HDMI video state machine. */
    wr32le(HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH | MAI_CTL_DELAYED |
                       MAI_CTL_ERROR_EMPTY | MAI_CTL_ERROR_FULL);

    wr32le(HD_MAI_SMP, (ratio_n << 8) | (ratio_m - 1u));

    /* VC4 MAI hardware sample-rate code 9 corresponds to 48 kHz; PCM = 2. */
    wr32le(HD_MAI_FMT, (9u << 8) | (2u << 16));

    /* Conservative FIFO/DREQ/panic thresholds; polling does not consume DREQ. */
    wr32le(HD_MAI_THR, 0x10101010u);

    /* Stereo, bit and format reversal required by the VC4 MAI path. */
    wr32le(HDMI_MAI_CONFIG, MAI_CFG_BIT_REVERSE | MAI_CFG_FMT_REVERSE | 0x03u);
    wr32le(HDMI_MAI_CHANNEL_MAP, 0x00000008u);

    wr32le(HDMI_AUDIO_PACKET_CFG,
           AUDIO_PKT_ZERO_FLAT | AUDIO_PKT_ZERO_INACTIVE |
           AUDIO_PKT_B_PREAMBLE | AUDIO_PKT_CEA_STEREO);

    wr32le(HDMI_CRP_CFG, CRP_EXTERNAL_CTS | acr_n);
    wr32le(HDMI_CTS_0, acr_cts);
    wr32le(HDMI_CTS_1, acr_cts);

    poc_message(0, "Installing HDMI Audio InfoFrame");
    if (!install_audio_infoframe())
    {
        poc_message(0, "FAIL: Audio InfoFrame timeout");
        return 0;
    }

    poc_message(0, "InfoFrame active; enabling MAI");
    /* Keep the HDMI PHY RNG powered while audio is active. */
    wr32le(HDMI_TX_PHY_CTL_0, rd32le(HDMI_TX_PHY_CTL_0) & ~TX_PHY_RNG_POWER_DOWN);

    wr32le(HD_MAI_CTL, MAI_CTL_CHANNELS_2 | MAI_CTL_WHOLE_SAMPLE |
                       MAI_CTL_CHANNEL_ALIGN | MAI_CTL_ENABLE);

    poc_message(0, "MAI enabled: ctl=%08x", rd32le(HD_MAI_CTL));
    return 1;
}


static int poc_wait_fifo(void)
{
    const uint64_t start = read_cntvct();
    const uint64_t timeout = read_cntfrq() / 4u; /* 250 ms */
    while (rd32le(HD_MAI_CTL) & MAI_CTL_FULL) {
        if (read_cntvct() - start >= timeout)
            return 0;
        __asm__ volatile("yield");
    }
    return 1;
}

/* Convert a bounded float sample into signed 24-bit HDMI PCM. */
static int32_t poc_float_to_s24(float sample)
{
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    return (int32_t)(sample * 8388607.0f);
}

typedef struct {
    uint32_t loc[4];
    uint16_t len[4];
    uint16_t dma;
} poc_live_state;

static void poc_prefetch_channel(const poc_live_state *state, unsigned ch)
{
    const uint32_t bytes = (state->len[ch] ? state->len[ch] : 65536u) * 2u;
    /* Only warm the beginning. The sample callback requests subsequent
     * pages as playback advances; never queue a whole 128 KiB sample. */
    paula_mirror_prefetch(state->loc[ch], bytes < 1024u ? bytes : 1024u);
}

/* Keep the original register ordering and SET/CLR semantics. This is a
 * virtual write only; the physical bus transaction has already happened. */
static void poc_apply_live_event(poc_live_state *state,
                                 const paula_mirror_event *event)
{
    if (event->width != 1u && event->width != 2u) {
        paula_probe_add(PP_IGNORED,1);
        return;
    }
    if (event->width == 2u && (event->address & 1u)) {
        paula_probe_add(PP_IGNORED,1);
        return;
    }

    const uint32_t r = event->address & 0xffeu;
    const uint16_t value = event->width == 1u
        ? (uint16_t)((event->value & 255u) * 0x0101u)
        : event->value;

    if (r >= 0xa0u && r <= 0xdau) {
        const unsigned ch = (r - 0xa0u) >> 4;
        const unsigned reg = (r - 0xa0u) & 0x0fu;
        if (ch < 4u) {
            if (reg == 0u)
                state->loc[ch] = (state->loc[ch] & 0xffffu) |
                                 ((uint32_t)value << 16);
            else if (reg == 2u) {
                state->loc[ch] = (state->loc[ch] & 0xffff0000u) | value;
                if (state->len[ch])
                    poc_prefetch_channel(state, ch);
            } else if (reg == 4u) {
                state->len[ch] = value;
                poc_prefetch_channel(state, ch);
            }
        }
    } else if (r == 0x096u) {
        const uint16_t before = state->dma;
        if (value & 0x8000u)
            state->dma |= value & 0x7fffu;
        else
            state->dma &= (uint16_t)~(value & 0x7fffu);
        if (state->dma & 0x0200u) {
            for (unsigned ch = 0; ch < 4u; ++ch) {
                const uint16_t mask = (uint16_t)(0x0200u | (1u << ch));
                if ((state->dma & mask) == mask &&
                    (before & mask) != mask)
                    poc_prefetch_channel(state, ch);
            }
        }
    }

    paula_live_write(0xdff000u | r, value);
    if (r==0x096u || (r>=0x0a0u && r<=0x0dau &&
        ((r-0x0a0u)&15u)<=4u))
        paula_live_prefetch();
}

/* Recover from a lost event or excessive backlog without permanently
 * silencing HDMI. This reconstructs the latest CPU-observed register state,
 * not the exact internal DMA byte position of physical Paula. */
static int poc_restore_live_state(poc_live_state *state,
                                  paula_mirror_snapshot *snapshot)
{
    if (!paula_mirror_snapshot_take(snapshot))
        return 0;

    paula_probe_flag(PP_FLAG_CAPTURE_FAILED,1);
    paula_live_reset();
    for (unsigned ch = 0; ch < 4u; ++ch) {
        state->loc[ch] = 0;
        state->len[ch] = 0;
    }
    state->dma = 0;

    /* Restore pointer/length/period/volume first. DMA is enabled last. */
    for (unsigned ch = 0; ch < 4u; ++ch) {
        for (unsigned reg = 0; reg < 5u; ++reg) {
            if (!(snapshot->seen[ch] & (1u << reg)))
                continue;
            const uint32_t address = 0xdff0a0u + ch * 16u + reg * 2u;
            paula_mirror_event event;
            event.address = address;
            event.value = snapshot->reg[ch][reg];
            event.width = 2u;
            event.reserved = 0;
            event.ticks = snapshot->ticks;
            poc_apply_live_event(state, &event);
        }
    }
    if (snapshot->seen_global & 2u)
        paula_live_write(0xdff09au, (uint16_t)(0x8000u | snapshot->intena));
    if (snapshot->seen_global & 4u)
        paula_live_write(0xdff09cu, (uint16_t)(0x8000u | snapshot->intreq));
    if (snapshot->seen_global & 8u)
        paula_live_write(0xdff09eu, (uint16_t)(0x8000u | snapshot->adkcon));
    if (snapshot->seen_global & 1u) {
        paula_mirror_event event;
        event.address = 0xdff096u;
        event.value = (uint16_t)(0x8000u | snapshot->dma);
        event.width = 2u;
        event.reserved = 0;
        event.ticks = snapshot->ticks;
        poc_apply_live_event(state, &event);
    }

    paula_probe_add(PP_RESYNCS,1u);
    paula_probe_add(PP_RESYNC_SKIPPED,snapshot->skipped);
    paula_probe_flag(PP_FLAG_CAPTURE_FAILED,0);
    return 1;
}

void hdmi_audio_poc_run(void)
{
    while (!__atomic_load_n(&poc_start, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield");

    paula_probe_init();
    paula_probe_set(PP_SAMPLE_RATE,48000u);
    paula_probe_set(PP_COUNTER_FREQ,(uint32_t)read_cntfrq());
    poc_message(0, "CPU1: live Paula mirror, 48 kHz stereo");
    paula_live_init();
    paula_mirror_enable();
    poc_message(0, "Passive registers and Chip RAM reader armed");

    if (!hdmi_audio_init()) {
        paula_probe_set(PP_MAI_ERROR,1u);
        paula_probe_flag(PP_FLAG_MAI_FAILED,1);
        poc_message(1, "FAILED: see last completed stage");
        while (1) __asm__ volatile("wfe");
    }

    paula_probe_flag(PP_FLAG_HDMI_READY|PP_FLAG_RENDERING,1);
    poc_message(0, "Live source: Amiga CPU writes and Chip RAM");
    poc_message(1, "Waiting for real Paula audio");

    poc_live_state state;
    for (unsigned ch=0;ch<4u;++ch) {
        state.loc[ch]=0;
        state.len[ch]=0;
    }
    state.dma=0;

    paula_mirror_event event;
    paula_mirror_snapshot snapshot;
    uint32_t frame=0;
    uint32_t frames_written=0;
    uint32_t last_drop=paula_mirror_dropped();
    unsigned recovery_pending=0;
    uint64_t last_report=read_cntvct();
    const uint64_t frequency=read_cntfrq();
    const uint64_t latency=frequency/20u;     /* 50 ms */
    const uint64_t max_lag=frequency/4u;      /* 250 ms */
    paula_replay_clock clock;
    paula_replay_start(&clock,last_report,(uint32_t)frequency,latency);
    float left,right;

    for (;;) {
        /* CPU1 owns the mixer. Adopt controls even while replay is recovering
         * or waiting for an audio frame; never perform bus I/O here. */
        paula_live_mixer_apply();
        uint64_t now=read_cntvct();
        const uint32_t drops=paula_mirror_dropped();

        /* A lost event invalidates the exact register history. Reconstruct
         * the latest CPU-observed state instead of muting forever. Also
         * prevent a sustained backlog from reaching the ring's hard limit. */
        if (drops!=last_drop ||
            paula_mirror_pending()>=PAULA_MIRROR_EVENT_CAPACITY*3u/4u ||
            paula_replay_late(&clock,now,latency,max_lag))
            recovery_pending=1;

        if (recovery_pending) {
            paula_probe_flag(PP_FLAG_CAPTURE_FAILED,1);
            if (poc_restore_live_state(&state,&snapshot)) {
                /* Use the count captured with the snapshot. A new drop
                 * during restoration must trigger another recovery. */
                last_drop=snapshot.dropped;
                paula_replay_start(&clock,read_cntvct(),
                                   (uint32_t)frequency,latency);
                poc_message(0,"Paula replay resync=%u drop=%u",
                            paula_probe_get(PP_RESYNCS),last_drop);
                recovery_pending=0;
            } else {
                paula_probe_add(PP_SNAPSHOT_RETRIES,1u);
            }
        }
        const unsigned recovering=recovery_pending;

        /* Do not run the renderer faster than its 48 kHz audio clock.
         * MAI FIFO backpressure remains the final output pacing source. */
        while (!recovering &&
               !paula_replay_due(read_cntvct(),clock.tick+latency))
            __asm__ volatile("yield");

        /* Apply events in timestamp order at the corresponding audio frame.
         * A bounded burst never monopolizes CPU1 indefinitely. If more
         * events are due, emit silence for this frame and continue draining
         * rather than rendering audio with an incomplete register state. */
        unsigned processed=0;
        while (!recovering && processed<128u &&
               paula_mirror_peek(&event)) {
            if (!paula_replay_due(clock.tick,event.ticks))
                break;
            poc_apply_live_event(&state,&event);
            paula_mirror_pop();
            ++processed;
        }

        unsigned backlog=0;
        if (!recovering && paula_mirror_peek(&event) &&
            paula_replay_due(clock.tick,event.ticks))
            backlog=1;

        /* The sample cache is asynchronous. A missing page returns zero and
         * requests a CPU0 refill; it never makes CPU1 touch the Amiga bus. */
        left=right=0.0f;
        if (!recovering && !backlog)
            paula_live_render(&left,&right,1u);
        else
            paula_probe_add(PP_REPLAY_SILENT_FRAMES,1u);

        const int32_t sample_l=poc_float_to_s24(left);
        const int32_t sample_r=poc_float_to_s24(right);
        if (sample_l || sample_r)
            paula_probe_add(PP_NONZERO_FRAMES,1u);
        const uint32_t abs_l=(uint32_t)(sample_l<0 ? -sample_l : sample_l);
        const uint32_t abs_r=(uint32_t)(sample_r<0 ? -sample_r : sample_r);
        if (abs_l>paula_probe_get(PP_PEAK_LEFT))
            paula_probe_set(PP_PEAK_LEFT,abs_l);
        if (abs_r>paula_probe_get(PP_PEAK_RIGHT))
            paula_probe_set(PP_PEAK_RIGHT,abs_r);

        /* Preserve the validated POC5 HDMI FIFO and IEC958 write sequence. */
        if (!poc_wait_fifo()) {
            poc_message(0,"FAIL: MAI FIFO full for 250 ms");
            poc_message(1,"FIFO stalled: ctl=%08x",rd32le(HD_MAI_CTL));
            paula_probe_set(PP_MAI_ERROR,2u);
            paula_probe_flag(PP_FLAG_MAI_FAILED,1);
            paula_probe_flag(PP_FLAG_RENDERING,0);
            wr32le(HD_MAI_CTL,0);
            while (1) __asm__ volatile("wfe");
        }
        wr32le(HD_MAI_DATA,iec958_word(sample_l,frame));

        if (!poc_wait_fifo()) {
            poc_message(0,"FAIL: MAI FIFO full for 250 ms");
            poc_message(1,"FIFO stalled: ctl=%08x",rd32le(HD_MAI_CTL));
            paula_probe_set(PP_MAI_ERROR,2u);
            paula_probe_flag(PP_FLAG_MAI_FAILED,1);
            paula_probe_flag(PP_FLAG_RENDERING,0);
            wr32le(HD_MAI_CTL,0);
            while (1) __asm__ volatile("wfe");
        }
        wr32le(HD_MAI_DATA,iec958_word(sample_r,frame));

        if (++frame==IEC958_FRAMES_PER_BLOCK) frame=0;
        ++frames_written;
        paula_probe_set(PP_PCM_FRAMES,frames_written);
        paula_replay_advance(&clock);

        now=read_cntvct();
        if (now-last_report>=frequency) {
            const uint64_t target=clock.tick+latency;
            const uint64_t lag=paula_replay_due(now,target)
                ? now-target : 0;
            paula_probe_set(PP_REPLAY_LAG_US,
                (uint32_t)((lag*1000000u)/frequency));
            paula_probe_set(PP_MAI_CTL,rd32le(HD_MAI_CTL));
            poc_message(1,"PCM=%u writes=%u miss=%u drop=%u",
                        frames_written,paula_mirror_captured(),
                        paula_mirror_misses(),paula_mirror_dropped());
            last_report=now;
        }
    }
}

/*
 * Register-level live mirror of the original Akustikrausch Paula core.
 * MIT license in LICENSE. No Amiga hardware is driven from this file.
 */
#include "paula.h"
#include "paula_live.h"
#include "paula_mirror.h"
#include "paula_probe.h"

inline void *operator new(__SIZE_TYPE__, void *p) noexcept { return p; }

namespace {
alignas(amiga::Paula) unsigned char storage[sizeof(amiga::Paula)];
amiga::Paula *core = nullptr;

uint8_t read_sample(uint32_t address)
{
    return paula_mirror_read_byte(address);
}
void virtual_irq(uint16_t)
{
    paula_probe_add(PP_VIRTUAL_IRQS,1);
    /* The physical Paula remains the sole source of real audio IRQs. */
}
}

extern "C" void paula_live_init(void)
{
    core = new (storage) amiga::Paula(amiga::kPalColorClockHz);
    core->setReadByteCallback(read_sample);
    core->setInterruptCallback(virtual_irq);
    core->setStereoSeparation(1.0f);
    core->setInterpolation(amiga::Paula::Interp::Nearest);
    for (unsigned ch=0;ch<4;++ch)
        paula_probe_set(PP_CHANNEL_BASE+ch*PP_CHANNEL_STRIDE+PP_CH_PER,1);
}

extern "C" void paula_live_reset(void)
{
    if (core) core->reset();
    paula_probe_set(PP_DMACON,0);
    paula_probe_set(PP_INTENA,0);
    paula_probe_set(PP_INTREQ,0);
    paula_probe_set(PP_ADKCON,0);
    for (unsigned ch=0;ch<4;++ch) {
        const unsigned b=PP_CHANNEL_BASE+ch*PP_CHANNEL_STRIDE;
        paula_probe_set(b+PP_CH_LOC,0);
        paula_probe_set(b+PP_CH_LEN,0);
        paula_probe_set(b+PP_CH_PER,1);
        paula_probe_set(b+PP_CH_VOL,0);
        paula_probe_set(b+PP_CH_DAT,0);
        paula_probe_set(b+PP_CH_FLAGS,0);
    }
}

static void shadow_setclr(unsigned index, uint16_t value)
{
    uint32_t bits=paula_probe_get(index);
    if (value & 0x8000u) bits |= value & 0x7fffu;
    else bits &= ~(uint32_t)(value & 0x7fffu);
    paula_probe_set(index,bits);
}

extern "C" void paula_live_write(uint32_t address, uint16_t value)
{
    if (!core) return;
    core->writeRegister16(address,value);
    paula_probe_add(PP_APPLIED,1);
    const uint32_t r=address & 0xfffu;

    if (r==0x096u) {
        shadow_setclr(PP_DMACON,value);
        const uint32_t dma=paula_probe_get(PP_DMACON);
        for (unsigned ch=0;ch<4;++ch)
            paula_probe_set(PP_CHANNEL_BASE+ch*PP_CHANNEL_STRIDE+PP_CH_FLAGS,
                            ((dma & 0x0200u) && (dma & (1u<<ch))) ? 1u : 0u);
    } else if (r==0x09au) shadow_setclr(PP_INTENA,value);
    else if (r==0x09cu) shadow_setclr(PP_INTREQ,value);
    else if (r==0x09eu) shadow_setclr(PP_ADKCON,value);
    else if (r>=0x0a0u && r<=0x0dau) {
        const unsigned ch=(r-0x0a0u)>>4;
        const unsigned reg=(r-0x0a0u)&15u;
        const unsigned b=PP_CHANNEL_BASE+ch*PP_CHANNEL_STRIDE;
        if (ch>=4u) return;
        if (reg==0u) {
            paula_probe_set(b+PP_CH_LOC,
                (paula_probe_get(b+PP_CH_LOC)&0xffffu)|((uint32_t)value<<16));
        } else if (reg==2u) {
            paula_probe_set(b+PP_CH_LOC,
                (paula_probe_get(b+PP_CH_LOC)&0xffff0000u)|value);
        } else if (reg==4u) paula_probe_set(b+PP_CH_LEN,value);
        else if (reg==6u) paula_probe_set(b+PP_CH_PER,value);
        else if (reg==8u) paula_probe_set(b+PP_CH_VOL,value&0x7fu);
        else if (reg==10u) {
            paula_probe_set(b+PP_CH_DAT,value);
            paula_probe_add(PP_AUDDAT_WRITES,1);
        } else paula_probe_add(PP_IGNORED,1);
        paula_probe_add(b+PP_CH_WRITES,1);
    } else paula_probe_add(PP_IGNORED,1);
}

extern "C" void paula_live_render(float *left, float *right, unsigned frames)
{
    if (core) {
        core->render(left, right, (int)frames, 48000.0);
        paula_probe_add(PP_RENDER_FRAMES,frames);
    }
}

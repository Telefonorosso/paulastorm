/* SPDX-License-Identifier: MIT
 * Scalar diagnostic register bank. Never calls the Amiga bus or renderer.
 */
#include "paula_probe.h"

static uint32_t metrics[PAULA_PROBE_WORDS];

void paula_probe_init(void)
{
    for (unsigned i=0;i<PAULA_PROBE_WORDS;++i)
        __atomic_store_n(&metrics[i],0u,__ATOMIC_RELAXED);
}

void paula_probe_set(unsigned index, uint32_t value)
{
    if (index>=3u && index<PAULA_PROBE_WORDS)
        __atomic_store_n(&metrics[index],value,__ATOMIC_RELAXED);
}

void paula_probe_add(unsigned index, uint32_t value)
{
    if (index>=3u && index<PAULA_PROBE_WORDS)
        __atomic_add_fetch(&metrics[index],value,__ATOMIC_RELAXED);
}

void paula_probe_flag(uint32_t mask, int enabled)
{
    if (enabled)
        __atomic_fetch_or(&metrics[PP_FLAGS],mask,__ATOMIC_RELAXED);
    else
        __atomic_fetch_and(&metrics[PP_FLAGS],~mask,__ATOMIC_RELAXED);
}

uint32_t paula_probe_get(unsigned index)
{
    if (index==PP_MAGIC) return PAULA_PROBE_MAGIC;
    if (index==PP_VERSION) return PAULA_PROBE_VERSION;
    if (index==PP_WORDS) return PAULA_PROBE_WORDS;
    if (index>=PAULA_PROBE_WORDS) return 0;
    return __atomic_load_n(&metrics[index],__ATOMIC_RELAXED);
}

/* The existing Emu68 debug page 0xDEADB000 is deliberately unmapped.
 * The Amiga accesses only the first 512 bytes of that page.
 * Return CPU-native values; the existing JIT read path applies its normal
 * guest byte-order handling. No new MMU mapping or physical access.
 */
int paula_probe_bus_read(uint64_t *value, uint64_t *value2,
                         int size, uint64_t address)
{
    if (address<PAULA_PROBE_BASE ||
        address>=PAULA_PROBE_BASE+PAULA_PROBE_WORDS*4u)
        return 0;

    const uint32_t offset=(uint32_t)(address-PAULA_PROBE_BASE);
    if (value2) *value2=0;
    if (value) *value=0;
    if (!value) return 1;

    /* The Amiga CLI deliberately uses aligned 32-bit reads. Reject other
     * sizes without falling through to an unrelated physical bus handler. */
    if (size!=4 || (offset&3u)) return 1;
    *value=paula_probe_get(offset>>2);
    return 1;
}

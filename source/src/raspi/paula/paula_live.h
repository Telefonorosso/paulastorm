/* SPDX-License-Identifier: MIT */
#ifndef EMU68_PAULA_LIVE_H
#define EMU68_PAULA_LIVE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void paula_live_init(void);
void paula_live_reset(void);
void paula_live_write(uint32_t address, uint16_t value);
void paula_live_render(float *left, float *right, unsigned frames);
#ifdef __cplusplus
}
#endif
#endif

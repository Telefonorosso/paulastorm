/* Emu68 HDMI audio diagnostic POC. SPDX-License-Identifier: MPL-2.0 */
#ifndef EMU68_HDMI_AUDIO_POC_H
#define EMU68_HDMI_AUDIO_POC_H
#ifdef __cplusplus
extern "C" {
#endif
void hdmi_audio_poc_run(void) __attribute__((noreturn));
void hdmi_audio_poc_boot_test(void);
void hdmi_audio_poc_splash(const char *const *lines, unsigned count,
                           const char *status, unsigned seconds_left);
#ifdef __cplusplus
}
#endif
#endif

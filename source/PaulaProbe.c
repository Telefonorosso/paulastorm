/*
 * PaulaProbe - AmigaOS 3.x CLI monitor for Emu68 HDMI Audio POC7.
 * Read-only. Requires the matching experimental Emu68 probe firmware.
 * No audio.device, Paula register, GPIO, or Chip RAM accesses.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef PAULA_PROBE_HOST_TEST
#define SIGBREAKF_CTRL_C (1UL<<12)
unsigned long SetSignal(unsigned long, unsigned long);
void Delay(unsigned long);
#else
#include <exec/exec.h>
#include <proto/exec.h>
#include <proto/dos.h>
#endif
#include "paula_probe.h"

static uint32_t read_word(unsigned index)
{
#ifdef PAULA_PROBE_HOST_TEST
    return paula_probe_get(index);
#else
    volatile const uint32_t *p =
        (volatile const uint32_t *)(uintptr_t)PAULA_PROBE_BASE;
    return p[index];
#endif
}

static void read_all(uint32_t *v)
{
    unsigned i;
    for (i=0;i<PAULA_PROBE_WORDS;++i)
        v[i]=read_word(i);
}

static void flags_text(uint32_t flags, char *buf, size_t size)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        {PP_FLAG_ARMED,"armed"}, {PP_FLAG_HDMI_READY,"HDMI"},
        {PP_FLAG_RENDERING,"rendering"}, {PP_FLAG_CAPTURE_FAILED,"resync"},
        {PP_FLAG_MAI_FAILED,"MAI-fail"}, {PP_FLAG_CACHE_WORKER,"cache-worker"}
    };
    unsigned i;
    buf[0]=0;
    for (i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        if (flags & names[i].bit) {
            size_t used=strlen(buf);
            if (used && used+1<size) {
                buf[used++]=' ';
                buf[used]=0;
            }
            if (used+strlen(names[i].name)<size)
                strcat(buf,names[i].name);
        }
    }
    if (!buf[0]) snprintf(buf,size,"none");
}

static void print_snapshot(const uint32_t *v,const uint32_t *prev,int have_prev)
{
    unsigned ch;
    char flags[128];
    uint32_t delta_pcm=have_prev ? v[PP_PCM_FRAMES]-prev[PP_PCM_FRAMES] : 0;
    uint32_t delta_cap=have_prev ? v[PP_CAPTURED]-prev[PP_CAPTURED] : 0;
    uint32_t delta_applied=have_prev ? v[PP_APPLIED]-prev[PP_APPLIED] : 0;
    uint32_t delta_nonzero=have_prev ? v[PP_NONZERO_FRAMES]-prev[PP_NONZERO_FRAMES] : 0;
    flags_text(v[PP_FLAGS],flags,sizeof(flags));

    printf("\nPaulaProbe v%lu  |  %s\n",
           (unsigned long)v[PP_VERSION],flags);
    printf("PCM: %lu  ",(unsigned long)v[PP_PCM_FRAMES]);
    if(have_prev) printf("rate: %lu Hz  ",(unsigned long)delta_pcm);
    else printf("rate: --  ");
    printf("nonzero: %lu",(unsigned long)v[PP_NONZERO_FRAMES]);
    if(have_prev) printf(" (+%lu)",(unsigned long)delta_nonzero);
    printf("\n");
    printf("Capture: %lu (+%lu)  dropped: %lu  consumed: %lu  pending: %lu\n",
           (unsigned long)v[PP_CAPTURED],(unsigned long)delta_cap,
           (unsigned long)v[PP_DROPPED],(unsigned long)v[PP_CONSUMED],
           (unsigned long)v[PP_PENDING]);
    printf("Applied(core): %lu (+%lu)  ignored: %lu  AUDxDAT: %lu  virtual IRQ: %lu\n",
           (unsigned long)v[PP_APPLIED],(unsigned long)delta_applied,
           (unsigned long)v[PP_IGNORED],(unsigned long)v[PP_AUDDAT_WRITES],
           (unsigned long)v[PP_VIRTUAL_IRQS]);
    printf("Last write: $%06lX = $%04lX  width=%lu  ticks=%08lX%08lX\n",
           (unsigned long)v[PP_LAST_ADDR],
           (unsigned long)(v[PP_LAST_VALUE]&0xffffu),
           (unsigned long)((v[PP_LAST_VALUE]>>16)&255u),
           (unsigned long)v[PP_LAST_TICKS_HI],
           (unsigned long)v[PP_LAST_TICKS_LO]);
    printf("DMA=$%04lX INTENA=$%04lX INTREQ=$%04lX ADKCON=$%04lX\n",
           (unsigned long)v[PP_DMACON],(unsigned long)v[PP_INTENA],
           (unsigned long)v[PP_INTREQ],(unsigned long)v[PP_ADKCON]);
    printf("Chip cache: ready=%lu/8192 pending=%lu fetches=%lu requests=%lu\n",
           (unsigned long)v[PP_CACHE_READY],(unsigned long)v[PP_CACHE_PENDING],
           (unsigned long)v[PP_CACHE_FETCHES],(unsigned long)v[PP_CACHE_REQUESTS]);
    printf("Sample reads=%lu misses=%lu stale=%lu invalidations=%lu invalid-addr=%lu\n",
           (unsigned long)v[PP_SAMPLE_READS],(unsigned long)v[PP_CACHE_MISSES],
           (unsigned long)v[PP_CACHE_STALE],(unsigned long)v[PP_CACHE_INVALIDATIONS],
           (unsigned long)v[PP_CACHE_INVALID_ADDR]);
    printf("Peak L/R=%lu/%lu  MAI_CTL=$%08lX  error=%lu\n",
           (unsigned long)v[PP_PEAK_LEFT],(unsigned long)v[PP_PEAK_RIGHT],
           (unsigned long)v[PP_MAI_CTL],(unsigned long)v[PP_MAI_ERROR]);
    printf("Replay: resync=%lu skipped=%lu lag=%lu us silent=%lu retries=%lu\n",
           (unsigned long)v[PP_RESYNCS],
           (unsigned long)v[PP_RESYNC_SKIPPED],
           (unsigned long)v[PP_REPLAY_LAG_US],
           (unsigned long)v[PP_REPLAY_SILENT_FRAMES],
           (unsigned long)v[PP_SNAPSHOT_RETRIES]);
    printf("Ch  Location  Len(words) Period Volume LastDAT DMA Writes\n");
    for(ch=0;ch<4;++ch) {
        const uint32_t *c=&v[PP_CHANNEL_BASE+ch*PP_CHANNEL_STRIDE];
        printf("%u   $%08lX %-10lu %-6lu %-6lu $%04lX  %s  %lu\n",
               ch,(unsigned long)c[PP_CH_LOC],
               (unsigned long)c[PP_CH_LEN],
               (unsigned long)c[PP_CH_PER],(unsigned long)c[PP_CH_VOL],
               (unsigned long)c[PP_CH_DAT],
               (c[PP_CH_FLAGS]&1u) ? "on " : "off",
               (unsigned long)c[PP_CH_WRITES]);
    }
    printf("LEN=0 represents 65536 words when the channel is programmed.\n");
    if(v[PP_DROPPED])
        printf("NOTE: capture loss occurred; the mirror attempts automatic state recovery.\n");
    if(v[PP_AUDDAT_WRITES])
        printf("NOTE: this POC7 core does not implement direct AUDxDAT playback.\n");
    fflush(stdout);
}

int main(int argc,char **argv)
{
    uint32_t current[PAULA_PROBE_WORDS],previous[PAULA_PROBE_WORDS];
    int once=0,have_prev=0;
    int i;

    for(i=1;i<argc;++i) {
        if(!strcmp(argv[i],"ONCE") || !strcmp(argv[i],"--once"))
            once=1;
        else if(!strcmp(argv[i],"WATCH") || !strcmp(argv[i],"--watch"))
            once=0;
        else if(!strcmp(argv[i],"--help") || !strcmp(argv[i],"?")) {
            printf("Usage: PaulaProbe [ONCE|WATCH]\n");
            printf("WATCH: one report per second, Ctrl+C to exit.\n");
            return 0;
        } else {
            fprintf(stderr,"Unknown option: %s\n",argv[i]);
            return 20;
        }
    }

    /* Read the magic first. On a non-matching Emu68 build the existing
     * unmapped-Z3 handler returns its sentinel, rather than a probe record. */
    if(read_word(PP_MAGIC)!=PAULA_PROBE_MAGIC ||
       read_word(PP_VERSION)!=PAULA_PROBE_VERSION ||
       read_word(PP_WORDS)!=PAULA_PROBE_WORDS) {
        fprintf(stderr,"PaulaProbe: matching Emu68 probe firmware not detected.\n");
        fprintf(stderr,"Install the probe firmware first; no physical Paula access was made.\n");
        return 10;
    }

    printf("Read-only Emu68 Paula monitor. Ctrl+C to exit.\n");
    for(;;) {
        read_all(current);
        print_snapshot(current,previous,have_prev);
        if(once) break;
        for(i=0;i<(int)PAULA_PROBE_WORDS;++i)
            previous[i]=current[i];
        have_prev=1;
        if(SetSignal(0L,0L)&SIGBREAKF_CTRL_C) {
            SetSignal(0L,SIGBREAKF_CTRL_C);
            break;
        }
        Delay(50);
    }
    return 0;
}

/* SPDX-License-Identifier: MIT */
/* PaulaMixer - native AmigaOS 3.x GadTools UI prototype.
 * Mixer v2 live controls over the versioned Emu68 diagnostic mailbox.
 * Never writes physical Paula registers or Chip RAM.
 * Build: m68k-amigaos-gcc -O2 -m68020 -noixemul -Iinclude PaulaMixer.c -o PaulaMixer
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <exec/types.h>
#include <exec/exec.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <graphics/text.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include "paula_probe.h"

struct Library *GadToolsBase;

static struct Screen *scr;
static struct Window *win;
static struct Gadget *gad, *ctx;
static struct Gadget *master_g, *stereo_g, *interp_g;
static struct Gadget *master_value, *stereo_value;
static struct Gadget *mute_g[4], *channel_g[4];
static struct Gadget *status_g[2], *defaults_g;
static APTR vi;
static struct TextAttr font = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct MsgPort *timerport;
static struct timerequest *timerreq;
static int timer_open, timer_pending;

enum {
    G_MASTER=1, G_STEREO, G_INTERP,
    G_MUTE0, G_MUTE1, G_MUTE2, G_MUTE3,
    G_DEFAULTS, G_CLOSE
};
static ULONG master=100, stereo=100, interp=0, mute[4]={0,0,0,0};
static int present, backend, initialized, active_slider;
static uint32_t requested_word, applied_word;
static ULONG remote_defaults=PM_DEFAULT;
static char master_text[12], stereo_text[12];
static char channel_text[4][64], status_text[2][96];

#define INNER_W 468
#define INNER_H 274
#define CH_Y 110
#define CH_STEP 19

static void write_word(unsigned index, uint32_t value)
{
    volatile uint32_t *p =
        (volatile uint32_t *)(uintptr_t)PAULA_PROBE_BASE;
    p[index]=value;
}

static ULONG read_word(unsigned index)
{
    volatile const uint32_t *p =
        (volatile const uint32_t *)(uintptr_t)PAULA_PROBE_BASE;
    return p[index];
}

static struct Gadget *make(ULONG id, ULONG kind,
                            int x, int y, int w, int h,
                            const char *label, ULONG placement,
                            struct TagItem *tags)
{
    struct NewGadget ng;
    ng.ng_LeftEdge=x;
    ng.ng_TopEdge=y;
    ng.ng_Width=w;
    ng.ng_Height=h;
    ng.ng_GadgetText=(STRPTR)label;
    ng.ng_TextAttr=&font;
    ng.ng_GadgetID=id;
    ng.ng_Flags=placement;
    ng.ng_VisualInfo=vi;
    ng.ng_UserData=NULL;
    gad=CreateGadgetA(kind,gad,&ng,tags);
    return gad;
}

static struct Gadget *text_gadget(int x,int y,int w,const char *value)
{
    struct TagItem tags[] = {
        { GTTX_Text, (ULONG)value },
        { GTTX_Border, FALSE },
        { TAG_END, 0 }
    };
    return make(0,TEXT_KIND,x,y,w,12,"",PLACETEXT_LEFT,tags);
}

static void set_text(struct Gadget *g, char *buffer, const char *value)
{
    if (!g || !win || !strcmp(buffer,value))
        return;
    strcpy(buffer,value);
    GT_SetGadgetAttrs(g,win,NULL,GTTX_Text,(ULONG)buffer,TAG_END);
}

static uint32_t settings_word(void)
{
    unsigned mask=0, ch;
    for(ch=0;ch<4;ch++)
        if(mute[ch]) mask|=1u<<ch;
    return paula_mixer_pack((unsigned)master,(unsigned)stereo,
                            (unsigned)interp,mask);
}

static void enable_controls(int enabled)
{
    unsigned i;
    ULONG disabled=enabled ? FALSE : TRUE;
    GT_SetGadgetAttrs(master_g,win,NULL,GA_Disabled,disabled,TAG_END);
    GT_SetGadgetAttrs(stereo_g,win,NULL,GA_Disabled,disabled,TAG_END);
    GT_SetGadgetAttrs(interp_g,win,NULL,GA_Disabled,disabled,TAG_END);
    for(i=0;i<4;i++)
        GT_SetGadgetAttrs(mute_g[i],win,NULL,GA_Disabled,disabled,TAG_END);
    if(defaults_g)
        GT_SetGadgetAttrs(defaults_g,win,NULL,GA_Disabled,disabled,TAG_END);
}

static void load_settings(uint32_t word)
{
    unsigned i;
    if(!paula_mixer_valid(word)) return;
    master=word&127u;
    stereo=(word>>7)&127u;
    interp=(word>>14)&1u;
    GT_SetGadgetAttrs(master_g,win,NULL,GTSL_Level,master,TAG_END);
    GT_SetGadgetAttrs(stereo_g,win,NULL,GTSL_Level,stereo,TAG_END);
    GT_SetGadgetAttrs(interp_g,win,NULL,GTCY_Active,interp,TAG_END);
    for(i=0;i<4;i++) {
        mute[i]=(word>>(15+i))&1u;
        GT_SetGadgetAttrs(mute_g[i],win,NULL,GTCB_Checked,mute[i],TAG_END);
    }
}

static void send_settings(void)
{
    uint32_t word;
    if(!backend || !initialized) return;
    word=settings_word();
    if(!paula_mixer_valid(word)) return;
    write_word(PM_REQUEST,word);
    /* Read-back confirms the command was accepted, not that audio has
     * finished its gain transition. */
    requested_word=read_word(PM_REQUEST);
    if(requested_word!=word) {
        backend=0;
        initialized=0;
        active_slider=0;
        enable_controls(0);
    }
}

static void update_values(void)
{
    char s[96];
    unsigned ch;
    ULONG flags=0, dma=0;

    sprintf(s,"%3lu%%",(unsigned long)master);
    set_text(master_value,master_text,s);
    sprintf(s,"%3lu%%",(unsigned long)stereo);
    set_text(stereo_value,stereo_text,s);

    present=(read_word(PP_MAGIC)==PAULA_PROBE_MAGIC &&
             read_word(PP_VERSION)==PAULA_PROBE_VERSION &&
             read_word(PP_WORDS)==PAULA_PROBE_WORDS);
    if (present) {
        flags=read_word(PP_FLAGS);
        dma=read_word(PP_DMACON);
    }

    {
        int available=present &&
            (flags & (PP_FLAG_HDMI_READY|PP_FLAG_RENDERING)) ==
                (PP_FLAG_HDMI_READY|PP_FLAG_RENDERING) &&
            !(flags & PP_FLAG_MAI_FAILED) &&
            read_word(PM_MAGIC)==PAULA_MIXER_MAGIC &&
            read_word(PM_VERSION)==PAULA_MIXER_VERSION;
        if(available && !backend) {
            uint32_t word=read_word(PM_REQUEST);
            uint32_t defaults=read_word(PM_DEFAULTS);
            if(paula_mixer_valid(word) && paula_mixer_valid(defaults)) {
                backend=1;
                remote_defaults=defaults;
                requested_word=word;
                load_settings(word);
                initialized=1;
                enable_controls(1);
            }
        } else if(!available && backend) {
            backend=0;
            initialized=0;
            active_slider=0;
            enable_controls(0);
        }
        if(backend) {
            uint32_t word=read_word(PM_REQUEST);
            applied_word=read_word(PM_APPLIED);
            if(paula_mixer_valid(word)) {
                if(word!=requested_word && !active_slider) {
                    load_settings(word);
                    requested_word=word;
                }
            } else {
                backend=0;
                initialized=0;
                enable_controls(0);
            }
        }
    }

    for(ch=0;ch<4;ch++) {
        if(present) {
            ULONG b=PP_CHANNEL_BASE+ch*PP_CHANNEL_STRIDE;
            ULONG vol=read_word(b+PP_CH_VOL);
            sprintf(s,"CH%u   Paula vol %2lu   %s",ch+1,
                (unsigned long)vol,
                (dma & (0x0200UL|(1UL<<ch))) ==
                (0x0200UL|(1UL<<ch)) ? "DMA" : "---");
        } else {
            sprintf(s,"CH%u   --",ch+1);
        }
        set_text(channel_g[ch],channel_text[ch],s);
    }

    if(!present) {
        strcpy(s,"Firmware: unavailable");
        set_text(status_g[0],status_text[0],s);
        set_text(status_g[1],status_text[1],
                 "No matching PaulaProbe v1 interface");
    } else {
        if(flags & PP_FLAG_MAI_FAILED)
            strcpy(s,"HDMI: FAILED");
        else if(flags & PP_FLAG_HDMI_READY)
            strcpy(s,backend ? "HDMI: active - Mixer live" :
                               "HDMI: active - Mixer backend unavailable");
        else
            strcpy(s,"HDMI: initializing");
        set_text(status_g[0],status_text[0],s);
        sprintf(s,"Dropped: %lu   Resyncs: %lu%s",
            (unsigned long)read_word(PP_DROPPED),
            (unsigned long)read_word(PP_RESYNCS),
            backend && applied_word!=requested_word ? "   Applying..." : "");
        set_text(status_g[1],status_text[1],s);
    }
}

static int setup(void)
{
    int i, x, y;
    struct TagItem slider1[] = {
        { GTSL_Min,0 },{ GTSL_Max,100 },{ GTSL_Level,master },
        { GA_Immediate,TRUE },{ GA_RelVerify,TRUE },{ TAG_END,0 }
    };
    struct TagItem slider2[] = {
        { GTSL_Min,0 },{ GTSL_Max,100 },{ GTSL_Level,stereo },
        { GA_Immediate,TRUE },{ GA_RelVerify,TRUE },{ TAG_END,0 }
    };
    static STRPTR modes[] = { (STRPTR)"Nearest", (STRPTR)"Linear", NULL };
    struct TagItem cycle[] = {
        { GTCY_Labels,(ULONG)modes },{ GTCY_Active,0 },{ TAG_END,0 }
    };
    struct TagItem check[] = {
        { GTCB_Checked,FALSE },{ TAG_END,0 }
    };

    if(!(GadToolsBase=OpenLibrary("gadtools.library",37))) return 0;
    scr=LockPubScreen(NULL);
    if(!scr) return 0;
    vi=GetVisualInfo(scr,TAG_END);
    if(!vi) return 0;

    /* The layout is relative to the window's INNER area. Gadget coordinates
     * themselves are relative to the OUTER window, so add the actual borders. */
    x=scr->WBorLeft;
    y=scr->WBorTop+scr->Font->ta_YSize+1;

    if(scr->Width < INNER_W+x+scr->WBorRight ||
       scr->Height < INNER_H+y+scr->WBorBottom)
        return 0;

    if(!CreateContext(&ctx)) return 0;
    gad=ctx;

    /* Static labels are their own GadTools gadgets: no manual drawing
     * and no slider-generated percentage labels that can overwrite them. */
    /* Start empty so the first update initializes the visible text gadgets. */
    master_text[0]=0;
    stereo_text[0]=0;
    for(i=0;i<4;i++)
        sprintf(channel_text[i],"CH%d   --",i+1);
    strcpy(status_text[0],"HDMI: initializing");
    strcpy(status_text[1],"");

    master_g=make(G_MASTER,SLIDER_KIND,x+122,y+15,252,14,
                  "Master",PLACETEXT_LEFT,slider1);
    if(!master_g) return 0;
    master_value=text_gadget(x+388,y+16,60,master_text);
    if(!master_value) return 0;

    stereo_g=make(G_STEREO,SLIDER_KIND,x+122,y+42,252,14,
                  "Stereo",PLACETEXT_LEFT,slider2);
    if(!stereo_g) return 0;
    stereo_value=text_gadget(x+388,y+43,60,stereo_text);
    if(!stereo_value) return 0;

    interp_g=make(G_INTERP,CYCLE_KIND,x+122,y+69,150,16,
                  "Interpolation",PLACETEXT_LEFT,cycle);
    if(!interp_g) return 0;

    if(!text_gadget(x+12,y+94,170,"Channels")) return 0;

    for(i=0;i<4;i++) {
        channel_g[i]=text_gadget(x+12,y+CH_Y+i*CH_STEP,325,
                                 channel_text[i]);
        if(!channel_g[i]) return 0;
        mute_g[i]=make(G_MUTE0+i,CHECKBOX_KIND,
                       x+394,y+CH_Y+i*CH_STEP,26,12,
                       "Mute",PLACETEXT_RIGHT,check);
        if(!mute_g[i]) return 0;
    }

    if(!text_gadget(x+12,y+192,100,"Status")) return 0;
    status_g[0]=text_gadget(x+12,y+207,440,status_text[0]);
    if(!status_g[0]) return 0;
    status_g[1]=text_gadget(x+12,y+222,440,status_text[1]);
    if(!status_g[1]) return 0;

    /* Buttons use the native GadTools text-inside placement. */
    defaults_g=make(G_DEFAULTS,BUTTON_KIND,x+12,y+247,120,18,
             "Defaults",PLACETEXT_IN,NULL);
    if(!defaults_g) return 0;
    if(!make(G_CLOSE,BUTTON_KIND,x+336,y+247,120,18,
             "Close",PLACETEXT_IN,NULL)) return 0;

    win=OpenWindowTags(NULL,
        WA_PubScreen,(ULONG)scr,
        WA_Left,20,WA_Top,25,
        WA_InnerWidth,INNER_W,WA_InnerHeight,INNER_H,
        WA_Title,(ULONG)"Paulamixer",
        WA_Gadgets,(ULONG)ctx,
        WA_IDCMP,IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|
                 IDCMP_GADGETDOWN|IDCMP_MOUSEMOVE|
                 IDCMP_REFRESHWINDOW|IDCMP_VANILLAKEY,
        WA_Flags,WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_CLOSEGADGET|
                 WFLG_ACTIVATE|WFLG_SMART_REFRESH,
        WA_AutoAdjust,TRUE,TAG_END);
    if(win) enable_controls(0);
    return win!=NULL;
}

static void defaults(void)
{
    if(!backend) return;
    load_settings((uint32_t)remote_defaults);
    send_settings();
    update_values();
}

static void timer_start(void)
{
    if(timer_open && !timer_pending) {
        timerreq->tr_node.io_Command=TR_ADDREQUEST;
        timerreq->tr_time.tv_secs=0;
        timerreq->tr_time.tv_micro=200000;
        SendIO((struct IORequest *)timerreq);
        timer_pending=1;
    }
}

int main(void)
{
    int running=1;
    if(!setup()) {
        PutStr("PaulaMixer: cannot open GadTools window (minimum 500x300 screen).\n");
        goto done;
    }

    timerport=CreateMsgPort();
    if(timerport)
        timerreq=(struct timerequest *)CreateIORequest(timerport,sizeof(*timerreq));
    if(timerreq && !OpenDevice("timer.device",UNIT_VBLANK,
                               (struct IORequest *)timerreq,0))
        timer_open=1;

    update_values();
    timer_start();

    while(running) {
        ULONG sig=1UL<<win->UserPort->mp_SigBit;
        struct IntuiMessage *msg;
        if(timer_open) sig|=1UL<<timerport->mp_SigBit;
        sig|=SIGBREAKF_CTRL_C;
        sig=Wait(sig);
        if(sig & SIGBREAKF_CTRL_C) running=0;

        while((msg=GT_GetIMsg(win->UserPort))!=NULL) {
            ULONG cls=msg->Class;
            UWORD code=msg->Code;
            ULONG id=0;
            if(cls==IDCMP_MOUSEMOVE)
                id=(ULONG)active_slider;
            else if(msg->IAddress)
                id=((struct Gadget *)msg->IAddress)->GadgetID;
            GT_ReplyIMsg(msg);

            if(cls==IDCMP_CLOSEWINDOW ||
               (cls==IDCMP_VANILLAKEY && code==27))
                running=0;
            else if(cls==IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(win);
                GT_EndRefresh(win,TRUE);
                update_values();
            } else if(cls==IDCMP_GADGETUP || cls==IDCMP_GADGETDOWN ||
                      (cls==IDCMP_MOUSEMOVE && active_slider)) {
                if(id==G_MASTER && backend && code<=100) {
                    master=code;
                    if(cls==IDCMP_GADGETDOWN) active_slider=G_MASTER;
                    else if(cls==IDCMP_GADGETUP) active_slider=0;
                    send_settings();
                } else if(id==G_STEREO && backend && code<=100) {
                    stereo=code;
                    if(cls==IDCMP_GADGETDOWN) active_slider=G_STEREO;
                    else if(cls==IDCMP_GADGETUP) active_slider=0;
                    send_settings();
                }
                else if(id==G_INTERP && backend && cls==IDCMP_GADGETUP) {
                    interp=code;
                    send_settings();
                } else if(backend && id>=G_MUTE0 && id<=G_MUTE3) {
                    mute[id-G_MUTE0]=code?1:0;
                    send_settings();
                }
                else if(id==G_DEFAULTS && cls==IDCMP_GADGETUP) defaults();
                else if(id==G_CLOSE && cls==IDCMP_GADGETUP) running=0;
                update_values();
            }
        }

        if(timer_open && (sig & (1UL<<timerport->mp_SigBit))) {
            WaitIO((struct IORequest *)timerreq);
            timer_pending=0;
            update_values();
            timer_start();
        }
    }

done:
    if(timer_open) {
        if(timer_pending) {
            AbortIO((struct IORequest *)timerreq);
            WaitIO((struct IORequest *)timerreq);
        }
        CloseDevice((struct IORequest *)timerreq);
    }
    if(timerreq) DeleteIORequest((struct IORequest *)timerreq);
    if(timerport) DeleteMsgPort(timerport);
    if(win) CloseWindow(win);
    if(ctx) FreeGadgets(ctx);
    if(vi) FreeVisualInfo(vi);
    if(scr) UnlockPubScreen(NULL,scr);
    if(GadToolsBase) CloseLibrary(GadToolsBase);
    return 0;
}

/*
 * gpgx_config.c - MUI 3.8 configuration GUI for Genesis Plus GX Amiga port
 *
 * Author: Pedro Cotter
 *
 * Reads and writes PROGDIR:GenesisPlusGX.cfg
 * Requires: muimaster.library v19+ (MUI 3.8)
 *
 * Compile:
 *   m68k-amigaos-gcc -noixemul -O2 -Wall -Wno-pointer-sign \
 *     -o GenesisPlusGX-Config amiga/gpgx_config.c -lmui
 *
 * MUI 3.8 compatible, C89 compliant
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <libraries/mui.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/muimaster.h>
#include <proto/intuition.h>
#include <clib/alib_protos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char version[] = "$VER: GenesisPlusGX-Config 1.0 (18.03.2026) by Pedro Cotter";

/* ---- MUI library base ---- */
struct Library *MUIMasterBase = NULL;

#ifndef MUIMASTER_NAME
#define MUIMASTER_NAME "muimaster.library"
#endif
#ifndef MUIMASTER_VMIN
#define MUIMASTER_VMIN 19
#endif

/* ---- Helpers missing from some NDK versions ---- */
#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG)(a)<<24|(ULONG)(b)<<16|(ULONG)(c)<<8|(ULONG)(d))
#endif
#ifndef CheckMark
#define CheckMark(sel) MUI_MakeObject(MUIO_Checkmark,(ULONG)(sel))
#endif
#ifndef SimpleButton
#define SimpleButton(t) MUI_MakeObject(MUIO_Button,(ULONG)(t))
#endif
#ifndef Label1
#define Label1(t) MUI_MakeObject(MUIO_Label,(ULONG)(t),MUIO_Label_SingleFrame)
#endif

#define STR(s)  ((CONST_STRPTR)(s))
#define ASTR(s) ((STRPTR)(s))

/* ---- Config file path ---- */
#define CFG_PATH "PROGDIR:GenesisPlusGX.cfg"

/* ---- Return IDs ---- */
enum {
    ID_SAVE = 1,
    ID_CANCEL
};

/* ---- Audio modes ---- */
#define AUDIO_PAULA  0
#define AUDIO_14BIT  1
#define AUDIO_AHI    2
#define AUDIO_OFF    3

/* ---- Cycle entry arrays ---- */
static const char *audio_entries[] = {
    "Paula 8-bit  (direct DMA, lowest CPU)",
    "Paula 14-bit  (Buchner technique, best quality)",
    "AHI  (OS audio, highest compatibility)",
    "Off  (no sound, maximum speed)",
    NULL
};
static const char *scanline_entries[] = {
    "Off  (fastest - 1x screen)",
    "Subtle  (2x screen)",
    "Medium  (2x screen)",
    "Strong  (2x screen)",
    NULL
};
static const char *frameskip_entries[] = {
    "Off / Auto  (use Auto Skip for dynamic skip)",
    "Skip 1 of 2  (50% video)",
    "Skip 2 of 3  (33% video)",
    "Skip 3 of 4  (25% video)",
    NULL
};
static const char *boost_entries[] = {
    "1x  (no boost)",
    "2x",
    "4x",
    "8x",
    "16x",
    NULL
};

/* ---- Key table (rawkey code <-> name) ---- */
static const struct { int code; const char *name; } key_table[] = {
    {0x20,"a"},{0x35,"b"},{0x33,"c"},{0x22,"d"},{0x12,"e"},
    {0x23,"f"},{0x24,"g"},{0x25,"h"},{0x17,"i"},{0x26,"j"},
    {0x27,"k"},{0x28,"l"},{0x37,"m"},{0x36,"n"},{0x18,"o"},
    {0x19,"p"},{0x10,"q"},{0x13,"r"},{0x21,"s"},{0x14,"t"},
    {0x16,"u"},{0x34,"v"},{0x11,"w"},{0x32,"x"},{0x15,"y"},{0x31,"z"},
    {0x01,"1"},{0x02,"2"},{0x03,"3"},{0x04,"4"},{0x05,"5"},
    {0x06,"6"},{0x07,"7"},{0x08,"8"},{0x09,"9"},{0x0A,"0"},
    {0x40,"space"},{0x44,"enter"},{0x41,"backspace"},{0x42,"tab"},
    {0x5F,"help"},{0x46,"del"},
    {0x60,"lshift"},{0x61,"rshift"},{0x64,"lalt"},{0x65,"ralt"},{0x63,"lctrl"},
    {0x38,"comma"},{0x39,"period"},{0x3A,"slash"},
    {0x29,"semicolon"},{0x0B,"minus"},{0x0C,"equals"},
    {0x1A,"lbracket"},{0x1B,"rbracket"},{0x0D,"backslash"},
    {0x50,"f1"},{0x51,"f2"},{0x52,"f3"},{0x53,"f4"},{0x54,"f5"},
    {0x55,"f6"},{0x56,"f7"},{0x57,"f8"},{0x58,"f9"},{0x59,"f10"},
    {0x0F,"np0"},{0x1D,"np1"},{0x1E,"np2"},{0x1F,"np3"},
    {0x2D,"np4"},{0x2E,"np5"},{0x2F,"np6"},
    {0x3D,"np7"},{0x3E,"np8"},{0x3F,"np9"},
    {0x3C,"npdot"},{0x43,"npenter"},{0x4A,"npminus"},{0x5E,"npplus"},
    {-1, NULL}
};

static void rawkey_to_name(int code, char *buf, int bufsize)
{
    int i;
    if (code < 0) { buf[0] = 0; return; }
    for (i = 0; key_table[i].name; i++) {
        if (key_table[i].code == code) {
            strncpy(buf, key_table[i].name, bufsize - 1);
            buf[bufsize - 1] = 0;
            return;
        }
    }
    snprintf(buf, bufsize, "0x%02x", code);
}

static int name_to_rawkey(const char *name)
{
    char lower[32];
    int i, len;
    if (!name || !name[0]) return -1;
    len = strlen(name);
    if (len >= 32) len = 31;
    for (i = 0; i < len; i++)
        lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
    lower[len] = 0;
    if (lower[0] == '0' && lower[1] == 'x')
        return (int)strtol(lower, NULL, 16);
    for (i = 0; key_table[i].name; i++)
        if (strcmp(lower, key_table[i].name) == 0) return key_table[i].code;
    return -1;
}

/* ---- Config structure ---- */
typedef struct {
    int audio_mode;
    int paula_freq;
    int ahi_freq;
    int scanlines;
    int vsync;
    int frameskip;
    int autoskip;
    int fps;
    int fmvol;
    int psgvol;
    int paulaboost;
    int paulastereo;
    int paulafilter;
    int ahiboost;
    int p1_a, p1_b, p1_c, p1_x, p1_y, p1_z, p1_start, p1_mode;
    int p2_a, p2_b, p2_c, p2_x, p2_y, p2_z, p2_start, p2_mode;
} Config;

static void config_defaults(Config *c)
{
    c->audio_mode  = AUDIO_14BIT;
    c->paula_freq  = 0;       /* 0=auto (period 128, ~27710 Hz PAL) */
    c->ahi_freq    = 44100;
    c->scanlines   = 1;
    c->vsync       = 0;
    c->frameskip   = 0;
    c->autoskip    = 1;
    c->fps         = 1;
    c->fmvol       = 60;      /* FM clips blip_buf above ~65 */
    c->psgvol      = 75;
    c->paulaboost  = 0;       /* 0=1x: fmvol=60 already at good level */
    c->paulastereo = 1;
    c->paulafilter = 0;
    c->ahiboost    = 0;       /* 0=1x */
    c->p1_a = c->p1_b = c->p1_c = c->p1_x = c->p1_y = c->p1_z = -1;
    c->p1_start = c->p1_mode = -1;
    c->p2_a = c->p2_b = c->p2_c = c->p2_x = c->p2_y = c->p2_z = -1;
    c->p2_start = c->p2_mode = -1;
}

static void config_load(Config *c)
{
    BPTR f;
    char line[256], key[64], value[64];
    int pos, ch;
    char *eq;

    config_defaults(c);
    f = Open(CFG_PATH, MODE_OLDFILE);
    if (!f) return;

    for (;;) {
        pos = 0;
        while (pos < (int)sizeof(line) - 1) {
            if (Read(f, &line[pos], 1) != 1) { if (pos == 0) goto done; break; }
            ch = (unsigned char)line[pos];
            if (ch == '\n' || ch == '\r') break;
            pos++;
        }
        line[pos] = 0;
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        strncpy(key,   line,   sizeof(key)   - 1); key[sizeof(key)-1]   = 0;
        strncpy(value, eq + 1, sizeof(value) - 1); value[sizeof(value)-1] = 0;

        if      (!strcmp(key,"audio")) {
            if      (!strcmp(value,"paula")) c->audio_mode = AUDIO_PAULA;
            else if (!strcmp(value,"14bit")) c->audio_mode = AUDIO_14BIT;
            else if (!strcmp(value,"ahi"))   c->audio_mode = AUDIO_AHI;
            else if (!strcmp(value,"off"))   c->audio_mode = AUDIO_OFF;
        }
        else if (!strcmp(key,"paulafreq"))   c->paula_freq  = atoi(value);
        else if (!strcmp(key,"ahifreq"))     c->ahi_freq    = atoi(value);
        else if (!strcmp(key,"scanlines"))   c->scanlines   = atoi(value);
        else if (!strcmp(key,"vsync"))       c->vsync       = atoi(value) ? 1 : 0;
        else if (!strcmp(key,"frameskip"))   c->frameskip   = atoi(value);
        else if (!strcmp(key,"autoskip"))    c->autoskip    = atoi(value) ? 1 : 0;
        else if (!strcmp(key,"fps"))         c->fps         = atoi(value) ? 1 : 0;
        else if (!strcmp(key,"fmvol"))       c->fmvol       = atoi(value);
        else if (!strcmp(key,"psgvol"))      c->psgvol      = atoi(value);
        else if (!strcmp(key,"paulaboost"))  c->paulaboost  = atoi(value);
        else if (!strcmp(key,"paulastereo")) c->paulastereo = atoi(value) ? 1 : 0;
        else if (!strcmp(key,"paulafilter")) c->paulafilter = atoi(value) ? 1 : 0;
        else if (!strcmp(key,"ahiboost"))    c->ahiboost    = atoi(value);
        else if (!strcmp(key,"p1_a"))        c->p1_a     = name_to_rawkey(value);
        else if (!strcmp(key,"p1_b"))        c->p1_b     = name_to_rawkey(value);
        else if (!strcmp(key,"p1_c"))        c->p1_c     = name_to_rawkey(value);
        else if (!strcmp(key,"p1_x"))        c->p1_x     = name_to_rawkey(value);
        else if (!strcmp(key,"p1_y"))        c->p1_y     = name_to_rawkey(value);
        else if (!strcmp(key,"p1_z"))        c->p1_z     = name_to_rawkey(value);
        else if (!strcmp(key,"p1_start"))    c->p1_start = name_to_rawkey(value);
        else if (!strcmp(key,"p1_mode"))     c->p1_mode  = name_to_rawkey(value);
        else if (!strcmp(key,"p2_a"))        c->p2_a     = name_to_rawkey(value);
        else if (!strcmp(key,"p2_b"))        c->p2_b     = name_to_rawkey(value);
        else if (!strcmp(key,"p2_c"))        c->p2_c     = name_to_rawkey(value);
        else if (!strcmp(key,"p2_x"))        c->p2_x     = name_to_rawkey(value);
        else if (!strcmp(key,"p2_y"))        c->p2_y     = name_to_rawkey(value);
        else if (!strcmp(key,"p2_z"))        c->p2_z     = name_to_rawkey(value);
        else if (!strcmp(key,"p2_start"))    c->p2_start = name_to_rawkey(value);
        else if (!strcmp(key,"p2_mode"))     c->p2_mode  = name_to_rawkey(value);
    }
done:
    Close(f);
    if (c->scanlines  < 0) c->scanlines  = 0;
    if (c->scanlines  > 3) c->scanlines  = 3;
    if (c->frameskip  < 0) c->frameskip  = 0;
    if (c->frameskip  > 3) c->frameskip  = 3;
    if (c->fmvol     < 10) c->fmvol      = 10;
    if (c->fmvol    > 100) c->fmvol      = 100;
    if (c->psgvol    < 10) c->psgvol     = 10;
    if (c->psgvol   > 200) c->psgvol     = 200;
    if (c->paulaboost < 0) c->paulaboost = 0;
    if (c->paulaboost > 4) c->paulaboost = 4;
    if (c->ahiboost   < 0) c->ahiboost   = 0;
    if (c->ahiboost   > 4) c->ahiboost   = 4;
}

static void write_key(BPTR f, const char *cfgkey, int rawcode)
{
    char name[32], line[64];
    if (rawcode < 0) return;
    rawkey_to_name(rawcode, name, sizeof(name));
    snprintf(line, sizeof(line), "%s=%s\n", cfgkey, name);
    Write(f, line, strlen(line));
}

static void config_save(Object *app_obj, Object *win_obj, const Config *c)
{
    BPTR f;
    char line[128];
    static const char *audio_names[] = { "paula", "14bit", "ahi", "off" };

    f = Open(CFG_PATH, MODE_NEWFILE);
    if (!f) {
        MUI_Request(app_obj, win_obj, 0,
                    STR("Error"), STR("OK"),
                    STR("Cannot write " CFG_PATH));
        return;
    }

#define WR(s) Write(f, s, strlen(s))

    WR("; Genesis Plus GX - AmigaOS Configuration\n;\n");

    WR("; audio: paula (8-bit DMA), 14bit (Buchner), ahi, off\n");
    snprintf(line, sizeof(line), "audio=%s\n", audio_names[c->audio_mode]);   WR(line);

    WR("; paulafreq: 0=auto (~27710 Hz)  41243=full quality (CM4/Pi4, RTG only)\n");
    snprintf(line, sizeof(line), "paulafreq=%d\n", c->paula_freq);             WR(line);

    WR("; ahifreq: AHI sample rate in Hz\n");
    snprintf(line, sizeof(line), "ahifreq=%d\n",   c->ahi_freq);               WR(line);

    WR("; paulaboost: 0=1x 1=2x 2=4x 3=8x 4=16x\n");
    snprintf(line, sizeof(line), "paulaboost=%d\n", c->paulaboost);            WR(line);

    WR("; paulastereo: 1=stereo 0=mono (8-bit mode)\n");
    snprintf(line, sizeof(line), "paulastereo=%d\n", c->paulastereo);          WR(line);

    WR("; paulafilter: 1=enable Paula hardware low-pass filter\n");
    snprintf(line, sizeof(line), "paulafilter=%d\n", c->paulafilter);          WR(line);

    WR("; ahiboost: 0=1x 1=2x 2=4x 3=8x 4=16x\n");
    snprintf(line, sizeof(line), "ahiboost=%d\n",    c->ahiboost);             WR(line);

    WR("; fmvol: FM synthesiser preamp 10-100 (default 85)\n");
    snprintf(line, sizeof(line), "fmvol=%d\n",       c->fmvol);                WR(line);

    WR("; psgvol: PSG square-wave preamp 10-200 (default 80)\n");
    snprintf(line, sizeof(line), "psgvol=%d\n",      c->psgvol);               WR(line);

    WR(";\n; scanlines: 0=off 1=subtle 2=medium 3=strong (1-3 open 2x screen)\n");
    snprintf(line, sizeof(line), "scanlines=%d\n",   c->scanlines);            WR(line);

    WR("; vsync: 1=double-buffer tear-free (auto skip disabled when vsync=1)\n");
    snprintf(line, sizeof(line), "vsync=%d\n",       c->vsync);                WR(line);

    WR("; frameskip: 0=off/auto 1=skip1/2 2=skip2/3 3=skip3/4\n");
    snprintf(line, sizeof(line), "frameskip=%d\n",   c->frameskip);            WR(line);

    WR("; autoskip: 1=skip render when over frame budget (Pi3/slow hardware)\n");
    snprintf(line, sizeof(line), "autoskip=%d\n",    c->autoskip);             WR(line);

    WR("; fps: 1=show FPS counter\n");
    snprintf(line, sizeof(line), "fps=%d\n",         c->fps);                  WR(line);

    WR(";\n; Button mappings - leave out to use built-in defaults\n");
    WR("; Keys: a-z 0-9 space enter lshift rshift lalt ralt lctrl\n");
    WR(";   help del tab f1-f10 np0-np9 npdot npenter npminus npplus\n");
    WR(";   comma period slash semicolon minus equals lbracket rbracket backslash\n");
    WR(";   Or hex rawkey: 0x20\n;\n");
    WR("; Player 1 defaults: A=1 B=2 C=3 X=q Y=w Z=e Start=space Mode=tab\n");
    write_key(f, "p1_a",     c->p1_a);
    write_key(f, "p1_b",     c->p1_b);
    write_key(f, "p1_c",     c->p1_c);
    write_key(f, "p1_x",     c->p1_x);
    write_key(f, "p1_y",     c->p1_y);
    write_key(f, "p1_z",     c->p1_z);
    write_key(f, "p1_start", c->p1_start);
    write_key(f, "p1_mode",  c->p1_mode);
    WR(";\n; Player 2 defaults: A=np0 B=npdot C=npenter Start=help\n");
    write_key(f, "p2_a",     c->p2_a);
    write_key(f, "p2_b",     c->p2_b);
    write_key(f, "p2_c",     c->p2_c);
    write_key(f, "p2_x",     c->p2_x);
    write_key(f, "p2_y",     c->p2_y);
    write_key(f, "p2_z",     c->p2_z);
    write_key(f, "p2_start", c->p2_start);
    write_key(f, "p2_mode",  c->p2_mode);

#undef WR
    Close(f);
}

/* ---- GUI data ---- */
struct AppData {
    Object *app;
    Object *window;
    /* Video */
    Object *cy_scanlines;
    Object *cm_vsync;
    Object *cy_frameskip;
    Object *cm_autoskip;
    Object *cm_fps;
    /* Audio */
    Object *cy_audio;
    Object *str_paulafreq;
    Object *str_ahifreq;
    Object *cy_paulaboost;
    Object *cm_paulastereo;
    Object *cm_paulafilter;
    Object *cy_ahiboost;
    Object *sl_fmvol;
    Object *sl_psgvol;
    /* Player 1 */
    Object *str_p1[8];   /* a b c x y z start mode */
    /* Player 2 */
    Object *str_p2[8];
    /* Buttons */
    Object *bt_save;
    Object *bt_cancel;
};

static const char *page_titles[] = { "Video", "Audio", "Player 1", "Player 2", NULL };
static const char *p_names[] = { "A:", "B:", "C:", "X:", "Y:", "Z:", "Start:", "Mode:" };

static Object *create_gui(struct AppData *d, const Config *c)
{
    Object *win;
    char pf[16], af[16];
    char k[8][16];  /* key name buffers p1 */
    char k2[8][16]; /* key name buffers p2 */
    int i;
    int p1raw[8], p2raw[8];

    snprintf(pf, sizeof(pf), "%d", c->paula_freq);
    snprintf(af, sizeof(af), "%d", c->ahi_freq);

    p1raw[0]=c->p1_a; p1raw[1]=c->p1_b; p1raw[2]=c->p1_c; p1raw[3]=c->p1_x;
    p1raw[4]=c->p1_y; p1raw[5]=c->p1_z; p1raw[6]=c->p1_start; p1raw[7]=c->p1_mode;
    p2raw[0]=c->p2_a; p2raw[1]=c->p2_b; p2raw[2]=c->p2_c; p2raw[3]=c->p2_x;
    p2raw[4]=c->p2_y; p2raw[5]=c->p2_z; p2raw[6]=c->p2_start; p2raw[7]=c->p2_mode;

    for (i = 0; i < 8; i++) {
        rawkey_to_name(p1raw[i], k[i],  sizeof(k[i]));
        rawkey_to_name(p2raw[i], k2[i], sizeof(k2[i]));
    }

    d->app = ApplicationObject,
        MUIA_Application_Title,       STR("GenesisPlusGX Config"),
        MUIA_Application_Version,     STR(version),
        MUIA_Application_Copyright,   STR("Pedro Cotter"),
        MUIA_Application_Author,      STR("Pedro Cotter"),
        MUIA_Application_Description, STR("Genesis Plus GX Amiga configuration"),
        MUIA_Application_Base,        STR("GPGXCONFIG"),

        SubWindow, win = d->window = WindowObject,
            MUIA_Window_Title, STR("Genesis Plus GX - Configuration"),
            MUIA_Window_ID,    MAKE_ID('G','P','G','X'),

            WindowContents, VGroup,

                Child, RegisterGroup(page_titles),

                    /* ======== VIDEO ======== */
                    Child, VGroup,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("Display"),
                            Child, Label1(STR("Scanlines:")),
                            Child, d->cy_scanlines = CycleObject,
                                MUIA_Cycle_Entries, scanline_entries,
                                MUIA_Cycle_Active,  c->scanlines,
                                MUIA_ShortHelp, STR("0=off (fastest). 1-3 open a 2x screen with 4x pixel writes per frame."),
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("Sync"),
                            Child, d->cm_vsync = CheckMark(c->vsync),
                            MUIA_ShortHelp, STR("Double-buffer + ChangeScreenBuffer. Eliminates tearing. Auto Skip is disabled when VSync=on."),
                            Child, Label1(STR("VSync  (tear-free, disables Auto Skip)")),
                            Child, HSpace(0),
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("Frame Skip"),
                            Child, Label1(STR("Fixed Skip:")),
                            Child, d->cy_frameskip = CycleObject,
                                MUIA_Cycle_Entries, frameskip_entries,
                                MUIA_Cycle_Active,  c->frameskip,
                                MUIA_ShortHelp, STR("Set to 'Off/Auto' and enable Auto Skip below for best quality on slow hardware."),
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            Child, d->cm_autoskip = CheckMark(c->autoskip),
                            MUIA_ShortHelp, STR("Skip render when frame is over budget. Best for Pi 3. Leave on; automatically ignored when VSync=1."),
                            Child, Label1(STR("Auto Skip  (dynamic, best for Pi 3)")),
                            Child, HSpace(0),
                        End,

                        Child, HGroup, GroupFrame,
                            Child, d->cm_fps = CheckMark(c->fps),
                            MUIA_ShortHelp, STR("Show FPS counter in top-right corner of screen."),
                            Child, Label1(STR("Show FPS counter")),
                            Child, HSpace(0),
                        End,

                        Child, VSpace(0),
                    End, /* Video VGroup */

                    /* ======== AUDIO ======== */
                    Child, VGroup,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("Audio Mode"),
                            Child, Label1(STR("Mode:")),
                            Child, d->cy_audio = CycleObject,
                                MUIA_Cycle_Entries, audio_entries,
                                MUIA_Cycle_Active,  c->audio_mode,
                                MUIA_ShortHelp, STR("Paula 8-bit: lowest CPU. Paula 14-bit: best quality (Buchner). AHI: OS audio subsystem. Off: no sound."),
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("Sample Rates"),
                            Child, Label1(STR("Paula Freq (Hz):")),
                            Child, d->str_paulafreq = StringObject,
                                StringFrame,
                                MUIA_String_Contents, STR(pf),
                                MUIA_String_MaxLen,   8,
                                MUIA_String_Accept,   STR("0123456789"),
                                MUIA_FixWidthTxt,     STR("88888888"),
                                MUIA_ShortHelp,       STR("0=auto (~27710 Hz, period 128). 41243=full quality (CM4/Pi4 with RTG only)."),
                            End,
                            Child, Label1(STR("AHI Freq (Hz):")),
                            Child, d->str_ahifreq = StringObject,
                                StringFrame,
                                MUIA_String_Contents, STR(af),
                                MUIA_String_MaxLen,   8,
                                MUIA_String_Accept,   STR("0123456789"),
                                MUIA_FixWidthTxt,     STR("88888888"),
                                MUIA_ShortHelp,       STR("AHI sample rate in Hz (used when mode=AHI)."),
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("Volume"),
                            Child, Label1(STR("Paula Boost:")),
                            Child, d->cy_paulaboost = CycleObject,
                                MUIA_Cycle_Entries, boost_entries,
                                MUIA_Cycle_Active,  c->paulaboost,
                                MUIA_ShortHelp,     STR("Volume boost for Paula/14-bit. 4x+ can clip - use with lower FM/PSG volumes."),
                            End,
                            Child, Label1(STR("AHI Boost:")),
                            Child, d->cy_ahiboost = CycleObject,
                                MUIA_Cycle_Entries, boost_entries,
                                MUIA_Cycle_Active,  c->ahiboost,
                                MUIA_ShortHelp,     STR("Volume boost for AHI mode."),
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("FM Volume  (10-100)"),
                            Child, d->sl_fmvol = SliderObject,
                                MUIA_Numeric_Min,   10,
                                MUIA_Numeric_Max,   100,
                                MUIA_Numeric_Value, c->fmvol,
                                MUIA_ShortHelp,     STR("FM synthesiser preamp. Default 60. FM clips blip_buf above ~65; FM+PSG peaks add."),
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("PSG Volume  (10-200)"),
                            Child, d->sl_psgvol = SliderObject,
                                MUIA_Numeric_Min,   10,
                                MUIA_Numeric_Max,   200,
                                MUIA_Numeric_Value, c->psgvol,
                                MUIA_ShortHelp,     STR("PSG square-wave preamp. Default 75. PSG is quieter than FM on real hardware."),
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_ShortHelp, STR("Stereo output in Paula 8-bit mode using all 4 DMA channels (L+R doubled)."),
                            Child, d->cm_paulastereo = CheckMark(c->paulastereo),
                            Child, Label1(STR("Paula Stereo  (8-bit mode)")),
                            Child, HSpace(0),
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_ShortHelp, STR("Enable Paula hardware low-pass filter (~5 kHz). Reduces high-frequency aliasing noise."),
                            Child, d->cm_paulafilter = CheckMark(c->paulafilter),
                            Child, Label1(STR("Paula Filter")),
                            Child, HSpace(0),
                        End,

                        Child, VSpace(0),
                    End, /* Audio VGroup */

                    /* ======== PLAYER 1 ======== */
                    Child, VGroup,

                        Child, TextObject,
                            MUIA_Text_Contents, STR(
                                "\033cKey names: a-z  0-9  space  enter  lshift  rshift  lalt  ralt  lctrl\n"
                                "help  del  tab  f1-f10  np0-np9  npdot  npenter  npminus  npplus\n"
                                "comma  period  slash  semicolon  minus  equals  lbracket  rbracket\n"
                                "Or hex rawkey code: 0x20   \033nLeave empty for built-in default."),
                            TextFrame,
                        End,

                        Child, ColGroup(4), GroupFrame,
                            MUIA_FrameTitle, STR("Player 1  (defaults: A=1 B=2 C=3 X=q Y=w Z=e Start=space Mode=tab)"),

                            Child, Label1(STR("A:")),
                            Child, d->str_p1[0] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k[0]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,
                            Child, Label1(STR("B:")),
                            Child, d->str_p1[1] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k[1]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,

                            Child, Label1(STR("C:")),
                            Child, d->str_p1[2] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k[2]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,
                            Child, Label1(STR("X:")),
                            Child, d->str_p1[3] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k[3]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,

                            Child, Label1(STR("Y:")),
                            Child, d->str_p1[4] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k[4]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,
                            Child, Label1(STR("Z:")),
                            Child, d->str_p1[5] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k[5]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,

                            Child, Label1(STR("Start:")),
                            Child, d->str_p1[6] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k[6]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,
                            Child, Label1(STR("Mode:")),
                            Child, d->str_p1[7] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k[7]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,
                        End,

                        Child, VSpace(0),
                    End, /* P1 VGroup */

                    /* ======== PLAYER 2 ======== */
                    Child, VGroup,

                        Child, TextObject,
                            MUIA_Text_Contents, STR(
                                "\033cSame key names as Player 1. Leave empty for built-in default."),
                            TextFrame,
                        End,

                        Child, ColGroup(4), GroupFrame,
                            MUIA_FrameTitle, STR("Player 2  (defaults: A=np0 B=npdot C=npenter Start=help)"),

                            Child, Label1(STR("A:")),
                            Child, d->str_p2[0] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k2[0]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,
                            Child, Label1(STR("B:")),
                            Child, d->str_p2[1] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k2[1]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,

                            Child, Label1(STR("C:")),
                            Child, d->str_p2[2] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k2[2]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,
                            Child, Label1(STR("X:")),
                            Child, d->str_p2[3] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k2[3]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,

                            Child, Label1(STR("Y:")),
                            Child, d->str_p2[4] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k2[4]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,
                            Child, Label1(STR("Z:")),
                            Child, d->str_p2[5] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k2[5]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,

                            Child, Label1(STR("Start:")),
                            Child, d->str_p2[6] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k2[6]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,
                            Child, Label1(STR("Mode:")),
                            Child, d->str_p2[7] = StringObject, StringFrame,
                                MUIA_String_Contents, STR(k2[7]), MUIA_String_MaxLen, 16,
                                MUIA_FixWidthTxt, STR("lshift___"),
                            End,
                        End,

                        Child, VSpace(0),
                    End, /* P2 VGroup */

                End, /* RegisterGroup */

                /* ---- Buttons ---- */
                Child, HGroup,
                    Child, d->bt_save   = SimpleButton(STR("_Save")),
                    Child, d->bt_cancel = SimpleButton(STR("_Cancel")),
                End,

            End, /* WindowContents VGroup */
        End, /* Window */
    End; /* Application */

    if (!d->app) return NULL;

    /* Notifications */
    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             d->app, 2, MUIM_Application_ReturnID, ID_CANCEL);

    DoMethod(d->bt_save,   MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_SAVE);
    DoMethod(d->bt_cancel, MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_CANCEL);

    return d->app;
}

/* ---- Collect values from widgets into Config ---- */
static void collect(struct AppData *d, Config *c)
{
    ULONG val;
    STRPTR s;
    int i;
    int *p1dst[8], *p2dst[8];

    p1dst[0]=&c->p1_a; p1dst[1]=&c->p1_b; p1dst[2]=&c->p1_c; p1dst[3]=&c->p1_x;
    p1dst[4]=&c->p1_y; p1dst[5]=&c->p1_z; p1dst[6]=&c->p1_start; p1dst[7]=&c->p1_mode;
    p2dst[0]=&c->p2_a; p2dst[1]=&c->p2_b; p2dst[2]=&c->p2_c; p2dst[3]=&c->p2_x;
    p2dst[4]=&c->p2_y; p2dst[5]=&c->p2_z; p2dst[6]=&c->p2_start; p2dst[7]=&c->p2_mode;

    get(d->cy_audio,       MUIA_Cycle_Active,   &val); c->audio_mode  = (int)val;
    get(d->cy_scanlines,   MUIA_Cycle_Active,   &val); c->scanlines   = (int)val;
    get(d->cm_vsync,       MUIA_Selected,       &val); c->vsync       = (int)val;
    get(d->cy_frameskip,   MUIA_Cycle_Active,   &val); c->frameskip   = (int)val;
    get(d->cm_autoskip,    MUIA_Selected,       &val); c->autoskip    = (int)val;
    get(d->cm_fps,         MUIA_Selected,       &val); c->fps         = (int)val;
    get(d->cy_paulaboost,  MUIA_Cycle_Active,   &val); c->paulaboost  = (int)val;
    get(d->cy_ahiboost,    MUIA_Cycle_Active,   &val); c->ahiboost    = (int)val;
    get(d->cm_paulastereo, MUIA_Selected,       &val); c->paulastereo = (int)val;
    get(d->cm_paulafilter, MUIA_Selected,       &val); c->paulafilter = (int)val;
    get(d->sl_fmvol,       MUIA_Numeric_Value,  &val); c->fmvol       = (int)val;
    get(d->sl_psgvol,      MUIA_Numeric_Value,  &val); c->psgvol      = (int)val;

    get(d->str_paulafreq,  MUIA_String_Contents, &s);
    c->paula_freq = s ? atoi((char *)s) : 0;
    get(d->str_ahifreq,    MUIA_String_Contents, &s);
    c->ahi_freq   = s ? atoi((char *)s) : 44100;

    for (i = 0; i < 8; i++) {
        get(d->str_p1[i], MUIA_String_Contents, &s);
        *p1dst[i] = s ? name_to_rawkey((char *)s) : -1;
        get(d->str_p2[i], MUIA_String_Contents, &s);
        *p2dst[i] = s ? name_to_rawkey((char *)s) : -1;
    }
}

int main(void)
{
    struct AppData d;
    Config c;
    ULONG signals, id;
    BOOL running;

    memset(&d, 0, sizeof(d));
    config_load(&c);

    MUIMasterBase = OpenLibrary(STR(MUIMASTER_NAME), MUIMASTER_VMIN);
    if (!MUIMasterBase) {
        puts("Cannot open muimaster.library v19!");
        return 30;
    }

    if (!create_gui(&d, &c)) {
        puts("Failed to create GUI!");
        CloseLibrary(MUIMasterBase);
        return 20;
    }

    /* Open window */
    set(d.window, MUIA_Window_Open, TRUE);

    running = TRUE;
    while (running) {
        id = DoMethod(d.app, MUIM_Application_NewInput, &signals);
        switch (id) {
            case ID_SAVE:
                collect(&d, &c);
                config_save(d.app, d.window, &c);
                MUI_Request(d.app, d.window, 0,
                            STR("Saved"), STR("OK"),
                            STR("Configuration saved to\n" CFG_PATH));
                running = FALSE;
                break;
            case ID_CANCEL:
            case MUIV_Application_ReturnID_Quit:
                running = FALSE;
                break;
        }
        if (running && signals)
            Wait(signals);
    }

    set(d.window, MUIA_Window_Open, FALSE);
    MUI_DisposeObject(d.app);
    CloseLibrary(MUIMasterBase);
    return 0;
}

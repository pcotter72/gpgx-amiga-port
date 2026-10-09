/*
 * gpgx_launcher.c - MUI 3.8 game launcher + configuration GUI
 *                   for Genesis Plus GX Amiga port
 *
 * Scans ROM directories:
 *   PROGDIR:Roms/              - Genesis / Mega Drive
 *   PROGDIR:Roms/Gamegear/     - Sega Game Gear
 *   PROGDIR:Roms/Mastersystem/ - Sega Master System
 *
 * Launches PROGDIR:GenesisPlusGX with the selected ROM.
 * Reads/writes PROGDIR:GenesisPlusGX.cfg (replaces GenesisPlusGX-Config).
 *
 * Compile: make launcher
 * MUI 3.8 compatible, C89 compliant
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <libraries/mui.h>
#include <libraries/lowlevel.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/muimaster.h>
#include <proto/intuition.h>
#include <proto/lowlevel.h>
#include <clib/alib_protos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char version[] =
    "$VER: GenesisPlusGX-Launcher 1.1 (06.07.2026) by Pedro Cotter";

/* ---- MUI library ---- */
struct Library *MUIMasterBase = NULL;
struct Library *LowLevelBase  = NULL;
#ifndef MUIMASTER_NAME
#define MUIMASTER_NAME "muimaster.library"
#endif
#ifndef MUIMASTER_VMIN
#define MUIMASTER_VMIN 19
#endif

/* ---- MUI helpers ---- */
#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) \
    ((ULONG)(a)<<24|(ULONG)(b)<<16|(ULONG)(c)<<8|(ULONG)(d))
#endif
#ifndef CheckMark
#define CheckMark(sel)  MUI_MakeObject(MUIO_Checkmark,(ULONG)(sel))
#endif
#ifndef SimpleButton
#define SimpleButton(t) MUI_MakeObject(MUIO_Button,(ULONG)(t))
#endif
#ifndef Label1
#define Label1(t) \
    MUI_MakeObject(MUIO_Label,(ULONG)(t),MUIO_Label_SingleFrame)
#endif
#define STR(s)  ((CONST_STRPTR)(s))
#define ASTR(s) ((STRPTR)(s))

/* ---- Paths ---- */
#define CFG_PATH     "PROGDIR:GenesisPlusGX.cfg"
#define EMU_CMD      "PROGDIR:GenesisPlusGX"
#define ROM_PATH_GEN "PROGDIR:Roms/"
#define ROM_PATH_GG  "PROGDIR:Roms/Gamegear/"
#define ROM_PATH_SMS "PROGDIR:Roms/Mastersystem/"
#define CHEAT_DIR    "PROGDIR:Cheats/"

/* ---- Return IDs ---- */
enum { ID_SAVE = 1, ID_CANCEL, ID_LAUNCH, ID_SCAN, ID_FILTER, ID_SORT, ID_ABOUT,
       ID_ROMSEL, ID_CHEAT_TOGGLE, ID_CHEAT_ALL, ID_CHEAT_NONE,
       ID_CHEAT_SAVE, ID_CHEAT_RELOAD };

/* ---- System types ---- */
#define SYSTEM_GENESIS 0
#define SYSTEM_GG      1
#define SYSTEM_SMS     2
static const char *system_names[] = { "Genesis", "Game Gear", "Mstr.Sys" };

/* ---- Audio mode ---- */
#define AUDIO_PAULA 0
#define AUDIO_14BIT 1
#define AUDIO_AHI   2
#define AUDIO_OFF   3

/* ---- Cycle entries ---- */
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
    "Skip 4 of 5  (20% video)",
    "Skip 5 of 6  (17% video)",
    NULL
};
static const char *boost_entries[] = {
    "1x  (no boost)", "2x", "4x", "8x", "16x", NULL
};
static const char *depth_entries[] = {
    "32-bit  (PiStorm/VideoCore fast path)",
    "16-bit  (classic RTG cards, half bandwidth)",
    NULL
};
static const char *sort_entries[] = { "Sort: Name", "Sort: System", NULL };

/* ---- Key table ---- */
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
    {0x60,"lshift"},{0x61,"rshift"},{0x64,"lalt"},{0x65,"ralt"},
    {0x63,"lctrl"},
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
        lower[i] = (name[i]>='A'&&name[i]<='Z') ? name[i]+32 : name[i];
    lower[len] = 0;
    if (lower[0]=='0' && lower[1]=='x')
        return (int)strtol(lower, NULL, 16);
    for (i = 0; key_table[i].name; i++)
        if (!strcmp(lower, key_table[i].name)) return key_table[i].code;
    return -1;
}

/* ================================================================
 * CONFIG
 * ================================================================ */

typedef struct {
    int audio_mode, paula_freq, ahi_freq;
    int scanlines, vsync, frameskip, autoskip, fps;
    int fmvol, psgvol;
    int paulaboost, paulastereo, paulafilter, ahiboost;
    int paulamargin, ahimargin;
    int hqfm, hqpsg, screendepth;
    int cheats;         /* preserved so saving the config does not drop the key */
    int p1_a,p1_b,p1_c,p1_x,p1_y,p1_z,p1_start,p1_mode;
    int p2_a,p2_b,p2_c,p2_x,p2_y,p2_z,p2_start,p2_mode;
} Config;

static void config_defaults(Config *c)
{
    c->audio_mode=AUDIO_PAULA; c->paula_freq=41243; c->ahi_freq=44100;
    c->scanlines=0; c->vsync=0; c->frameskip=0; c->autoskip=1; c->fps=1;
    c->fmvol=60; c->psgvol=60;
    c->paulaboost=1; c->paulastereo=1; c->paulafilter=0; c->ahiboost=0;
    c->paulamargin=0; c->ahimargin=0;
    c->hqfm=1; c->hqpsg=1; c->screendepth=32;
    c->cheats=1;
    c->p1_a=c->p1_b=c->p1_c=c->p1_x=c->p1_y=c->p1_z=-1;
    c->p1_start=c->p1_mode=-1;
    c->p2_a=c->p2_b=c->p2_c=c->p2_x=c->p2_y=c->p2_z=-1;
    c->p2_start=c->p2_mode=-1;
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
        while (pos < (int)sizeof(line)-1) {
            if (Read(f, &line[pos], 1) != 1) { if (!pos) goto done; break; }
            ch = (unsigned char)line[pos];
            if (ch=='\n'||ch=='\r') break;
            pos++;
        }
        line[pos] = 0;
        if (!line[0] || line[0]==';' || line[0]=='#') continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        /* Trim whitespace around both halves, exactly as the emulator's own
         * parser does. Without this a hand-edited "vsync = 1" is read as the
         * key "vsync " here, silently missed, and then dropped when this
         * launcher rewrites the file on exit. */
        {
            char *src = line;
            int kl;
            while (*src == ' ' || *src == '\t') src++;
            strncpy(key, src, sizeof(key)-1);  key[sizeof(key)-1] = 0;
            kl = strlen(key);
            while (kl > 0 && (key[kl-1] == ' ' || key[kl-1] == '\t')) key[--kl] = 0;

            src = eq + 1;
            while (*src == ' ' || *src == '\t') src++;
            strncpy(value, src, sizeof(value)-1);  value[sizeof(value)-1] = 0;
            kl = strlen(value);
            while (kl > 0 && (value[kl-1] == ' ' || value[kl-1] == '\t')) value[--kl] = 0;
        }

        if (!strcmp(key,"audio")) {
            if      (!strcmp(value,"paula")) c->audio_mode=AUDIO_PAULA;
            else if (!strcmp(value,"14bit")) c->audio_mode=AUDIO_14BIT;
            else if (!strcmp(value,"ahi"))   c->audio_mode=AUDIO_AHI;
            else if (!strcmp(value,"off"))   c->audio_mode=AUDIO_OFF;
        }
        else if (!strcmp(key,"paulafreq"))   c->paula_freq  = atoi(value);
        else if (!strcmp(key,"ahifreq"))     c->ahi_freq    = atoi(value);
        else if (!strcmp(key,"scanlines"))   c->scanlines   = atoi(value);
        else if (!strcmp(key,"vsync"))       c->vsync       = atoi(value)?1:0;
        else if (!strcmp(key,"frameskip"))   c->frameskip   = atoi(value);
        else if (!strcmp(key,"autoskip"))    c->autoskip    = atoi(value)?1:0;
        else if (!strcmp(key,"fps"))         c->fps         = atoi(value)?1:0;
        else if (!strcmp(key,"fmvol"))       c->fmvol       = atoi(value);
        else if (!strcmp(key,"psgvol"))      c->psgvol      = atoi(value);
        else if (!strcmp(key,"paulaboost"))   c->paulaboost   = atoi(value);
        else if (!strcmp(key,"paulastereo")) c->paulastereo  = atoi(value)?1:0;
        else if (!strcmp(key,"paulafilter")) c->paulafilter  = atoi(value)?1:0;
        else if (!strcmp(key,"ahiboost"))    c->ahiboost     = atoi(value);
        else if (!strcmp(key,"paulamargin")) c->paulamargin  = atoi(value);
        else if (!strcmp(key,"ahimargin"))   c->ahimargin    = atoi(value);
        else if (!strcmp(key,"hqfm"))        c->hqfm         = atoi(value)?1:0;
        else if (!strcmp(key,"hqpsg"))       c->hqpsg        = atoi(value)?1:0;
        else if (!strcmp(key,"screendepth")) c->screendepth  = atoi(value);
        else if (!strcmp(key,"cheats"))      c->cheats       = atoi(value)?1:0;
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
    if (c->frameskip  > 5) c->frameskip  = 5;  /* matches the emulator's clamp */
    if (c->fmvol     <10) c->fmvol      = 10;
    if (c->fmvol    >100) c->fmvol      = 100;
    if (c->psgvol    <10) c->psgvol     = 10;
    if (c->psgvol   >200) c->psgvol     = 200;
    if (c->paulaboost < 0) c->paulaboost = 0;
    if (c->paulaboost > 4) c->paulaboost = 4;
    if (c->ahiboost   < 0) c->ahiboost  = 0;
    if (c->ahiboost   > 4) c->ahiboost  = 4;
    if (c->paulamargin < 0)   c->paulamargin = 0;
    if (c->paulamargin > 882) c->paulamargin = 882;
    if (c->ahimargin   < 0)   c->ahimargin   = 0;
    if (c->ahimargin   > 882) c->ahimargin   = 882;
    if (c->screendepth != 16 && c->screendepth != 32) c->screendepth = 32;
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
    static const char *audio_names[] = { "paula","14bit","ahi","off" };
    f = Open(CFG_PATH, MODE_NEWFILE);
    if (!f) {
        if (app_obj && win_obj)
            MUI_Request(app_obj, win_obj, 0, STR("Error"), STR("OK"),
                        STR("Cannot write " CFG_PATH));
        return;
    }
#define WR(s) Write(f, s, strlen(s))
    WR("; Genesis Plus GX - AmigaOS Configuration\n;\n");
    WR("; audio: paula (8-bit DMA), 14bit (Buchner), ahi, off\n");
    snprintf(line,sizeof(line),"audio=%s\n",audio_names[c->audio_mode]); WR(line);
    WR("; paulafreq: 0=auto (~27710 Hz)  41243=full quality (CM4/Pi4, RTG only)\n");
    snprintf(line,sizeof(line),"paulafreq=%d\n",c->paula_freq);          WR(line);
    WR("; ahifreq: AHI sample rate in Hz\n");
    snprintf(line,sizeof(line),"ahifreq=%d\n",c->ahi_freq);              WR(line);
    WR("; paulaboost: 0=1x 1=2x 2=4x 3=8x 4=16x\n");
    snprintf(line,sizeof(line),"paulaboost=%d\n",c->paulaboost);         WR(line);
    WR("; paulastereo: 1=stereo 0=mono (8-bit mode)\n");
    snprintf(line,sizeof(line),"paulastereo=%d\n",c->paulastereo);       WR(line);
    WR("; paulafilter: 1=enable Paula hardware low-pass filter\n");
    snprintf(line,sizeof(line),"paulafilter=%d\n",c->paulafilter);       WR(line);
    WR("; ahiboost: 0=1x 1=2x 2=4x 3=8x 4=16x\n");
    snprintf(line,sizeof(line),"ahiboost=%d\n",c->ahiboost);             WR(line);
    WR("; paulamargin/ahimargin: extra buffer samples to absorb timer latency.\n");
    WR(";   0=none (WinUAE/accurate timer)  200/220=PiStorm/Emu68\n");
    snprintf(line,sizeof(line),"paulamargin=%d\n",c->paulamargin);       WR(line);
    snprintf(line,sizeof(line),"ahimargin=%d\n",  c->ahimargin);         WR(line);
    WR("; hqfm/hqpsg: 1=band-limited synthesis, 0=linear (much faster FM on slow machines)\n");
    snprintf(line,sizeof(line),"hqfm=%d\n",c->hqfm);                     WR(line);
    snprintf(line,sizeof(line),"hqpsg=%d\n",c->hqpsg);                   WR(line);
    WR("; fmvol: FM synthesiser preamp 10-100 (default 60). Clips above ~65.\n");
    snprintf(line,sizeof(line),"fmvol=%d\n",c->fmvol);                   WR(line);
    WR("; psgvol: PSG square-wave preamp 10-200 (default 60)\n");
    snprintf(line,sizeof(line),"psgvol=%d\n",c->psgvol);                 WR(line);
    WR(";\n; screendepth: 32 (PiStorm/VideoCore fast path) or 16 (half blit bandwidth,\n");
    WR(";   recommended for classic Amigas with Zorro RTG cards)\n");
    snprintf(line,sizeof(line),"screendepth=%d\n",c->screendepth);       WR(line);
    WR("; scanlines: 0=off 1=subtle 2=medium 3=strong (1-3 open 2x screen)\n");
    snprintf(line,sizeof(line),"scanlines=%d\n",c->scanlines);           WR(line);
    WR("; vsync: 1=double-buffer tear-free (auto skip disabled when vsync=1)\n");
    snprintf(line,sizeof(line),"vsync=%d\n",c->vsync);                   WR(line);
    WR("; frameskip: 0=off/auto 1=skip1/2 2=skip2/3 3=skip3/4\n");
    snprintf(line,sizeof(line),"frameskip=%d\n",c->frameskip);           WR(line);
    WR("; autoskip: 1=skip render when over frame budget\n");
    snprintf(line,sizeof(line),"autoskip=%d\n",c->autoskip);             WR(line);
    WR("; fps: 1=show FPS counter\n");
    snprintf(line,sizeof(line),"fps=%d\n",c->fps);                       WR(line);
    WR("; cheats: 1=apply PROGDIR:Cheats/<romname>.cht (Mega Drive only), 0=ignore it.\n");
    WR(";   Individual codes are switched with the cheatN_enable lines in that file,\n");
    WR(";   which the launcher's Cheats page edits for you.\n");
    snprintf(line,sizeof(line),"cheats=%d\n",c->cheats);                 WR(line);
    WR(";\n; Button mappings\n");
    WR("; Player 1 defaults: A=1 B=2 C=3 X=q Y=w Z=e Start=space Mode=tab\n");
    write_key(f,"p1_a",c->p1_a); write_key(f,"p1_b",c->p1_b);
    write_key(f,"p1_c",c->p1_c); write_key(f,"p1_x",c->p1_x);
    write_key(f,"p1_y",c->p1_y); write_key(f,"p1_z",c->p1_z);
    write_key(f,"p1_start",c->p1_start); write_key(f,"p1_mode",c->p1_mode);
    WR("; Player 2 defaults: A=np0 B=npdot C=npenter Start=help\n");
    write_key(f,"p2_a",c->p2_a); write_key(f,"p2_b",c->p2_b);
    write_key(f,"p2_c",c->p2_c); write_key(f,"p2_x",c->p2_x);
    write_key(f,"p2_y",c->p2_y); write_key(f,"p2_z",c->p2_z);
    write_key(f,"p2_start",c->p2_start); write_key(f,"p2_mode",c->p2_mode);
#undef WR
    Close(f);
}

/* ================================================================
 * ROM LIST
 * ================================================================ */

#define MAX_ROMS 1024

typedef struct {
    char name[64];   /* display name, no extension */
    char path[192];  /* full AmigaOS path */
    int  system;     /* SYSTEM_GENESIS / GG / SMS */
} RomEntry;

static RomEntry      rom_list[MAX_ROMS];
static int           rom_count = 0;
static int           vis_count = 0;
static char          count_buf[64];
static struct Hook   disp_hook;

static int ci_char(int c)
{
    return (c>='a'&&c<='z') ? c-32 : c;
}

static int ci_contains(const char *hay, const char *needle)
{
    int nh, nn, i, j;
    if (!needle || !needle[0]) return 1;
    nh = strlen(hay); nn = strlen(needle);
    for (i = 0; i <= nh-nn; i++) {
        for (j = 0; j < nn; j++)
            if (ci_char((unsigned char)hay[i+j]) !=
                ci_char((unsigned char)needle[j])) break;
        if (j==nn) return 1;
    }
    return 0;
}

static int sort_order = 0; /* 0=by name, 1=by system */

static int rom_cmp(const void *a, const void *b)
{
    const RomEntry *ra = (const RomEntry *)a;
    const RomEntry *rb = (const RomEntry *)b;
    const char *na = ra->name, *nb = rb->name;
    while (*na && *nb) {
        int ca = ci_char((unsigned char)*na);
        int cb = ci_char((unsigned char)*nb);
        if (ca!=cb) return ca-cb;
        na++; nb++;
    }
    if (*na||*nb) return *na?1:-1;
    return ra->system - rb->system;
}

static int rom_cmp_sys(const void *a, const void *b)
{
    const RomEntry *ra = (const RomEntry *)a;
    const RomEntry *rb = (const RomEntry *)b;
    const char *na, *nb;
    if (ra->system != rb->system) return ra->system - rb->system;
    na = ra->name; nb = rb->name;
    while (*na && *nb) {
        int ca = ci_char((unsigned char)*na);
        int cb = ci_char((unsigned char)*nb);
        if (ca!=cb) return ca-cb;
        na++; nb++;
    }
    if (*na||*nb) return *na?1:-1;
    return 0;
}

static void apply_sort(void)
{
    qsort(rom_list, rom_count, sizeof(RomEntry),
          sort_order ? rom_cmp_sys : rom_cmp);
}

/* MUI List display hook.
 * Called via HookEntry relay (amiga.lib): h_Entry=HookEntry, h_SubEntry=this.
 * MUI calls: a0=hook, a1=char**array, a2=entry (NULL for title row). */
static ULONG disp_hook_func(struct Hook *h, char **arr, RomEntry *entry)
{
    (void)h;
    if (entry) {
        arr[0] = entry->name;
        arr[1] = (char *)system_names[entry->system];
    } else {
        arr[0] = (char *)"Game Name";
        arr[1] = (char *)"System";
    }
    return 0;
}

static int ext_match(const char *fn, const char **exts)
{
    const char *dot=NULL, *p=fn;
    int i;
    while (*p) { if (*p=='.') dot=p; p++; }
    if (!dot) return 0;
    for (i=0; exts[i]; i++) {
        const char *e=exts[i], *d=dot;
        while (*e&&*d&&ci_char((unsigned char)*e)==ci_char((unsigned char)*d))
            { e++; d++; }
        if (!*e&&!*d) return 1;
    }
    return 0;
}

static void strip_ext(const char *fn, char *out, int sz)
{
    const char *dot=NULL, *p=fn;
    int len;
    while (*p) { if (*p=='.') dot=p; p++; }
    len = dot ? (int)(dot-fn) : (int)strlen(fn);
    if (len>=sz) len=sz-1;
    strncpy(out, fn, len);
    out[len]=0;
}

static const char *gen_exts[] = { ".md",".bin",".gen",".smd",".rom",".zip",NULL };
static const char *gg_exts[]  = { ".gg",".zip",NULL };
static const char *sms_exts[] = { ".sms",".zip",NULL };

static void scan_dir(const char *dir, int sys)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    const char **exts;
    char full[192];

    switch (sys) {
        case SYSTEM_GG:  exts=gg_exts;  break;
        case SYSTEM_SMS: exts=sms_exts; break;
        default:         exts=gen_exts; break;
    }
    lock = Lock(dir, ACCESS_READ);
    if (!lock) return;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) { UnLock(lock); return; }

    if (Examine(lock, fib)) {
        while (rom_count < MAX_ROMS && ExNext(lock, fib)) {
            if (fib->fib_DirEntryType < 0 &&
                ext_match(fib->fib_FileName, exts))
            {
                strip_ext(fib->fib_FileName,
                          rom_list[rom_count].name,
                          sizeof(rom_list[0].name));
                snprintf(full, sizeof(full), "%s%s", dir, fib->fib_FileName);
                strncpy(rom_list[rom_count].path, full,
                        sizeof(rom_list[0].path)-1);
                rom_list[rom_count].path[sizeof(rom_list[0].path)-1]=0;
                rom_list[rom_count].system = sys;
                rom_count++;
            }
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
}

static void do_scan(void)
{
    rom_count = 0;
    scan_dir(ROM_PATH_GEN, SYSTEM_GENESIS);
    scan_dir(ROM_PATH_GG,  SYSTEM_GG);
    scan_dir(ROM_PATH_SMS, SYSTEM_SMS);
    apply_sort();
}

static void rebuild_list(Object *list, Object *tx, const char *filter)
{
    int i;
    RomEntry *rp;
    set(list, MUIA_List_Quiet, TRUE);
    DoMethod(list, MUIM_List_Clear);
    vis_count = 0;
    for (i = 0; i < rom_count; i++) {
        if (ci_contains(rom_list[i].name, filter)) {
            rp = &rom_list[i];
            DoMethod(list, MUIM_List_Insert, &rp, 1, MUIV_List_Insert_Bottom);
            vis_count++;
        }
    }
    set(list, MUIA_List_Quiet, FALSE);
    if (rom_count == 0)
        snprintf(count_buf, sizeof(count_buf), "No games found - press Scan");
    else
        snprintf(count_buf, sizeof(count_buf),
                 "%d of %d games", vis_count, rom_count);
    set(tx, MUIA_Text_Contents, (ULONG)count_buf);
}

/* ================================================================
 * CHEATS  -  PROGDIR:Cheats/<romname>.cht  (RetroArch .cht format)
 *
 * The emulator reads this file itself (load_cheats_cht() in amiga/main.c) and
 * applies only the entries whose cheatN_enable line says "true". This page
 * lists the entries of the selected game and rewrites just those lines:
 * descriptions, codes, comments, the cheats= count and any key this launcher
 * does not understand are copied through untouched.
 *
 * The emulator's parser is order sensitive - a cheatN_enable line is only
 * honoured when it comes after the cheatN_code line of the same N - so the
 * enable line of every entry is re-emitted at the end of that entry's own
 * block rather than left wherever it happened to sit.
 * ================================================================ */

#define MAX_CHEATS      128
#define CHEAT_FILE_MAX  65536L   /* refuse to load anything bigger rather than
                                  * risk writing a truncated file back out */

typedef struct {
    int  index;        /* the N in cheatN_desc, not our position in the list */
    int  enabled;
    char desc[128];
    char code[160];
} CheatEntry;

static CheatEntry  cheat_list[MAX_CHEATS];
static int         cheat_count     = 0;
static int         cheat_have_file = 0;  /* a .cht was found and read */
static int         cheat_dirty     = 0;  /* a toggle is not on disk yet */
static char        cheat_path[256];      /* "" = no game selected */
static char        cheat_status[320] = "Select a game on the Games page.";
static struct Hook cheat_disp_hook;

/* MUI List display hook, same HookEntry relay as disp_hook_func().
 * Three columns: enabled mark, description, code. */
static ULONG cheat_disp_func(struct Hook *h, char **arr, CheatEntry *entry)
{
    (void)h;
    if (entry) {
        /* A plain character, not the "\033I[6:15]" MUII_CheckMark image: MUI's
         * configurable checkmark only draws its tick in the IDS_SELECTED
         * state and the ESC I[] text escape cannot ask for a state, so the
         * image would come out blank for enabled and disabled alike. */
        arr[0] = (char *)(entry->enabled ? "\033cX" : "\033c-");
        arr[1] = entry->desc[0] ? entry->desc : (char *)"(no description)";
        arr[2] = entry->code;
    } else {
        arr[0] = (char *)"\033cOn";
        arr[1] = (char *)"Cheat";
        arr[2] = (char *)"Code";
    }
    return 0;
}

/* Build PROGDIR:Cheats/<basename without extension>.cht from a ROM path.
 * Mirrors load_cheats_cht() in amiga/main.c exactly: the basename starts after
 * the last '/' or ':', and only the last '.' within it is the extension. Both
 * programs run from the same directory, so the literal PROGDIR: matches. */
static void cheat_path_for_rom(const char *rompath, char *out, int outsz)
{
    const char *p, *base, *dot;
    int len;

    base = rompath;
    for (p = rompath; *p; p++)
        if (*p == '/' || *p == ':') base = p + 1;
    dot = NULL;
    for (p = base; *p; p++)
        if (*p == '.') dot = p;
    len = dot ? (int)(dot - base) : (int)strlen(base);
    if (len > 200) len = 200;   /* CHEAT_DIR + 200 + ".cht" + NUL < outsz */
    snprintf(out, outsz, CHEAT_DIR "%.*s.cht", len, base);
}

/* Read a whole file into one AllocVec'd, NUL-terminated buffer.
 * The size comes from Lock()+Examine() and not from a seek-to-end: the V40
 * autodoc for Seek() calls its result both "oldPosition" and "the current
 * absolute position", so that idiom is not something to rely on here.
 * Returns NULL when the file is missing, empty, oversized or unreadable. */
static char *cheat_file_read(const char *path, LONG *out_len)
{
    BPTR lock, f;
    struct FileInfoBlock *fib;
    LONG size = -1, got;
    char *buf;

    *out_len = 0;
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock) return NULL;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (Examine(lock, fib) && fib->fib_DirEntryType < 0)
            size = fib->fib_Size;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    if (size <= 0 || size > CHEAT_FILE_MAX) return NULL;

    buf = (char *)AllocVec((ULONG)size + 1, MEMF_ANY);
    if (!buf) return NULL;
    f = Open((STRPTR)path, MODE_OLDFILE);
    if (!f) { FreeVec(buf); return NULL; }
    got = Read(f, buf, size);
    Close(f);
    if (got <= 0) { FreeVec(buf); return NULL; }
    buf[got] = 0;
    *out_len = got;
    return buf;
}

/* Split the next line out of an in-memory file image, in place.
 * Handles LF, CR and CRLF. Returns NULL once the image is exhausted. */
static char *cheat_next_line(char **pos)
{
    char *s = *pos, *p;
    int cr;

    if (!s || !*s) { *pos = NULL; return NULL; }
    p = s;
    while (*p && *p != '\n' && *p != '\r') p++;
    if (*p) {
        cr = (*p == '\r');
        *p++ = 0;
        if (cr && *p == '\n') p++;
    }
    *pos = p;
    return s;
}

/* Split "key = value" the way the emulator's cheat parser does: trim blanks
 * and tabs around both halves, then strip one surrounding pair of double
 * quotes from the value. Returns 0 when the line holds no '='. */
static int cheat_split_kv(const char *line, char *key, int keysz,
                          char *value, int valsz)
{
    const char *eq, *s;
    int n;

    eq = strchr(line, '=');
    if (!eq) return 0;

    s = line;
    while (*s == ' ' || *s == '\t') s++;
    n = (int)(eq - s);
    if (n < 0) n = 0;
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) n--;
    if (n >= keysz) n = keysz - 1;
    memcpy(key, s, (size_t)n);
    key[n] = 0;

    s = eq + 1;
    while (*s == ' ' || *s == '\t') s++;
    n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) n--;
    if (n >= 2 && s[0] == '"' && s[n-1] == '"') { s++; n -= 2; }
    if (n >= valsz) n = valsz - 1;
    memcpy(value, s, (size_t)n);
    value[n] = 0;
    return 1;
}

/* "cheat12_desc" -> idx 12, field "desc".
 * Requires a digit straight after "cheat", which rejects the file's own
 * "cheats = N" count line. Returns 0 for anything else. */
static int cheat_parse_key(const char *key, int *idx, const char **field)
{
    const char *p = key;

    if (strncmp(p, "cheat", 5) != 0) return 0;
    p += 5;
    if (*p < '0' || *p > '9') return 0;
    *idx = atoi(p);
    while (*p >= '0' && *p <= '9') p++;
    if (*p != '_') return 0;
    *field = p + 1;
    return 1;
}

static void cheats_status_update(void)
{
    int i, on = 0;

    if (!cheat_path[0]) {
        strcpy(cheat_status, "Select a game on the Games page.");
        return;
    }
    if (!cheat_have_file) {
        snprintf(cheat_status, sizeof(cheat_status),
                 "No cheat file - create %s", cheat_path);
        return;
    }
    if (!cheat_count) {
        snprintf(cheat_status, sizeof(cheat_status),
                 "No cheat entries in %s", cheat_path);
        return;
    }
    for (i = 0; i < cheat_count; i++)
        if (cheat_list[i].enabled) on++;
    snprintf(cheat_status, sizeof(cheat_status), "%d cheat%s, %d on%s  -  %s",
             cheat_count, (cheat_count == 1) ? "" : "s", on,
             cheat_dirty ? "  (UNSAVED)" : "", cheat_path);
}

/* Parse the .cht of one ROM into cheat_list[]. List order follows the file;
 * each entry keeps its own cheatN index so the rewrite can find its lines.
 * Pass NULL to mean "no game selected". */
static void cheats_load(const char *rompath)
{
    char *img, *pos, *line;
    char key[64], value[256];
    const char *field;
    LONG len;
    int idx, i, w;

    cheat_count     = 0;
    cheat_have_file = 0;
    cheat_dirty     = 0;
    cheat_path[0]   = 0;
    idx             = 0;

    if (!rompath || !rompath[0]) { cheats_status_update(); return; }
    cheat_path_for_rom(rompath, cheat_path, sizeof(cheat_path));

    img = cheat_file_read(cheat_path, &len);
    if (!img) { cheats_status_update(); return; }
    cheat_have_file = 1;

    pos = img;
    while ((line = cheat_next_line(&pos)) != NULL) {
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        if (!cheat_split_kv(line, key, sizeof(key), value, sizeof(value)))
            continue;
        if (!cheat_parse_key(key, &idx, &field)) continue;

        for (i = 0; i < cheat_count; i++)
            if (cheat_list[i].index == idx) break;
        if (i == cheat_count) {
            if (cheat_count >= MAX_CHEATS) continue;
            cheat_list[i].index   = idx;
            cheat_list[i].enabled = 0;
            cheat_list[i].desc[0] = 0;
            cheat_list[i].code[0] = 0;
            cheat_count++;
        }

        if (!strcmp(field, "desc")) {
            strncpy(cheat_list[i].desc, value, sizeof(cheat_list[i].desc) - 1);
            cheat_list[i].desc[sizeof(cheat_list[i].desc) - 1] = 0;
        }
        else if (!strcmp(field, "code")) {
            strncpy(cheat_list[i].code, value, sizeof(cheat_list[i].code) - 1);
            cheat_list[i].code[sizeof(cheat_list[i].code) - 1] = 0;
        }
        else if (!strcmp(field, "enable")) {
            cheat_list[i].enabled = !strcmp(value, "true") ? 1 : 0;
        }
    }
    FreeVec(img);

    /* Drop entries with no code line: there is nothing for the emulator to
     * apply, so offering a checkmark for them would be a lie. Their lines
     * still survive in the file - cheats_save() copies what it does not own. */
    for (i = 0, w = 0; i < cheat_count; i++) {
        if (!cheat_list[i].code[0]) continue;
        if (w != i) cheat_list[w] = cheat_list[i];
        w++;
    }
    cheat_count = w;

    cheats_status_update();
}

/* Rewrite the cheatN_enable lines of the loaded .cht.
 *
 * Every line this launcher does not own is copied through verbatim. The enable
 * line of each managed entry is dropped from wherever it was and re-emitted at
 * the end of that entry's block, because load_cheats_cht() in amiga/main.c
 * only honours an enable line that follows the cheatN_code line of the same N.
 *
 * The whole file is assembled in memory and written with a single Write(), so
 * there is no window in which a half-rewritten cheat file is on disk.
 * Returns 1 on success, 0 on failure (nothing is written on failure except in
 * the case of a short Write(), which the caller reports). */
static int cheats_save(void)
{
    char *img, *pos, *line, *out;
    char key[64], value[256], eline[48];
    const char *field;
    LONG len, outcap, outlen = 0;
    int cur_idx = -1, cur_slot = -1, need = 0, ovf = 0;
    int idx = -1, i, is_cheat_line, ok;
    BPTR f;

    if (!cheat_count || !cheat_path[0] || !cheat_have_file) return 0;

    img = cheat_file_read(cheat_path, &len);
    if (!img) return 0;

    /* Worst case we add one "cheatNNNNNNNNNN_enable = false\n" per entry
     * (31 chars) on top of the original, plus a final newline. */
    outcap = len + (LONG)cheat_count * 32 + 64;
    out = (char *)AllocVec((ULONG)outcap, MEMF_ANY);
    if (!out) { FreeVec(img); return 0; }

#define EMIT(s) do {                                                      \
        LONG _l = (LONG)strlen(s);                                        \
        if (outlen + _l <= outcap) {                                      \
            memcpy(out + outlen, (s), (size_t)_l); outlen += _l;          \
        } else ovf = 1;                                                   \
    } while (0)

#define FLUSH_ENABLE() do {                                               \
        if (need && cur_slot >= 0) {                                      \
            snprintf(eline, sizeof(eline), "cheat%d_enable = %s\n",        \
                     cheat_list[cur_slot].index,                          \
                     cheat_list[cur_slot].enabled ? "true" : "false");    \
            EMIT(eline);                                                  \
        }                                                                 \
        need = 0; cur_idx = -1; cur_slot = -1;                            \
    } while (0)

    pos = img;
    while ((line = cheat_next_line(&pos)) != NULL) {
        is_cheat_line = 0;
        if (line[0] && line[0] != ';' && line[0] != '#' &&
            cheat_split_kv(line, key, sizeof(key), value, sizeof(value)) &&
            cheat_parse_key(key, &idx, &field))
            is_cheat_line = 1;

        /* Anything that is not another line of the current block closes it. */
        if (!is_cheat_line || idx != cur_idx)
            FLUSH_ENABLE();

        if (is_cheat_line) {
            if (idx != cur_idx) {
                cur_idx  = idx;
                cur_slot = -1;
                for (i = 0; i < cheat_count; i++)
                    if (cheat_list[i].index == idx) { cur_slot = i; break; }
                need = (cur_slot >= 0);
            }
            /* Our own enable line is re-emitted at the end of the block.
             * An enable line belonging to an index we do not manage is left
             * exactly where it is. */
            if (cur_slot >= 0 && !strcmp(field, "enable"))
                continue;
        }

        EMIT(line);
        EMIT("\n");
    }
    FLUSH_ENABLE();

#undef FLUSH_ENABLE
#undef EMIT

    FreeVec(img);
    if (ovf) { FreeVec(out); return 0; }

    f = Open((STRPTR)cheat_path, MODE_NEWFILE);
    if (!f) { FreeVec(out); return 0; }
    ok = (Write(f, out, outlen) == outlen);
    Close(f);
    FreeVec(out);
    if (ok) {
        cheat_dirty = 0;
        cheats_status_update();
    }
    return ok;
}

/* ================================================================
 * GUI
 * ================================================================ */

struct AppData {
    Object *app, *window;
    /* Games tab */
    Object *list_roms, *lv_roms, *str_filter, *tx_count;
    Object *bt_scan, *bt_start, *cy_sort;
    /* Video */
    Object *cy_scanlines, *cm_vsync, *cy_frameskip, *cm_autoskip, *cm_fps;
    Object *cy_depth;
    /* Audio */
    Object *cy_audio, *str_paulafreq, *str_ahifreq;
    Object *cy_paulaboost, *cm_paulastereo, *cm_paulafilter, *cy_ahiboost;
    Object *sl_fmvol, *sl_psgvol;
    Object *cm_hqfm, *cm_hqpsg, *sl_paulamargin, *sl_ahimargin;
    /* Cheats */
    Object *lv_cheats, *list_cheats, *tx_cheats, *cm_cheats;
    Object *bt_cheat_toggle, *bt_cheat_all, *bt_cheat_none;
    Object *bt_cheat_save, *bt_cheat_reload;
    /* Players */
    Object *str_p1[8], *str_p2[8];
    /* Buttons */
    Object *bt_save, *bt_cancel, *bt_about;
};

/* One entry per RegisterGroup child, in the same order as the children. */
static const char *page_titles[] =
    { "Games","Video","Audio","Cheats","Player 1","Player 2",NULL };

static Object *create_gui(struct AppData *d, const Config *c)
{
    Object *win;
    char pf[16], af[16], k[8][16], k2[8][16];
    int i, p1r[8], p2r[8];

    snprintf(pf, sizeof(pf), "%d", c->paula_freq);
    snprintf(af, sizeof(af), "%d", c->ahi_freq);
    p1r[0]=c->p1_a; p1r[1]=c->p1_b; p1r[2]=c->p1_c; p1r[3]=c->p1_x;
    p1r[4]=c->p1_y; p1r[5]=c->p1_z; p1r[6]=c->p1_start; p1r[7]=c->p1_mode;
    p2r[0]=c->p2_a; p2r[1]=c->p2_b; p2r[2]=c->p2_c; p2r[3]=c->p2_x;
    p2r[4]=c->p2_y; p2r[5]=c->p2_z; p2r[6]=c->p2_start; p2r[7]=c->p2_mode;
    for (i=0; i<8; i++) {
        rawkey_to_name(p1r[i], k[i],  sizeof(k[i]));
        rawkey_to_name(p2r[i], k2[i], sizeof(k2[i]));
    }

    d->app = ApplicationObject,
        MUIA_Application_Title,       STR("GenesisPlusGX Launcher"),
        MUIA_Application_Version,     STR(version),
        MUIA_Application_Copyright,   STR("Pedro Cotter"),
        MUIA_Application_Author,      STR("Pedro Cotter"),
        MUIA_Application_Description, STR("Genesis Plus GX Launcher"),
        MUIA_Application_Base,        STR("GPGXLAUNCHER"),

        SubWindow, win = d->window = WindowObject,
            MUIA_Window_Title, STR("Genesis Plus GX  -  Launcher"),
            MUIA_Window_ID,    MAKE_ID('G','P','G','L'),

            WindowContents, VGroup,

                Child, RegisterGroup(page_titles),

                    /* ======== GAMES ======== */
                    Child, VGroup,

                        /* Sort row */
                        Child, HGroup,
                            Child, d->cy_sort = CycleObject,
                                MUIA_Cycle_Entries, sort_entries,
                                MUIA_Cycle_Active,  sort_order,
                            End,
                            Child, HSpace(0),
                        End,

                        Child, d->lv_roms = ListviewObject,
                            MUIA_Listview_List, d->list_roms = ListObject,
                                InputListFrame,
                                MUIA_List_DisplayHook, &disp_hook,
                                MUIA_List_Format,      STR("MAXWIDTH=100 BAR,MAXWIDTH=15"),
                                MUIA_List_Title,       TRUE,
                            End,
                        End,

                        Child, HGroup,
                            Child, Label1(STR("Filter:")),
                            Child, d->str_filter = StringObject,
                                StringFrame,
                                MUIA_String_MaxLen, 64,
                            End,
                            Child, d->tx_count = TextObject,
                                MUIA_Text_Contents, STR("Press Scan"),
                                MUIA_Text_SetMin,   FALSE,
                                MUIA_Weight, 50,
                            End,
                        End,

                        Child, HGroup,
                            Child, d->bt_scan  = SimpleButton(STR("_Scan Roms")),
                            Child, HSpace(0),
                            Child, d->bt_start = SimpleButton(STR("_Start Game")),
                        End,

                    End, /* Games */

                    /* ======== VIDEO ======== */
                    Child, VGroup,

                        Child, ColGroup(2), GroupFrame,
                            MUIA_FrameTitle, STR("Display"),
                            Child, Label1(STR("Scanlines:")),
                            Child, d->cy_scanlines = CycleObject,
                                MUIA_Cycle_Entries, scanline_entries,
                                MUIA_Cycle_Active,  c->scanlines,
                            End,
                            Child, Label1(STR("Screen Depth:")),
                            Child, d->cy_depth = CycleObject,
                                MUIA_Cycle_Entries, depth_entries,
                                MUIA_Cycle_Active,  (c->screendepth == 16) ? 1 : 0,
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            Child, d->cm_vsync = CheckMark(c->vsync),
                            Child, Label1(STR("VSync  (tear-free, disables Auto Skip)")),
                            Child, HSpace(0),
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("Frame Skip"),
                            Child, Label1(STR("Fixed Skip:")),
                            Child, d->cy_frameskip = CycleObject,
                                MUIA_Cycle_Entries, frameskip_entries,
                                MUIA_Cycle_Active,  c->frameskip,
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            Child, d->cm_autoskip = CheckMark(c->autoskip),
                            Child, Label1(STR("Auto Skip  (dynamic, best for Pi 3)")),
                            Child, HSpace(0),
                        End,

                        Child, HGroup, GroupFrame,
                            Child, d->cm_fps = CheckMark(c->fps),
                            Child, Label1(STR("Show FPS counter")),
                            Child, HSpace(0),
                        End,

                        Child, VSpace(0),
                    End, /* Video */

                    /* ======== AUDIO ======== */
                    Child, VGroup,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("Audio Mode"),
                            Child, Label1(STR("Mode:")),
                            Child, d->cy_audio = CycleObject,
                                MUIA_Cycle_Entries, audio_entries,
                                MUIA_Cycle_Active,  c->audio_mode,
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
                            End,
                            Child, Label1(STR("AHI Freq (Hz):")),
                            Child, d->str_ahifreq = StringObject,
                                StringFrame,
                                MUIA_String_Contents, STR(af),
                                MUIA_String_MaxLen,   8,
                                MUIA_String_Accept,   STR("0123456789"),
                                MUIA_FixWidthTxt,     STR("88888888"),
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("Volume Boost"),
                            Child, Label1(STR("Paula Boost:")),
                            Child, d->cy_paulaboost = CycleObject,
                                MUIA_Cycle_Entries, boost_entries,
                                MUIA_Cycle_Active,  c->paulaboost,
                            End,
                            Child, Label1(STR("AHI Boost:")),
                            Child, d->cy_ahiboost = CycleObject,
                                MUIA_Cycle_Entries, boost_entries,
                                MUIA_Cycle_Active,  c->ahiboost,
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("FM Volume  (10-100)"),
                            Child, d->sl_fmvol = SliderObject,
                                MUIA_Numeric_Min,   10,
                                MUIA_Numeric_Max,   100,
                                MUIA_Numeric_Value, c->fmvol,
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("PSG Volume  (10-200)"),
                            Child, d->sl_psgvol = SliderObject,
                                MUIA_Numeric_Min,   10,
                                MUIA_Numeric_Max,   200,
                                MUIA_Numeric_Value, c->psgvol,
                            End,
                        End,

                        Child, HGroup, GroupFrame,
                            Child, d->cm_paulastereo = CheckMark(c->paulastereo),
                            Child, Label1(STR("Paula Stereo  (8-bit mode)")),
                            Child, HSpace(0),
                        End,

                        Child, HGroup, GroupFrame,
                            Child, d->cm_paulafilter = CheckMark(c->paulafilter),
                            Child, Label1(STR("Paula Filter")),
                            Child, HSpace(0),
                        End,

                        Child, HGroup, GroupFrame,
                            MUIA_FrameTitle, STR("Synthesis Quality  (uncheck for speed on Pi 3 / 68030)"),
                            Child, d->cm_hqfm = CheckMark(c->hqfm),
                            Child, Label1(STR("HQ FM")),
                            Child, d->cm_hqpsg = CheckMark(c->hqpsg),
                            Child, Label1(STR("HQ PSG")),
                            Child, HSpace(0),
                        End,

                        Child, ColGroup(2), GroupFrame,
                            MUIA_FrameTitle, STR("Buffer Margin  (0=WinUAE; PiStorm: Paula 200, AHI 220)"),
                            Child, Label1(STR("Paula:")),
                            Child, d->sl_paulamargin = SliderObject,
                                MUIA_Numeric_Min,   0,
                                MUIA_Numeric_Max,   882,
                                MUIA_Numeric_Value, c->paulamargin,
                            End,
                            Child, Label1(STR("AHI:")),
                            Child, d->sl_ahimargin = SliderObject,
                                MUIA_Numeric_Min,   0,
                                MUIA_Numeric_Max,   882,
                                MUIA_Numeric_Value, c->ahimargin,
                            End,
                        End,

                        Child, VSpace(0),
                    End, /* Audio */

                    /* ======== CHEATS ======== */
                    Child, VGroup,

                        Child, HGroup, GroupFrame,
                            Child, d->cm_cheats = CheckMark(c->cheats),
                            Child, Label1(STR("Apply cheat files  (Mega Drive only)")),
                            Child, HSpace(0),
                        End,

                        Child, d->lv_cheats = ListviewObject,
                            MUIA_Listview_List, d->list_cheats = ListObject,
                                InputListFrame,
                                MUIA_List_DisplayHook, &cheat_disp_hook,
                                /* Column 0 auto-sizes to the "On" title. */
                                MUIA_List_Format, STR("BAR,MAXWIDTH=55 BAR,MAXWIDTH=40"),
                                MUIA_List_Title,  TRUE,
                            End,
                        End,

                        Child, d->tx_cheats = TextObject,
                            MUIA_Text_Contents, STR(cheat_status),
                            MUIA_Text_SetMin,   FALSE,
                            TextFrame,
                        End,

                        Child, HGroup,
                            Child, d->bt_cheat_toggle = SimpleButton(STR("_Toggle")),
                            Child, d->bt_cheat_all    = SimpleButton(STR("All O_n")),
                            Child, d->bt_cheat_none   = SimpleButton(STR("All O_ff")),
                            Child, d->bt_cheat_reload = SimpleButton(STR("R_eload")),
                            Child, d->bt_cheat_save   = SimpleButton(STR("Sa_ve Cheats")),
                        End,

                        Child, TextObject,
                            MUIA_Text_Contents, STR(
                                "\033cCheats follow the game picked on the Games page,"
                                " from PROGDIR:Cheats/<game>.cht\n"
                                "Double-click or Toggle switches one cheat."
                                "  Reload writes pending changes, then re-reads the file.\n"
                                "\033nOnly the cheatN_enable lines are rewritten"
                                " - codes and descriptions are left alone."),
                            TextFrame,
                        End,

                    End, /* Cheats */

                    /* ======== PLAYER 1 ======== */
                    Child, VGroup,
                        Child, TextObject,
                            MUIA_Text_Contents, STR(
                                "\033cKey names: a-z  0-9  space  enter  lshift  rshift"
                                "  lalt  ralt  lctrl\n"
                                "help  del  tab  f1-f10  np0-np9  npdot  npenter"
                                "  npminus  npplus\n"
                                "comma  period  slash  semicolon  lbracket  rbracket\n"
                                "\033nLeave empty for built-in default."),
                            TextFrame,
                        End,
                        Child, ColGroup(4), GroupFrame,
                            MUIA_FrameTitle, STR("Player 1  (defaults: A=1 B=2 C=3 X=q Y=w Z=e Start=space Mode=tab)"),
                            Child, Label1(STR("A:")),
                            Child, d->str_p1[0]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k[0]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("B:")),
                            Child, d->str_p1[1]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k[1]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("C:")),
                            Child, d->str_p1[2]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k[2]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("X:")),
                            Child, d->str_p1[3]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k[3]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("Y:")),
                            Child, d->str_p1[4]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k[4]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("Z:")),
                            Child, d->str_p1[5]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k[5]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("Start:")),
                            Child, d->str_p1[6]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k[6]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("Mode:")),
                            Child, d->str_p1[7]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k[7]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                        End,
                        Child, VSpace(0),
                    End, /* P1 */

                    /* ======== PLAYER 2 ======== */
                    Child, VGroup,
                        Child, TextObject,
                            MUIA_Text_Contents, STR(
                                "\033cSame key names as Player 1."
                                "  Leave empty for built-in default."),
                            TextFrame,
                        End,
                        Child, ColGroup(4), GroupFrame,
                            MUIA_FrameTitle, STR("Player 2  (defaults: A=np0 B=npdot C=npenter Start=help)"),
                            Child, Label1(STR("A:")),
                            Child, d->str_p2[0]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k2[0]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("B:")),
                            Child, d->str_p2[1]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k2[1]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("C:")),
                            Child, d->str_p2[2]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k2[2]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("X:")),
                            Child, d->str_p2[3]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k2[3]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("Y:")),
                            Child, d->str_p2[4]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k2[4]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("Z:")),
                            Child, d->str_p2[5]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k2[5]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("Start:")),
                            Child, d->str_p2[6]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k2[6]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                            Child, Label1(STR("Mode:")),
                            Child, d->str_p2[7]=StringObject, StringFrame,
                                MUIA_String_Contents,STR(k2[7]),MUIA_String_MaxLen,16,
                                MUIA_FixWidthTxt,STR("lshift___"), End,
                        End,
                        Child, VSpace(0),
                    End, /* P2 */

                End, /* RegisterGroup */

                /* ---- Bottom buttons ---- */
                Child, HGroup,
                    Child, d->bt_save   = SimpleButton(STR("_Save Config")),
                    Child, d->bt_cancel = SimpleButton(STR("_Cancel")),
                    Child, d->bt_about  = SimpleButton(STR("_About")),
                End,

            End, /* WindowContents VGroup */
        End, /* Window */
    End; /* Application */

    if (!d->app) return NULL;

    /* Window close -> cancel */
    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             d->app, 2, MUIM_Application_ReturnID, ID_CANCEL);

    /* Save/Cancel */
    DoMethod(d->bt_save,   MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_SAVE);
    DoMethod(d->bt_cancel, MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_CANCEL);
    DoMethod(d->bt_about,  MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_ABOUT);

    /* Scan */
    DoMethod(d->bt_scan, MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_SCAN);

    /* Start (button) */
    DoMethod(d->bt_start, MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_LAUNCH);

    /* Start (double-click) */
    DoMethod(d->lv_roms, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE,
             d->app, 2, MUIM_Application_ReturnID, ID_LAUNCH);

    /* Filter on Enter */
    DoMethod(d->str_filter, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             d->app, 2, MUIM_Application_ReturnID, ID_FILTER);

    /* Sort order change */
    DoMethod(d->cy_sort, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             d->app, 2, MUIM_Application_ReturnID, ID_SORT);

    /* There is one .cht per game, so the ROM selection drives the Cheats
     * page. Notifying on the list rather than on the register group's
     * MUIA_Group_ActivePage keeps this on documented ground. */
    DoMethod(d->list_roms, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             d->app, 2, MUIM_Application_ReturnID, ID_ROMSEL);

    /* Cheat page controls */
    DoMethod(d->bt_cheat_toggle, MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_CHEAT_TOGGLE);
    DoMethod(d->lv_cheats, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE,
             d->app, 2, MUIM_Application_ReturnID, ID_CHEAT_TOGGLE);
    DoMethod(d->bt_cheat_all, MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_CHEAT_ALL);
    DoMethod(d->bt_cheat_none, MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_CHEAT_NONE);
    DoMethod(d->bt_cheat_reload, MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_CHEAT_RELOAD);
    DoMethod(d->bt_cheat_save, MUIM_Notify, MUIA_Pressed, FALSE,
             d->app, 2, MUIM_Application_ReturnID, ID_CHEAT_SAVE);

    /* List gets keyboard input by default (arrow keys, Return = launch) */
    set(win, MUIA_Window_DefaultObject, (ULONG)d->lv_roms);

    return d->app;
}

/* ---- Collect widgets -> Config ---- */
static void collect(struct AppData *d, Config *c)
{
    ULONG val;
    STRPTR s;
    int i, *p1d[8], *p2d[8];

    p1d[0]=&c->p1_a; p1d[1]=&c->p1_b; p1d[2]=&c->p1_c; p1d[3]=&c->p1_x;
    p1d[4]=&c->p1_y; p1d[5]=&c->p1_z; p1d[6]=&c->p1_start; p1d[7]=&c->p1_mode;
    p2d[0]=&c->p2_a; p2d[1]=&c->p2_b; p2d[2]=&c->p2_c; p2d[3]=&c->p2_x;
    p2d[4]=&c->p2_y; p2d[5]=&c->p2_z; p2d[6]=&c->p2_start; p2d[7]=&c->p2_mode;

    get(d->cy_audio,       MUIA_Cycle_Active,  &val); c->audio_mode  =(int)val;
    get(d->cy_scanlines,   MUIA_Cycle_Active,  &val); c->scanlines   =(int)val;
    get(d->cm_vsync,       MUIA_Selected,      &val); c->vsync       =(int)val;
    get(d->cy_frameskip,   MUIA_Cycle_Active,  &val); c->frameskip   =(int)val;
    get(d->cm_autoskip,    MUIA_Selected,      &val); c->autoskip    =(int)val;
    get(d->cm_fps,         MUIA_Selected,      &val); c->fps         =(int)val;
    get(d->cy_paulaboost,  MUIA_Cycle_Active,  &val); c->paulaboost  =(int)val;
    get(d->cy_ahiboost,    MUIA_Cycle_Active,  &val); c->ahiboost    =(int)val;
    get(d->cm_paulastereo, MUIA_Selected,      &val); c->paulastereo =(int)val;
    get(d->cm_paulafilter, MUIA_Selected,      &val); c->paulafilter =(int)val;
    get(d->sl_fmvol,       MUIA_Numeric_Value, &val); c->fmvol       =(int)val;
    get(d->sl_psgvol,      MUIA_Numeric_Value, &val); c->psgvol      =(int)val;
    get(d->cy_depth,       MUIA_Cycle_Active,  &val); c->screendepth = val ? 16 : 32;
    get(d->cm_hqfm,        MUIA_Selected,      &val); c->hqfm        =(int)val;
    get(d->cm_hqpsg,       MUIA_Selected,      &val); c->hqpsg       =(int)val;
    get(d->sl_paulamargin, MUIA_Numeric_Value, &val); c->paulamargin =(int)val;
    get(d->sl_ahimargin,   MUIA_Numeric_Value, &val); c->ahimargin   =(int)val;
    get(d->cm_cheats,      MUIA_Selected,      &val); c->cheats      =(int)val;
    get(d->str_paulafreq,   MUIA_String_Contents, &s);
    c->paula_freq   = s ? atoi((char*)s) : 0;
    get(d->str_ahifreq,     MUIA_String_Contents, &s);
    c->ahi_freq     = s ? atoi((char*)s) : 44100;
    for (i=0; i<8; i++) {
        get(d->str_p1[i], MUIA_String_Contents, &s);
        *p1d[i] = s ? name_to_rawkey((char*)s) : -1;
        get(d->str_p2[i], MUIA_String_Contents, &s);
        *p2d[i] = s ? name_to_rawkey((char*)s) : -1;
    }
}

/* ================================================================
 * CHEATS  -  GUI side
 * ================================================================ */

static RomEntry *selected_rom(struct AppData *d)
{
    ULONG active;
    RomEntry *entry = NULL;

    get(d->list_roms, MUIA_List_Active, &active);
    if ((LONG)active < 0) return NULL;
    DoMethod(d->list_roms, MUIM_List_GetEntry, (ULONG)active, &entry);
    return entry;
}

static void cheats_show_status(struct AppData *d)
{
    cheats_status_update();
    set(d->tx_cheats, MUIA_Text_Contents, (ULONG)cheat_status);
}

static void cheats_rebuild_list(struct AppData *d)
{
    CheatEntry *cp;
    int i;

    set(d->list_cheats, MUIA_List_Quiet, TRUE);
    DoMethod(d->list_cheats, MUIM_List_Clear);
    for (i = 0; i < cheat_count; i++) {
        cp = &cheat_list[i];
        DoMethod(d->list_cheats, MUIM_List_Insert, &cp, 1,
                 MUIV_List_Insert_Bottom);
    }
    set(d->list_cheats, MUIA_List_Quiet, FALSE);
    cheats_show_status(d);
}

/* Write pending toggles out, reporting a failure the user needs to know
 * about (MODE_NEWFILE has already truncated the file by then). */
static void cheats_flush(struct AppData *d)
{
    if (!cheat_dirty) return;
    if (!cheats_save())
        MUI_Request(d->app, d->window, 0, STR("Cheats"), STR("OK"),
                    STR("Could not write the cheat file:\n%s"), cheat_path);
}

/* Point the Cheats page at the game selected on the Games page. Pending
 * toggles for the previous game are written out first, so changing the
 * selection never discards them. 'force' re-reads the same file again. */
static void cheats_sync(struct AppData *d, int force)
{
    RomEntry *entry = selected_rom(d);
    char path[sizeof(cheat_path)];

    path[0] = 0;
    if (entry) cheat_path_for_rom(entry->path, path, sizeof(path));
    if (!force && !strcmp(path, cheat_path)) return;

    cheats_flush(d);
    cheats_load(entry ? entry->path : NULL);
    cheats_rebuild_list(d);
}

static void cheats_set_all(struct AppData *d, int on)
{
    int i;

    if (!cheat_count) return;
    for (i = 0; i < cheat_count; i++) {
        if (cheat_list[i].enabled != on) {
            cheat_list[i].enabled = on;
            cheat_dirty = 1;
        }
    }
    DoMethod(d->list_cheats, MUIM_List_Redraw, MUIV_List_Redraw_All);
    cheats_show_status(d);
}

static void cheats_toggle_active(struct AppData *d)
{
    ULONG active;
    CheatEntry *e = NULL;

    get(d->list_cheats, MUIA_List_Active, &active);
    if ((LONG)active < 0) return;
    DoMethod(d->list_cheats, MUIM_List_GetEntry, (ULONG)active, &e);
    if (!e) return;
    e->enabled = !e->enabled;
    cheat_dirty = 1;
    DoMethod(d->list_cheats, MUIM_List_Redraw, MUIV_List_Redraw_Active);
    cheats_show_status(d);
}

/* ================================================================
 * LAUNCH HELPER
 * ================================================================ */

static void launch_game(struct AppData *d, Config *c,
                        const char *progdir, char *cmd, int cmdsz)
{
    RomEntry *entry = selected_rom(d);

    if (!entry) {
        MUI_Request(d->app, d->window, 0,
                    STR("Launch"), STR("OK"),
                    STR("Please select a game first."));
        return;
    }

    collect(d, c);
    config_save(NULL, NULL, c);
    /* The emulator reads the .cht itself, so pending toggles have to be on
     * disk before it starts or they will not apply to this run. */
    cheats_flush(d);
    set(d->window, MUIA_Window_Open, FALSE);
    snprintf(cmd, cmdsz, "\"%sGenesisPlusGX\" \"%s\"", progdir, entry->path);
    {
        BPTR nil_in = Open("NIL:", MODE_OLDFILE);
        Execute(cmd, nil_in, 0);
        if (nil_in) Close(nil_in);
    }
    set(d->window, MUIA_Window_Open, TRUE);
    /* Restore list focus after returning from game */
    set(d->window, MUIA_Window_ActiveObject, (ULONG)d->lv_roms);
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(void)
{
    struct AppData d;
    Config c;
    ULONG signals, id;
    BOOL running;
    char progdir[200];
    char cmd[1024];

    /* ---- Timer for joystick polling ---- */
    struct MsgPort    *timer_mp  = NULL;
    struct timerequest *timer_io = NULL;
    ULONG  timer_sig = 0;

    /* ---- Joystick edge detection / auto-repeat ---- */
    ULONG joy_prev   = 0;
    int   joy_repeat = 0;   /* 100ms ticks held */

    memset(&d, 0, sizeof(d));

    /* Resolve our directory to an absolute path now.
     * Execute() spawns a new shell whose PROGDIR: is the shell's own dir,
     * not ours - so we must use an absolute path when launching the emulator. */
    {
        BPTR plock = GetProgramDir();
        int len;
        progdir[0] = 0;
        if (plock) NameFromLock(plock, progdir, sizeof(progdir));
        len = (int)strlen(progdir);
        if (len > 0 && progdir[len-1] != '/' && progdir[len-1] != ':')
            strncat(progdir, "/", sizeof(progdir) - (size_t)len - 1);
    }

    config_load(&c);
    do_scan();

    /* Initialise display hook (HookEntry bridges 68k register ABI -> C) */
    memset(&disp_hook, 0, sizeof(disp_hook));
    disp_hook.h_Entry    = (HOOKFUNC)HookEntry;
    disp_hook.h_SubEntry = (HOOKFUNC)disp_hook_func;
    memset(&cheat_disp_hook, 0, sizeof(cheat_disp_hook));
    cheat_disp_hook.h_Entry    = (HOOKFUNC)HookEntry;
    cheat_disp_hook.h_SubEntry = (HOOKFUNC)cheat_disp_func;

    MUIMasterBase = OpenLibrary(STR(MUIMASTER_NAME), MUIMASTER_VMIN);
    if (!MUIMasterBase) {
        puts("Cannot open muimaster.library v19!");
        return 30;
    }

    if (!create_gui(&d, &c)) {
        puts("Failed to create MUI application!");
        CloseLibrary(MUIMasterBase);
        return 20;
    }

    /* ---- Open lowlevel.library for joystick (optional) ---- */
    LowLevelBase = OpenLibrary("lowlevel.library", 40);

    /* ---- Open timer for 100ms joystick polling ---- */
    timer_mp = CreateMsgPort();
    if (timer_mp) {
        timer_io = (struct timerequest *)
            CreateIORequest(timer_mp, sizeof(struct timerequest));
        if (timer_io) {
            if (OpenDevice("timer.device", UNIT_VBLANK,
                           (struct IORequest *)timer_io, 0) == 0) {
                timer_sig = 1UL << timer_mp->mp_SigBit;
                timer_io->tr_node.io_Command = TR_ADDREQUEST;
                timer_io->tr_time.tv_secs    = 0;
                timer_io->tr_time.tv_micro   = 100000; /* 100 ms */
                SendIO((struct IORequest *)timer_io);
            } else {
                DeleteIORequest((struct IORequest *)timer_io);
                timer_io = NULL;
            }
        }
    }

    rebuild_list(d.list_roms, d.tx_count, "");
    cheats_sync(&d, 1);
    set(d.window, MUIA_Window_Open, TRUE);
    /* Give the list keyboard focus immediately */
    set(d.window, MUIA_Window_ActiveObject, (ULONG)d.lv_roms);

    running = TRUE;
    while (running) {
        id = DoMethod(d.app, MUIM_Application_NewInput, &signals);
        switch (id) {

            case ID_FILTER: {
                STRPTR fs;
                get(d.str_filter, MUIA_String_Contents, &fs);
                rebuild_list(d.list_roms, d.tx_count,
                             fs ? (const char*)fs : "");
                cheats_sync(&d, 0);
                break;
            }

            case ID_SCAN:
                do_scan();
                {
                    STRPTR fs;
                    get(d.str_filter, MUIA_String_Contents, &fs);
                    rebuild_list(d.list_roms, d.tx_count,
                                 fs ? (const char*)fs : "");
                }
                cheats_sync(&d, 0);
                break;

            case ID_SORT: {
                ULONG val;
                STRPTR fs;
                get(d.cy_sort, MUIA_Cycle_Active, &val);
                sort_order = (int)val;
                apply_sort();
                get(d.str_filter, MUIA_String_Contents, &fs);
                rebuild_list(d.list_roms, d.tx_count,
                             fs ? (const char*)fs : "");
                cheats_sync(&d, 0);
                break;
            }

            /* The ROM list rebuilds above can leave rom_list[] reordered
             * under an unchanged active index, so these go through
             * cheats_sync() too rather than relying on the notification. */
            case ID_ROMSEL:
                cheats_sync(&d, 0);
                break;

            case ID_CHEAT_TOGGLE:
                cheats_toggle_active(&d);
                break;

            case ID_CHEAT_ALL:
                cheats_set_all(&d, 1);
                break;

            case ID_CHEAT_NONE:
                cheats_set_all(&d, 0);
                break;

            case ID_CHEAT_RELOAD:
                cheats_sync(&d, 1);
                break;

            case ID_CHEAT_SAVE:
                if (!cheat_count) {
                    MUI_Request(d.app, d.window, 0,
                                STR("Cheats"), STR("OK"), STR("%s"),
                                cheat_path[0]
                                  ? "There are no cheats to save for this game."
                                  : "Select a game on the Games page first.");
                } else if (cheats_save()) {
                    cheats_show_status(&d);
                    MUI_Request(d.app, d.window, 0,
                                STR("Cheats"), STR("OK"),
                                STR("Cheat switches saved to\n%s"), cheat_path);
                } else {
                    MUI_Request(d.app, d.window, 0,
                                STR("Cheats"), STR("OK"),
                                STR("Could not write the cheat file:\n%s"),
                                cheat_path);
                }
                break;

            case ID_LAUNCH:
                /* running stays TRUE - event loop continues after game exits */
                launch_game(&d, &c, progdir, cmd, sizeof(cmd));
                break;

            case ID_SAVE:
                collect(&d, &c);
                config_save(d.app, d.window, &c);
                MUI_Request(d.app, d.window, 0,
                            STR("Saved"), STR("OK"),
                            STR("Configuration saved to\n" CFG_PATH));
                break;

            case ID_ABOUT:
                MUI_Request(d.app, d.window, 0,
                            STR("About Genesis Plus GX"), STR("OK"),
                            STR("Genesis Plus GX v1.6\n"
                                "AmigaOS/CaffeineOS port\n\n"
                                "Port by Pedro Cotter (2026)\n\n"
                                "Based on Genesis Plus GX\n"
                                "by Charles MacDonald & Eke-Eke\n\n"
                                "MUI Launcher by Pedro Cotter"));
                break;

            case ID_CANCEL:
            case MUIV_Application_ReturnID_Quit:
                running = FALSE;
                break;
        }

        /* ---- Joystick polling on timer tick ---- */
        if (running && timer_io && LowLevelBase &&
            CheckIO((struct IORequest *)timer_io))
        {
            ULONG joy, joy_type, cur_dir;
            LONG  active, next;

            WaitIO((struct IORequest *)timer_io);

            joy      = ReadJoyPort(1);          /* port 1 = joystick port */
            joy_type = joy & JP_TYPE_MASK;

            if (joy_type == JP_TYPE_JOYSTK || joy_type == JP_TYPE_GAMECTLR) {
                ULONG pressed = joy & ~joy_prev; /* newly pressed bits */
                cur_dir = joy & (JPF_JOY_UP | JPF_JOY_DOWN);

                /* Auto-repeat: move on press, then 500ms delay, 200ms repeat */
                if (cur_dir != (joy_prev & (JPF_JOY_UP | JPF_JOY_DOWN)))
                    joy_repeat = 0;             /* direction changed: reset */
                else if (cur_dir)
                    joy_repeat++;

                if (cur_dir && (joy_repeat == 0 ||
                    (joy_repeat >= 5 && ((joy_repeat - 5) & 1) == 0)))
                {
                    get(d.list_roms, MUIA_List_Active, &active);
                    if (joy & JPF_JOY_UP) {
                        next = active - 1;
                        if (next >= 0)
                            set(d.list_roms, MUIA_List_Active, (ULONG)next);
                    }
                    if (joy & JPF_JOY_DOWN) {
                        next = active + 1;
                        if (next < vis_count)
                            set(d.list_roms, MUIA_List_Active, (ULONG)next);
                    }
                }

                /* Fire button: launch on press (not on hold) */
                if (pressed & (JPF_BUTTON_RED | JPF_BUTTON_BLUE))
                    launch_game(&d, &c, progdir, cmd, sizeof(cmd));
            }

            joy_prev = joy;

            /* Restart timer */
            timer_io->tr_node.io_Command = TR_ADDREQUEST;
            timer_io->tr_time.tv_secs    = 0;
            timer_io->tr_time.tv_micro   = 100000;
            SendIO((struct IORequest *)timer_io);
        }

        if (running)
            Wait(signals | timer_sig);
    }

    /* ---- Cleanup timer ---- */
    if (timer_io) {
        if (!CheckIO((struct IORequest *)timer_io))
            AbortIO((struct IORequest *)timer_io);
        WaitIO((struct IORequest *)timer_io);
        CloseDevice((struct IORequest *)timer_io);
        DeleteIORequest((struct IORequest *)timer_io);
    }
    if (timer_mp)
        DeleteMsgPort(timer_mp);

    if (LowLevelBase) {
        CloseLibrary(LowLevelBase);
        LowLevelBase = NULL;
    }

    /* Save config on every exit */
    collect(&d, &c);
    config_save(NULL, NULL, &c);
    /* Same for any cheat toggle that has not reached the .cht yet. */
    cheats_flush(&d);

    set(d.window, MUIA_Window_Open, FALSE);
    MUI_DisposeObject(d.app);
    CloseLibrary(MUIMasterBase);

    return 0;
}

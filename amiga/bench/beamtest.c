/*
 * beamtest.c - what scan rate is the native chipset running?
 *
 * Samples graphics.library VBeamPos() (V33) for a while and prints the
 * highest vertical beam position seen, the number of frame wraps and
 * GfxBase->DisplayFlags. A 15 kHz PAL frame has 312/313 lines, NTSC 262/263;
 * a 31 kHz (doubled) mode about twice that. Used to decide whether Paula can
 * fetch samples above ~28 kHz (minimum period 124 in 15 kHz modes).
 *
 * Build: D:/amiga-gcc-amiga6/bin/m68k-amigaos-gcc -m68040 -noixemul -O2
 *        -o beamtest beamtest.c
 */
#include <exec/types.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <stdio.h>

struct GfxBase *GfxBase;

int main(void)
{
    LONG max = 0, last = -1, wraps = 0, i;

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 0);
    if (!GfxBase)
    {
        printf("no graphics.library\n");
        return 20;
    }
    for (i = 0; i < 200000; i++)
    {
        LONG p = VBeamPos();
        if (p > max) max = p;
        if (p < last) wraps++;
        last = p;
    }
    printf("VBeamPos max %ld, %ld frame wraps in 200000 reads, DisplayFlags $%04lx (%s)\n",
           (long)max, (long)wraps, (unsigned long)GfxBase->DisplayFlags,
           (GfxBase->DisplayFlags & PAL) ? "PAL" : "NTSC");
    printf("graphics.library %ld.%ld\n", (long)GfxBase->LibNode.lib_Version,
           (long)GfxBase->LibNode.lib_Revision);
    CloseLibrary((struct Library *)GfxBase);
    return 0;
}

/****************************************************************************
 *  crc32.c
 *
 *  CRC32 calculation for Genesis Plus GX Amiga Port
 *  Standard CRC32 implementation (IEEE 802.3 polynomial)
 *
 ****************************************************************************/

#include "crc32.h"

/* CRC32 lookup table */
static unsigned long crc_table[256];
static int crc_table_ready = 0;

/* Generate the CRC32 lookup table */
static void make_crc_table(void)
{
    unsigned long c;
    int n, k;
    
    for (n = 0; n < 256; n++)
    {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++)
        {
            if (c & 1)
                c = 0xEDB88320UL ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc_table[n] = c;
    }
    crc_table_ready = 1;
}

/* Calculate CRC32 */
unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len)
{
    unsigned long c;
    unsigned int n;
    
    if (!crc_table_ready)
        make_crc_table();
    
    c = crc ^ 0xFFFFFFFFUL;
    
    for (n = 0; n < len; n++)
    {
        c = crc_table[(c ^ buf[n]) & 0xFF] ^ (c >> 8);
    }
    
    return c ^ 0xFFFFFFFFUL;
}

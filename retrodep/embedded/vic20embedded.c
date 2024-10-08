/*
 * vic20embedded.c - Code for embedding data files.
 *
 * This feature is only active when --enable-embedded is given to the
 * configure script, its main use is to make developing new ports easier
 * and to allow ports for platforms which don't have a filesystem, or a
 * filesystem which is hard/impossible to load data files from.
 *
 * Written by
 *  Marco van den Heuvel <blackystardust68@yahoo.com>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#ifdef USE_EMBEDDED
#include <string.h>
#include <stdio.h>

#include "embedded.h"
#include "machine.h"
#include "vic20mem.h"
#include "vic20rom.h"
#include "vic20memrom.h"

#include "vic20basic.h"
#include "vic20kernalpal.h"
#include "vic20kernalntsc.h"
#include "vic20chargen.h"

#include "vic_colodore_vic_vpl.h"
#include "vic_mike_ntsc_vpl.h"
#include "vic_mike_pal_vpl.h"
#include "vic_palette_vpl.h"
#include "vic_vice_vpl.h"

static embedded_t vic20files[] = {
    { VIC20_BASIC_NAME, VIC20_BASIC_ROM_SIZE, VIC20_BASIC_ROM_SIZE, VIC20_BASIC_ROM_SIZE, vic20_basic_rom },
    { VIC20_KERNAL_REV7_NAME, VIC20_KERNAL_ROM_SIZE, VIC20_KERNAL_ROM_SIZE, VIC20_KERNAL_ROM_SIZE, vic20_kernal_pal_rom },
    { VIC20_KERNAL_REV6_NAME, VIC20_KERNAL_ROM_SIZE, VIC20_KERNAL_ROM_SIZE, VIC20_KERNAL_ROM_SIZE, vic20_kernal_ntsc_rom },
    { VIC20_CHARGEN_NAME, VIC20_CHARGEN_ROM_SIZE, VIC20_CHARGEN_ROM_SIZE, VIC20_CHARGEN_ROM_SIZE, vic20_chargen_rom },
    EMBEDDED_LIST_END
};

static embedded_palette_t palette_files[] = {
    { "colodore_vic", "colodore_vic.vpl", 16, vic_colodore_vic_vpl },
    { "mike-ntsc", "mike-ntsc.vpl", 16, vic_mike_ntsc_vpl },
    { "mike-pal", "mike-pal.vpl", 16, vic_mike_pal_vpl },
    { "palette", "palette.vpl", 16, vic_palette_vpl },
    { "vice", "vice.vpl", 16, vic_vice_vpl },
    EMBEDDED_PALETTE_LIST_END
};

static size_t embedded_match_file(const char *name, unsigned char *dest, int minsize, int maxsize, embedded_t *emb)
{
    int i = 0;

    while (emb[i].name != NULL) {
        if (!strcmp(name, emb[i].name) && minsize == emb[i].minsize && maxsize == emb[i].maxsize) {
            if (emb[i].esrc != NULL) {
                if (emb[i].size != minsize) {
                    memcpy(dest, emb[i].esrc, maxsize);
                } else {
                    memcpy(dest + maxsize - minsize, emb[i].esrc, minsize);
                }
            }
            return emb[i].size;
        }
        i++;
    }
    return 0;
}

size_t embedded_check_file(const char *name, unsigned char *dest, int minsize, int maxsize)
{
    size_t retval;

    if ((retval = embedded_check_extra(name, dest, minsize, maxsize)) != 0) {
        return retval;
    }

    if ((retval = embedded_match_file(name, dest, minsize, maxsize, vic20files)) != 0) {
        return retval;
    }
    return 0;
}

int embedded_palette_load(const char *fname, palette_t *p)
{
    int i = 0;
    int j;
    unsigned char *entries;

    while (palette_files[i].name1 != NULL) {
        if (!strcmp(palette_files[i].name1, fname) || !strcmp(palette_files[i].name2, fname)) {
            entries = palette_files[i].palette;
            for (j = 0; j < palette_files[i].num_entries; j++) {
                p->entries[j].red = entries[(j * 4) + 0];
                p->entries[j].green = entries[(j * 4) + 1];
                p->entries[j].blue = entries[(j * 4) + 2];
            }
            return 0;
        }
        i++;
    }
    return -1;
}
#endif

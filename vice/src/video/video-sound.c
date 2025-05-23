/*
 * video-sound.c - Video to Audio leak emulation
 *
 * Written by
 *  groepaz <groepaz@gmx.net>
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

/* #define DEBUG_VIDEOSOUND */

#ifdef DEBUG_VIDEOSOUND
#define DBG(x) log_printf  x
#else
#define DBG(x)
#endif

#include <string.h>

#include "archdep.h"
#include "log.h"
#include "machine.h"
#include "sound.h"
#include "vice.h"
#include "viewport.h"
#include "video-sound.h"
#include "video.h"

#ifdef __LIBRETRO__
extern unsigned int opt_audio_leak_volume;
#define TOTAL_VOLUME            (1.50f * opt_audio_leak_volume)
#else
#define TOTAL_VOLUME            (1.50f)
#endif /* __LIBRETRO__ */

#define NOISE_VOLUME            (0.15f * TOTAL_VOLUME)
#define LUMALINES_VOLUME        (1.00f * TOTAL_VOLUME)

#define NOISE_RATE              (44100)
#define LUMALINES_RATE          (15000)

#define MAX_LUMALINES   512 /* maximum height of picture */

/* noise floor vaguely resembling random spikes at line frequency (~15khz) */
static const signed char noise_sample[] = {
    2, 1, 1, 1, 3, 2, 1, 1, 2, 1, 1, 1, 3, 2, 1, 1
};

static sound_chip_t video_sound;

static uint16_t video_sound_offset;
static int cycles_per_sec = 1000000;
static int sample_rate = 22050;
static int numchips = 1;

typedef struct {
    float lumas[MAX_LUMALINES];
    float avglum;
    const signed char *sampleptr;
    float *lumaptr;
    int firstline;
    int lastline;
    int enabled;
    int div1;
    int div2;
} videosound_t;
static videosound_t chip[2];

#ifdef SOUND_SYSTEM_FLOAT
/* FIXME */
static int video_sound_machine_calculate_samples(sound_t **psid, float *pbuf, int nr, int scc, CLOCK *delta_t)
{
    int i, num;
    float smpval1, smpval2;

    /* DBG(("video_sound_machine_calculate_samples")); */

    for (i = 0; i < nr; i++) {
        for (num = 0; num < numchips; num++) {
            smpval1 = (((float)(*chip[num].sampleptr) * chip[num].avglum * NOISE_VOLUME) / (1 << 16)) / 32767.0;
            smpval2 = (((*chip[num].lumaptr) * LUMALINES_VOLUME) / (1 << 16)) / 32767.0;
            pbuf[i] = smpval1 + smpval2;

            chip[num].div1 += NOISE_RATE;
            while (chip[num].div1 >= sample_rate) {
                chip[num].div1 -= sample_rate;
                chip[num].sampleptr++;
                if (chip[num].sampleptr == &noise_sample[sizeof(noise_sample)]) {
                    chip[num].sampleptr = noise_sample;
                }
            }
            chip[num].div2 += LUMALINES_RATE;
            while (chip[num].div2 >= sample_rate) {
                chip[num].div2 -= sample_rate;
                chip[num].lumaptr++;
                if (chip[num].lumaptr == &chip[num].lumas[chip[num].lastline + 1]) {
                    chip[num].lumaptr = &chip[num].lumas[chip[num].firstline];
                }
            }
        }
    }
    return nr;
}
#else
static int video_sound_machine_calculate_samples(sound_t **psid, int16_t *pbuf, int nr, int soc, int scc, CLOCK *delta_t)
{
    int i, num;
    int smpval1, smpval2;

    /* DBG(("video_sound_machine_calculate_samples")); */

    for (i = 0; i < nr; i++) {
        for (num = 0; num < numchips; num++) {
            smpval1 = (int)((float)(*chip[num].sampleptr) * chip[num].avglum * NOISE_VOLUME) / (1 << 16);
            smpval2 = (int)((*chip[num].lumaptr) * LUMALINES_VOLUME) / (1 << 16);
            switch (soc) {
                default:
                case SOUND_OUTPUT_MONO:
                    pbuf[i] = sound_audio_mix(pbuf[i], smpval1 + smpval2);
                    break;
                case SOUND_OUTPUT_STEREO:
                    pbuf[i * soc] = sound_audio_mix(pbuf[i * 2], smpval1 + smpval2);
                    pbuf[(i * soc) + 1] = sound_audio_mix(pbuf[(i * soc) + 1], smpval1 + smpval2);
                    break;
            }

            chip[num].div1 += NOISE_RATE;
            while (chip[num].div1 >= sample_rate) {
                chip[num].div1 -= sample_rate;
                chip[num].sampleptr++;
                if (chip[num].sampleptr == &noise_sample[sizeof(noise_sample)]) {
                    chip[num].sampleptr = noise_sample;
                }
            }
            chip[num].div2 += LUMALINES_RATE;
            while (chip[num].div2 >= sample_rate) {
                chip[num].div2 -= sample_rate;
                chip[num].lumaptr++;
                if (chip[num].lumaptr == &chip[num].lumas[chip[num].lastline + 1]) {
                    chip[num].lumaptr = &chip[num].lumas[chip[num].firstline];
                }
            }
        }
    }
    return nr;
}
#endif

static int video_sound_machine_init(sound_t *psid, int speed, int cycles)
{
    cycles_per_sec = cycles;
    sample_rate = speed;
    return 1;
}

static int video_sound_machine_cycle_based(void)
{
    return 0;
}

static int video_sound_machine_channels(void)
{
    return 1;
}

#ifdef SOUND_SYSTEM_FLOAT
/* stereo mixing placement of the Video sound interference 'device' sound */
static sound_chip_mixing_spec_t video_sound_mixing_spec[SOUND_CHIP_CHANNELS_MAX] = {
    {
        100, /* left channel volume % in case of stereo output, default output to both */
        100  /* right channel volume % in case of stereo output, default output to both */
    }
};
#endif

/* Video sound interference 'device' */
static sound_chip_t video_sound = {
    NULL,                                  /* NO sound chip open function */
    video_sound_machine_init,              /* sound chip init function */
    NULL,                                  /* NO sound chip close function */
    video_sound_machine_calculate_samples, /* sound chip calculate samples function */
    NULL,                                  /* NO sound chip store function */
    NULL,                                  /* NO sound chip read function */
    NULL,                                  /* NO sound chip reset function */
    video_sound_machine_cycle_based,       /* sound chip 'is_cycle_based()' function, chip is NOT cycle based */
    video_sound_machine_channels,          /* sound chip 'get_amount_of_channels()' function, sound chip has 1 channel */
#ifdef SOUND_SYSTEM_FLOAT
    video_sound_mixing_spec,               /* stereo mixing placement specs */
#endif
    0                                      /* sound chip enabled flag, toggled upon device (de-)activation */
};

/*
    this is a sort of ugly hack, unfortunately the video_render_config_t does
    not tell us which chip it belongs to by other means.
 */
static inline int get_chip_num(video_render_config_t *config)
{
    if ((numchips == 2) &&
        (config->chip_name[0] == 'V') &&
        (config->chip_name[1] == 'D') &&
        (config->chip_name[2] == 'C')) {
        return 1;
    }
    return 0;
}

static inline int check_enabled(void)
{
    int i;
    for (i = 0; i < numchips; i++) {
        if (chip[i].enabled) {
            return 1;
        }
    }
    return 0;
}

void video_sound_update(video_render_config_t *config, const uint8_t *src,
                        unsigned int width, unsigned int height,
                        unsigned int xs, unsigned int ys,
                        unsigned int pitchs, viewport_t *viewport)
{
    const int32_t *c1 = config->color_tables.ytablel;
    const int32_t *c2 = config->color_tables.ytableh;
    unsigned int x, y;
    const uint8_t *tmpsrc;
    float lum;
    int chipnum = get_chip_num(config);

    chip[chipnum].enabled = config->video_resources.audioleak;
    if (!check_enabled()) {
        video_sound.chip_enabled = 0;
        return;
    }
    video_sound.chip_enabled = 1;

    chip[chipnum].firstline = viewport->first_line;
    chip[chipnum].lastline = viewport->last_line;
    DBG(("video_sound_update (firstline:%d lastline:%d w:%d h:%d xs:%d ys:%d)",
         chip[chipnum].firstline, chip[chipnum].lastline, width, height, xs, ys));

    width /= config->scalex;
    /* height /= scaley; */

    /* width += xs; */
    ys = chip[chipnum].firstline;
    height = chip[chipnum].lastline - chip[chipnum].firstline;

    src += (pitchs * ys) + xs;

    for (y = 0; y < height; y++) {
        lum = 0;
        tmpsrc = src;
        for (x = 0; x < width; x++) {
            lum += (c1[*tmpsrc] << 2) + c2[*tmpsrc] + 0x10000;
            tmpsrc++;
        }
        chip[chipnum].lumas[ys] = lum / (float)(width * 5);
        src += pitchs;
        ys++;
    }
    lum = 0;
    for (y = chip[chipnum].firstline; y < (unsigned int)chip[chipnum].lastline; y++) {
        lum += chip[chipnum].lumas[y];
    }
    chip[chipnum].avglum = lum / (float)height;
}

void video_sound_init(void)
{
    int i;

    DBG(("video_sound_init"));

    video_sound_offset = sound_chip_register(&video_sound);

    if (machine_class == VICE_MACHINE_C128) {
        numchips = 2;
    } else {
        numchips = 1;
    }

    for (i = 0; i < numchips; i++) {
        chip[i].sampleptr = noise_sample;
        chip[i].lumaptr = &chip[i].lumas[0];
        memset (chip[i].lumas, 0, sizeof(float) * MAX_LUMALINES);
    }
}

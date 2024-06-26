/*
 * video-render-crtmono.c - Monochrome CRT renderers (used for CRTC)
 *
 * Written by
 *  groepaz <groepaz@gmx.net>
 *  John Selck <graham@cruise.de>
 *  Dag Lem <resid@nimrod.no>
 *  Andreas Boose <viceteam@t-online.de>
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

#include <stdio.h>

#include "log.h"
#include "machine.h"
#include "render1x1.h"
#include "render1x1rgbi.h"
#include "render1x2.h"
#include "render1x2rgbi.h"
#include "render2x2.h"
#include "render2x2rgbi.h"
#include "render2x4.h"
#include "render2x4rgbi.h"
#include "renderscale2x.h"
#include "resources.h"
#include "types.h"
#include "video-render.h"
#include "video.h"


static int rendermode_error = -1;

void video_render_crt_mono_main(video_render_config_t *config,
                           uint8_t *src, uint8_t *trg,
                           int width, int height, int xs, int ys, int xt,
                           int yt, int pitchs, int pitcht,
                           unsigned int viewport_first_line, unsigned int viewport_last_line)
{
    video_render_color_tables_t *colortab;
    int doublescan, crtemulation, rendermode, scale2x;

    rendermode = config->rendermode;
    doublescan = config->doublescan;
    colortab = &config->color_tables;

    scale2x = (config->filter == VIDEO_FILTER_SCALE2X);
    crtemulation = (config->filter == VIDEO_FILTER_CRT);

    if ((rendermode == VIDEO_RENDER_CRT_MONO_1X1
         || rendermode == VIDEO_RENDER_CRT_MONO_1X2
         || rendermode == VIDEO_RENDER_CRT_MONO_2X2
         || rendermode == VIDEO_RENDER_CRT_MONO_2X4)
        && config->video_resources.pal_scanlineshade <= 0) {
        doublescan = 0;
    }

    switch (rendermode) {
        case VIDEO_RENDER_NULL:
            return;
            break;

        case VIDEO_RENDER_CRT_MONO_1X1:
            if (crtemulation) {
                /* FIXME: open end, this should use a dedicated monochrome CRT renderer */
                render_32_1x1_rgbi(colortab, src, trg, width, height,
                                   xs, ys, xt, yt, pitchs, pitcht);
                return;
            } else {
                render_32_1x1_04(colortab, src, trg, width, height,
                                 xs, ys, xt, yt, pitchs, pitcht);
                return;
            }
            break;
#ifndef __LIBRETRO__
        case VIDEO_RENDER_CRT_MONO_1X2:
            if (crtemulation) {
                /* FIXME: open end, this should use a dedicated monochrome CRT renderer */
                render_32_1x2_rgbi(colortab, src, trg, width, height,
                                  xs, ys, xt, yt, pitchs, pitcht,
                                  viewport_first_line, viewport_last_line,
                                  config);
                return;
            } else {
                render_32_1x2(colortab, src, trg, width, height,
                              xs, ys, xt, yt, pitchs, pitcht, doublescan, config);
                return;
            }
            break;
        case VIDEO_RENDER_CRT_MONO_2X2:
            if (scale2x) {
                render_32_scale2x(colortab, src, trg, width, height,
                                  xs, ys, xt, yt, pitchs, pitcht);
                return;
            } else if (crtemulation) {
                /* FIXME: open end, this should use a dedicated monochrome CRT renderer */
                render_32_2x2_rgbi(colortab, src, trg, width, height,
                                  xs, ys, xt, yt, pitchs, pitcht,
                                  viewport_first_line, viewport_last_line, config);
                return;
            } else {
                render_32_2x2(colortab, src, trg, width, height,
                                 xs, ys, xt, yt, pitchs, pitcht, doublescan, config);
                return;
            }
            break;
        case VIDEO_RENDER_CRT_MONO_2X4:
            if (crtemulation) {
                /* FIXME: open end, this should use a dedicated monochrome CRT renderer */
                render_32_2x4_rgbi(colortab, src, trg, width, height,
                                  xs, ys, xt, yt, pitchs, pitcht,
                                  viewport_first_line, viewport_last_line, config);
                return;
            } else {
                render_32_2x4(colortab, src, trg, width, height,
                              xs, ys, xt, yt, pitchs, pitcht, doublescan, config);
                return;
            }
            break;
#endif
    }
    if (rendermode_error != rendermode) {
        log_error(LOG_DEFAULT, "video_render_crt_mono_main: unsupported rendermode (%d)", rendermode);
    }
    rendermode_error = rendermode;
}

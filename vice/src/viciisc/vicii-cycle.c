/*
 * vicii-cycle.c - Cycle based VIC-II emulation.
 *
 * Written by
 *  Hannu Nuotio <hannu.nuotio@tut.fi>
 *  Daniel Kahlin <daniel@kahlin.net>
 *
 * Based on code by
 *  Ettore Perazzoli <ettore@comm2000.it>
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

#include <string.h>

#include "debug.h"
#include "lib.h"
#include "log.h"
#include "maincpu.h"
#include "types.h"
#include "vicii-chip-model.h"
#include "vicii-cycle.h"
#include "vicii-draw-cycle.h"
#include "vicii-fetch.h"
#include "vicii-irq.h"
#include "vicii-lightpen.h"
#include "vicii-resources.h"
#include "vicii.h"
#include "viciitypes.h"

static inline void check_badline(void)
{
    /* Check badline condition (line range and "allow bad lines" handled outside */
    if ((vicii.raster_line & 7) == vicii.ysmooth) {
        vicii.bad_line = 1;
        vicii.idle_state = 0;
    } else {
        vicii.bad_line = 0;
    }
}

static inline void check_sprite_display(void)
{
    int i, b;
    int enable = vicii.regs[0x15];

    for (i = 0, b = 1; i < VICII_NUM_SPRITES; i++, b <<= 1) {
        unsigned int y = vicii.regs[i * 2 + 1];
        vicii.sprite[i].mc = vicii.sprite[i].mcbase;

        if (vicii.sprite_dma & b) {
            if ((enable & b) && (y == (vicii.raster_line & 0xff))) {
                vicii.sprite_display_bits |= b;
            }
        } else {
            vicii.sprite_display_bits &= ~b;
        }
    }
}

static inline void sprite_mcbase_update(void)
{
    int i;

    for (i = 0; i < VICII_NUM_SPRITES; i++) {
        if (vicii.sprite[i].exp_flop) {
            vicii.sprite[i].mcbase = vicii.sprite[i].mc;
            if (vicii.sprite[i].mcbase == 63) {
                vicii.sprite_dma &= ~(1 << i);
            }
        }
    }
}

static inline void check_exp(void)
{
    int i, b;
    int y_exp = vicii.regs[0x17];

    for (i = 0, b = 1; i < VICII_NUM_SPRITES; i++, b <<= 1) {
        if ((vicii.sprite_dma & b) && (y_exp & b)) {
            vicii.sprite[i].exp_flop ^= 1;
        }
    }
}

/* Enable DMA for sprite i.  */
static inline void turn_sprite_dma_on(unsigned int i, int y_exp)
{
    vicii.sprite_dma |= 1 << i;
    vicii.sprite[i].mcbase = 0;
    vicii.sprite[i].exp_flop = 1;
}

static inline void check_sprite_dma(void)
{
    int i, b;
    int enable = vicii.regs[0x15];
    int y_exp = vicii.regs[0x17];

    for (i = 0, b = 1; i < VICII_NUM_SPRITES; i++, b <<= 1) {
        unsigned int y = vicii.regs[i * 2 + 1];

        if ((enable & b) && (y == (vicii.raster_line & 0xff)) && !(vicii.sprite_dma & b)) {
            turn_sprite_dma_on(i, y_exp & b);
        }
    }
}

static inline uint8_t cycle_phi1_fetch(unsigned int cycle_flags)
{
    uint8_t data;
    int s;

    if (cycle_is_fetch_g(cycle_flags)) {
        if (!vicii.idle_state) {
            data = vicii_fetch_graphics();
        } else {
            data = vicii_fetch_idle_gfx();
        }
        return data;
    }

    if (cycle_is_sprite_ptr_dma0(cycle_flags)) {
        s = cycle_get_sprite_num(cycle_flags);
        data = vicii_fetch_sprite_pointer(s);
        return data;
    }
    if (cycle_is_sprite_dma1_dma2(cycle_flags)) {
        s = cycle_get_sprite_num(cycle_flags);
        data = vicii_fetch_sprite_dma_1(s);
        return data;
    }

    if (cycle_is_refresh(cycle_flags)) {
        data = vicii_fetch_refresh();
        return data;
    }

    data = vicii_fetch_idle();

    return data;
}

static inline void check_vborder_top(int line)
{
    int rsel = vicii.regs[0x11] & 0x08;

    if ((line == (rsel ? VICII_25ROW_START_LINE : VICII_24ROW_START_LINE)) && (vicii.regs[0x11] & 0x10)) {
        vicii.vborder = 0;
        vicii.set_vborder = 0;
    }
}

static inline void check_vborder_bottom(int line)
{
    int rsel = vicii.regs[0x11] & 0x08;

    if (line == (rsel ? VICII_25ROW_STOP_LINE : VICII_24ROW_STOP_LINE)) {
        vicii.set_vborder = 1;
    }
}

static inline void check_hborder(unsigned int cycle_flags)
{
    int csel = vicii.regs[0x16] & 0x08;

    /* Left border ends at cycles 17 (csel=1) or 18 (csel=0) on PAL. */
    if (cycle_is_check_border_l(cycle_flags, csel)) {
        check_vborder_bottom(vicii.raster_line);
        vicii.vborder = vicii.set_vborder;
        if (vicii.vborder == 0) {
            vicii.main_border = 0;
        }
    }
    /* Right border starts at cycles 56 (csel=0) or 57 (csel=1) on PAL. */
    if (cycle_is_check_border_r(cycle_flags, csel)) {
        vicii.main_border = 1;
    }
}

static inline void vicii_cycle_start_of_frame(void)
{
    vicii.start_of_frame = 0;
    vicii.raster_line = 0;
    vicii.refresh_counter = 0xff;
    vicii.allow_bad_lines = 0;
    vicii.vcbase = 0;
    vicii.vc = 0;
    vicii.light_pen.triggered = 0;

    /* Retrigger light pen if line is still held low */
    if (vicii.light_pen.state) {
        /* add offset depending on chip model (FIXME use proper variable) */
        vicii.light_pen.x_extra_bits = (vicii.color_latency ? 2 : 1);
        vicii_trigger_light_pen_internal(1);
    }
}

static inline void vicii_cycle_end_of_line(void)
{
    vicii_raster_draw_handler();
    if (vicii.raster_line == vicii.screen_height - 1) {
        vicii.start_of_frame = 1;
    }
}

static inline void vicii_cycle_start_of_line(void)
{
    /* Check DEN bit on first cycle of the line following the first DMA line  */
    if ((vicii.raster_line == VICII_FIRST_DMA_LINE) && !vicii.allow_bad_lines && (vicii.regs[0x11] & 0x10)) {
        vicii.allow_bad_lines = 1;
    }

    /* Disallow bad lines after the last possible one has passed */
    if (vicii.raster_line == VICII_LAST_DMA_LINE) {
        vicii.allow_bad_lines = 0;
    }

    vicii.bad_line = 0;
}


static inline void next_vicii_cycle(void)
{
    /* Next cycle */
    vicii.raster_cycle++;

    /* Handle wrapping */
    if (vicii.raster_cycle == (unsigned int)vicii.cycles_per_line) {
        vicii.raster_cycle = 0;
    }
}

/* on "powercycle" re-init masks and counters */
#define VSP_PROB_MAX 4
#define VSP_PROB_MIN 0
#define VSP_PROB_THRESH 3

static unsigned int vsp_ysmoothold = 0;
static unsigned int vsp_buglines[8];
static unsigned int vsp_bugchannels[8];
static unsigned int vsp_bugwarn = 0;
static unsigned int vsp_buginitialized = 0;

/* FIXME: reset on "powercycle */
void vicii_init_vsp_bug(void)
{
    unsigned int val, i;

#ifdef __LIBRETRO__
    if (!vicii_resources.vsp_bug_enabled)
       return;
#endif

    vsp_ysmoothold = vicii.ysmooth;
    vsp_bugwarn = 100;  /* max 100 lines of warnings before we give up */

    for (i = 0; i < 8; i++) {
        vsp_buglines[i] =  VSP_PROB_MAX / 2;
        vsp_bugchannels[i] =  VSP_PROB_MAX / 2;
    }
    /* FIXME: we might want to make this a user setting */
    /* get a random mask for channels that we want to make immune */
    val = lib_unsigned_rand(0, 0xff);
    log_message(vicii.log,
            "VSP Bug: safe channels are: %s%s%s%s%s%s%s%s. Emulation of memory corruption is %s.",
            (val & 1) ? "0" : "",
            (val & 2) ? "1" : "",
            (val & 4) ? "2" : "",
            (val & 8) ? "3" : "",
            (val & 0x10) ? "4" : "",
            (val & 0x20) ? "5" : "",
            (val & 0x40) ? "6" : "",
            (val & 0x80) ? "7" : "",
            vicii_resources.vsp_bug_enabled ? "enabled" : "disabled"
               );
    for (i = 0; i < 8; i++) {
        if (val & 1) {
            vsp_bugchannels[i] = VSP_PROB_MIN;
        }
        val >>= 1;
    }
    /* get a random mask for lines that we want to make weaker */
    val = lib_unsigned_rand(0, 0xff);
    for (i = 0; i < 8; i++) {
        if (val & 1) {
            vsp_buglines[i] >>= 1;
        }
        val >>= 1;
    }

    vsp_buginitialized = 1;
}

/* see VSP Lab (http://csdb.dk/release/?id=120810) */
static inline void vicii_handle_vsp_bug(void)
{
    unsigned int page, row, channel = 0, line = 0;

    /* FIXME: we should instead init at "powercycle" */
    if (!vsp_buginitialized) {
        vicii_init_vsp_bug();
    }

    if (vsp_bugwarn || vicii_resources.vsp_bug_enabled) {
        channel = (vicii.ysmooth ^ vsp_ysmoothold) & 7;
        line = vicii.raster_line & 7;
    }

    /* if emulation is disabled, warn only */
    if (vsp_bugwarn) {
        log_message(vicii.log,
                "VSP Bug: Line: %u/%2u  Cycle: %2u  Channel: %u %s",
                line, vicii.raster_line, vicii.raster_cycle, channel,
                ((vsp_buglines[line] + vsp_bugchannels[channel] + 1) > VSP_PROB_THRESH) ? "*" :""
                   );
        vsp_bugwarn--;
        if (vsp_bugwarn == 0) {
            log_message(vicii.log, "VSP Bug: further warnings supressed");
        }
    }

    /* simulate the "VSP bug" problem */
    if(vicii_resources.vsp_bug_enabled) {
        if((vsp_buglines[line] + vsp_bugchannels[channel] + lib_unsigned_rand(0, 1)) > VSP_PROB_THRESH) {
            for(page = 0x00; page < 0xff; page++) {
                /* keep 98,5% of all pages untouched. this is hand tweaked to result in
                 * somewhat convincing long term plots in vsp-lab */
                if (lib_unsigned_rand(0, 1000) > 985) {
                    int seen0 = 0, seen1 = 0, fragile, result;
                    int firstrow = 7;
                    /* in each page, all addresses ending with 7 or F are affected */
                    for(row = firstrow; row <= 0xff; row += 0x08) {
                        seen0 |= vicii.ram_base_phi1[((page << 8) | row) & 0xffff] ^ 255;
                        seen1 |= vicii.ram_base_phi1[((page << 8) | row) & 0xffff];
                    }
                    fragile = (seen0 & seen1);
                    result = fragile & lib_unsigned_rand(0, 0xff);

                    for(row = firstrow; row <= 0xff; row += 0x08) {
                        vicii.ram_base_phi1[((page << 8) | row) & 0xffff] &= ~fragile;
                        vicii.ram_base_phi1[((page << 8) | row) & 0xffff] |= result;
#if 0
                        if (vsp_bugwarn) {
                            log_message(vicii.log,
                                "VSP Bug: Corrupting %04x, fragile %02x, new bits %02x",
                                (unsigned int)(page << 8) | row,
                                (unsigned int)fragile, (unsigned int)result);
                        }
#endif
                    }
                }
            }
        }
    }
}

int vicii_cycle(void)
{
    int ba_low = 0;
    int can_sprite_sprite, can_sprite_background;
    int vsp_may_crash;

    /*VICII_DEBUG_CYCLE(("cycle: line %i, clk %i", vicii.raster_line, vicii.raster_cycle));*/

    /* perform phi2 fetch after the cpu has executed */
    vicii_fetch_sprites(vicii.cycle_flags);

    /*
     *
     * End of Phi2
     *
     ******/

    /* Next cycle */
    next_vicii_cycle();
    vicii.cycle_flags = vicii.cycle_table[vicii.raster_cycle];

    /******
     *
     * Start of Phi1
     *
     */

    /* Phi1 fetch */
    vicii.last_read_phi1 = cycle_phi1_fetch(vicii.cycle_flags);

    /* Check horizontal border flag */
    check_hborder(vicii.cycle_flags);

    can_sprite_sprite = (vicii.sprite_sprite_collisions == 0);
    can_sprite_background = (vicii.sprite_background_collisions == 0);

    /* Draw one cycle of pixels */
    vicii_draw_cycle();

    /* clear any collision registers as initiated by $d01e or $d01f reads */
    switch (vicii.clear_collisions) {
        case 0x1e:
            vicii.sprite_sprite_collisions = 0;
            vicii.clear_collisions = 0;
            break;
        case 0x1f:
            vicii.sprite_background_collisions = 0;
            vicii.clear_collisions = 0;
            break;
        default:
            break;
    }

    /* Trigger collision IRQs */
    if (can_sprite_sprite && vicii.sprite_sprite_collisions) {
        vicii_irq_sscoll_set();
    }
    if (can_sprite_background && vicii.sprite_background_collisions) {
        vicii_irq_sbcoll_set();
    }

    /*
     *
     * End of Phi1
     *
     ******/

    /******
     *
     * Start of Phi2
     *
     */

    /* Handle end of line/start of new line */
    if (vicii.raster_cycle == VICII_PAL_CYCLE(1)) {
        vicii_cycle_end_of_line();
        vicii_cycle_start_of_line();
    }

    if (vicii.start_of_frame) {
        if (vicii.raster_cycle == VICII_PAL_CYCLE(2)) {
            vicii_cycle_start_of_frame();
        }
    } else {
        if (vicii.raster_cycle == VICII_PAL_CYCLE(1)) {
            vicii.raster_line++;
        }
    }

    /*
     * Trigger a raster IRQ if the raster comparison goes from
     * non-match to match.
     */
    if (vicii.raster_line == vicii.raster_irq_line) {
        if (!vicii.raster_irq_triggered) {
            vicii_irq_raster_trigger();
            vicii.raster_irq_triggered = 1;
        }
    } else {
        vicii.raster_irq_triggered = 0;
    }

    /* Check vertical border flag */
    check_vborder_top(vicii.raster_line);
    /* Check vertical border flag */
    check_vborder_bottom(vicii.raster_line);
    if (vicii.raster_cycle == VICII_PAL_CYCLE(1)) {
        vicii.vborder = vicii.set_vborder;
    }

    /******
     *
     * Sprite logic
     *
     */

    /* Update sprite mcbase (Cycle 16 on PAL) */
    /* if (vicii.raster_cycle == VICII_PAL_CYCLE(16)) { */
    if (cycle_is_update_mcbase(vicii.cycle_flags)) {
        sprite_mcbase_update();
    }

    /* Check sprite DMA (Cycles 55 & 56 on PAL) */
    /* if (vicii.raster_cycle == VICII_PAL_CYCLE(55)
       || vicii.raster_cycle == VICII_PAL_CYCLE(56) ) { */
    if (cycle_is_check_spr_dma(vicii.cycle_flags)) {
        check_sprite_dma();
    }

    /* Check sprite expansion flags (Cycle 56 on PAL) */
    /* if (vicii.raster_cycle == VICII_PAL_CYCLE(56)) { */
    if (cycle_is_check_spr_exp(vicii.cycle_flags)) {
        check_exp();
    }

    /* Check sprite display (Cycle 58 on PAL) */
    /* if (vicii.raster_cycle == VICII_PAL_CYCLE(58)) { */
    if (cycle_is_check_spr_disp(vicii.cycle_flags)) {
        check_sprite_display();
    }

    /******
     *
     * Graphics logic
     *
     */

    vsp_may_crash = !vicii.bad_line && vicii.idle_state; /* flag for "VSP bug" simulation */

    /* Check DEN bit on first DMA line */
    if ((vicii.raster_line == VICII_FIRST_DMA_LINE) && !vicii.allow_bad_lines) {
        vicii.allow_bad_lines = (vicii.regs[0x11] & 0x10) ? 1 : 0;
    }

    /* Check badline condition, trigger fetches */
    if (vicii.allow_bad_lines) {
        check_badline();
    }

    /* VSP-bug condition */
    if (vicii.bad_line && vsp_may_crash &&
        (vicii.raster_cycle >= VICII_PAL_CYCLE(16)) &&
        (vicii.raster_cycle < VICII_PAL_CYCLE(55))) {
            vicii_handle_vsp_bug();
    }
    vsp_ysmoothold = vicii.ysmooth;

    /* Update VC (Cycle 14 on PAL) */
    /*  if (vicii.raster_cycle == VICII_PAL_CYCLE(14)) { */
    if (cycle_is_update_vc(vicii.cycle_flags)) {
        vicii.vc = vicii.vcbase;
        vicii.vmli = 0;
        if (vicii.bad_line) {
            vicii.rc = 0;
        }
    }

    /* Update RC (Cycle 58 on PAL) */
    /* if (vicii.raster_cycle == VICII_PAL_CYCLE(58)) { */
    if (cycle_is_update_rc(vicii.cycle_flags)) {
        /* `rc' makes the chip go to idle state when it reaches the
           maximum value.  */
        if (vicii.rc == 7) {
            vicii.idle_state = 1;
            vicii.vcbase = vicii.vc;
        }
        if (!vicii.idle_state || vicii.bad_line) {
            vicii.rc = (vicii.rc + 1) & 0x7;
            vicii.idle_state = 0;
        }
    }

    /******
     *
     * BA logic
     *
     */

    /* Check BA for matrix fetch */
    if (vicii.bad_line && cycle_is_fetch_ba(vicii.cycle_flags)) {
        ba_low = 1;
    }

    /* Check BA for Sprite Phi2 fetch */
    ba_low |= vicii_check_sprite_ba(vicii.cycle_flags);

    /* if ba_low transitioning from non-active to active, always count
       3 cycles before allowing any Phi2 accesses. */
    if (ba_low) {
        /* count down prefetch cycles */
        if (vicii.prefetch_cycles) {
            vicii.prefetch_cycles--;
        }
    } else {
        /* this needs to be +1 because it gets decremented already in the
           first ba cycle */
        vicii.prefetch_cycles = 3 + 1;
    }


    /* Matrix fetch */
    if (vicii.bad_line && cycle_may_fetch_c(vicii.cycle_flags)) {
#ifdef DEBUG
        if (debug.maincpu_traceflg) {
            log_debug(LOG_DEFAULT, "DMA at cycle %u   %"PRIu64"", vicii.raster_cycle, maincpu_clk);
        }
#endif
        vicii_fetch_matrix();
    }

    /* clear internal bus (may get set by a VIC-II read or write) */
    vicii.last_bus_phi2 = 0xff;

    /* delay video mode for fetches by one cycle */
    vicii.reg11_delay = vicii.regs[0x11];

    /* trigger light pen if scheduled */
    if (vicii.light_pen.trigger_cycle == maincpu_clk) {
        vicii_trigger_light_pen_internal(0);
    }

    return ba_low;
}

/* The REU can use an additional cycle at the point where the dma of sprite 0 is turned on */
/* this is because of late setting of BA due to internal delays */
/* The CPU can't use this cycle as it checks the state later */
int vicii_cycle_reu(void)
{
    int check = vicii.raster_cycle == VICII_PAL_CYCLE(54) && (vicii.regs[0x15] & 1) && (vicii.regs[1] == (vicii.raster_line & 0xff)) && !(vicii.sprite_dma & 1);
    return vicii_cycle() && !check;
}

/* Steal cycles from CPU  */
void vicii_steal_cycles(void)
{
    int ba_low;

    do {
        maincpu_clk++;
        ba_low = vicii_cycle();
    } while (ba_low);
}

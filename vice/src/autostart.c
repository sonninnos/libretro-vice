/** \file   autostart.
 * \brief   Automatic image loading and starting.
 *
 * \author  Teemu Rantanen <tvr@cs.hut.fi>
 * \author  Ettore Perazzoli <ettore@comm2000.it>
 * \author  Andre Fachat <a.fachat@physik.tu-chemnitz.de>
 * \author  Andreas Boose <viceteam@t-online.de>
 * \author  Thomas Bretz <tbretz@ph.tum.de>
 * \author  groepaz <groepaz@gmx.net>
 */

/*
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

/* #define DEBUG_AUTOSTART */
/* #define DEBUG_AUTOSTARTWAIT */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archdep.h"
#include "autostart.h"
#include "autostart-prg.h"
#include "attach.h"
#include "cartridge.h"
#include "charset.h"
#include "cmdline.h"
#include "datasette.h"
#include "diskimage.h"
#include "drive.h"
#include "driveimage.h"
#include "fileio.h"
#include "fsdevice.h"
#include "fsdevice-filename.h"
#include "imagecontents.h"
#include "tapecontents.h"
#include "diskcontents.h"
#include "initcmdline.h"
#include "interrupt.h"
#include "kbdbuf.h"
#include "lib.h"
#include "log.h"
#include "machine-bus.h"
#include "machine.h"
#include "maincpu.h"
#include "mainlock.h"
#include "mem.h"
#include "monitor.h"
#include "mon_breakpoint.h"
;
#include "network.h"
#include "resources.h"
#include "snapshot.h"
#include "tape.h"
#include "tapecart.h"
#include "tapeport.h"
#include "types.h"
#include "uiapi.h"
#include "util.h"
#include "vdrive.h"
#include "vdrive-bam.h"
#include "vice-event.h"
#include "vsync.h"

#ifdef __LIBRETRO__
extern unsigned int retro_warpmode;
extern int tape_counter;
extern int tape_found_counter;
#endif

#ifdef DEBUG_AUTOSTART
#define DBG(_x_) log_debug _x_
#else
#define DBG(_x_)
#endif

#ifdef DEBUG_AUTOSTARTWAIT
#define DBGWAIT(_x_) log_debug _x_
#else
#define DBGWAIT(_x_)
#endif

static void autostart_done(void);
static void autostart_finish(void);

/* Current state of the autostart routine.  */
static enum {
    AUTOSTART_NONE,
    AUTOSTART_ERROR,
    AUTOSTART_HASTAPE,
    AUTOSTART_PRESSPLAYONTAPE,
    AUTOSTART_LOADINGTAPE,
    AUTOSTART_HASDISK,
    AUTOSTART_LOADINGDISK,
    AUTOSTART_HASSNAPSHOT,
    AUTOSTART_WAITLOADREADY,
    AUTOSTART_WAITLOADING,
    AUTOSTART_WAITSEARCHINGFOR,
    AUTOSTART_INJECT,
    AUTOSTART_DONE
} autostartmode = AUTOSTART_NONE;

#define AUTOSTART_WAIT_BLINK   0
#define AUTOSTART_NOWAIT_BLINK 1

#define AUTOSTART_CHECK_ANY_COLUMN      0
#define AUTOSTART_CHECK_FIRST_COLUMN    1

/* Log descriptor.  */
log_t autostart_log = LOG_DEFAULT;

/* Flag: was true drive emulation turned on when we started booting the disk image?  */
static int orig_drive_true_emulation_state = -1;
/* Flag: were device traps turned on when we started booting the disk image?  */
static int orig_device_traps_state = -1;
/* Flag: was iec device turned on when we started booting the disk image?  */
static int orig_iec_device_state = -1;
/* Flag: warp mode state before booting */
static int orig_warp_mode = -1;
static int orig_FileSystemDevice8 = -1;
static int orig_FSDevice8ConvertP00 = -1;
static int orig_FSDeviceLongNames = -1;

/* PETSCII name of the program to load. NULL if default */
static char *autostart_program_name = NULL;

/* Minimum number of cycles before we feed BASIC with commands.  */
static CLOCK autostart_initial_delay_cycles;

/* Flag: Do we want to switch true drive emulation on/off during autostart?
 * Normally, this is the same as handle_drive_true_emulation_by_machine;
 * however, the user can override this decision by specifying
 * -autostart-no-true-drive-emulation
 */
static int handle_drive_true_emulation_overridden;

/* Flag: Does the machine want us to switch true drive emulation on/off during autostart? */
static int handle_drive_true_emulation_by_machine;

/* Flag: autostart is initialized.  */
static int autostart_enabled = 0;

/* Flag: Autostart the file or just load it?  */
static unsigned int autostart_run_mode;

/* Flag: maincpu_clk isn't resetted yet */
static int autostart_wait_for_reset;

/* Flag: load stage after LOADING enters ROM area */
static int entered_rom = 0;

/* Flag: trap monitor after done */
static int trigger_monitor = 0;

int autostart_ignore_reset = 0; /* FIXME: only used by datasette.c, does it really have to be global? */

static int autostart_disk_unit = DRIVE_UNIT_MIN; /* set by setup_for_disk */
static int autostart_disk_drive = 0; /* set by setup_for_disk */

static int autostart_tape_unit = 1; /* set by autostart_tape */

#define AUTOSTART_DISK_IMAGE    0
#define AUTOSTART_PRG_VFS       1
#define AUTOSTART_PRG_DISK      2
#define AUTOSTART_PRG_INJECT    3

static int autostart_type = -1;

/* ------------------------------------------------------------------------- */
static size_t tap_initial_raw_offset = 0;

int autostart_basic_load = 0;

int autostart_tape_basic_load = 1;

static int AutostartRunWithColon = 0;

static int AutostartHandleTrueDriveEmulation = 0;

static int AutostartWarp = 0;

static int AutostartDelay = 0;
static int AutostartDelayDefaultSeconds = 0;
static int AutostartDelayRandom = 0;

static int AutostartPrgMode = AUTOSTART_PRG_MODE_VFS;

static char *AutostartPrgDiskImage = NULL;

static int AutostartDropMode = AUTOSTART_DROP_MODE_RUN;


static const char * const AutostartRunCommandsAvailable[] = {
    "RUN\r", "RUN:\r"
};

/** \brief  Keep track of the generated 'factory' value for the default disk
 *
 * Factory values are const, so we need a little extra code to avoid free'ing
 * a const.
 */
static char *autostart_default_diskimage = NULL;


static const char * AutostartRunCommand = NULL;


static void set_handle_true_drive_emulation_state(void)
{
    handle_drive_true_emulation_overridden =
        AutostartHandleTrueDriveEmulation ?
        handle_drive_true_emulation_by_machine : 0;
}

/*! \internal \brief set if autostart should use LOAD ... ,1 */
static int set_autostart_basic_load(int val, void *param)
{
    autostart_basic_load = val ? 1 : 0;

    return 0;
}

/*! \internal \brief set if autostart from tape should use LOAD ... ,1 */
static int set_autostart_tape_basic_load(int val, void *param)
{
    autostart_tape_basic_load = val ? 1 : 0;

    return 0;
}

/*! \internal \brief set if autostart should execute with a colon or not

 \param val
   if 0, the "RUN" command at the end of autostart is executed without
   a colon; else, it will be executed with a colon.

 \param param
   unused

 \return
   0 on success. else -1.
*/
static int set_autostart_run_with_colon(int val, void *param)
{
    AutostartRunWithColon = val ? 1 : 0;

    AutostartRunCommand = AutostartRunCommandsAvailable[AutostartRunWithColon];

    return 0;
}

/*! \internal \brief set if autostart should handle TDE or not

 \param val
   if 0, autostart does not handle TDE even if the machine says it can
   handle it.

 \param param
   unused

 \return
   0 on success. else -1.
*/
static int set_autostart_handle_tde(int val, void *param)
{
    AutostartHandleTrueDriveEmulation = val ? 1 : 0;

    set_handle_true_drive_emulation_state();

    return 0;
}

/*! \internal \brief set if autostart should enable warp mode */
static int set_autostart_warp(int val, void *param)
{
    AutostartWarp = val ? 1 : 0;

    return 0;
}

/*! \internal \brief set initial autostart delay. 0 means default. */
static int set_autostart_delay(int val, void *param)
{
    if (val < 0) {
        val = 0;
    } else if (val > 1000) {
        val = 1000;
    }
    AutostartDelay = val;
    return 0;
}

/*! \internal \brief set initial autostart random delay. 0 means off, 1 means on. */
static int set_autostart_delayrandom(int val, void *param)
{
    AutostartDelayRandom = val ? 1 : 0;
    return 0;
}

/*! \internal \brief set autostart prg mode */
static int set_autostart_prg_mode(int val, void *param)
{
    if ((val < 0) || (val > AUTOSTART_PRG_MODE_LAST)) {
        val = AUTOSTART_PRG_MODE_DEFAULT;
    }
    AutostartPrgMode = val;

    return 0;
}

/*! \internal \brief set disk image name of autostart prg mode */

static int set_autostart_prg_disk_image(const char *val, void *param)
{
    if (util_string_set(&AutostartPrgDiskImage, val)) {
        return 0;
    }

    return 0;
}

/** \brief  Resource setter for "AutostartDropMode" resource
 *
 * \param[in]   mode    new mode
 * \param[in]   unused  unused
 *
 * \return  0 on success, -1 on error
 */
static int set_autostart_drop_mode(int mode, void *unused)
{
    int result = 0;

    switch (mode) {
        case AUTOSTART_DROP_MODE_ATTACH:    /* fall through */
        case AUTOSTART_DROP_MODE_LOAD:      /* fall through */
        case AUTOSTART_DROP_MODE_RUN:
            AutostartDropMode = mode;
            break;
        default:
            result = 1;
            break;
    }
    return result;
}

/** \brief  Command line resource setter for "AutostartDropMode" resource
 *
 * \param[in]   value   new mode (0, 1, 2 or "attach", "load", "run")
 * \param[in]   unused  unused
 *
 * \return  0 on success, -1 on error
 */
static int cmdline_set_autostart_drop_mode(const char *value, void *unused)
{
    if ((strcmp(value, "0") == 0) || (strcmp(value, "attach") == 0)) {
        AutostartDropMode = AUTOSTART_DROP_MODE_ATTACH;
    } else if ((strcmp(value, "1") == 0) || (strcmp(value, "load") == 0)) {
        AutostartDropMode = AUTOSTART_DROP_MODE_LOAD;
    } else if ((strcmp(value, "2") == 0) || (strcmp(value, "run") == 0)) {
        AutostartDropMode = AUTOSTART_DROP_MODE_RUN;
    } else {
        return -1;
    }
    return 0;
}

/*! \brief string resources used by autostart */
static resource_string_t resources_string[] = {
    /* caution: position is hardcoded below */
    { "AutostartPrgDiskImage", NULL, RES_EVENT_NO, NULL,
      &AutostartPrgDiskImage, set_autostart_prg_disk_image, NULL },
    RESOURCE_STRING_LIST_END
};

/*! \brief integer resources used by autostart */
static resource_int_t resources_int_basicload[] = {
    /* caution: position is hardcoded below */
    { "AutostartBasicLoad", 0, RES_EVENT_NO, (resource_value_t)0,
      &autostart_basic_load, set_autostart_basic_load, NULL },
    /* caution: position is hardcoded below */
#ifdef __LIBRETRO__
    { "AutostartTapeBasicLoad", 1, RES_EVENT_NO, (resource_value_t)1,
#else
    { "AutostartTapeBasicLoad", 0, RES_EVENT_NO, (resource_value_t)1,
#endif
      &autostart_tape_basic_load, set_autostart_tape_basic_load, NULL },
    RESOURCE_INT_LIST_END
};

static resource_int_t resources_int_basicload_pet[] = {
    /* caution: position is hardcoded below */
    { "AutostartBasicLoad", 1, RES_EVENT_NO, (resource_value_t)0,
      &autostart_basic_load, set_autostart_basic_load, NULL },
    RESOURCE_INT_LIST_END
};

static resource_int_t resources_int[] = {
    { "AutostartRunWithColon", 0, RES_EVENT_NO, (resource_value_t)1,
      &AutostartRunWithColon, set_autostart_run_with_colon, NULL },
    { "AutostartHandleTrueDriveEmulation", 0, RES_EVENT_NO, (resource_value_t)0,
      &AutostartHandleTrueDriveEmulation, set_autostart_handle_tde, NULL },
    { "AutostartWarp", 1, RES_EVENT_NO, (resource_value_t)0,
      &AutostartWarp, set_autostart_warp, NULL },
    { "AutostartPrgMode", AUTOSTART_PRG_MODE_DEFAULT, RES_EVENT_NO, (resource_value_t)0,
      &AutostartPrgMode, set_autostart_prg_mode, NULL },
    { "AutostartDelay", 0, RES_EVENT_NO, (resource_value_t)0,
      &AutostartDelay, set_autostart_delay, NULL },
    { "AutostartDelayRandom", 1, RES_EVENT_NO, (resource_value_t)0,
      &AutostartDelayRandom, set_autostart_delayrandom, NULL },
    { "AutostartDropMode",  AUTOSTART_DROP_MODE_RUN, RES_EVENT_NO, (resource_value_t)0,
      &AutostartDropMode, set_autostart_drop_mode, NULL },
    RESOURCE_INT_LIST_END
};

/*! \brief initialize the resources
 \return
   0 on success, else -1.

 \remark
   Registers the integer resources
*/
int autostart_resources_init(void)
{
    autostart_default_diskimage = archdep_default_autostart_disk_image_file_name();
    resources_string[0].factory_value = autostart_default_diskimage;

    if (machine_class == VICE_MACHINE_VIC20) {
        resources_int_basicload[0].factory_value = 1;
        resources_int_basicload[1].factory_value = 1;
    }

    if ((machine_class == VICE_MACHINE_CBM5x0) ||
        (machine_class == VICE_MACHINE_CBM6x0) ||
        (machine_class == VICE_MACHINE_PET)) {
        if (resources_register_int(resources_int_basicload_pet) < 0) {
            return -1;
        }
    } else {
        if (resources_register_int(resources_int_basicload) < 0) {
            return -1;
        }
    }

    if (resources_register_string(resources_string) < 0) {
        return -1;
    }
    return resources_register_int(resources_int);
}

void autostart_resources_shutdown(void)
{
    lib_free(AutostartPrgDiskImage);
    lib_free(autostart_default_diskimage);
}

/* ------------------------------------------------------------------------- */
int autostart_set_initial_tap_offset(unsigned long offset)
{
    tap_initial_raw_offset = offset;
    return 0;
}

static int cmdline_set_tap_offset(const char *arg, void *param)
{
    long val = strtol(arg, NULL, 0);
    if (val < 0) {
        tap_initial_raw_offset = 0;
        return -1;
    }
    tap_initial_raw_offset = val;
    return 0;
}

static const cmdline_option_t cmdline_options_basicload[] =
{
    { "-basicload", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartBasicLoad", (resource_value_t)1,
      NULL, "On autostart from disk, load to BASIC start (without ',1')" },
    { "+basicload", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartBasicLoad", (resource_value_t)0,
      NULL, "On autostart from disk, load with ',1'" },
    { "-tapebasicload", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartTapeBasicLoad", (resource_value_t)1,
      NULL, "On autostart from tape, load to BASIC start (without ',1')" },
    { "+tapebasicload", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartTapeBasicLoad", (resource_value_t)0,
      NULL, "On autostart from tape, load with ',1'" },
    CMDLINE_LIST_END
};

static const cmdline_option_t cmdline_options_basicload_pet[] =
{
    { "-basicload", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartBasicLoad", (resource_value_t)1,
      NULL, "On autostart from disk, load to BASIC start (without ',1')" },
    { "+basicload", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartBasicLoad", (resource_value_t)0,
      NULL, "On autostart from disk, load with ',1'" },
    CMDLINE_LIST_END
};

static const cmdline_option_t cmdline_options[] =
{
    { "-autostartwithcolon", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartRunWithColon", (resource_value_t)1,
      NULL, "On autostart, use the 'RUN' command with a colon, i.e., 'RUN:'" },
    { "+autostartwithcolon", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartRunWithColon", (resource_value_t)0,
      NULL, "On autostart, do not use the 'RUN' command with a colon; i.e., 'RUN'" },
    { "-autostart-handle-tde", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartHandleTrueDriveEmulation", (resource_value_t)1,
      NULL, "Handle True Drive Emulation on autostart" },
    { "+autostart-handle-tde", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartHandleTrueDriveEmulation", (resource_value_t)0,
      NULL, "Do not handle True Drive Emulation on autostart" },
    { "-autostart-warp", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartWarp", (resource_value_t)1,
      NULL, "Enable warp mode during autostart" },
    { "+autostart-warp", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartWarp", (resource_value_t)0,
      NULL, "Disable warp mode during autostart" },
    { "-autostartprgmode", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "AutostartPrgMode", NULL,
      "<Mode>", "Set autostart mode for PRG files (0: VirtualFS, 1: Inject, 2: Disk image)" },
    { "-autostartprgdiskimage", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "AutostartPrgDiskImage", NULL,
      "<Name>", "Set disk image for autostart of PRG files" },
    { "-autostart-delay", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "AutostartDelay", NULL,
      "<seconds>", "Set initial autostart delay (0: use default)" },
    { "-autostart-delay-random", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartDelayRandom", (resource_value_t)1,
      NULL, "Enable random initial autostart delay." },
    { "+autostart-delay-random", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AutostartDelayRandom", (resource_value_t)0,
      NULL, "Disable random initial autostart delay." },
    { "-autostarttapoffset", CALL_FUNCTION, CMDLINE_ATTRIB_NEED_ARGS,
      &cmdline_set_tap_offset, NULL, NULL, NULL,
      "<value>", "Set initial offset in .tap file" },
    { "-autostart-drop-mode", CALL_FUNCTION, CMDLINE_ATTRIB_NEED_ARGS,
      &cmdline_set_autostart_drop_mode, NULL, NULL, NULL, "<Mode>",
      "Set autostart drop mode (0/attach: attach only, 1/load: attach and load, "
      "2/run: attach, load and run)" },
    CMDLINE_LIST_END
};

/*! \brief initialize the command-line options

 \return
   0 on success, else -1.

 \remark
   Registers the command-line options
*/
int autostart_cmdline_options_init(void)
{
    if ((machine_class == VICE_MACHINE_CBM5x0) ||
        (machine_class == VICE_MACHINE_CBM6x0) ||
        (machine_class == VICE_MACHINE_PET)) {
        if (cmdline_register_options(cmdline_options_basicload_pet) < 0) {
            return -1;
        }
    } else {
        if (cmdline_register_options(cmdline_options_basicload) < 0) {
            return -1;
        }
    }

    return cmdline_register_options(cmdline_options);
}

/* ------------------------------------------------------------------------- */

/* Deallocate program name if we have one */
static void deallocate_program_name(void)
{
    lib_free(autostart_program_name);
    autostart_program_name = NULL;
}

typedef enum { YES, NO, NOT_YET } CHECKYESNO;

static CHECKYESNO check2(const char *s, unsigned int blink_mode, int lineoffset, int checkcursor)
{
    uint16_t screen_addr, addr;
    uint8_t line_length, cursor_column;
    int i, blinking;

    mem_get_cursor_parameter(&screen_addr, &cursor_column, &line_length, &blinking);

    if (!kbdbuf_is_empty() || !kbdbuf_queue_is_empty()) {
        DBGWAIT(("check2(%s) [kbd buffer not empty] screen addr:%04x column:%d, linelen:%d lineoffset: %d blinking:%d (check:%s)",
            s, screen_addr, cursor_column, line_length, lineoffset, blinking, (blink_mode == AUTOSTART_WAIT_BLINK) ? "yes" : "no"));
        return NOT_YET;
    }

    /* wait until cursor is in the first column */
    if (checkcursor == AUTOSTART_CHECK_FIRST_COLUMN) {
        if (cursor_column != 0) {
            DBGWAIT(("check2(%s) [cursor not in 1st column] screen addr:%04x column:%d, linelen:%d lineoffset: %d blinking:%d (check:%s)",
                s, screen_addr, cursor_column, line_length, lineoffset, blinking, (blink_mode == AUTOSTART_WAIT_BLINK) ? "yes" : "no"));
            return NOT_YET;
        }
    }

    if (blink_mode == AUTOSTART_WAIT_BLINK) {
        /* if blink state can be checked, wait until the cursor is in "on" state */
        if ((blinking != -1) && (blinking == 0)) {
            DBGWAIT(("check2(%s) [cursor not in ON state] screen addr:%04x column:%d, linelen:%d lineoffset: %d blinking:%d (check:%s)",
                s, screen_addr, cursor_column, line_length, lineoffset, blinking, (blink_mode == AUTOSTART_WAIT_BLINK) ? "yes" : "no"));
            return NOT_YET;
        }
        /* now we expect the string in the previous line (typically "READY.") */
        addr = screen_addr - line_length;
    } else {
        addr = screen_addr;
    }

    addr += line_length * lineoffset;

    DBGWAIT(("check2(%s) effective addr:%04x screen addr:%04x column:%d, linelen:%d lineoffset: %d blinking:%d (check:%s)",
        s, addr, screen_addr, cursor_column, line_length, lineoffset, blinking, (blink_mode == AUTOSTART_WAIT_BLINK) ? "yes" : "no"));

    for (i = 0; s[i] != '\0'; i++) {
        int checkbyte = mem_read_screen((uint16_t)(addr + i) & 0xffff);
        DBGWAIT(("checkbyte: %04x:%02x '%c' (expected:%02x '%c')",
                    (unsigned int)(addr + i),
                    (unsigned int)checkbyte, (int)(checkbyte % 0x3f) + 64,
                    (unsigned int)(s[i] % 64), (int)(s[i] % 64) + 64));
        if (checkbyte != s[i] % 64) {
            if (checkbyte != 0x20
                && checkbyte != 0xC
                && checkbyte != 0x13
                ) {
                DBGWAIT(("check2: return NO"));
                return NO;
            }
            DBGWAIT(("check2: return NOT_YET"));
            return NOT_YET;
        }
    }
    DBGWAIT(("check2: return YES"));
    return YES;
}

static CHECKYESNO check(const char *s, unsigned int blink_mode)
{
    return check2(s, blink_mode, 0, AUTOSTART_CHECK_FIRST_COLUMN);
}

/* ------------------------------------------------------------------------- */

static void set_true_drive_emulation_mode(int on, int unit)
{
    log_message(autostart_log, "Turning TDE %s for unit %d.", on ? "on" : "off", unit);
    resources_set_int_sprintf("Drive%dTrueEmulation", on, unit);
}

static int get_true_drive_emulation_state(int unit)
{
    int value;

    if (resources_get_int_sprintf("Drive%dTrueEmulation", &value, unit) < 0) {
        return 0;
    }

    return value;
}

static void set_iec_device_state(int on, int unit)
{
    if ((machine_class != VICE_MACHINE_VIC20) &&
        (machine_class != VICE_MACHINE_PET) &&
        (machine_class != VICE_MACHINE_CBM5x0) &&
        (machine_class != VICE_MACHINE_CBM6x0)) {
        log_message(autostart_log, "Turning IECDevice %s for unit %d.", on ? "on" : "off", unit);
        resources_set_int_sprintf("IECDevice%d", on, unit);
    }
}

static int get_iec_device_state(int unit)
{
    int value = 0;

    if ((machine_class != VICE_MACHINE_VIC20) &&
        (machine_class != VICE_MACHINE_PET) &&
        (machine_class != VICE_MACHINE_CBM5x0) &&
        (machine_class != VICE_MACHINE_CBM6x0)) {
        if (resources_get_int_sprintf("IECDevice%d", &value, unit) < 0) {
            return 0;
        }
    }
    return value;
}

static void set_warp_mode(int on)
{
    log_message(autostart_log, "Turning Warp mode %s.", on ? "on" : "off");
    vsync_set_warp_mode(on);
}

static int get_device_traps_state(int unit)
{
    int value;

    if (resources_get_int_sprintf("VirtualDevice%d", &value, unit) < 0) {
        return 0;
    }

    return value;
}

static void set_device_traps_state(int unit, int on)
{
    log_message(autostart_log, "Turning virtual device traps %s.", on ? "on" : "off");
    resources_set_int_sprintf("VirtualDevice%d", on, unit);
}

static void enable_warp_if_requested(void)
{
    /* enable warp mode? */
    if (AutostartWarp) {
        orig_warp_mode = vsync_get_warp_mode();
        if (!orig_warp_mode) {
            set_warp_mode(1);
        }
    }
}

static void disable_warp_if_was_requested(void)
{
    /* disable warp mode */
    if (AutostartWarp) {
        if (!orig_warp_mode) {
            set_warp_mode(0);
        }
    }
}

/* ------------------------------------------------------------------------- */

/* returns 0 if we left ROM area and should disable autostart */
static int check_rom_area(void)
{
    static int lastmode = -1;

    /* enter ROM ? */
    if (!entered_rom) {
        if (reg_pc >= 0xe000) {
            log_message(autostart_log, "Entered ROM at $%04x", reg_pc);
            entered_rom = 1;
        }
        lastmode = autostartmode;
    } else {
        /* special case for auto-starters: ROM left. We also consider
         * BASIC area to be ROM, because it's responsible for writing "READY."
         */
        if (lastmode != autostartmode) {
            lastmode = autostartmode;
        }
        if (machine_addr_in_ram(reg_pc)) {
            log_message(autostart_log, "Left ROM for $%04x", reg_pc);
            log_message(autostart_log, "aborting.");
            lastmode = -1;
            disable_warp_if_was_requested();
            autostart_done(); /* -> AUTOSTART_DONE */
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */

/* remember the state of some settings before we do autostart:
 * tde enabled/disabled, device traps enabled/disabled, warpmode enabled/disabled
 *
 * this should get called once, at the beginning of the autostart
 */
/* FIXME: perhaps we should not call this when autostarting from tape */
static void init_drive_emulation_state(int unit, int drive)
{
    DBG(("init_drive_emulation_state(unit: %d drive: %d) tde:%d iecdevice:%d traps:%d warp:%d",
        unit, drive, get_true_drive_emulation_state(unit), get_iec_device_state(unit),
        get_device_traps_state(unit), vsync_get_warp_mode()
    ));
    if (orig_drive_true_emulation_state == -1) {
        orig_drive_true_emulation_state = get_true_drive_emulation_state(unit);
    }
    if (orig_device_traps_state == -1) {
        orig_device_traps_state = get_device_traps_state(unit);
    }
    if (orig_iec_device_state == -1) {
        orig_iec_device_state = get_iec_device_state(unit);
    }
    if (orig_warp_mode == -1) {
        orig_warp_mode = vsync_get_warp_mode();
    }
    if (orig_FileSystemDevice8 == -1) {
        resources_get_int_sprintf("FileSystemDevice%d", &orig_FileSystemDevice8, unit);
    }
    if (orig_FSDevice8ConvertP00 == -1) {
        resources_get_int_sprintf("FSDevice%dConvertP00", &orig_FSDevice8ConvertP00, unit);
    }
    if (orig_FSDeviceLongNames == -1) {
        resources_get_int("FSDeviceLongNames", &orig_FSDeviceLongNames);
    }
}

/* restore the state of all settings we changed during autostart.
 *
 * this should get called on any "exit" of the autostart, error etc
 */
/* FIXME: perhaps we should not call this when autostarting from tape */
static void restore_drive_emulation_state(int unit, int drive)
{
    DBG(("restore_drive_emulation_state(unit: %d drive: %d)", unit, drive));
    if (orig_device_traps_state != -1) {
        /* set device traps to original state */
        if (get_device_traps_state(unit) != orig_device_traps_state) {
            set_device_traps_state(unit, orig_device_traps_state);
        }
    }
    if (orig_iec_device_state != -1) {
        /* set iec device to original state */
        if (get_iec_device_state(unit) != orig_iec_device_state) {
            set_iec_device_state(orig_iec_device_state, unit);
        }
    }
    if (orig_drive_true_emulation_state != -1) {
        /* set TDE to original state */
        if (get_true_drive_emulation_state(unit) != orig_drive_true_emulation_state) {
            set_true_drive_emulation_mode(orig_drive_true_emulation_state, unit);
        }
    }
    if (orig_warp_mode != -1) {
        /* set warp to original state */
#ifdef __LIBRETRO__
        /* Not if holding warp manually */
        if (vsync_get_warp_mode() != orig_warp_mode && !retro_warpmode) {
#else
        if (vsync_get_warp_mode() != orig_warp_mode) {
#endif
            set_warp_mode(orig_warp_mode);
        }
    }
    if (orig_FileSystemDevice8 != -1) {
        log_message(autostart_log, "Restoring FileSystemDevice%d to %d.", unit, orig_FileSystemDevice8);
        resources_set_int_sprintf("FileSystemDevice%d", orig_FileSystemDevice8, unit);
    }
    if (orig_FSDevice8ConvertP00 != -1) {
        log_message(autostart_log, "Restoring FSDevice%dConvertP00 to %d.", unit, orig_FSDevice8ConvertP00);
        resources_set_int_sprintf("FSDevice%dConvertP00", orig_FSDevice8ConvertP00, unit);
    }
    if (orig_FSDeviceLongNames != -1) {
        log_message(autostart_log, "Restoring FSDeviceLongNames to %d.", orig_FSDeviceLongNames);
        resources_set_int("FSDeviceLongNames", orig_FSDeviceLongNames);
    }

    /* make sure we refresh these next time we do autostart via gui */
    orig_drive_true_emulation_state = - 1;
    orig_device_traps_state = - 1;
    orig_iec_device_state = - 1;
    orig_warp_mode = -1;
    orig_FileSystemDevice8 = -1;
    orig_FSDevice8ConvertP00 = -1;
    orig_FSDeviceLongNames = -1;

    autostart_disk_unit = DRIVE_UNIT_MIN;
    autostart_disk_drive = 0;

    autostart_tape_unit = 1;

    autostart_type = -1;

    DBG(("restore_drive_emulation_state(unit: %d drive: %d) tde:%d iecdevice:%d traps:%d warp:%d", unit, drive,
        get_true_drive_emulation_state(unit), get_iec_device_state(unit), get_device_traps_state(unit), vsync_get_warp_mode()
    ));

}

/* ------------------------------------------------------------------------- */

static void load_snapshot_trap(uint16_t unused_addr, void *unused_data)
{
    if (autostart_program_name
        && machine_read_snapshot((char *)autostart_program_name, 0) < 0) {
        snapshot_display_error();
    }

    /* Make sure breakpoints are still working after loading the snapshot */
    mon_update_all_checkpoint_state();

    /* Enter monitor after done */
    if (trigger_monitor) {
        trigger_monitor = 0;
        monitor_startup_trap();
        log_message(autostart_log, "Returning to Monitor.");
    }
}

/* ------------------------------------------------------------------------- */

/* Reset autostart.  */
/* FIXME: cbm2 and pet pass 0,0 into this function before loading
            kernal ... why is this?

    default_seconds : initial delay before checking for READY
    handle_tde : if zero, "handle tde at autostart" will never be done
*/
static void autostart_reinit(int default_seconds, int handle_tde)
{
    DBG(("autostart_reinit default_seconds: %d\n", default_seconds));

    handle_drive_true_emulation_by_machine = handle_tde;

    set_handle_true_drive_emulation_state();

    if (default_seconds) {
        AutostartDelayDefaultSeconds = default_seconds; /* remember for later */
    }

    /* FIXME: pet and cbm2 need this for some reason, see comment above */
    if (default_seconds) {
        autostart_enabled = 1;
    } else {
        autostart_enabled = 0;
    }
}

/* Initialize autostart.  */
/* FIXME: cbm2 and pet pass 0,0 into this function before loading
            kernal ... why is this?

    default_seconds : initial delay before checking for READY
    handle_tde : if zero, "handle tde at autostart" will never be done
*/
int autostart_init(int default_seconds, int handle_drive_true_emulation)
{
    autostart_prg_init();

    autostart_reinit(default_seconds, handle_drive_true_emulation);

    if (autostart_log == LOG_DEFAULT) {
        autostart_log = log_open("AUTOSTART");
        if (autostart_log == LOG_DEFAULT) {
            return -1;
        }
    }

    return 0;
}

void autostart_disable(void)
{
    if (!autostart_enabled) {
        return;
    }

    DBG(("autostart_disable (ERROR)"));

    autostartmode = AUTOSTART_ERROR;
    trigger_monitor = 0;
    deallocate_program_name();
    log_error(autostart_log, "Turned off.");
}

/* Control if the monitor will be triggered after an autostart */
void autostart_trigger_monitor(int enable)
{
    trigger_monitor = enable;
}

/* this is called after successful loading */
static void autostart_finish(void)
{
    DBG(("autostart_finish"));

    if (autostart_run_mode == AUTOSTART_MODE_RUN) {
        log_message(autostart_log, "Starting program.");
        /* log_message(autostart_log, "Run command is: '%s' (%s)", AutostartRunCommand, AutostartDelayRandom ? "delayed" : "no delay"); */
        if (AutostartDelayRandom) {
            kbdbuf_feed_runcmd(AutostartRunCommand);
        } else {
            kbdbuf_feed(AutostartRunCommand);
        }
    } else {
        log_message(autostart_log, "Program loaded.");
    }
    /* printf("autostart_finish cmdline_get_autostart_mode(): %d\n", cmdline_get_autostart_mode()); */
    /* inject string given to -keybuf option on commandline into keyboard buffer */
    if (cmdline_get_autostart_mode() != AUTOSTART_MODE_NONE) {
        kbdbuf_feed_cmdline();
    }
}

/* This is called if all steps of an autostart operation were passed successfully */
static void autostart_done(void)
{
    DBG(("autostart_done"));

    restore_drive_emulation_state(autostart_disk_unit, autostart_disk_drive);

    autostartmode = AUTOSTART_DONE;

    log_message(autostart_log, "Done.");
}

/* ------------------------------------------------------------------------- */

/* This function is called by the `serialreceivebyte()' trap as soon as EOF
   is reached.  */
static void disk_eof_callback(void)
{
    DBG(("disk_eof_callback(%d:%d)", autostart_disk_unit, autostart_disk_drive));

    if (handle_drive_true_emulation_overridden) {
        uint8_t id[2], *buffer = NULL;
        unsigned int track, sector;
        /* FIXME: shouldnt this loop over all drives? */
        /* FIXME: what exactly is this stuff supposed to do? */
        if (orig_drive_true_emulation_state) {
            /* log_message(autostart_log, "Turning true drive emulation on."); */
            if (vdrive_bam_get_disk_id(autostart_disk_unit, autostart_disk_drive, id) == 0) {
                vdrive_get_last_read(&track, &sector, &buffer);
            }
        }
        /* set_true_drive_emulation_mode(orig_drive_true_emulation_state, unit); */
        if (orig_drive_true_emulation_state) {
            if (buffer) {
                log_message(autostart_log, "Restoring true drive state of drive %d:%d.",
                            autostart_disk_unit, autostart_disk_drive);
                drive_set_disk_memory(id, track, sector,
                                    diskunit_context[autostart_disk_unit - DRIVE_UNIT_MIN]);
                drive_set_last_read(track, sector, buffer,
                                    diskunit_context[autostart_disk_unit - DRIVE_UNIT_MIN]);
            } else {
                log_message(autostart_log, "No Disk Image in drive %d:%d.",
                            autostart_disk_unit, autostart_disk_drive);
            }
        }
    }

    if (autostartmode != AUTOSTART_NONE) {
        autostart_finish();
    }

    autostart_done(); /* -> AUTOSTART_DONE */

    machine_bus_eof_callback_set(NULL);

    disable_warp_if_was_requested();
}

/* This function is called by the `serialattention()' trap before
   returning.  */
static void disk_attention_callback(void)
{
    machine_bus_attention_callback_set(NULL);

    /* Next step is waiting for end of loading, to turn true drive emulation
       on.  */
    machine_bus_eof_callback_set(disk_eof_callback);
}

/* ------------------------------------------------------------------------- */

static void advance_hastape(void)
{
    char *tmp;
    DBG(("advance_hastape"));

    switch (check("READY.", AUTOSTART_WAIT_BLINK)) {
        case YES:
            /* NOTE: when loading from tape, we can not easily force to load
                     absolute or to BASIC start, because:
                     a) when the tape header type is = 1, then the kernal will
                        load to the basic start, except when secondary address 1
                        is used.
                     b) when the header type is = 3, then the kernal will always
                        load absolute (even if secondary address is 0)
                     c) PET and CBM2 machines will always load absolute

                     So the best thing we can do here is to always load without
                     secondary address (which will then respect what the header
                     type says), and to force header type = 1 (in tape.c) when
                     loading to basic start was requested.
            */
            log_message(autostart_log, "Loading file.");
            if (autostart_tape_unit == 2) {
                if (autostart_program_name) {
                    tmp = util_concat("LOAD\"", autostart_program_name, "\",2\r", NULL);
                } else {
                    tmp = lib_strdup("LOAD\"\",2\r");
                }
            } else {
#ifdef __LIBRETRO__
                if (autostart_program_name) {
                    tmp = util_concat("LOAD\"", autostart_program_name, "\"",
                                    autostart_tape_basic_load ? "" : ",1,1", "\r", NULL);
                } else {
                    if (autostart_tape_basic_load) {
                        tmp = lib_strdup("LOAD\r");
                    } else {
                        tmp = lib_strdup("LOAD\"\",1,1\r");
                    }
                }
#else
                if (autostart_program_name) {
                    tmp = util_concat("LOAD\"", autostart_program_name, "\"\r", NULL);
                } else {
                    tmp = lib_strdup("LOAD\r");
                }
#endif
            }
            kbdbuf_feed(tmp);
            lib_free(tmp);
            autostartmode = AUTOSTART_PRESSPLAYONTAPE;
            entered_rom = 0;
            deallocate_program_name();
            break;
        case NO:
            disable_warp_if_was_requested();
            autostart_disable();
            break;
        case NOT_YET:
            break;
    }
}

static void advance_pressplayontape(void)
{
    int port = (autostart_tape_unit == 2) ? TAPEPORT_PORT_2 : TAPEPORT_PORT_1;
    switch (check2("PRESS PLAY ON TAPE", AUTOSTART_NOWAIT_BLINK, 0, AUTOSTART_CHECK_ANY_COLUMN)) {
        case YES:
            autostartmode = AUTOSTART_LOADINGTAPE;
            datasette_control(port, DATASETTE_CONTROL_START);
            break;
        case NO:
            disable_warp_if_was_requested();
            autostart_disable();
            break;
        case NOT_YET:
            break;
    }
}

static void advance_loadingtape(void)
{
    switch (check("READY.", AUTOSTART_WAIT_BLINK)) {
        case YES:
            disable_warp_if_was_requested();
            autostart_finish();
            autostart_done(); /* -> AUTOSTART_DONE */
            break;
        case NO:
            disable_warp_if_was_requested();
            autostart_disable();
            break;
        case NOT_YET:
            /* leave autostart and disable warp if ROM area was left */
            check_rom_area();
#ifdef __LIBRETRO__
            if (!tape_counter)
                break;
            switch (check2("FOUND ", AUTOSTART_NOWAIT_BLINK, 0 , 0)) {
                case YES:
                    if (!tape_found_counter)
                       tape_found_counter = tape_counter;
                    break;
                default:
                    break;
            }
#endif
            break;
    }
}


static void setup_for_disk_ready(int unit, int drive);

static void advance_hasdisk(int unit, int drive)
{
    char *tmp, *temp_name;
    char drivestring[3] = {'0',':',0};

    /* DBG(("advance_hasdisk(unit: %d drive: %d)", unit, drive)); */

    switch (check("READY.", AUTOSTART_WAIT_BLINK)) {
        case YES:
            /* complete the drive setup */
            setup_for_disk_ready(unit, drive);

            /* autostart_program_name may be petscii or ascii at this point,
               ANDing the charcodes with 0x7f here is a cheap way to prevent
               illegal characters in the printed message */
            if (autostart_program_name) {
                temp_name = tmp = lib_strdup(autostart_program_name);
                while (*tmp) {
                    *tmp++ &= 0x7f;
                }
                log_message(autostart_log, "Loading program '%s'", temp_name);
                lib_free(temp_name);
            } else {
                log_message(autostart_log, "Loading program '*'");
            }

            DBG(("advance_hasdisk(%d) traps:%d tde:%d", unit,
                 get_device_traps_state(unit), get_true_drive_emulation_state(unit)));

            /* now either device traps or TDE is enabled, but not both */

            /* emit LOAD command */
            if (drive_is_dualdrive_by_devnr(unit)) {
                drivestring[0] = (drive == 1) ? '1' : '0';
            } else {
                drivestring[0] = 0;
            }
            tmp = lib_msprintf("LOAD\"%s%s\",%d%s\r",
                               drivestring,
                               autostart_program_name ?
                               autostart_program_name : "*",
                               unit,
                               autostart_basic_load ? "" : ",1");
            DBG(("advance_hasdisk(unit: %d drive: %d) LOAD\"%s%s\",%d%s",
                               unit, drive,
                               drivestring,
                               autostart_program_name ?
                               autostart_program_name : "*",
                               unit,
                               autostart_basic_load ? "" : ",1"));
            kbdbuf_feed(tmp);
            lib_free(tmp);

            /* FIXME: before, the code selected one of three different code
               pathes, depending on the state of traps and warpmode.
               it seems a bit strange that this is needed, and also that simple
               doing the same will work better too.
               i am leaving the following here for experimentation while
               completely debugging the autostart madness */

            /* switch to next state ("searching...") */
#if 1
            autostartmode = AUTOSTART_WAITSEARCHINGFOR;
#endif

#if 0
            /* be most compatible if warp is disabled */
            autostart_finish();
            autostart_done(); /* -> AUTOSTART_DONE */
#endif
#if 1
            /* autostartmode = AUTOSTART_LOADINGDISK; */
            /* if TDE is disabled during load, setup the callback that will
               copy the vdrive status into the TDE and complete the autostart */
            if (!get_true_drive_emulation_state(unit) && (autostart_type != AUTOSTART_PRG_VFS)) {
                machine_bus_attention_callback_set(disk_attention_callback);
            }
#endif

#if 0
            /* this is what the code did before the rework. but why? */
            if (!traps) {
                if (AutostartWarp) {
                    autostartmode = AUTOSTART_WAITSEARCHINGFOR;
                } else {
                    /* be most compatible if warp is disabled */
                    autostart_finish();
                    autostart_done(); /* -> AUTOSTART_DONE */
                }
            } else {
                 autostartmode = AUTOSTART_LOADINGDISK;
                 machine_bus_attention_callback_set(disk_attention_callback);
            }
#endif
            deallocate_program_name();
            break;
        case NO:
            orig_drive_true_emulation_state = get_true_drive_emulation_state(unit);
            orig_device_traps_state = get_device_traps_state(unit);
            orig_iec_device_state = get_iec_device_state(unit);
            disable_warp_if_was_requested();
            autostart_disable();
            break;
        case NOT_YET:
            /* leave autostart and disable warp if ROM area was left */
            check_rom_area();
            break;
    }
}

static void advance_hassnapshot(void)
{
    switch (check("READY.", AUTOSTART_WAIT_BLINK)) {
        case YES:
            autostart_done(); /* -> AUTOSTART_DONE */
            log_message(autostart_log, "Restoring snapshot.");
            interrupt_maincpu_trigger_trap(load_snapshot_trap, 0);
            break;
        case NO:
            autostart_disable();
            break;
        case NOT_YET:
            break;
    }
}

/* ----- stages for tde disk loading with warp --------------------------- */

static void advance_waitsearchingfor(void)
{
    DBGWAIT(("advance_waitsearchingfor"));
    switch (check2("SEARCHING FOR", AUTOSTART_NOWAIT_BLINK, 0, AUTOSTART_CHECK_ANY_COLUMN)) {
        case YES:
            log_message(autostart_log, "Searching for ...");
            autostartmode = AUTOSTART_WAITLOADING;
            break;
        case NO:
#if 0
            /* if we are still in the line with the LOAD command, still wait */
            if (check("LOAD\"", AUTOSTART_NOWAIT_BLINK) == YES) {
                /* leave autostart and disable warp if ROM area was left */
                check_rom_area();
                break;
            }
            /* check if we are already in the next line showing LOADING ? */
            if (check("LOADING", AUTOSTART_NOWAIT_BLINK) == YES) {
                log_message(autostart_log, "Searching for ... missed, got LOADING");
                /* proceed as if mode was AUTOSTART_WAITLOADING */
                entered_rom = 0;
                autostartmode = AUTOSTART_WAITLOADREADY;
                break;
            }
            /* if we are already way ahead and basically missed everything until
               READY, then "searching for" is 3 lines above */
            if (check2("READY", AUTOSTART_NOWAIT_BLINK, -1) == YES) {
                if (check2("LOADING", AUTOSTART_NOWAIT_BLINK, -2) == YES) {
                    if (check2("SEARCHING FOR", AUTOSTART_NOWAIT_BLINK, -3) == YES) {
                        log_message(autostart_log, "Searching for ... missed, got Ready");
                        disable_warp_if_was_requested();
                        autostart_finish();
                        autostart_done(); /* -> AUTOSTART_DONE */
                        break;
                    }
                }
            }
            /* HACK: the LOAD(ING) might not be fully printed yet, in that case wait some more */
            if (check("L", AUTOSTART_NOWAIT_BLINK) == YES) {
                /* leave autostart and disable warp if ROM area was left */
                check_rom_area();
                break;
            }
#endif
            log_message(autostart_log, "NO Searching for ...");
            disable_warp_if_was_requested();
            autostart_disable();
            break;
        case NOT_YET:
            /* leave autostart and disable warp if ROM area was left */
            check_rom_area();
            break;
    }
}

static void advance_waitloading(void)
{
    DBGWAIT(("advance_waitloading"));
    switch (check2("LOADING", AUTOSTART_NOWAIT_BLINK, 0, AUTOSTART_CHECK_ANY_COLUMN)) {
        case YES:
            log_message(autostart_log, "Loading");
            entered_rom = 0;
            autostartmode = AUTOSTART_WAITLOADREADY;
            break;
        case NO:
#if 0
            /* still showing SEARCHING FOR ? */
            if (check("SEARCHING FOR", AUTOSTART_NOWAIT_BLINK) == YES) {
                /* leave autostart and disable warp if ROM area was left */
                check_rom_area();
                return;
            }
            /* if we are already way ahead and basically missed everything until
               READY, then "LOADING" is 2 lines above */
            if (check2("READY", AUTOSTART_NOWAIT_BLINK, -1) == YES) {
                if (check2("LOADING", AUTOSTART_NOWAIT_BLINK, -2) == YES) {
                    log_message(autostart_log, "Loading missed, got Ready");
                    entered_rom = 0;
                    autostartmode = AUTOSTART_WAITLOADREADY;
                    break;
                }
            }
#endif
            /* no something else is shown -> error! */
            log_message(autostart_log, "NO Loading");
            disable_warp_if_was_requested();
            autostart_disable();
            break;
        case NOT_YET:
#if 0
            /* if we are already way ahead and basically missed everything until
               READY, then "LOADING" is 2 lines above */
            if (check2("READY", AUTOSTART_NOWAIT_BLINK, -1) == YES) {
                if (check2("LOADING", AUTOSTART_NOWAIT_BLINK, -2) == YES) {
                    log_message(autostart_log, "Loading missed, got Ready");
                    entered_rom = 0;
                    autostartmode = AUTOSTART_WAITLOADREADY;
                    break;
                }
            }
#endif
            /* leave autostart and disable warp if ROM area was left */
            check_rom_area();
            break;
    }
}

static void advance_waitloadready(void)
{
    DBGWAIT(("advance_waitloadready"));
    switch (check("READY.", AUTOSTART_WAIT_BLINK)) {
        case YES:
            log_message(autostart_log, "Ready");
            disable_warp_if_was_requested();
            autostart_finish();
            autostart_done(); /* -> AUTOSTART_DONE */
            break;
        case NO:
            log_message(autostart_log, "NO Ready");
            disable_warp_if_was_requested();
            autostart_disable();
            break;
        case NOT_YET:
            /* leave autostart and disable warp if ROM area was left */
            check_rom_area();
            break;
    }
}

/* After a reset a PRG file has to be injected into RAM */
static void advance_inject(void)
{
    if (autostart_prg_perform_injection(autostart_log) < 0) {
        disable_warp_if_was_requested();
        autostart_disable();
    } else {
        /* wait for ready cursor and type RUN */
        autostartmode = AUTOSTART_WAITLOADREADY;
    }
}

/* Execute the actions for the current `autostartmode', advancing to the next
   mode if necessary.  */
void autostart_advance(void)
{
    if (!autostart_enabled) {
        return;
    }

    if (maincpu_clk < autostart_initial_delay_cycles) {
        autostart_wait_for_reset = 0;
        return;
    }

    if (autostart_wait_for_reset) {
        return;
    }

    /* DBG(("autostart_advance (%d)", autostartmode)); */

    switch (autostartmode) {
        case AUTOSTART_HASTAPE: /* wait for "READY.", to AUTOSTART_PRESSPLAYONTAPE */
            advance_hastape();
            break;
        case AUTOSTART_PRESSPLAYONTAPE: /* wait for "PRESS PLAY ON TAPE", to AUTOSTART_LOADINGTAPE */
            advance_pressplayontape();
            break;
        case AUTOSTART_LOADINGTAPE: /* wait for "READY." */
            advance_loadingtape();
            break;

        case AUTOSTART_HASSNAPSHOT: /* wait for "READY." */
            advance_hassnapshot();
            break;

        case AUTOSTART_HASDISK: /* wait for "READY.", to  AUTOSTART_WAITSEARCHINGFOR or AUTOSTART_LOADINGDISK */
            advance_hasdisk(autostart_disk_unit, autostart_disk_drive);
            break;
        case AUTOSTART_WAITSEARCHINGFOR: /* wait for "SEARCHING FOR", to AUTOSTART_WAITLOADING */
            advance_waitsearchingfor();
            break;
        case AUTOSTART_WAITLOADING:/* wait for "LOADING", to AUTOSTART_WAITLOADREADY */
            advance_waitloading();
            break;
        case AUTOSTART_WAITLOADREADY: /* wait for "READY." */
            advance_waitloadready();
            break;

        case AUTOSTART_INJECT: /* to AUTOSTART_WAITLOADREADY */
            advance_inject();
            break;

        case AUTOSTART_ERROR:
#ifndef __LIBRETRO__
            log_message(autostart_log, "Error");
#endif
            restore_drive_emulation_state(autostart_disk_unit, autostart_disk_drive);
            autostartmode = AUTOSTART_DONE;
            break;

        default:
            return;
    }
}

/* Clean memory and reboot for autostart.  */
static void reboot_for_autostart(const char *program_name, unsigned int mode,
                                 unsigned int runmode)
{
    int rnd;
    char *temp_name = NULL, *temp;

    if (!autostart_enabled) {
        return;
    }

    DBG(("reboot_for_autostart %s mode: %u runmode: %u", program_name, mode, runmode));

    /* program_name may be petscii or ascii at this point, ANDing the charcodes
       with 0x7f here is a cheap way to prevent illegal characters in the
       printed message */
    if (program_name) {
        temp_name = temp = lib_strdup(program_name);
        while (*temp) {
            *temp++ &= 0x7f;
        }
    }
    log_message(autostart_log, "Resetting the machine to autostart '%s'",
                program_name ? temp_name : "*");
    if (program_name) {
        lib_free(temp_name);
    }

    /* mem_powerup(); */ /* power cycle takes care of this */

    autostart_ignore_reset = 1;
    deallocate_program_name();
    if (program_name && program_name[0]) {
        autostart_program_name = lib_strdup(program_name);
    }

    autostartmode = mode;
    autostart_run_mode = runmode;
    autostart_wait_for_reset = 1;

    autostart_initial_delay_cycles =
        (CLOCK)(((AutostartDelay == 0) ? AutostartDelayDefaultSeconds : AutostartDelay)
                        * machine_get_cycles_per_second());
    DBG(("reboot_for_autostart AutostartDelay: %d AutostartDelayDefaultSeconds: %d autostart_initial_delay_cycles: %"PRIu64"",
           AutostartDelay, AutostartDelayDefaultSeconds, autostart_initial_delay_cycles));

    resources_get_int("AutostartDelayRandom", &rnd);
    if (rnd) {
        /* additional random delay of up to 10 frames */
        autostart_initial_delay_cycles += lib_unsigned_rand(1, (int)machine_get_cycles_per_frame() * 10);
    }
    DBG(("reboot_for_autostart - autostart_initial_delay_cycles: %"PRIu64, autostart_initial_delay_cycles));

    machine_trigger_reset(MACHINE_RESET_MODE_POWER_CYCLE);

    /* enable warp before reset */
    if (mode != AUTOSTART_HASSNAPSHOT) {
        enable_warp_if_requested();
    }
}

/* ------------------------------------------------------------------------- */

/* Autostart snapshot file `file_name'.  */
int autostart_snapshot(const char *file_name, const char *program_name)
{
    uint8_t vmajor, vminor;
    snapshot_t *snap;

    if (network_connected() || event_record_active() || event_playback_active()
        || file_name == NULL || !autostart_enabled) {
        return -1;
    }

    deallocate_program_name();  /* not needed at all */

    if (!(snap = snapshot_open(file_name, &vmajor, &vminor, machine_get_name()))) {
        autostartmode = AUTOSTART_ERROR;
        return -1;
    }

    log_message(autostart_log, "Loading snapshot file `%s'.", file_name);
    snapshot_close(snap);

    /*autostart_program_name = lib_strdup(file_name);
    interrupt_maincpu_trigger_trap(load_snapshot_trap, 0);*/
    /* use for snapshot */
    reboot_for_autostart(file_name, AUTOSTART_HASSNAPSHOT, AUTOSTART_MODE_RUN);

    return 0;
}

/* Autostart tape image `file_name'.  */
int autostart_tape(const char *file_name, const char *program_name,
                   unsigned int program_number, unsigned int runmode,
                   unsigned int tapeport)
{
    uint8_t do_seek = 1;
    unsigned int tapeunit = (tapeport == TAPEPORT_PORT_2) ? 2 : 1;

    if (network_connected() || event_record_active() || event_playback_active()
        || !file_name || !autostart_enabled) {
        return -1;
    }

    /* make sure to init TDE and traps status before each autostart */
    /* FIXME: this should perhaps be handled differently for tape */
    init_drive_emulation_state(DRIVE_UNIT_MIN, 0);

    /* reset datasette emulation and remove the tape image. */
    datasette_control(tapeport, DATASETTE_CONTROL_RESET);
    tape_image_detach(tapeunit);

    if (!(tape_image_attach(tapeunit, file_name) < 0)) {
        log_message(autostart_log,
                    "Attached file `%s' as a tape image on unit #%u.", file_name, tapeunit);
        if (!tape_tap_attached(tapeport)) {
            if (program_number == 0 || program_number == 1) {
                do_seek = 0;
            }
            program_number -= 1;
        }
        if (tap_initial_raw_offset > 0) {
            tape_seek_to_offset(tape_image_dev[tapeport], tap_initial_raw_offset);
            tap_initial_raw_offset = 0;
        } else if (do_seek) {
            if (program_number > 0) {
                /* program numbers in tape_seek_to_file() start at 0 */
                tape_seek_to_file(tape_image_dev[tapeport], program_number - 1);
            } else {
                tape_seek_start(tape_image_dev[tapeport]);
            }
        }
        if (!tape_tap_attached(tapeport)) {
            /* Kludge: for t64 images we need devtraps ON */
            if (!get_device_traps_state(1)) {
                set_device_traps_state(1, 1);
            }
        }

        autostart_tape_unit = tapeunit;

        reboot_for_autostart(program_name, AUTOSTART_HASTAPE, runmode);

        return 0;
    }

    DBG(("autostart_tape (ERROR)"));
    autostartmode = AUTOSTART_ERROR;
    deallocate_program_name();

    /* restore_drive_emulation_state(DRIVE_UNIT_MIN); */
    return -1;
}

/* Cope with 0xa0 padded file names.  */
static void autostart_disk_cook_name(char **name)
{
    unsigned int pos;

    pos = 0;

    while ((*name)[pos] != '\0') {
        if (((unsigned char)((*name)[pos])) == 0xa0) {
            char *ptr;

            ptr = lib_malloc(pos + 1);
            memcpy(ptr, *name, pos);
            ptr[pos] = '\0';
            lib_free(*name);
            *name = ptr;
            break;
        }
        pos++;
    }
}

static void setup_for_disk(int unit, int drive)
{
    if (handle_drive_true_emulation_overridden) {
        DBG(("setup_for_disk - handle TDE"));
#if 0
        /* disable TDE if device traps are enabled,
           enable TDE if device traps are disabled */
        if (orig_device_traps_state) {
            if (orig_drive_true_emulation_state) {
                set_true_drive_emulation_mode(0, unit);
            }
        } else {
            if (!orig_drive_true_emulation_state) {
                set_true_drive_emulation_mode(1, unit);
            }
            if (!get_true_drive_emulation_state(unit)) {
                log_error(LOG_DEFAULT, "True drive emulation is not enabled.");
                set_device_traps_state(unit, 1);
                if (!get_device_traps_state(unit)) {
                    log_error(LOG_DEFAULT, "Virtual device traps are not enabled.");
                }
            }
        }
#endif
    } else {
        DBG(("setup_for_disk - do not handle TDE"));
        if (orig_drive_true_emulation_state) {
            /* disable traps when TDE is enabled, */
            if (orig_device_traps_state) {
                set_device_traps_state(unit, 0);
            }
        } else {
            /* enable traps when TDE is disabled. */
            if (!orig_device_traps_state) {
                set_device_traps_state(unit, 1);
            }
            if (!get_device_traps_state(unit)) {
                log_error(LOG_DEFAULT, "Virtual device traps are not enabled.");
            }
        }
    }
    DBG(("setup_for_disk: unit: %d drive: %d TDE: %s IECDevice: %s Traps: %s handle TDE: %s",
        unit, drive,
        get_true_drive_emulation_state(unit) ? "on" : "off",
        get_iec_device_state(unit) ? "on" : "off",
        get_device_traps_state(unit) ? "on" : "off",
        handle_drive_true_emulation_overridden ? "yes" : "no"
        ));
    autostart_disk_unit = unit;
    autostart_disk_drive = drive;
}

/* once RESET completed and we are at READY, complete the setup. The drive
   has hopefully completed its reset by now.
*/
static void setup_for_disk_ready(int unit, int drive)
{
    if (handle_drive_true_emulation_overridden) {
        DBG(("setup_for_disk_ready - handle TDE"));
        if (orig_device_traps_state || orig_iec_device_state) {
#if 0
            if (orig_drive_true_emulation_state) {
                /* if traps are enabled, and TDE was on before autostart, disable it now */
                set_true_drive_emulation_mode(0, unit);
            }
#endif
            /* disable TDE if device traps or iecdevice are enabled */
            set_true_drive_emulation_mode(0, unit);
            /* if both traps and iec device is enabled, disable traps */
            if (orig_device_traps_state && orig_iec_device_state) {
                set_device_traps_state(0, unit);
            }
        } else {
            /* enable TDE if device traps and iecdevice are disabled */
            if (!orig_drive_true_emulation_state) {
                set_true_drive_emulation_mode(1, unit);
            }
            if (!get_true_drive_emulation_state(unit)) {
                log_error(LOG_DEFAULT, "True drive emulation is not enabled.");
                set_device_traps_state(unit, 1);
                if (!get_device_traps_state(unit)) {
                    log_error(LOG_DEFAULT, "Virtual device traps are not enabled.");
                }
            }
        }
    }
    DBG(("setup_for_disk_ready: unit: %d drive: %d TDE: %s IECDevice: %s Traps: %s handle TDE: %s",
        unit, drive,
        get_true_drive_emulation_state(unit) ? "on" : "off",
        get_iec_device_state(unit) ? "on" : "off",
        get_device_traps_state(unit) ? "on" : "off",
        handle_drive_true_emulation_overridden ? "yes" : "no"
        ));
}

/* Autostart disk image `file_name'.  */
int autostart_disk(int unit, int drive, const char *file_name, const char *program_name,
                   unsigned int program_number, unsigned int runmode)
{
    char *name = NULL;

    DBG(("autostart_disk(unit: %d drive: %d)", unit, drive));

    if (network_connected() || event_record_active() || event_playback_active()
        || !file_name || !autostart_enabled) {
        return -1;
    }

    /* make sure to init TDE and traps status before each autostart */
    init_drive_emulation_state(unit, drive);

    /* Get program name first to avoid more than one file handle open on
       image.  */
    if (!program_name && program_number > 0) {
        image_contents_t *contents = diskcontents_filesystem_read(file_name);
        if (contents) {
            name = image_contents_filename_by_number(contents, program_number);
            image_contents_destroy(contents);
        }
    } else {
        name = lib_strdup(program_name ? program_name : "*");
    }

    if (name) {
        autostart_disk_cook_name(&name);
#ifdef __LIBRETRO__
        file_system_detach_disk(unit, drive);
#endif
        if (!(file_system_attach_disk(unit, drive, file_name) < 0)) {
#if 1
            struct disk_image_s *diskimg;
#endif

            log_message(autostart_log,
                        "Attached file `%s' as a disk image.", file_name);
#if 1
            /*
             * Simple attempt at implementing setting the current drive type
             * based on the image type as per feature request #319.
             */

            /* shitty code, we really need to extend the drive API to
             * get at these sorts for things without breaking into core code
             */
            diskimg = file_system_get_image(unit, drive);

            if (diskimg == NULL) {
                log_error(LOG_DEFAULT, "Failed to get disk image for unit %d.", unit);
            } else {
                int chk = drive_check_image_format(diskimg->type, 0);
                log_message(autostart_log, "mounted image is type: %u, %schanging drive.",
                            diskimg->type, (chk < 0) ? "" : "not ");
                /* change drive type only when image does not work in current drive */
                if (chk < 0) {
                    if (resources_set_int_sprintf("Drive%dType", drive_image_type_to_drive_type(diskimg->type), unit) < 0) {
                        log_error(LOG_DEFAULT, "Failed to set drive type.");
                    }
                }

                /* detach disk before reattaching */
                file_system_detach_disk(unit, drive);

                if (file_system_attach_disk(unit, drive, file_name) < 0) {
                    goto exiterror;
                }
                /* if TDE was enabled before autostarting but is disabled now, enable it again */
                if (orig_drive_true_emulation_state && !get_true_drive_emulation_state(unit)) {
                    log_message(autostart_log, "Turning TDE on to allow drive reset");
                    set_true_drive_emulation_mode(1, unit);
                }
                /* if TDE is now enabled, trigger a drive reset */
                if (get_true_drive_emulation_state(unit)) {
                    log_message(autostart_log, "Resetting drive %d", unit);
                    drive_cpu_trigger_reset(unit - DRIVE_UNIT_MIN);
                }
            }
#endif
            autostart_type = AUTOSTART_DISK_IMAGE;
            setup_for_disk(unit, drive);
            reboot_for_autostart(name, AUTOSTART_HASDISK, runmode);
            lib_free(name);

            return 0;
        }
    }
exiterror:
    DBG(("autostart_disk: ERROR"));
    autostartmode = AUTOSTART_ERROR;
    deallocate_program_name();
    lib_free(name);

    /* restore_drive_emulation_state(DRIVE_UNIT_MIN); */
    return -1;
}

static void setup_for_prg_vfs(int unit)
{
#if 1
    if (handle_drive_true_emulation_overridden) {
        if (orig_drive_true_emulation_state) {
            set_true_drive_emulation_mode(0, unit);
        }
    }
    if (get_true_drive_emulation_state(unit)) {
        log_error(LOG_DEFAULT, "True drive emulation is still enabled.");
    }
#endif
    if (!orig_device_traps_state) {
        set_device_traps_state(unit, 1);
    }
    if (!get_device_traps_state(unit)) {
        log_error(LOG_DEFAULT, "Virtual device traps are not enabled.");
    }
    /* always shorten the long names when autostarting, the long names cause
       nothing but problems */
    resources_set_int("FSDeviceLongNames", 0);

    DBG(("setup for prg VFS: TDE: %s  Traps: %s handle TDE: %s",
        get_true_drive_emulation_state(unit) ? "on" : "off",
        get_device_traps_state(unit) ? "on" : "off",
        handle_drive_true_emulation_overridden ? "yes" : "no"
        ));
}

#if 0
static void setup_for_prg_vfs_ready(void)
{
    if (handle_drive_true_emulation_overridden) {
        if (orig_drive_true_emulation_state) {
            set_true_drive_emulation_mode(0, unit);
        }
    }
    if (get_true_drive_emulation_state(unit)) {
        log_error(LOG_DEFAULT, "True drive emulation is still enabled.");
    }
}
#endif

/* Autostart PRG file `file_name'.  The PRG file can either be a raw CBM file
   or a P00 file */
/* FIXME: if we want to be able to autostart prg files from different devices
          than device nr 8, either pass the device nr here, or use some
          resource for this */
int autostart_prg(const char *file_name, unsigned int runmode)
{
    fileio_info_t *finfo;
    vdrive_t *vdrive;
    int result;
    const char *boot_file_name;
    static char tempname[32];
    int mode;

    const int unit = DRIVE_UNIT_MIN, drive = 0;

    DBG(("autostart_prg (unit: %d drive: %d file_name:%s)", unit, drive, file_name));

    if (network_connected() || event_record_active() || event_playback_active()) {
        return -1;
    }

    /* open prg file */
    finfo = fileio_open(file_name, NULL, FILEIO_FORMAT_RAW | FILEIO_FORMAT_P00,
                        FILEIO_COMMAND_READ | FILEIO_COMMAND_FSNAME,
                        FILEIO_TYPE_PRG, NULL);

    /* can't open file */
    if (finfo == NULL) {
        log_error(autostart_log, "Cannot open `%s'.", file_name);
        return -1;
    }

    /* make sure to init TDE and traps status before each autostart */
    init_drive_emulation_state(unit, drive);

    /* determine how to load file */
    switch (AutostartPrgMode) {
        case AUTOSTART_PRG_MODE_VFS:
            log_message(autostart_log, "Loading PRG file `%s' with virtual FS on unit #%d:%d.",
                        file_name, unit, drive);
            setup_for_prg_vfs(unit);
            result = autostart_prg_with_virtual_fs(unit, drive, file_name, finfo, autostart_log);
            mode = AUTOSTART_HASDISK;
            boot_file_name = (const char *)finfo->name;
            /* shorten the filename to 16 chars (if enabled) */
            vdrive = file_system_get_vdrive(unit);
            if (vdrive == NULL) {
                log_error(LOG_DEFAULT, "Failed to get vdrive reference for unit #%d:%d.", unit, drive);
                return -1;
            }
            fsdevice_limit_namelength(vdrive, (uint8_t*)boot_file_name);
            autostart_type = AUTOSTART_PRG_VFS;
            break;
        case AUTOSTART_PRG_MODE_INJECT:
            log_message(autostart_log, "Loading PRG file `%s' with direct RAM injection.", file_name);
            result = autostart_prg_with_ram_injection(file_name, finfo, autostart_log);
            mode = AUTOSTART_INJECT;
            boot_file_name = NULL;
            autostart_type = AUTOSTART_PRG_INJECT;
            break;
        case AUTOSTART_PRG_MODE_DISK:
            {
            char *savedir; int n;
            log_message(autostart_log, "Loading PRG file `%s' with autostart disk image.", file_name);
            setup_for_disk(unit, drive);
            /* create the directory where the image should be written first */
            util_fname_split(AutostartPrgDiskImage, &savedir, NULL);
            if ((savedir != NULL) && (*savedir != 0) && (strcmp(savedir, "."))) {
                archdep_mkdir(savedir, ARCHDEP_MKDIR_RWXU);
            }
            lib_free(savedir);
            result = autostart_prg_with_disk_image(unit, drive, file_name, finfo,
                                                   autostart_log, AutostartPrgDiskImage);
            mode = AUTOSTART_HASDISK;
            /* create temporary name for loading, use "*" when the name is longer
               than 16 characters, remove ".prg" extension when found */
            n = 0;while (finfo->name[n]) {
                if (n == 17) {
                    tempname[0] = '*';
                    n = 1;
                    break;
                }
                if ((n < 17) && (!util_strcasecmp((const char*)&finfo->name[n], ".prg"))) {
                    break;
                }
                tempname[n] = finfo->name[n];
                n++;
            }
            tempname[n] = 0;
            boot_file_name = (const char *)tempname;
            }
            /* enable TDE and reset the drive to prepare the eof callback */
            /* if TDE was enabled before autostarting but is disabled now, enable it again */
            if (orig_drive_true_emulation_state && !get_true_drive_emulation_state(unit)) {
                log_message(autostart_log, "Turning TDE on to allow drive reset");
                set_true_drive_emulation_mode(1, unit);
            }
            /* if TDE is now enabled, trigger a drive reset */
            if (get_true_drive_emulation_state(unit)) {
                log_message(autostart_log, "Resetting drive %d", unit);
                drive_cpu_trigger_reset(unit - DRIVE_UNIT_MIN);
            }

            autostart_type = AUTOSTART_PRG_DISK;
            break;
        default:
            log_error(autostart_log, "Invalid PRG autostart mode: %d", AutostartPrgMode);
            mode = result = -1;
            break;
    }

    /* Now either proceed with disk image booting or prg injection after reset */
    if (result >= 0) {
        reboot_for_autostart(boot_file_name, mode, runmode);
    }

    /* close prg file */
    fileio_close(finfo);

    /* restore_drive_emulation_state(DRIVE_UNIT_MIN); */

    return result;
}


/** \brief  Autostart tapecart image \a file_name
 *
 * \param[in]   file_name   path to tapecart image
 * \param[in]   unused      unused
 *
 * \return  0 on success, -1 on failure
 */
/* FIXME: make sure init_drive_emulation_state() does the right thing in this
          function (perhaps we need to handle all drives?) */
int autostart_tapecart(const char *file_name, void *unused)
{
    /* check if a file_name is actually a TCRT image */
    if (!tapecart_is_valid(file_name)) {
        return -1;
    }

    /* make sure to init TDE and traps status before each autostart */
    /* FIXME: this likely needs to be handled differently for tapecart */
    init_drive_emulation_state(DRIVE_UNIT_MIN, 0);

    /* attach image and trigger autostart */
    if (tapecart_attach_tcrt(file_name, NULL) == 0) {
        autostart_tape_unit = 1; /* FIXME: may be 2 on xpet */
        reboot_for_autostart(NULL, AUTOSTART_HASTAPE, AUTOSTART_MODE_RUN);
        return 0;
    }
    return -1;
}



/* ------------------------------------------------------------------------- */

int autostart_autodetect_opt_prgname(const char *file_prog_name,
                                     unsigned int alt_prg_number,
                                     unsigned int runmode)
{
    char *tmp;
    int result;

    /* Check for image:prg -format.  */
    tmp = strrchr(file_prog_name, ':');
    if (tmp) {
        char *autostart_prg_name;
        char *autostart_file;

        autostart_file = lib_strdup(file_prog_name);
        autostart_prg_name = strrchr(autostart_file, ':');
        *autostart_prg_name++ = '\0';
        /* Does the image exist?  */
        if (util_file_exists(autostart_file)) {
            char *name;

            charset_petconvstring((uint8_t *)autostart_prg_name, CONVERT_TO_PETSCII);
            name = charset_replace_hexcodes(autostart_prg_name);
            result = autostart_autodetect(autostart_file, name, 0, runmode);
            lib_free(name);
        } else {
            result = autostart_autodetect(file_prog_name, NULL, alt_prg_number, runmode);
        }
        lib_free(autostart_file);
    } else {
        result = autostart_autodetect(file_prog_name, NULL, alt_prg_number, runmode);
    }
    return result;
}

static void set_tapeport_device(int datasette, int tapecart)
{
    /* first disable all devices, so we dont get any conflicts */
    if (resources_set_int("TapePort1Device", TAPEPORT_DEVICE_NONE) < 0) {
        log_error(LOG_DEFAULT, "Failed to disable the tape port device.");
    }
    /* now enable the one we want to enable */
    if (datasette) {
        if (resources_set_int("TapePort1Device", TAPEPORT_DEVICE_DATASETTE) < 0) {
            log_error(LOG_DEFAULT, "Failed to enable the Datasette.");
        }
    }
    if (tapecart) {
        if (resources_set_int("TapePort1Device", TAPEPORT_DEVICE_TAPECART) < 0) {
            log_error(LOG_DEFAULT, "Failed to enable the Tapecart.");
        }
    }
}

/* Autostart `file_name', trying to auto-detect its type.
   FIXME: pass device nr into this function
*/
int autostart_autodetect(const char *file_name, const char *program_name,
                         unsigned int program_number, unsigned int runmode)
{
    int unit = DRIVE_UNIT_MIN, drive = 0;
#ifdef HAVE_NATIVE_GTK3
    if (!mainlock_is_vice_thread()) {
        mainlock_assert_lock_obtained();
    }
#endif
    if (network_connected() || event_record_active() || event_playback_active()
        || file_name == NULL) {
        return -1;
    }

    if (!autostart_enabled) {
        log_error(autostart_log,
                  "Autostart is not available on this setup.");
        return -1;
    }

    /* make sure to init TDE and traps status before each autostart */
    init_drive_emulation_state(unit, drive);

    log_message(autostart_log, "Autodetecting image type of `%s'.", file_name);

    if (autostart_disk(unit, drive, file_name, program_name, program_number, runmode) == 0) {
        log_message(autostart_log, "`%s' recognized as disk image.", file_name);
        return 0;
    }

    /* DTV has no tape port, SCPU makes tape non operational */
    if ((machine_class != VICE_MACHINE_C64DTV) &&
        (machine_class != VICE_MACHINE_SCPU64)) {
        int tapedevice_temp;

        if (resources_get_int("TapePort1Device", &tapedevice_temp) < 0) {
            log_error(LOG_DEFAULT, "Failed to get Datasette status.");
        }

        set_tapeport_device(1, 0);  /* select datasette on, tapecart off */

        if (autostart_tape(file_name, program_name, program_number, runmode, TAPEPORT_PORT_1) == 0) {
            log_message(autostart_log, "`%s' recognized as tape image.", file_name);
            return 0;
        }

        /* tapecart can only be used with C64 (or C64 mode of C128) */
        if ((machine_class == VICE_MACHINE_C64) ||
            (machine_class == VICE_MACHINE_C64SC) ||
            (machine_class == VICE_MACHINE_C128)) {
            set_tapeport_device(0, 1); /* select datasette off, tapecart on */
            if (autostart_tapecart(file_name, NULL) == 0) {
                log_message(autostart_log, "`%s' recognized as tapecart image.", file_name);
                return 0;
            }
        }

        resources_set_int("TapePort1Device", tapedevice_temp);
    }

#ifdef __LIBRETRO__
    /* Don't try to detect snapshots from files that for sure aren't.
     * This should be the last resort anyway. */
    if (strstr(file_name, ".vsf"))
#endif
    if (autostart_snapshot(file_name, program_name) == 0) {
        log_message(autostart_log, "`%s' recognized as snapshot image.",
                    file_name);
        return 0;
    }

    if ((machine_class == VICE_MACHINE_C64) ||
        (machine_class == VICE_MACHINE_C64SC) ||
        (machine_class == VICE_MACHINE_SCPU64) ||
        (machine_class == VICE_MACHINE_VIC20) ||
        (machine_class == VICE_MACHINE_PLUS4) ||
        (machine_class == VICE_MACHINE_CBM5x0) ||
        (machine_class == VICE_MACHINE_CBM6x0) ||
        (machine_class == VICE_MACHINE_C128)) {
        if (cartridge_attach_image(CARTRIDGE_CRT, file_name) == 0) {
            log_message(autostart_log, "`%s' recognized as cartridge image.",
                        file_name);
            return 0;
        }
    }

    if (autostart_prg(file_name, runmode) == 0) {
        log_message(autostart_log, "`%s' recognized as program/p00 file.",
                    file_name);
        return 0;
    }

    log_error(autostart_log, "`%s' is not a valid file.", file_name);
    return -1;
}

/* FIXME: dead code? */
#if 0
/* Autostart the image attached to device `device'.  */
int autostart_device(int device)
{
#if HAVE_NATIVE_GTK3
    if (!mainlock_is_vice_thread()) {
        mainlock_assert_lock_obtained();
    }
#endif
    if (network_connected() || event_playback_active() || event_record_active()
        || !autostart_enabled) {
        return -1;
    }

    /* make sure to init TDE and traps status before each autostart */
    if (device >= DRIVE_UNIT_MIN) {
        init_drive_emulation_state(device);
        reboot_for_autostart(NULL, AUTOSTART_HASDISK, AUTOSTART_MODE_RUN);
        return 0;
    } else if (device == 1) {
        init_drive_emulation_state(DRIVE_UNIT_MIN);
        reboot_for_autostart(NULL, AUTOSTART_HASTAPE, AUTOSTART_MODE_RUN);
        return 0;
    }
    return -1;
}
#endif

int autostart_in_progress(void)
{
    return ((autostartmode != AUTOSTART_NONE) && (autostartmode != AUTOSTART_DONE));
}

/* Disable autostart on reset.  */
/* FIXME: pass device nr into this function */
void autostart_reset(void)
{
    int oldmode;

    DBG(("autostart_reset (autostart_enabled:%d)", autostart_enabled));

    if (!autostart_enabled) {
        return;
    }

    if (!autostart_ignore_reset
        && autostartmode != AUTOSTART_NONE
        && autostartmode != AUTOSTART_ERROR) {
        oldmode = autostartmode;
        autostartmode = AUTOSTART_NONE;
        if (oldmode != AUTOSTART_DONE) {
            DBG(("autostart_reset oldmode != AUTOSTART_DONE"));
            disk_eof_callback();
        }
        autostartmode = AUTOSTART_NONE;
        trigger_monitor = 0;
        deallocate_program_name();
        log_message(autostart_log, "Turned off.");
    }
    autostart_ignore_reset = 0;
}

void autostart_shutdown(void)
{
    deallocate_program_name();

    autostart_prg_shutdown();
}

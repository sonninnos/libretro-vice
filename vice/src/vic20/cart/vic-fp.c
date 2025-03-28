/*
 * vic-fp.c -- Vic Flash Plugin emulation.
 *
 * Written by
 *  Marko Makela <marko.makela@iki.fi>
 *  Hannu Nuotio <hannu.nuotio@tut.fi>
 * Based on megacart.c and finalexpansion.c by
 *  Daniel Kahlin <daniel@kahlin.net>
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

/* #define DEBUGCART */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archdep.h"
#include "cartio.h"
#include "cartridge.h"
#include "cmdline.h"
#include "crt.h"
#include "export.h"
#include "flash040.h"
#include "lib.h"
#include "log.h"
#include "machine.h"
#include "maincpu.h"
#include "mem.h"
#include "monitor.h"
#include "ram.h"
#include "resources.h"
#include "snapshot.h"
#include "types.h"
#include "util.h"
#include "vic-fp.h"
#include "vic20cart.h"
#include "vic20cartmem.h"
#include "vic20mem.h"
#include "zfile.h"

#ifdef DEBUGCART
#define DBG(x) printf x
#else
#define DBG(x)
#endif

/* ------------------------------------------------------------------------- */
/*
 * Cartridge RAM (32 KiB)
 *
 * Mapping
 *      RAM                 VIC20
 *   0x0000 - 0x1fff  ->  0xa000 - 0xbfff
 *   0x2400 - 0x2fff  ->  0x0400 - 0x0fff
 *   0x2000 - 0x7fff  ->  0x2000 - 0x7fff
 */
#define CART_RAM_SIZE 0x8000
static uint8_t *cart_ram = NULL;

/*
 * Cartridge ROM (4 MiB)
 */
#define CART_ROM_SIZE 0x400000
static uint8_t *cart_rom = NULL;

#define CART_CFG_ENABLE (!(cart_cfg_reg & 0x80)) /* cart_cfg_reg enable */
#define CART_CFG_BLK5_WP (cart_cfg_reg & 0x40) /* BLK5 write protect */
#define CART_CFG_BLK5_RAM (cart_cfg_reg & 0x20) /* RAM at BLK5 instead of ROM */
#define CART_CFG_BLK1 ((cart_cfg_reg & 0x18) == 0x18) /* BLK1 enabled */
#define CART_CFG_RAM123 ((cart_cfg_reg & 0x18) == 0x08) /* RAM123 enabled */
#define CART_CFG_A21 (cart_cfg_reg & 0x01) /* ROM address line 21 */
#define CART_CFG_MASK 0xf9 /* write mask for cart_cfg_reg */

#define CART_BANK_DEFAULT 0x00
#define CART_CFG_DEFAULT 0x40

/** ROM bank switching register (A20..A13), mapped at $9800..$9bfe (even) */
static uint8_t cart_bank_reg;
/** configuration register, mapped at $9801..$9bff (odd)
 * b7 == 1 => I/O2 disabled until RESET
 * b6 == 1 => ROM write protect (set by default)
 * b5 == 1 => RAM at BLK5 (instead of ROM)
 * b4 => 0=3k (RAM123), 1=8k+ (BLK1)
 * b3 == 1 => BLK1/RAM123 enable
 * b2, b1=unused (always 0)
 * b0 => A21
 */
static uint8_t cart_cfg_reg;

/* Cartridge States */
/** Flash state */
static flash040_context_t flash_state;
/** configuration register enabled */
static int cfg_en_flop;
/** RAM at RAM123 enabled */
static int ram123_en_flop;
/** RAM at BLK1 enabled */
static int blk1_en_flop;
/** RAM at BLK5 instead of ROM */
static int ram5_flop;

#define CART_CFG_UPDATE                                      \
    do {                                                     \
        cfg_en_flop = CART_CFG_ENABLE;                       \
        ram123_en_flop = CART_CFG_RAM123;                    \
        blk1_en_flop = CART_CFG_BLK1;                        \
        ram5_flop = CART_CFG_BLK5_RAM;                       \
        cart_rom_bank = cart_bank_reg | (CART_CFG_A21 << 8); \
    } while (0)

#define CART_CFG_INIT(value)                  \
    do {                                      \
        cart_cfg_reg = value & CART_CFG_MASK; \
        CART_CFG_UPDATE;                      \
    } while (0)

/* ------------------------------------------------------------------------- */

/* helper variables */
static unsigned int cart_rom_bank;

static int vic_fp_writeback;
static log_t fp_log = LOG_DEFAULT;

static char *vic_fp_filename = NULL;
static int vic_fp_filetype = 0;
/* ------------------------------------------------------------------------- */

/* Some prototypes are needed */
static uint8_t vic_fp_io2_read(uint16_t addr);
static uint8_t vic_fp_io2_peek(uint16_t addr);
static void vic_fp_io2_store(uint16_t addr, uint8_t value);
static int vic_fp_mon_dump(void);

static io_source_t vfp_device = {
    CARTRIDGE_VIC20_NAME_FP, /* name of the device */
    IO_DETACH_CART,          /* use cartridge ID to detach the device when involved in a read-collision */
    IO_DETACH_NO_RESOURCE,   /* does not use a resource for detach */
    0x9800, 0x9bff, 0x01,    /* range for the device, regs:$9800-$9801, mirrors:$9802-$9bff */
    0,                       /* read validity determined by the device upon a read */
    vic_fp_io2_store,        /* store function */
    NULL,                    /* NO poke function */
    vic_fp_io2_read,         /* read function */
    vic_fp_io2_peek,         /* peek function */
    vic_fp_mon_dump,         /* device state information dump function */
    CARTRIDGE_VIC20_FP,      /* cartridge ID */
    IO_PRIO_NORMAL,          /* normal priority, device read needs to be checked for collisions */
    0,                       /* insertion order, gets filled in by the registration function */
    IO_MIRROR_NONE           /* NO mirroring */
};

static io_source_list_t *vfp_list_item = NULL;

static const export_resource_t export_res = {
    CARTRIDGE_VIC20_NAME_FP, 0, 0, &vfp_device, NULL, CARTRIDGE_VIC20_FP
};

/* ------------------------------------------------------------------------- */

/* read 0x0400-0x0fff */
uint8_t vic_fp_ram123_read(uint16_t addr)
{
    if (ram123_en_flop) {
        return cart_ram[(addr & 0x1fff) + 0x2000];
    } else {
        return vic20_v_bus_last_data;
    }
}

/* store 0x0400-0x0fff */
void vic_fp_ram123_store(uint16_t addr, uint8_t value)
{
    if (ram123_en_flop) {
        cart_ram[(addr & 0x1fff) + 0x2000] = value;
    }
}

/* read 0x2000-0x3fff */
uint8_t vic_fp_blk1_read(uint16_t addr)
{
    if (blk1_en_flop) {
        return cart_ram[addr];
    }

    return vic20_cpu_last_data;
}

/* store 0x2000-0x3fff */
void vic_fp_blk1_store(uint16_t addr, uint8_t value)
{
    if (blk1_en_flop) {
        cart_ram[addr] = value;
    }
}

/* read 0x4000-0x7fff */
uint8_t vic_fp_blk23_read(uint16_t addr)
{
    return cart_ram[addr];
}

/* store 0x4000-0x7fff */
void vic_fp_blk23_store(uint16_t addr, uint8_t value)
{
    cart_ram[addr] = value;
}

/* read 0xa000-0xbfff */
uint8_t vic_fp_blk5_read(uint16_t addr)
{
    if (ram5_flop) {
        return cart_ram[addr & 0x1fff];
    } else {
        return flash040core_read(&flash_state, (addr & 0x1fff) | (cart_rom_bank << 13));
    }
}

/* store 0xa000-0xbfff */
void vic_fp_blk5_store(uint16_t addr, uint8_t value)
{
    if (CART_CFG_BLK5_WP) {
    } else if (ram5_flop) {
        cart_ram[addr & 0x1fff] = value;
    } else {
        flash040core_store(&flash_state, (addr & 0x1fff) | (cart_rom_bank << 13), value);
    }
}

/* read 0x9800-0x9bff */
uint8_t vic_fp_io2_read(uint16_t addr)
{
    uint8_t value;

    vfp_device.io_source_valid = 0;

    if (!cfg_en_flop) {
        value = vic20_cpu_last_data;
    } else if (addr & 1) {
        value = cart_cfg_reg;
        vfp_device.io_source_valid = 1;
    } else {
        value = cart_bank_reg;
        vfp_device.io_source_valid = 1;
    }

    return value;
}

uint8_t vic_fp_io2_peek(uint16_t addr)
{
    uint8_t value;

    if (addr & 1) {
        value = cart_cfg_reg;
    } else {
        value = cart_bank_reg;
    }

    return value;
}

/* store 0x9800-0x9bff */
void vic_fp_io2_store(uint16_t addr, uint8_t value)
{
    if (!cfg_en_flop) {
        /* ignore */
    } else if (addr & 1) {
        CART_CFG_INIT(value);
    } else {
        cart_bank_reg = value;
        CART_CFG_UPDATE;
    }
}

/* ------------------------------------------------------------------------- */

/* FIXME: this still needs to be tweaked to match the hardware */
static RAMINITPARAM ramparam = {
    .start_value = 255,
    .value_invert = 2,
    .value_offset = 1,

    .pattern_invert = 0x100,
    .pattern_invert_value = 255,

    .random_start = 0,
    .random_repeat = 0,
    .random_chance = 0,
};

void vic_fp_powerup(void)
{
    if (cart_ram) {
        ram_init_with_pattern(cart_ram, CART_RAM_SIZE, &ramparam);
    }
}

void vic_fp_init(void)
{
    if (fp_log == LOG_DEFAULT) {
        fp_log = log_open(CARTRIDGE_VIC20_NAME_FP);
    }
}

void vic_fp_reset(void)
{
    flash040core_reset(&flash_state);
    cart_bank_reg = CART_BANK_DEFAULT;
    CART_CFG_INIT(CART_CFG_DEFAULT);
}

void vic_fp_config_setup(uint8_t *rawcart)
{
}

static int zfile_load(const char *filename, uint8_t *dest, size_t size)
{
    FILE *fd;
    off_t len;

    fd = zfile_fopen(filename, MODE_READ);
    if (!fd) {
        return -1;
    }
    len = archdep_file_size(fd);
    if (len < 0 || (size_t)len != size) {
        zfile_fclose(fd);
        return -1;
    }
    if (fread(dest, size, 1, fd) < 1) {
        zfile_fclose(fd);
        return -1;
    }
    zfile_fclose(fd);
    return 0;
}

int vic_fp_crt_attach(FILE *fd, uint8_t *rawcart, const char *filename)
{
    crt_chip_header_t chip;
    int idx = 0;

    vic_fp_filetype = 0;
    vic_fp_filename = NULL;

    if (!cart_ram) {
        cart_ram = lib_malloc(CART_RAM_SIZE);
    }
    if (!cart_rom) {
        cart_rom = lib_malloc(CART_ROM_SIZE);
    }

    /* util_string_set(&cartfile, filename); */ /* FIXME */

    for (idx = 0; idx < 512; idx++) {
        if (crt_read_chip_header(&chip, fd)) {
            goto exiterror;
        }

        DBG(("chip %d at %02x len %02x\n", idx, chip.start, chip.size));
        if (chip.size != 0x2000) {
            goto exiterror;
        }

        if (crt_read_chip(&cart_rom[0x2000 * idx], 0, &chip, fd)) {
            goto exiterror;
        }
    }

    if (export_add(&export_res) < 0) {
        goto exiterror;
    }

    flash040core_init(&flash_state, maincpu_alarm_context, FLASH040_TYPE_032B_A0_1_SWAP, cart_rom);

    mem_cart_blocks = VIC_CART_RAM123 |
                      VIC_CART_BLK1 | VIC_CART_BLK2 | VIC_CART_BLK3 | VIC_CART_BLK5 |
                      VIC_CART_IO2;
    mem_initialize_memory();

    vfp_list_item = io_source_register(&vfp_device);

    vic_fp_filetype = CARTRIDGE_FILETYPE_CRT;
    vic_fp_filename = lib_strdup(filename);

    return CARTRIDGE_VIC20_FP;
exiterror:
    vic_fp_detach();
    return -1;
}

int vic_fp_bin_attach(const char *filename)
{
    vic_fp_filetype = 0;
    vic_fp_filename = NULL;

    if (!cart_ram) {
        cart_ram = lib_malloc(CART_RAM_SIZE);
    }
    if (!cart_rom) {
        cart_rom = lib_malloc(CART_ROM_SIZE);
    }

    if (zfile_load(filename, cart_rom, (size_t)CART_ROM_SIZE) < 0) {
        vic_fp_detach();
        return -1;
    }

    if (export_add(&export_res) < 0) {
        return -1;
    }

    flash040core_init(&flash_state, maincpu_alarm_context, FLASH040_TYPE_032B_A0_1_SWAP, cart_rom);

    mem_cart_blocks = VIC_CART_RAM123 |
                      VIC_CART_BLK1 | VIC_CART_BLK2 | VIC_CART_BLK3 | VIC_CART_BLK5 |
                      VIC_CART_IO2;
    mem_initialize_memory();

    vfp_list_item = io_source_register(&vfp_device);

    vic_fp_filetype = CARTRIDGE_FILETYPE_BIN;
    vic_fp_filename = lib_strdup(filename);

    return 0;
}

void vic_fp_detach(void)
{
    /* try to write back cartridge contents if write back is enabled
       and cartridge wasn't from a snapshot */
    if (vic_fp_writeback && !cartridge_is_from_snapshot) {
        vic_fp_flush_image();
    }

    mem_cart_blocks = 0;
    mem_initialize_memory();
    lib_free(cart_ram);
    lib_free(cart_rom);
    cart_ram = NULL;
    cart_rom = NULL;
    lib_free(vic_fp_filename);
    vic_fp_filename = NULL;

    if (vfp_list_item != NULL) {
        export_remove(&export_res);
        io_source_unregister(vfp_list_item);
        vfp_list_item = NULL;
    }
}

int vic_fp_bin_save(const char *filename)
{
    FILE *fd;

    if (filename == NULL) {
        return -1;
    }

    fd = fopen(filename, MODE_WRITE);

    if (fd == NULL) {
        return -1;
    }

        if (fwrite(flash_state.flash_data, 1, CART_ROM_SIZE, fd) != CART_ROM_SIZE) {
            fclose(fd);
            return -1;
        }

    fclose(fd);

    return 0;
}

int vic_fp_crt_save(const char *filename)
{
    log_error(LOG_DEFAULT, "FIXME: vic_fp_crt_save not implemented");
    FILE *fd;
    crt_chip_header_t chip;
    uint8_t *data;
    int i;

    fd = crt_create_vic20(filename, CARTRIDGE_VIC20_FP, 0, CARTRIDGE_VIC20_NAME_FP);

    if (fd == NULL) {
        return -1;
    }

    chip.type = 2;
    chip.size = 0x2000;
    chip.start = 0xa000;

    data = flash_state.flash_data;

    for (i = 0; i < (CART_ROM_SIZE / chip.size); i++) {
        chip.bank = i; /* bank */

        if (crt_write_chip(data, &chip, fd)) {
            fclose(fd);
            return -1;
        }
        data += chip.size;
    }

    fclose(fd);
    return 0;
}

int vic_fp_flush_image(void)
{
    int ret = 0;
    if (flash_state.flash_dirty) {
        log_message(fp_log, "Flash dirty, trying to write back...");
        if (vic_fp_filetype == CARTRIDGE_FILETYPE_BIN) {
            ret = vic_fp_bin_save(vic_fp_filename);
        } else if (vic_fp_filetype == CARTRIDGE_FILETYPE_CRT) {
            ret = vic_fp_crt_save(vic_fp_filename);
        }
        if (ret < 1) {
            log_message(fp_log, "Failed to write back image `%s'!", vic_fp_filename);
        } else {
            log_message(fp_log, "Wrote back image `%s'.", vic_fp_filename);
        }
    } else {
        log_message(fp_log, "Flash clean, skipping write back.");
    }
    return ret;
}

/* ------------------------------------------------------------------------- */

static int set_vic_fp_writeback(int val, void *param)
{
    vic_fp_writeback = val ? 1 : 0;

    return 0;
}

static const resource_int_t resources_int[] = {
    { "VicFlashPluginWriteBack", 0, RES_EVENT_STRICT, (resource_value_t)0,
      &vic_fp_writeback, set_vic_fp_writeback, NULL },
    RESOURCE_INT_LIST_END
};

int vic_fp_resources_init(void)
{
    return resources_register_int(resources_int);
}

void vic_fp_resources_shutdown(void)
{
}

static const cmdline_option_t cmdline_options[] =
{
    { "-fpwriteback", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "VicFlashPluginWriteBack", (resource_value_t)1,
      NULL, "Enable Vic Flash Plugin write back to ROM file" },
    { "+fpwriteback", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "VicFlashPluginWriteBack", (resource_value_t)0,
      NULL, "Disable Vic Flash Plugin write back to ROM file" },
    CMDLINE_LIST_END
};

int vic_fp_cmdline_options_init(void)
{
    return cmdline_register_options(cmdline_options);
}

/* ------------------------------------------------------------------------- */

#define VIC20CART_DUMP_VER_MAJOR   2
#define VIC20CART_DUMP_VER_MINOR   0
#define SNAP_MODULE_NAME  "VICFLASHPLUGIN"
#define FLASH_SNAP_MODULE_NAME  "FLASH040FP"

int vic_fp_snapshot_write_module(snapshot_t *s)
{
    snapshot_module_t *m;

    m = snapshot_module_create(s, SNAP_MODULE_NAME,
                               VIC20CART_DUMP_VER_MAJOR,
                               VIC20CART_DUMP_VER_MINOR);
    if (m == NULL) {
        return -1;
    }

    if (0
        || (SMW_B(m, cart_bank_reg) < 0)
        || (SMW_B(m, cart_cfg_reg) < 0)
        || (SMW_BA(m, cart_ram, CART_RAM_SIZE) < 0)
        || (SMW_BA(m, cart_rom, CART_ROM_SIZE) < 0)) {
        snapshot_module_close(m);
        return -1;
    }

    snapshot_module_close(m);

    if ((flash040core_snapshot_write_module(s, &flash_state, FLASH_SNAP_MODULE_NAME) < 0)) {
        return -1;
    }

    return 0;
}

int vic_fp_snapshot_read_module(snapshot_t *s)
{
    uint8_t vmajor, vminor;
    snapshot_module_t *m;

    m = snapshot_module_open(s, SNAP_MODULE_NAME, &vmajor, &vminor);
    if (m == NULL) {
        return -1;
    }

    if (vmajor != VIC20CART_DUMP_VER_MAJOR) {
        snapshot_module_close(m);
        return -1;
    }

    if (!cart_ram) {
        cart_ram = lib_malloc(CART_RAM_SIZE);
    }
    if (!cart_rom) {
        cart_rom = lib_malloc(CART_ROM_SIZE);
    }

    flash040core_init(&flash_state, maincpu_alarm_context, FLASH040_TYPE_032B_A0_1_SWAP, cart_rom);

    if (0
        || (SMR_B(m, &cart_bank_reg) < 0)
        || (SMR_B(m, &cart_cfg_reg) < 0)
        || (SMR_BA(m, cart_ram, CART_RAM_SIZE) < 0)
        || (SMR_BA(m, cart_rom, CART_ROM_SIZE) < 0)) {
        snapshot_module_close(m);
        lib_free(cart_ram);
        lib_free(cart_rom);
        cart_ram = NULL;
        cart_rom = NULL;
        return -1;
    }

    snapshot_module_close(m);

    if ((flash040core_snapshot_read_module(s, &flash_state, FLASH_SNAP_MODULE_NAME) < 0)) {
        flash040core_shutdown(&flash_state);
        lib_free(cart_ram);
        lib_free(cart_rom);
        cart_ram = NULL;
        cart_rom = NULL;
        return -1;
    }

    CART_CFG_INIT(cart_cfg_reg);

    mem_cart_blocks = VIC_CART_RAM123 |
                      VIC_CART_BLK1 | VIC_CART_BLK2 | VIC_CART_BLK3 | VIC_CART_BLK5 |
                      VIC_CART_IO2;
    mem_initialize_memory();

    /* set filetype to none */
    vic_fp_filename = NULL;
    vic_fp_filetype = 0;

    return 0;
}

/* ------------------------------------------------------------------------- */

static int vic_fp_mon_dump(void)
{
    mon_out("I/O2 %sabled\n", cfg_en_flop ? "en" : "dis");
    mon_out("BLK5 is R%cM %s\n", ram5_flop ? 'A' : 'O', CART_CFG_BLK5_WP ? "(write protected)" : "");
    mon_out("BLK1 %sabled\n", blk1_en_flop ? "en" : "dis");
    mon_out("RAM123 %sabled\n", ram123_en_flop ? "en" : "dis");
    mon_out("ROM bank $%03x (offset $%06x)\n", cart_rom_bank, cart_rom_bank << 13);
    return 0;
}

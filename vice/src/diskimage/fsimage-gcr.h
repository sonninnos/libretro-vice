/*
 * fsimage-gcr.h
 *
 * Written by
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

#ifndef VICE_FSIMAGE_GCR_H
#define VICE_FSIMAGE_GCR_H

#include "types.h"

struct disk_image_s;
struct disk_track_s;
struct disk_addr_s;

void fsimage_gcr_init(void);

int fsimage_read_gcr_image(const disk_image_t *image);

int fsimage_gcr_read_sector(const struct disk_image_s *image, uint8_t *buf,
                            const struct disk_addr_s *dadr);
int fsimage_gcr_write_sector(struct disk_image_s *image, const uint8_t *buf,
                             const struct disk_addr_s *dadr);
int fsimage_gcr_read_half_track(const struct disk_image_s *image,
                                unsigned int half_track,
                                struct disk_track_s *raw);
int fsimage_gcr_write_half_track(struct disk_image_s *image,
                                 unsigned int half_track, const struct disk_track_s *raw);

#endif

/***************************************************************************
 *   ESP32-S2 flasher stub definitions                                     *
 *   Copyright (C) 2019 Espressif Systems Ltd.                             *
 *   Author: Alexey Gerenkov <alexey@espressif.com>                        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/
#ifndef ESP32_S2_FLASHER_STUB_H
#define ESP32_S2_FLASHER_STUB_H

#define STUB_FLASH_SECTOR_SIZE  4096
/* Flash geometry constants */
#define STUB_FLASH_BLOCK_SIZE   65536
#define STUB_FLASH_PAGE_SIZE    256
#define STUB_FLASH_STATUS_MASK  0xFFFF

struct stub_flash_state {
	uint32_t cache_flags[2];
};

#endif	/*ESP32_S2_FLASHER_STUB_H */

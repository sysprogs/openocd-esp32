/***************************************************************************
 *   ESP32-S3 target for OpenOCD                                           *
 *   Copyright (C) 2020 Espressif Systems Ltd.                             *
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

#ifndef XTENSA_ESP32_S3_H
#define XTENSA_ESP32_S3_H

#include "esp_xtensa_smp.h"

#define ESP32_S3_DROM_LOW             0x3D000000
#define ESP32_S3_DROM_HIGH            0x3D800000
#define ESP32_S3_IROM_LOW             0x42000000
#define ESP32_S3_IROM_HIGH            0x43000000

/*Number of registers returned directly by the G command
 *Corresponds to the amount of regs listed in regformats/reg-xtensa.dat in the gdb source */
#define ESP32_S3_NUM_REGS_G_COMMAND   128

enum esp32s3_reg_id {
	/* chip specific registers that extend ISA go after ISA-defined ones */
	ESP32_S3_REG_IDX_GPIOOUT = XT_NUM_REGS,
	ESP32_S3_REG_IDX_ACCX_0,
	ESP32_S3_REG_IDX_ACCX_1,
	ESP32_S3_REG_IDX_QACC_H_0,
	ESP32_S3_REG_IDX_QACC_H_1,
	ESP32_S3_REG_IDX_QACC_H_2,
	ESP32_S3_REG_IDX_QACC_H_3,
	ESP32_S3_REG_IDX_QACC_H_4,
	ESP32_S3_REG_IDX_QACC_L_0,
	ESP32_S3_REG_IDX_QACC_L_1,
	ESP32_S3_REG_IDX_QACC_L_2,
	ESP32_S3_REG_IDX_QACC_L_3,
	ESP32_S3_REG_IDX_QACC_L_4,
	ESP32_S3_REG_IDX_SAR_BYTE,
	ESP32_S3_REG_IDX_FFT_BIT_WIDTH,
	ESP32_S3_REG_IDX_UA_STATE_0,
	ESP32_S3_REG_IDX_UA_STATE_1,
	ESP32_S3_REG_IDX_UA_STATE_2,
	ESP32_S3_REG_IDX_UA_STATE_3,
	ESP32_S3_REG_IDX_Q0,
	ESP32_S3_REG_IDX_Q1,
	ESP32_S3_REG_IDX_Q2,
	ESP32_S3_REG_IDX_Q3,
	ESP32_S3_REG_IDX_Q4,
	ESP32_S3_REG_IDX_Q5,
	ESP32_S3_REG_IDX_Q6,
	ESP32_S3_REG_IDX_Q7,
	ESP32_S3_NUM_REGS,
};

struct esp32s3_common {
	struct esp_xtensa_smp_common esp_xtensa_smp;
};

static inline struct esp32s3_common *target_to_esp32s3(struct target *target)
{
	return container_of(target->arch_info, struct esp32s3_common, esp_xtensa_smp);
}

#endif	/* XTENSA_ESP32_S3_H */

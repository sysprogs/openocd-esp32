/***************************************************************************
 *   Generic ESP xtensa target implementation for OpenOCD                  *
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

#ifndef ESP_XTENSA_H
#define ESP_XTENSA_H

#include "target.h"
#include <helper/command.h>
#include "esp.h"
#include "xtensa.h"
#include "esp_xtensa_apptrace.h"
#include "esp_xtensa_semihosting.h"

struct esp_xtensa_common {
	struct xtensa xtensa;
	struct esp_common esp;
	struct esp_semihost_data semihost;
	struct esp_xtensa_apptrace_info apptrace;
};

static inline struct esp_xtensa_common *target_to_esp_xtensa(struct target *target)
{
	return container_of(target->arch_info, struct esp_xtensa_common, xtensa);
}

int esp_xtensa_init_arch_info(struct target *target,
	struct esp_xtensa_common *esp_xtensa,
	const struct xtensa_config *xtensa_cfg,
	struct xtensa_debug_module_config *dm_cfg,
	const struct esp_flash_breakpoint_ops *spec_brps_ops,
	const struct esp_semihost_ops *semihost_ops);
int esp_xtensa_target_init(struct command_context *cmd_ctx, struct target *target);
void esp_xtensa_target_deinit(struct target *target);
int esp_xtensa_arch_state(struct target *target);
void esp_xtensa_queue_tdi_idle(struct target *target);
int esp_xtensa_breakpoint_add(struct target *target, struct breakpoint *breakpoint);
int esp_xtensa_breakpoint_remove(struct target *target, struct breakpoint *breakpoint);
int esp_xtensa_poll(struct target *target);
int esp_xtensa_handle_target_event(struct target *target, enum target_event event,
	void *priv);

static inline int esp_xtensa_set_peri_reg_mask(struct target *target,
	target_addr_t addr,
	uint32_t mask,
	uint32_t val)
{
	uint32_t reg_val;
	int res = target_read_u32(target, addr, &reg_val);
	if (res != ERROR_OK)
		return res;
	reg_val = (reg_val & (~mask)) | val;
	res = target_write_u32(target, addr, reg_val);
	if (res != ERROR_OK)
		return res;

	return ERROR_OK;
}

extern const struct command_registration esp_command_handlers[];

#endif	/* ESP_XTENSA_H */

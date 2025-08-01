// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007-2010 Øyvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2008, Duane Ellis                                       *
 *   openocd@duaneeellis.com                                               *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2008 by Rick Altherr                                    *
 *   kc8apf@kc8apf.net>                                                    *
 *                                                                         *
 *   Copyright (C) 2011 by Broadcom Corporation                            *
 *   Evan Hunter - ehunter@broadcom.com                                    *
 *                                                                         *
 *   Copyright (C) ST-Ericsson SA 2011                                     *
 *   michel.jaouen@stericsson.com : smp minimum support                    *
 *                                                                         *
 *   Copyright (C) 2011 Andreas Fritiofson                                 *
 *   andreas.fritiofson@gmail.com                                          *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/align.h>
#include <helper/list.h>
#include <helper/nvp.h>
#include <helper/time_support.h>
#include <jtag/jtag.h>
#include <flash/nor/core.h>

#include "target.h"
#include "target_type.h"
#include "target_request.h"
#include "breakpoints.h"
#include "register.h"
#include "trace.h"
#include "image.h"
#include "rtos/rtos.h"
#include "transport/transport.h"
#include "arm_cti.h"
#include "smp.h"
#include "semihosting_common.h"

/* default halt wait timeout (ms) */
#define DEFAULT_HALT_TIMEOUT 5000

struct target_event_action {
	enum target_event event;
	Jim_Interp *interp;
	Jim_Obj *body;
	struct list_head list;
};

static int target_read_buffer_default(struct target *target, target_addr_t address,
		uint32_t count, uint8_t *buffer);
static int target_write_buffer_default(struct target *target, target_addr_t address,
		uint32_t count, const uint8_t *buffer);
static int target_register_user_commands(struct command_context *cmd_ctx);
static int target_get_gdb_fileio_info_default(struct target *target,
		struct gdb_fileio_info *fileio_info);
static int target_gdb_fileio_end_default(struct target *target, int retcode,
		int fileio_errno, bool ctrl_c);

static struct target_type *target_types[] = {
	// Keep in alphabetic order this list of targets
	&aarch64_target,
	&arcv2_target,
	&arm11_target,
	&arm720t_target,
	&arm7tdmi_target,
	&arm920t_target,
	&arm926ejs_target,
	&arm946e_target,
	&arm966e_target,
	&arm9tdmi_target,
	&armv8r_target,
	&avr32_ap7k_target,
	&avr_target,
	&cortexa_target,
	&cortexm_target,
	&cortexr4_target,
	&dragonite_target,
	&dsp563xx_target,
	&dsp5680xx_target,
	&esirisc_target,
	&esp32c2_target,
	&esp32c3_target,
	&esp32c5_target,
	&esp32c6_target,
	&esp32c61_target,
	&esp32h2_target,
	&esp32h4_target,
	&esp32p4_target,
	&esp32s2_target,
	&esp32s3_target,
	&esp32_target,
	&fa526_target,
	&feroceon_target,
	&hla_target,
	&ls1_sap_target,
	&mem_ap_target,
	&mips_m4k_target,
	&mips_mips64_target,
	&or1k_target,
	&quark_d20xx_target,
	&quark_x10xx_target,
	&riscv_target,
	&stm8_target,
	&testee_target,
	&xscale_target,
	&xtensa_chip_target,
};

struct target *all_targets;
static struct target_event_callback *target_event_callbacks;
static struct target_timer_callback *target_timer_callbacks;
static int64_t target_timer_next_event_value;
static OOCD_LIST_HEAD(target_reset_callback_list);
static OOCD_LIST_HEAD(target_trace_callback_list);
static const int polling_interval = TARGET_DEFAULT_POLLING_INTERVAL;
static OOCD_LIST_HEAD(empty_smp_targets);

enum nvp_assert {
	NVP_DEASSERT,
	NVP_ASSERT,
};

static const struct nvp nvp_assert[] = {
	{ .name = "assert", NVP_ASSERT },
	{ .name = "deassert", NVP_DEASSERT },
	{ .name = "T", NVP_ASSERT },
	{ .name = "F", NVP_DEASSERT },
	{ .name = "t", NVP_ASSERT },
	{ .name = "f", NVP_DEASSERT },
	{ .name = NULL, .value = -1 }
};

static const struct nvp nvp_error_target[] = {
	{ .value = ERROR_TARGET_INVALID, .name = "err-invalid" },
	{ .value = ERROR_TARGET_INIT_FAILED, .name = "err-init-failed" },
	{ .value = ERROR_TARGET_TIMEOUT, .name = "err-timeout" },
	{ .value = ERROR_TARGET_NOT_HALTED, .name = "err-not-halted" },
	{ .value = ERROR_TARGET_FAILURE, .name = "err-failure" },
	{ .value = ERROR_TARGET_UNALIGNED_ACCESS, .name = "err-unaligned-access" },
	{ .value = ERROR_TARGET_DATA_ABORT, .name = "err-data-abort" },
	{ .value = ERROR_TARGET_RESOURCE_NOT_AVAILABLE, .name = "err-resource-not-available" },
	{ .value = ERROR_TARGET_TRANSLATION_FAULT, .name = "err-translation-fault" },
	{ .value = ERROR_TARGET_NOT_RUNNING, .name = "err-not-running" },
	{ .value = ERROR_TARGET_NOT_EXAMINED, .name = "err-not-examined" },
	{ .value = -1, .name = NULL }
};

static const char *target_strerror_safe(int err)
{
	const struct nvp *n;

	n = nvp_value2name(nvp_error_target, err);
	if (!n->name)
		return "unknown";
	else
		return n->name;
}

static const struct nvp nvp_target_event[] = {

	{ .value = TARGET_EVENT_GDB_HALT, .name = "gdb-halt" },
	{ .value = TARGET_EVENT_HALTED, .name = "halted" },
	{ .value = TARGET_EVENT_RESUMED, .name = "resumed" },
	{ .value = TARGET_EVENT_RESUME_START, .name = "resume-start" },
	{ .value = TARGET_EVENT_RESUME_END, .name = "resume-end" },
	{ .value = TARGET_EVENT_STEP_START, .name = "step-start" },
	{ .value = TARGET_EVENT_STEP_END, .name = "step-end" },

	{ .name = "gdb-start", .value = TARGET_EVENT_GDB_START },
	{ .name = "gdb-end", .value = TARGET_EVENT_GDB_END },

	{ .value = TARGET_EVENT_RESET_START,         .name = "reset-start" },
	{ .value = TARGET_EVENT_RESET_ASSERT_PRE,    .name = "reset-assert-pre" },
	{ .value = TARGET_EVENT_RESET_ASSERT,        .name = "reset-assert" },
	{ .value = TARGET_EVENT_RESET_ASSERT_POST,   .name = "reset-assert-post" },
	{ .value = TARGET_EVENT_RESET_DEASSERT_PRE,  .name = "reset-deassert-pre" },
	{ .value = TARGET_EVENT_RESET_DEASSERT_POST, .name = "reset-deassert-post" },
	{ .value = TARGET_EVENT_RESET_INIT,          .name = "reset-init" },
	{ .value = TARGET_EVENT_RESET_END,           .name = "reset-end" },

	{ .value = TARGET_EVENT_EXAMINE_START, .name = "examine-start" },
	{ .value = TARGET_EVENT_EXAMINE_FAIL, .name = "examine-fail" },
	{ .value = TARGET_EVENT_EXAMINE_END, .name = "examine-end" },

	{ .value = TARGET_EVENT_DEBUG_HALTED, .name = "debug-halted" },
	{ .value = TARGET_EVENT_DEBUG_RESUMED, .name = "debug-resumed" },

	{ .value = TARGET_EVENT_GDB_ATTACH, .name = "gdb-attach" },
	{ .value = TARGET_EVENT_GDB_DETACH, .name = "gdb-detach" },

	{ .value = TARGET_EVENT_GDB_FLASH_WRITE_START, .name = "gdb-flash-write-start" },
	{ .value = TARGET_EVENT_GDB_FLASH_WRITE_END,   .name = "gdb-flash-write-end"   },

	{ .value = TARGET_EVENT_GDB_FLASH_ERASE_START, .name = "gdb-flash-erase-start" },
	{ .value = TARGET_EVENT_GDB_FLASH_ERASE_END,   .name = "gdb-flash-erase-end" },

	{ .value = TARGET_EVENT_TRACE_CONFIG, .name = "trace-config" },

	{ .value = TARGET_EVENT_QXFER_THREAD_READ_END, .name = "qxfer-thread-read-end" }, /* Espressif */

	{ .value = TARGET_EVENT_SEMIHOSTING_USER_CMD_0X100, .name = "semihosting-user-cmd-0x100" },
	{ .value = TARGET_EVENT_SEMIHOSTING_USER_CMD_0X101, .name = "semihosting-user-cmd-0x101" },
	{ .value = TARGET_EVENT_SEMIHOSTING_USER_CMD_0X102, .name = "semihosting-user-cmd-0x102" },
	{ .value = TARGET_EVENT_SEMIHOSTING_USER_CMD_0X103, .name = "semihosting-user-cmd-0x103" },
	{ .value = TARGET_EVENT_SEMIHOSTING_USER_CMD_0X104, .name = "semihosting-user-cmd-0x104" },
	{ .value = TARGET_EVENT_SEMIHOSTING_USER_CMD_0X105, .name = "semihosting-user-cmd-0x105" },
	{ .value = TARGET_EVENT_SEMIHOSTING_USER_CMD_0X106, .name = "semihosting-user-cmd-0x106" },
	{ .value = TARGET_EVENT_SEMIHOSTING_USER_CMD_0X107, .name = "semihosting-user-cmd-0x107" },

	{ .name = NULL, .value = -1 }
};

static const struct nvp nvp_target_state[] = {
	{ .name = "unknown", .value = TARGET_UNKNOWN },
	{ .name = "running", .value = TARGET_RUNNING },
	{ .name = "halted",  .value = TARGET_HALTED },
	{ .name = "reset",   .value = TARGET_RESET },
	{ .name = "debug-running", .value = TARGET_DEBUG_RUNNING },
	{ .name = "unavailable", .value = TARGET_UNAVAILABLE },
	{ .name = NULL, .value = -1 },
};

static const struct nvp nvp_target_debug_reason[] = {
	{ .name = "debug-request",             .value = DBG_REASON_DBGRQ },
	{ .name = "breakpoint",                .value = DBG_REASON_BREAKPOINT },
	{ .name = "watchpoint",                .value = DBG_REASON_WATCHPOINT },
	{ .name = "watchpoint-and-breakpoint", .value = DBG_REASON_WPTANDBKPT },
	{ .name = "single-step",               .value = DBG_REASON_SINGLESTEP },
	{ .name = "target-not-halted",         .value = DBG_REASON_NOTHALTED  },
	{ .name = "program-exit",              .value = DBG_REASON_EXIT },
	{ .name = "exception-catch",           .value = DBG_REASON_EXC_CATCH },
	{ .name = "undefined",                 .value = DBG_REASON_UNDEFINED },
	{ .name = NULL, .value = -1 },
};

static const struct nvp nvp_target_endian[] = {
	{ .name = "big",    .value = TARGET_BIG_ENDIAN },
	{ .name = "little", .value = TARGET_LITTLE_ENDIAN },
	{ .name = "be",     .value = TARGET_BIG_ENDIAN },
	{ .name = "le",     .value = TARGET_LITTLE_ENDIAN },
	{ .name = NULL,     .value = -1 },
};

static const struct nvp nvp_reset_modes[] = {
	{ .name = "unknown", .value = RESET_UNKNOWN },
	{ .name = "run",     .value = RESET_RUN },
	{ .name = "halt",    .value = RESET_HALT },
	{ .name = "init",    .value = RESET_INIT },
	{ .name = NULL,      .value = -1 },
};

const char *debug_reason_name(const struct target *t)
{
	const char *cp;

	cp = nvp_value2name(nvp_target_debug_reason,
			t->debug_reason)->name;
	if (!cp) {
		LOG_ERROR("Invalid debug reason: %d", (int)(t->debug_reason));
		cp = "(*BUG*unknown*BUG*)";
	}
	return cp;
}

const char *target_state_name(const struct target *t)
{
	const char *cp;
	cp = nvp_value2name(nvp_target_state, t->state)->name;
	if (!cp) {
		LOG_ERROR("Invalid target state: %d", (int)(t->state));
		cp = "(*BUG*unknown*BUG*)";
	}

	if (!target_was_examined(t) && t->defer_examine)
		cp = "examine deferred";

	return cp;
}

const char *target_event_name(enum target_event event)
{
	const char *cp;
	cp = nvp_value2name(nvp_target_event, event)->name;
	if (!cp) {
		LOG_ERROR("Invalid target event: %d", (int)(event));
		cp = "(*BUG*unknown*BUG*)";
	}
	return cp;
}

const char *target_reset_mode_name(enum target_reset_mode reset_mode)
{
	const char *cp;
	cp = nvp_value2name(nvp_reset_modes, reset_mode)->name;
	if (!cp) {
		LOG_ERROR("Invalid target reset mode: %d", (int)(reset_mode));
		cp = "(*BUG*unknown*BUG*)";
	}
	return cp;
}

static void append_to_list_all_targets(struct target *target)
{
	struct target **t = &all_targets;

	while (*t)
		t = &((*t)->next);
	*t = target;
}

/* read a uint64_t from a buffer in target memory endianness */
uint64_t target_buffer_get_u64(struct target *target, const uint8_t *buffer)
{
	if (target->endianness == TARGET_LITTLE_ENDIAN)
		return le_to_h_u64(buffer);
	else
		return be_to_h_u64(buffer);
}

/* read a uint32_t from a buffer in target memory endianness */
uint32_t target_buffer_get_u32(struct target *target, const uint8_t *buffer)
{
	if (target->endianness == TARGET_LITTLE_ENDIAN)
		return le_to_h_u32(buffer);
	else
		return be_to_h_u32(buffer);
}

/* read a uint24_t from a buffer in target memory endianness */
uint32_t target_buffer_get_u24(struct target *target, const uint8_t *buffer)
{
	if (target->endianness == TARGET_LITTLE_ENDIAN)
		return le_to_h_u24(buffer);
	else
		return be_to_h_u24(buffer);
}

/* read a uint16_t from a buffer in target memory endianness */
uint16_t target_buffer_get_u16(struct target *target, const uint8_t *buffer)
{
	if (target->endianness == TARGET_LITTLE_ENDIAN)
		return le_to_h_u16(buffer);
	else
		return be_to_h_u16(buffer);
}

/* write a uint64_t to a buffer in target memory endianness */
void target_buffer_set_u64(struct target *target, uint8_t *buffer, uint64_t value)
{
	if (target->endianness == TARGET_LITTLE_ENDIAN)
		h_u64_to_le(buffer, value);
	else
		h_u64_to_be(buffer, value);
}

/* write a uint32_t to a buffer in target memory endianness */
void target_buffer_set_u32(struct target *target, uint8_t *buffer, uint32_t value)
{
	if (target->endianness == TARGET_LITTLE_ENDIAN)
		h_u32_to_le(buffer, value);
	else
		h_u32_to_be(buffer, value);
}

/* write a uint24_t to a buffer in target memory endianness */
void target_buffer_set_u24(struct target *target, uint8_t *buffer, uint32_t value)
{
	if (target->endianness == TARGET_LITTLE_ENDIAN)
		h_u24_to_le(buffer, value);
	else
		h_u24_to_be(buffer, value);
}

/* write a uint16_t to a buffer in target memory endianness */
void target_buffer_set_u16(struct target *target, uint8_t *buffer, uint16_t value)
{
	if (target->endianness == TARGET_LITTLE_ENDIAN)
		h_u16_to_le(buffer, value);
	else
		h_u16_to_be(buffer, value);
}

/* write a uint8_t to a buffer in target memory endianness */
static void target_buffer_set_u8(struct target *target, uint8_t *buffer, uint8_t value)
{
	*buffer = value;
}

/* write a uint64_t array to a buffer in target memory endianness */
void target_buffer_get_u64_array(struct target *target, const uint8_t *buffer, uint32_t count, uint64_t *dstbuf)
{
	uint32_t i;
	for (i = 0; i < count; i++)
		dstbuf[i] = target_buffer_get_u64(target, &buffer[i * 8]);
}

/* write a uint32_t array to a buffer in target memory endianness */
void target_buffer_get_u32_array(struct target *target, const uint8_t *buffer, uint32_t count, uint32_t *dstbuf)
{
	uint32_t i;
	for (i = 0; i < count; i++)
		dstbuf[i] = target_buffer_get_u32(target, &buffer[i * 4]);
}

/* write a uint16_t array to a buffer in target memory endianness */
void target_buffer_get_u16_array(struct target *target, const uint8_t *buffer, uint32_t count, uint16_t *dstbuf)
{
	uint32_t i;
	for (i = 0; i < count; i++)
		dstbuf[i] = target_buffer_get_u16(target, &buffer[i * 2]);
}

/* write a uint64_t array to a buffer in target memory endianness */
void target_buffer_set_u64_array(struct target *target, uint8_t *buffer, uint32_t count, const uint64_t *srcbuf)
{
	uint32_t i;
	for (i = 0; i < count; i++)
		target_buffer_set_u64(target, &buffer[i * 8], srcbuf[i]);
}

/* write a uint32_t array to a buffer in target memory endianness */
void target_buffer_set_u32_array(struct target *target, uint8_t *buffer, uint32_t count, const uint32_t *srcbuf)
{
	uint32_t i;
	for (i = 0; i < count; i++)
		target_buffer_set_u32(target, &buffer[i * 4], srcbuf[i]);
}

/* write a uint16_t array to a buffer in target memory endianness */
void target_buffer_set_u16_array(struct target *target, uint8_t *buffer, uint32_t count, const uint16_t *srcbuf)
{
	uint32_t i;
	for (i = 0; i < count; i++)
		target_buffer_set_u16(target, &buffer[i * 2], srcbuf[i]);
}

/* return a pointer to a configured target; id is name or index in all_targets */
struct target *get_target(const char *id)
{
	struct target *target;

	/* try as tcltarget name */
	for (target = all_targets; target; target = target->next) {
		if (!target_name(target))
			continue;
		if (strcmp(id, target_name(target)) == 0)
			return target;
	}

	/* try as index */
	unsigned int index, counter;
	if (parse_uint(id, &index) != ERROR_OK)
		return NULL;

	for (target = all_targets, counter = index;
			target && counter;
			target = target->next, --counter)
		;

	return target;
}

struct target *get_current_target(struct command_context *cmd_ctx)
{
	struct target *target = get_current_target_or_null(cmd_ctx);

	if (!target) {
		LOG_ERROR("BUG: current_target out of bounds");
		exit(-1);
	}

	return target;
}

struct target *get_current_target_or_null(struct command_context *cmd_ctx)
{
	return cmd_ctx->current_target_override
		? cmd_ctx->current_target_override
		: cmd_ctx->current_target;
}

int target_poll(struct target *target)
{
	int retval;

	/* We can't poll until after examine */
	if (!target_was_examined(target)) {
		/* Fail silently lest we pollute the log */
		return ERROR_FAIL;
	}

	retval = target->type->poll(target);
	if (retval != ERROR_OK)
		return retval;

	if (target->halt_issued) {
		if (target->state == TARGET_HALTED)
			target->halt_issued = false;
		else {
			int64_t t = timeval_ms() - target->halt_issued_time;
			if (t > DEFAULT_HALT_TIMEOUT) {
				target->halt_issued = false;
				LOG_INFO("Halt timed out, wake up GDB.");
				target_call_event_callbacks(target, TARGET_EVENT_GDB_HALT);
			}
		}
	}

	return ERROR_OK;
}

int target_halt(struct target *target)
{
	int retval;
	/* We can't poll until after examine */
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	retval = target->type->halt(target);
	if (retval != ERROR_OK)
		return retval;

	target->halt_issued = true;
	target->halt_issued_time = timeval_ms();

	return ERROR_OK;
}

/**
 * Make the target (re)start executing using its saved execution
 * context (possibly with some modifications).
 *
 * @param target Which target should start executing.
 * @param current True to use the target's saved program counter instead
 *	of the address parameter
 * @param address Optionally used as the program counter.
 * @param handle_breakpoints True iff breakpoints at the resumption PC
 *	should be skipped.  (For example, maybe execution was stopped by
 *	such a breakpoint, in which case it would be counterproductive to
 *	let it re-trigger.
 * @param debug_execution False if all working areas allocated by OpenOCD
 *	should be released and/or restored to their original contents.
 *	(This would for example be true to run some downloaded "helper"
 *	algorithm code, which resides in one such working buffer and uses
 *	another for data storage.)
 *
 * @todo Resolve the ambiguity about what the "debug_execution" flag
 * signifies.  For example, Target implementations don't agree on how
 * it relates to invalidation of the register cache, or to whether
 * breakpoints and watchpoints should be enabled.  (It would seem wrong
 * to enable breakpoints when running downloaded "helper" algorithms
 * (debug_execution true), since the breakpoints would be set to match
 * target firmware being debugged, not the helper algorithm.... and
 * enabling them could cause such helpers to malfunction (for example,
 * by overwriting data with a breakpoint instruction.  On the other
 * hand the infrastructure for running such helpers might use this
 * procedure but rely on hardware breakpoint to detect termination.)
 */
int target_resume(struct target *target, bool current, target_addr_t address,
		bool handle_breakpoints, bool debug_execution)
{
	int retval;

	/* We can't poll until after examine */
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	target_call_event_callbacks(target, TARGET_EVENT_RESUME_START);

	/* note that resume *must* be asynchronous. The CPU can halt before
	 * we poll. The CPU can even halt at the current PC as a result of
	 * a software breakpoint being inserted by (a bug?) the application.
	 */
	/*
	 * resume() triggers the event 'resumed'. The execution of TCL commands
	 * in the event handler causes the polling of targets. If the target has
	 * already halted for a breakpoint, polling will run the 'halted' event
	 * handler before the pending 'resumed' handler.
	 * Disable polling during resume() to guarantee the execution of handlers
	 * in the correct order.
	 */
	bool save_poll_mask = jtag_poll_mask();
	retval = target->type->resume(target, current, address, handle_breakpoints,
		debug_execution);
	jtag_poll_unmask(save_poll_mask);

	if (retval != ERROR_OK)
		return retval;

	target_call_event_callbacks(target, TARGET_EVENT_RESUME_END);

	return retval;
}

static int target_process_reset(struct command_invocation *cmd, enum target_reset_mode reset_mode)
{
	char buf[100];
	int retval;
	const struct nvp *n;
	n = nvp_value2name(nvp_reset_modes, reset_mode);
	if (!n->name) {
		LOG_ERROR("invalid reset mode");
		return ERROR_FAIL;
	}

	struct target *target;
	for (target = all_targets; target; target = target->next)
		target_call_reset_callbacks(target, reset_mode);

	/* disable polling during reset to make reset event scripts
	 * more predictable, i.e. dr/irscan & pathmove in events will
	 * not have JTAG operations injected into the middle of a sequence.
	 */
	bool save_poll_mask = jtag_poll_mask();

	sprintf(buf, "ocd_process_reset %s", n->name);
	retval = Jim_Eval(cmd->ctx->interp, buf);

	jtag_poll_unmask(save_poll_mask);

	if (retval != JIM_OK) {
		Jim_MakeErrorMessage(cmd->ctx->interp);
		command_print(cmd, "%s", Jim_GetString(Jim_GetResult(cmd->ctx->interp), NULL));
		return ERROR_FAIL;
	}

	/* We want any events to be processed before the prompt */
	retval = target_call_timer_callbacks_now();

	for (target = all_targets; target; target = target->next) {
		target->type->check_reset(target);
		target->running_alg = false;
	}

	return retval;
}

static int identity_virt2phys(struct target *target,
		target_addr_t virtual, target_addr_t *physical)
{
	*physical = virtual;
	return ERROR_OK;
}

static int no_mmu(struct target *target, int *enabled)
{
	*enabled = 0;
	return ERROR_OK;
}

/**
 * Reset the @c examined flag for the given target.
 * Pure paranoia -- targets are zeroed on allocation.
 */
static inline void target_reset_examined(struct target *target)
{
	target->examined = false;
}

static int default_examine(struct target *target)
{
	target_set_examined(target);
	return ERROR_OK;
}

/* no check by default */
static int default_check_reset(struct target *target)
{
	return ERROR_OK;
}

/* Equivalent Tcl code arp_examine_one is in src/target/startup.tcl
 * Keep in sync */
int target_examine_one(struct target *target)
{
	LOG_TARGET_DEBUG(target, "Examination started");

	target_call_event_callbacks(target, TARGET_EVENT_EXAMINE_START);

	int retval = target->type->examine(target);
	if (retval != ERROR_OK) {
		LOG_TARGET_ERROR(target, "Examination failed");
		LOG_TARGET_DEBUG(target, "examine() returned error code %d", retval);
		target_reset_examined(target);
		target_call_event_callbacks(target, TARGET_EVENT_EXAMINE_FAIL);
		return retval;
	}

	target_set_examined(target);
	target_call_event_callbacks(target, TARGET_EVENT_EXAMINE_END);

	LOG_TARGET_INFO(target, "Examination succeed");
	return ERROR_OK;
}

static int jtag_enable_callback(enum jtag_event event, void *priv)
{
	struct target *target = priv;

	if (event != JTAG_TAP_EVENT_ENABLE || !target->tap->enabled)
		return ERROR_OK;

	jtag_unregister_event_callback(jtag_enable_callback, target);

	return target_examine_one(target);
}

/* Targets that correctly implement init + examine, i.e.
 * no communication with target during init:
 *
 * XScale
 */
int target_examine(void)
{
	int retval = ERROR_OK;
	struct target *target;

	for (target = all_targets; target; target = target->next) {
		/* defer examination, but don't skip it */
		if (!target->tap->enabled) {
			jtag_register_event_callback(jtag_enable_callback,
					target);
			continue;
		}

		if (target->defer_examine)
			continue;

		int retval2 = target_examine_one(target);
		if (retval2 != ERROR_OK) {
			LOG_WARNING("target %s examination failed", target_name(target));
			retval = retval2;
		}
	}
	return retval;
}

const char *target_type_name(const struct target *target)
{
	return target->type->name;
}

static int target_soft_reset_halt(struct target *target)
{
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}
	if (!target->type->soft_reset_halt) {
		LOG_ERROR("Target %s does not support soft_reset_halt",
				target_name(target));
		return ERROR_FAIL;
	}
	return target->type->soft_reset_halt(target);
}

/**
 * Downloads a target-specific native code algorithm to the target,
 * and executes it.  * Note that some targets may need to set up, enable,
 * and tear down a breakpoint (hard or * soft) to detect algorithm
 * termination, while others may support  lower overhead schemes where
 * soft breakpoints embedded in the algorithm automatically terminate the
 * algorithm.
 *
 * @param target used to run the algorithm
 * @param num_mem_params
 * @param mem_params
 * @param num_reg_params
 * @param reg_param
 * @param entry_point
 * @param exit_point
 * @param timeout_ms
 * @param arch_info target-specific description of the algorithm.
 */
int target_run_algorithm(struct target *target,
		int num_mem_params, struct mem_param *mem_params,
		int num_reg_params, struct reg_param *reg_param,
		target_addr_t entry_point, target_addr_t exit_point,
		unsigned int timeout_ms, void *arch_info)
{
	int retval = ERROR_FAIL;

	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		goto done;
	}
	if (!target->type->run_algorithm) {
		LOG_ERROR("Target type '%s' does not support %s",
				target_type_name(target), __func__);
		goto done;
	}

	target->running_alg = true;
	retval = target->type->run_algorithm(target,
			num_mem_params, mem_params,
			num_reg_params, reg_param,
			entry_point, exit_point, timeout_ms, arch_info);
	target->running_alg = false;

done:
	return retval;
}

/**
 * Executes a target-specific native code algorithm and leaves it running.
 *
 * @param target used to run the algorithm
 * @param num_mem_params
 * @param mem_params
 * @param num_reg_params
 * @param reg_params
 * @param entry_point
 * @param exit_point
 * @param arch_info target-specific description of the algorithm.
 */
int target_start_algorithm(struct target *target,
		int num_mem_params, struct mem_param *mem_params,
		int num_reg_params, struct reg_param *reg_params,
		target_addr_t entry_point, target_addr_t exit_point,
		void *arch_info)
{
	int retval = ERROR_FAIL;

	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		goto done;
	}
	if (!target->type->start_algorithm) {
		LOG_ERROR("Target type '%s' does not support %s",
				target_type_name(target), __func__);
		goto done;
	}
	if (target->running_alg) {
		LOG_ERROR("Target is already running an algorithm");
		goto done;
	}

	target->running_alg = true;
	retval = target->type->start_algorithm(target,
			num_mem_params, mem_params,
			num_reg_params, reg_params,
			entry_point, exit_point, arch_info);

done:
	return retval;
}

/**
 * Waits for an algorithm started with target_start_algorithm() to complete.
 *
 * @param target used to run the algorithm
 * @param num_mem_params
 * @param mem_params
 * @param num_reg_params
 * @param reg_params
 * @param exit_point
 * @param timeout_ms
 * @param arch_info target-specific description of the algorithm.
 */
int target_wait_algorithm(struct target *target,
		int num_mem_params, struct mem_param *mem_params,
		int num_reg_params, struct reg_param *reg_params,
		target_addr_t exit_point, unsigned int timeout_ms,
		void *arch_info)
{
	int retval = ERROR_FAIL;

	if (!target->type->wait_algorithm) {
		LOG_ERROR("Target type '%s' does not support %s",
				target_type_name(target), __func__);
		goto done;
	}
	if (!target->running_alg) {
		LOG_ERROR("Target is not running an algorithm");
		goto done;
	}

	retval = target->type->wait_algorithm(target,
			num_mem_params, mem_params,
			num_reg_params, reg_params,
			exit_point, timeout_ms, arch_info);
	if (retval != ERROR_TARGET_TIMEOUT)
		target->running_alg = false;

done:
	return retval;
}

/**
 * Streams data to a circular buffer on target intended for consumption by code
 * running asynchronously on target.
 *
 * This is intended for applications where target-specific native code runs
 * on the target, receives data from the circular buffer, does something with
 * it (most likely writing it to a flash memory), and advances the circular
 * buffer pointer.
 *
 * This assumes that the helper algorithm has already been loaded to the target,
 * but has not been started yet. Given memory and register parameters are passed
 * to the algorithm.
 *
 * The buffer is defined by (buffer_start, buffer_size) arguments and has the
 * following format:
 *
 *     [buffer_start + 0, buffer_start + 4):
 *         Write Pointer address (aka head). Written and updated by this
 *         routine when new data is written to the circular buffer.
 *     [buffer_start + 4, buffer_start + 8):
 *         Read Pointer address (aka tail). Updated by code running on the
 *         target after it consumes data.
 *     [buffer_start + 8, buffer_start + buffer_size):
 *         Circular buffer contents.
 *
 * See contrib/loaders/flash/stm32f1x.S for an example.
 *
 * @param target used to run the algorithm
 * @param buffer address on the host where data to be sent is located
 * @param count number of blocks to send
 * @param block_size size in bytes of each block
 * @param num_mem_params count of memory-based params to pass to algorithm
 * @param mem_params memory-based params to pass to algorithm
 * @param num_reg_params count of register-based params to pass to algorithm
 * @param reg_params memory-based params to pass to algorithm
 * @param buffer_start address on the target of the circular buffer structure
 * @param buffer_size size of the circular buffer structure
 * @param entry_point address on the target to execute to start the algorithm
 * @param exit_point address at which to set a breakpoint to catch the
 *     end of the algorithm; can be 0 if target triggers a breakpoint itself
 * @param arch_info
 */

int target_run_flash_async_algorithm(struct target *target,
		const uint8_t *buffer, uint32_t count, int block_size,
		int num_mem_params, struct mem_param *mem_params,
		int num_reg_params, struct reg_param *reg_params,
		uint32_t buffer_start, uint32_t buffer_size,
		uint32_t entry_point, uint32_t exit_point, void *arch_info)
{
	int retval;
	int timeout = 0;

	const uint8_t *buffer_orig = buffer;

	/* Set up working area. First word is write pointer, second word is read pointer,
	 * rest is fifo data area. */
	uint32_t wp_addr = buffer_start;
	uint32_t rp_addr = buffer_start + 4;
	uint32_t fifo_start_addr = buffer_start + 8;
	uint32_t fifo_end_addr = buffer_start + buffer_size;

	uint32_t wp = fifo_start_addr;
	uint32_t rp = fifo_start_addr;

	/* validate block_size is 2^n */
	assert(IS_PWR_OF_2(block_size));

	retval = target_write_u32(target, wp_addr, wp);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, rp_addr, rp);
	if (retval != ERROR_OK)
		return retval;

	/* Start up algorithm on target and let it idle while writing the first chunk */
	retval = target_start_algorithm(target, num_mem_params, mem_params,
			num_reg_params, reg_params,
			entry_point,
			exit_point,
			arch_info);

	if (retval != ERROR_OK) {
		LOG_ERROR("error starting target flash write algorithm");
		return retval;
	}

	while (count > 0) {

		retval = target_read_u32(target, rp_addr, &rp);
		if (retval != ERROR_OK) {
			LOG_ERROR("failed to get read pointer");
			break;
		}

		LOG_DEBUG("offs 0x%zx count 0x%" PRIx32 " wp 0x%" PRIx32 " rp 0x%" PRIx32,
			(size_t) (buffer - buffer_orig), count, wp, rp);

		if (rp == 0) {
			LOG_ERROR("flash write algorithm aborted by target");
			retval = ERROR_FLASH_OPERATION_FAILED;
			break;
		}

		if (!IS_ALIGNED(rp - fifo_start_addr, block_size) || rp < fifo_start_addr || rp >= fifo_end_addr) {
			LOG_ERROR("corrupted fifo read pointer 0x%" PRIx32, rp);
			break;
		}

		/* Count the number of bytes available in the fifo without
		 * crossing the wrap around. Make sure to not fill it completely,
		 * because that would make wp == rp and that's the empty condition. */
		uint32_t thisrun_bytes;
		if (rp > wp)
			thisrun_bytes = rp - wp - block_size;
		else if (rp > fifo_start_addr)
			thisrun_bytes = fifo_end_addr - wp;
		else
			thisrun_bytes = fifo_end_addr - wp - block_size;

		if (thisrun_bytes == 0) {
			/* Throttle polling a bit if transfer is (much) faster than flash
			 * programming. The exact delay shouldn't matter as long as it's
			 * less than buffer size / flash speed. This is very unlikely to
			 * run when using high latency connections such as USB. */
			alive_sleep(2);

			/* to stop an infinite loop on some targets check and increment a timeout
			 * this issue was observed on a stellaris using the new ICDI interface */
			if (timeout++ >= 2500) {
				LOG_ERROR("timeout waiting for algorithm, a target reset is recommended");
				return ERROR_FLASH_OPERATION_FAILED;
			}
			continue;
		}

		/* reset our timeout */
		timeout = 0;

		/* Limit to the amount of data we actually want to write */
		if (thisrun_bytes > count * block_size)
			thisrun_bytes = count * block_size;

		/* Force end of large blocks to be word aligned */
		if (thisrun_bytes >= 16)
			thisrun_bytes -= (rp + thisrun_bytes) & 0x03;

		/* Write data to fifo */
		retval = target_write_buffer(target, wp, thisrun_bytes, buffer);
		if (retval != ERROR_OK)
			break;

		/* Update counters and wrap write pointer */
		buffer += thisrun_bytes;
		count -= thisrun_bytes / block_size;
		wp += thisrun_bytes;
		if (wp >= fifo_end_addr)
			wp = fifo_start_addr;

		/* Store updated write pointer to target */
		retval = target_write_u32(target, wp_addr, wp);
		if (retval != ERROR_OK)
			break;

		/* Avoid GDB timeouts */
		keep_alive();
	}

	if (retval != ERROR_OK) {
		/* abort flash write algorithm on target */
		target_write_u32(target, wp_addr, 0);
	}

	int retval2 = target_wait_algorithm(target, num_mem_params, mem_params,
			num_reg_params, reg_params,
			exit_point,
			10000,
			arch_info);

	if (retval2 != ERROR_OK) {
		LOG_ERROR("error waiting for target flash write algorithm");
		retval = retval2;
	}

	if (retval == ERROR_OK) {
		/* check if algorithm set rp = 0 after fifo writer loop finished */
		retval = target_read_u32(target, rp_addr, &rp);
		if (retval == ERROR_OK && rp == 0) {
			LOG_ERROR("flash write algorithm aborted by target");
			retval = ERROR_FLASH_OPERATION_FAILED;
		}
	}

	return retval;
}

int target_run_read_async_algorithm(struct target *target,
		uint8_t *buffer, uint32_t count, int block_size,
		int num_mem_params, struct mem_param *mem_params,
		int num_reg_params, struct reg_param *reg_params,
		uint32_t buffer_start, uint32_t buffer_size,
		uint32_t entry_point, uint32_t exit_point, void *arch_info)
{
	int retval;
	int timeout = 0;

	const uint8_t *buffer_orig = buffer;

	/* Set up working area. First word is write pointer, second word is read pointer,
	 * rest is fifo data area. */
	uint32_t wp_addr = buffer_start;
	uint32_t rp_addr = buffer_start + 4;
	uint32_t fifo_start_addr = buffer_start + 8;
	uint32_t fifo_end_addr = buffer_start + buffer_size;

	uint32_t wp = fifo_start_addr;
	uint32_t rp = fifo_start_addr;

	/* validate block_size is 2^n */
	assert(IS_PWR_OF_2(block_size));

	retval = target_write_u32(target, wp_addr, wp);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, rp_addr, rp);
	if (retval != ERROR_OK)
		return retval;

	/* Start up algorithm on target */
	retval = target_start_algorithm(target, num_mem_params, mem_params,
			num_reg_params, reg_params,
			entry_point,
			exit_point,
			arch_info);

	if (retval != ERROR_OK) {
		LOG_ERROR("error starting target flash read algorithm");
		return retval;
	}

	while (count > 0) {
		retval = target_read_u32(target, wp_addr, &wp);
		if (retval != ERROR_OK) {
			LOG_ERROR("failed to get write pointer");
			break;
		}

		LOG_DEBUG("offs 0x%zx count 0x%" PRIx32 " wp 0x%" PRIx32 " rp 0x%" PRIx32,
			(size_t)(buffer - buffer_orig), count, wp, rp);

		if (wp == 0) {
			LOG_ERROR("flash read algorithm aborted by target");
			retval = ERROR_FLASH_OPERATION_FAILED;
			break;
		}

		if (!IS_ALIGNED(wp - fifo_start_addr, block_size) || wp < fifo_start_addr || wp >= fifo_end_addr) {
			LOG_ERROR("corrupted fifo write pointer 0x%" PRIx32, wp);
			break;
		}

		/* Count the number of bytes available in the fifo without
		 * crossing the wrap around. */
		uint32_t thisrun_bytes;
		if (wp >= rp)
			thisrun_bytes = wp - rp;
		else
			thisrun_bytes = fifo_end_addr - rp;

		if (thisrun_bytes == 0) {
			/* Throttle polling a bit if transfer is (much) faster than flash
			 * reading. The exact delay shouldn't matter as long as it's
			 * less than buffer size / flash speed. This is very unlikely to
			 * run when using high latency connections such as USB. */
			alive_sleep(2);

			/* to stop an infinite loop on some targets check and increment a timeout
			 * this issue was observed on a stellaris using the new ICDI interface */
			if (timeout++ >= 2500) {
				LOG_ERROR("timeout waiting for algorithm, a target reset is recommended");
				return ERROR_FLASH_OPERATION_FAILED;
			}
			continue;
		}

		/* Reset our timeout */
		timeout = 0;

		/* Limit to the amount of data we actually want to read */
		if (thisrun_bytes > count * block_size)
			thisrun_bytes = count * block_size;

		/* Force end of large blocks to be word aligned */
		if (thisrun_bytes >= 16)
			thisrun_bytes -= (rp + thisrun_bytes) & 0x03;

		/* Read data from fifo */
		retval = target_read_buffer(target, rp, thisrun_bytes, buffer);
		if (retval != ERROR_OK)
			break;

		/* Update counters and wrap write pointer */
		buffer += thisrun_bytes;
		count -= thisrun_bytes / block_size;
		rp += thisrun_bytes;
		if (rp >= fifo_end_addr)
			rp = fifo_start_addr;

		/* Store updated write pointer to target */
		retval = target_write_u32(target, rp_addr, rp);
		if (retval != ERROR_OK)
			break;

		/* Avoid GDB timeouts */
		keep_alive();

		if (openocd_is_shutdown_pending()) {
			retval = ERROR_SERVER_INTERRUPTED;
			break;
		}
	}

	if (retval != ERROR_OK) {
		/* abort flash write algorithm on target */
		target_write_u32(target, rp_addr, 0);
	}

	int retval2 = target_wait_algorithm(target, num_mem_params, mem_params,
			num_reg_params, reg_params,
			exit_point,
			10000,
			arch_info);

	if (retval2 != ERROR_OK) {
		LOG_ERROR("error waiting for target flash write algorithm");
		retval = retval2;
	}

	if (retval == ERROR_OK) {
		/* check if algorithm set wp = 0 after fifo writer loop finished */
		retval = target_read_u32(target, wp_addr, &wp);
		if (retval == ERROR_OK && wp == 0) {
			LOG_ERROR("flash read algorithm aborted by target");
			retval = ERROR_FLASH_OPERATION_FAILED;
		}
	}

	return retval;
}

int target_read_memory(struct target *target,
		target_addr_t address, uint32_t size, uint32_t count, uint8_t *buffer)
{
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}
	if (!target->type->read_memory) {
		LOG_ERROR("Target %s doesn't support read_memory", target_name(target));
		return ERROR_FAIL;
	}
	return target->type->read_memory(target, address, size, count, buffer);
}

int target_read_phys_memory(struct target *target,
		target_addr_t address, uint32_t size, uint32_t count, uint8_t *buffer)
{
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}
	if (!target->type->read_phys_memory) {
		LOG_ERROR("Target %s doesn't support read_phys_memory", target_name(target));
		return ERROR_FAIL;
	}
	return target->type->read_phys_memory(target, address, size, count, buffer);
}

int target_write_memory(struct target *target,
		target_addr_t address, uint32_t size, uint32_t count, const uint8_t *buffer)
{
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}
	if (!target->type->write_memory) {
		LOG_ERROR("Target %s doesn't support write_memory", target_name(target));
		return ERROR_FAIL;
	}
	return target->type->write_memory(target, address, size, count, buffer);
}

int target_write_phys_memory(struct target *target,
		target_addr_t address, uint32_t size, uint32_t count, const uint8_t *buffer)
{
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}
	if (!target->type->write_phys_memory) {
		LOG_ERROR("Target %s doesn't support write_phys_memory", target_name(target));
		return ERROR_FAIL;
	}
	return target->type->write_phys_memory(target, address, size, count, buffer);
}

int target_add_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	if ((target->state != TARGET_HALTED) && (breakpoint->type != BKPT_HARD)) {
		LOG_TARGET_ERROR(target, "not halted (add breakpoint)");
		return ERROR_TARGET_NOT_HALTED;
	}
	return target->type->add_breakpoint(target, breakpoint);
}

int target_add_context_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "not halted (add context breakpoint)");
		return ERROR_TARGET_NOT_HALTED;
	}
	return target->type->add_context_breakpoint(target, breakpoint);
}

int target_add_hybrid_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "not halted (add hybrid breakpoint)");
		return ERROR_TARGET_NOT_HALTED;
	}
	return target->type->add_hybrid_breakpoint(target, breakpoint);
}

int target_remove_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	return target->type->remove_breakpoint(target, breakpoint);
}

int target_add_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "not halted (add watchpoint)");
		return ERROR_TARGET_NOT_HALTED;
	}
	return target->type->add_watchpoint(target, watchpoint);
}
int target_remove_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	return target->type->remove_watchpoint(target, watchpoint);
}
int target_hit_watchpoint(struct target *target,
		struct watchpoint **hit_watchpoint)
{
	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "not halted (hit watchpoint)");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!target->type->hit_watchpoint) {
		/* For backward compatible, if hit_watchpoint is not implemented,
		 * return ERROR_FAIL such that gdb_server will not take the nonsense
		 * information. */
		return ERROR_FAIL;
	}

	return target->type->hit_watchpoint(target, hit_watchpoint);
}

const char *target_get_gdb_arch(const struct target *target)
{
	if (!target->type->get_gdb_arch)
		return NULL;
	return target->type->get_gdb_arch(target);
}

int target_get_gdb_reg_list(struct target *target,
		struct reg **reg_list[], int *reg_list_size,
		enum target_register_class reg_class)
{
	int result = ERROR_FAIL;

	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		goto done;
	}

	result = target->type->get_gdb_reg_list(target, reg_list,
			reg_list_size, reg_class);

done:
	if (result != ERROR_OK) {
		*reg_list = NULL;
		*reg_list_size = 0;
	}
	return result;
}

int target_get_gdb_reg_list_noread(struct target *target,
		struct reg **reg_list[], int *reg_list_size,
		enum target_register_class reg_class)
{
	if (target->type->get_gdb_reg_list_noread &&
			target->type->get_gdb_reg_list_noread(target, reg_list,
				reg_list_size, reg_class) == ERROR_OK)
		return ERROR_OK;
	return target_get_gdb_reg_list(target, reg_list, reg_list_size, reg_class);
}

bool target_supports_gdb_connection(const struct target *target)
{
	/*
	 * exclude all the targets that don't provide get_gdb_reg_list
	 * or that have explicit gdb_max_connection == 0
	 */
	return !!target->type->get_gdb_reg_list && !!target->gdb_max_connections;
}

int target_step(struct target *target,
		bool current, target_addr_t address, bool handle_breakpoints)
{
	int retval;

	target_call_event_callbacks(target, TARGET_EVENT_STEP_START);

	retval = target->type->step(target, current, address, handle_breakpoints);
	if (retval != ERROR_OK)
		return retval;

	target_call_event_callbacks(target, TARGET_EVENT_STEP_END);

	return retval;
}

int target_get_gdb_fileio_info(struct target *target, struct gdb_fileio_info *fileio_info)
{
	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "not halted (gdb fileio)");
		return ERROR_TARGET_NOT_HALTED;
	}
	return target->type->get_gdb_fileio_info(target, fileio_info);
}

int target_gdb_fileio_end(struct target *target, int retcode, int fileio_errno, bool ctrl_c)
{
	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "not halted (gdb fileio end)");
		return ERROR_TARGET_NOT_HALTED;
	}
	return target->type->gdb_fileio_end(target, retcode, fileio_errno, ctrl_c);
}

target_addr_t target_address_max(struct target *target)
{
	unsigned int bits = target_address_bits(target);
	if (sizeof(target_addr_t) * 8 == bits)
		return (target_addr_t) -1;
	else
		return (((target_addr_t) 1) << bits) - 1;
}

unsigned int target_address_bits(struct target *target)
{
	if (target->type->address_bits)
		return target->type->address_bits(target);
	return 32;
}

unsigned int target_data_bits(struct target *target)
{
	if (target->type->data_bits)
		return target->type->data_bits(target);
	return 32;
}

int target_add_memory_region(struct target_memory_map *map, const struct target_memory_region *region)
{
	/* Reallocate if needed */
	if (map->num_regions >= map->capacity) {
		unsigned int new_capacity = map->capacity + 8;  /* Grow by 8 regions at a time */
		struct target_memory_region *new_regions = realloc(map->regions, new_capacity * sizeof(*new_regions));
		if (!new_regions)
			return ERROR_FAIL;

		map->regions = new_regions;
		map->capacity = new_capacity;
	}

	/* Add the new region */
	map->regions[map->num_regions].type = region->type;
	map->regions[map->num_regions].start = region->start;
	map->regions[map->num_regions].length = region->length;
	map->regions[map->num_regions].block_size = region->block_size;
	map->num_regions++;

	return ERROR_OK;
}

static int target_profiling(struct target *target, uint32_t *samples,
			uint32_t max_num_samples, uint32_t *num_samples, uint32_t seconds)
{
	return target->type->profiling(target, samples, max_num_samples,
			num_samples, seconds);
}

static int handle_target(void *priv);

static int target_init_one(struct command_context *cmd_ctx,
		struct target *target)
{
	target_reset_examined(target);

	struct target_type *type = target->type;
	if (!type->examine)
		type->examine = default_examine;

	if (!type->check_reset)
		type->check_reset = default_check_reset;

	assert(type->init_target);

	int retval = type->init_target(cmd_ctx, target);
	if (retval != ERROR_OK) {
		LOG_ERROR("target '%s' init failed", target_name(target));
		return retval;
	}

	/* Sanity-check MMU support ... stub in what we must, to help
	 * implement it in stages, but warn if we need to do so.
	 */
	if (type->mmu) {
		if (!type->virt2phys) {
			LOG_ERROR("type '%s' is missing virt2phys", target_name(target));
			type->virt2phys = identity_virt2phys;
		}
	} else {
		/* Make sure no-MMU targets all behave the same:  make no
		 * distinction between physical and virtual addresses, and
		 * ensure that virt2phys() is always an identity mapping.
		 */
		if (type->write_phys_memory || type->read_phys_memory || type->virt2phys)
			LOG_WARNING("type '%s' has bad MMU hooks", target_name(target));

		type->mmu = no_mmu;
		type->write_phys_memory = type->write_memory;
		type->read_phys_memory = type->read_memory;
		type->virt2phys = identity_virt2phys;
	}

	if (!target->type->read_buffer)
		target->type->read_buffer = target_read_buffer_default;

	if (!target->type->write_buffer)
		target->type->write_buffer = target_write_buffer_default;

	if (!target->type->get_gdb_fileio_info)
		target->type->get_gdb_fileio_info = target_get_gdb_fileio_info_default;

	if (!target->type->gdb_fileio_end)
		target->type->gdb_fileio_end = target_gdb_fileio_end_default;

	if (!target->type->profiling)
		target->type->profiling = target_profiling_default;

	return ERROR_OK;
}

static int target_init(struct command_context *cmd_ctx)
{
	struct target *target;
	int retval;

	for (target = all_targets; target; target = target->next) {
		retval = target_init_one(cmd_ctx, target);
		if (retval != ERROR_OK)
			return retval;
	}

	if (!all_targets)
		return ERROR_OK;

	retval = target_register_user_commands(cmd_ctx);
	if (retval != ERROR_OK)
		return retval;

	retval = target_register_timer_callback(&handle_target,
			polling_interval, TARGET_TIMER_TYPE_PERIODIC, cmd_ctx->interp);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_init_command)
{
	int retval;

	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	static bool target_initialized;
	if (target_initialized) {
		LOG_INFO("'target init' has already been called");
		return ERROR_OK;
	}
	target_initialized = true;

	retval = command_run_line(CMD_CTX, "init_targets");
	if (retval != ERROR_OK)
		return retval;

	retval = command_run_line(CMD_CTX, "init_target_events");
	if (retval != ERROR_OK)
		return retval;

	retval = command_run_line(CMD_CTX, "init_board");
	if (retval != ERROR_OK)
		return retval;

	LOG_DEBUG("Initializing targets...");
	return target_init(CMD_CTX);
}

int target_register_event_callback(int (*callback)(struct target *target,
		enum target_event event, void *priv), void *priv)
{
	struct target_event_callback **callbacks_p = &target_event_callbacks;

	if (!callback)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (*callbacks_p) {
		while ((*callbacks_p)->next)
			callbacks_p = &((*callbacks_p)->next);
		callbacks_p = &((*callbacks_p)->next);
	}

	(*callbacks_p) = malloc(sizeof(struct target_event_callback));
	(*callbacks_p)->callback = callback;
	(*callbacks_p)->priv = priv;
	(*callbacks_p)->next = NULL;

	return ERROR_OK;
}

int target_register_reset_callback(int (*callback)(struct target *target,
		enum target_reset_mode reset_mode, void *priv), void *priv)
{
	struct target_reset_callback *entry;

	if (!callback)
		return ERROR_COMMAND_SYNTAX_ERROR;

	entry = malloc(sizeof(struct target_reset_callback));
	if (!entry) {
		LOG_ERROR("error allocating buffer for reset callback entry");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	entry->callback = callback;
	entry->priv = priv;
	list_add(&entry->list, &target_reset_callback_list);


	return ERROR_OK;
}

int target_register_trace_callback(int (*callback)(struct target *target,
		size_t len, uint8_t *data, void *priv), void *priv)
{
	struct target_trace_callback *entry;

	if (!callback)
		return ERROR_COMMAND_SYNTAX_ERROR;

	entry = malloc(sizeof(struct target_trace_callback));
	if (!entry) {
		LOG_ERROR("error allocating buffer for trace callback entry");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	entry->callback = callback;
	entry->priv = priv;
	list_add(&entry->list, &target_trace_callback_list);


	return ERROR_OK;
}

int target_register_timer_callback(int (*callback)(void *priv),
		unsigned int time_ms, enum target_timer_type type, void *priv)
{
	struct target_timer_callback **callbacks_p = &target_timer_callbacks;

	if (!callback)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (*callbacks_p) {
		while ((*callbacks_p)->next)
			callbacks_p = &((*callbacks_p)->next);
		callbacks_p = &((*callbacks_p)->next);
	}

	(*callbacks_p) = malloc(sizeof(struct target_timer_callback));
	(*callbacks_p)->callback = callback;
	(*callbacks_p)->type = type;
	(*callbacks_p)->time_ms = time_ms;
	(*callbacks_p)->removed = false;

	(*callbacks_p)->when = timeval_ms() + time_ms;
	target_timer_next_event_value = MIN(target_timer_next_event_value, (*callbacks_p)->when);

	(*callbacks_p)->priv = priv;
	(*callbacks_p)->next = NULL;

	return ERROR_OK;
}

int target_unregister_event_callback(int (*callback)(struct target *target,
		enum target_event event, void *priv), void *priv)
{
	struct target_event_callback **p = &target_event_callbacks;
	struct target_event_callback *c = target_event_callbacks;

	if (!callback)
		return ERROR_COMMAND_SYNTAX_ERROR;

	while (c) {
		struct target_event_callback *next = c->next;
		if ((c->callback == callback) && (c->priv == priv)) {
			*p = next;
			free(c);
			return ERROR_OK;
		} else
			p = &(c->next);
		c = next;
	}

	return ERROR_OK;
}

int target_unregister_reset_callback(int (*callback)(struct target *target,
		enum target_reset_mode reset_mode, void *priv), void *priv)
{
	struct target_reset_callback *entry;

	if (!callback)
		return ERROR_COMMAND_SYNTAX_ERROR;

	list_for_each_entry(entry, &target_reset_callback_list, list) {
		if (entry->callback == callback && entry->priv == priv) {
			list_del(&entry->list);
			free(entry);
			break;
		}
	}

	return ERROR_OK;
}

int target_unregister_trace_callback(int (*callback)(struct target *target,
		size_t len, uint8_t *data, void *priv), void *priv)
{
	struct target_trace_callback *entry;

	if (!callback)
		return ERROR_COMMAND_SYNTAX_ERROR;

	list_for_each_entry(entry, &target_trace_callback_list, list) {
		if (entry->callback == callback && entry->priv == priv) {
			list_del(&entry->list);
			free(entry);
			break;
		}
	}

	return ERROR_OK;
}

int target_unregister_timer_callback(int (*callback)(void *priv), void *priv)
{
	if (!callback)
		return ERROR_COMMAND_SYNTAX_ERROR;

	for (struct target_timer_callback *c = target_timer_callbacks;
	     c; c = c->next) {
		if ((c->callback == callback) && (c->priv == priv)) {
			c->removed = true;
			return ERROR_OK;
		}
	}

	return ERROR_FAIL;
}

int target_call_event_callbacks(struct target *target, enum target_event event)
{
	struct target_event_callback *callback = target_event_callbacks;
	struct target_event_callback *next_callback;

	if (event == TARGET_EVENT_HALTED) {
		/* execute early halted first */
		target_call_event_callbacks(target, TARGET_EVENT_GDB_HALT);
	}

	LOG_DEBUG("target event %i (%s) for core %s", event,
			target_event_name(event),
			target_name(target));

	target_handle_event(target, event);

	while (callback) {
		next_callback = callback->next;
		callback->callback(target, event, callback->priv);
		callback = next_callback;
	}

	return ERROR_OK;
}

int target_call_reset_callbacks(struct target *target, enum target_reset_mode reset_mode)
{
	struct target_reset_callback *callback;

	LOG_DEBUG("target reset %i (%s)", reset_mode,
			nvp_value2name(nvp_reset_modes, reset_mode)->name);

	list_for_each_entry(callback, &target_reset_callback_list, list)
		callback->callback(target, reset_mode, callback->priv);

	return ERROR_OK;
}

int target_call_trace_callbacks(struct target *target, size_t len, uint8_t *data)
{
	struct target_trace_callback *callback;

	list_for_each_entry(callback, &target_trace_callback_list, list)
		callback->callback(target, len, data, callback->priv);

	return ERROR_OK;
}

static int target_timer_callback_periodic_restart(
		struct target_timer_callback *cb, int64_t *now)
{
	cb->when = *now + cb->time_ms;
	return ERROR_OK;
}

static int target_call_timer_callback(struct target_timer_callback *cb,
		int64_t *now)
{
	cb->callback(cb->priv);

	if (cb->type == TARGET_TIMER_TYPE_PERIODIC)
		return target_timer_callback_periodic_restart(cb, now);

	return target_unregister_timer_callback(cb->callback, cb->priv);
}

static int target_call_timer_callbacks_check_time(int checktime)
{
	static bool callback_processing;

	/* Do not allow nesting */
	if (callback_processing)
		return ERROR_OK;

	callback_processing = true;

	keep_alive();

	int64_t now = timeval_ms();

	/* Initialize to a default value that's a ways into the future.
	 * The loop below will make it closer to now if there are
	 * callbacks that want to be called sooner. */
	target_timer_next_event_value = now + 1000;

	/* Store an address of the place containing a pointer to the
	 * next item; initially, that's a standalone "root of the
	 * list" variable. */
	struct target_timer_callback **callback = &target_timer_callbacks;
	while (callback && *callback) {
		if ((*callback)->removed) {
			struct target_timer_callback *p = *callback;
			*callback = (*callback)->next;
			free(p);
			continue;
		}

		bool call_it = (*callback)->callback &&
			((!checktime && (*callback)->type == TARGET_TIMER_TYPE_PERIODIC) ||
			 now >= (*callback)->when);

		if (call_it)
			target_call_timer_callback(*callback, &now);

		if (!(*callback)->removed && (*callback)->when < target_timer_next_event_value)
			target_timer_next_event_value = (*callback)->when;

		callback = &(*callback)->next;
	}

	callback_processing = false;
	return ERROR_OK;
}

int target_call_timer_callbacks(void)
{
	return target_call_timer_callbacks_check_time(1);
}

/* invoke periodic callbacks immediately */
int target_call_timer_callbacks_now(void)
{
	return target_call_timer_callbacks_check_time(0);
}

int64_t target_timer_next_event(void)
{
	return target_timer_next_event_value;
}

/* Prints the working area layout for debug purposes */
static void print_wa_layout(struct target *target)
{
	struct working_area *c = target->working_areas;

	while (c) {
		LOG_DEBUG("%c%c " TARGET_ADDR_FMT "-" TARGET_ADDR_FMT " (%" PRIu32 " bytes)",
			c->backup ? 'b' : ' ', c->free ? ' ' : '*',
			c->address, c->address + c->size - 1, c->size);
		c = c->next;
	}
}

/* Reduce area to size bytes, create a new free area from the remaining bytes, if any. */
static void target_split_working_area(struct working_area *area, uint32_t size)
{
	assert(area->free); /* Shouldn't split an allocated area */
	assert(size <= area->size); /* Caller should guarantee this */

	/* Split only if not already the right size */
	if (size < area->size) {
		struct working_area *new_wa = malloc(sizeof(*new_wa));

		if (!new_wa)
			return;

		new_wa->next = area->next;
		new_wa->size = area->size - size;
		new_wa->address = area->address + size;
		new_wa->backup = NULL;
		new_wa->user = NULL;
		new_wa->free = true;

		area->next = new_wa;
		area->size = size;

		/* If backup memory was allocated to this area, it has the wrong size
		 * now so free it and it will be reallocated if/when needed */
		free(area->backup);
		area->backup = NULL;
	}
}

/* Merge all adjacent free areas into one */
static void target_merge_working_areas(struct target *target)
{
	struct working_area *c = target->working_areas;

	while (c && c->next) {
		assert(c->next->address == c->address + c->size); /* This is an invariant */

		/* Find two adjacent free areas */
		if (c->free && c->next->free) {
			/* Merge the last into the first */
			c->size += c->next->size;

			/* Remove the last */
			struct working_area *to_be_freed = c->next;
			c->next = c->next->next;
			free(to_be_freed->backup);
			free(to_be_freed);

			/* If backup memory was allocated to the remaining area, it's has
			 * the wrong size now */
			free(c->backup);
			c->backup = NULL;
		} else {
			c = c->next;
		}
	}
}

int target_alloc_working_area_try(struct target *target, uint32_t size, struct working_area **area)
{
	/* Reevaluate working area address based on MMU state*/
	if (!target->working_areas) {
		int retval;
		int enabled;

		retval = target->type->mmu(target, &enabled);
		if (retval != ERROR_OK)
			return retval;

		if (!enabled) {
			if (target->working_area_phys_spec) {
				LOG_DEBUG("MMU disabled, using physical "
					"address for working memory " TARGET_ADDR_FMT,
					target->working_area_phys);
				target->working_area = target->working_area_phys;
			} else {
				LOG_ERROR("No working memory available. "
					"Specify -work-area-phys to target.");
				return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
			}
		} else {
			if (target->working_area_virt_spec) {
				LOG_DEBUG("MMU enabled, using virtual "
					"address for working memory " TARGET_ADDR_FMT,
					target->working_area_virt);
				target->working_area = target->working_area_virt;
			} else {
				LOG_ERROR("No working memory available. "
					"Specify -work-area-virt to target.");
				return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
			}
		}

		/* Set up initial working area on first call */
		struct working_area *new_wa = malloc(sizeof(*new_wa));
		if (new_wa) {
			new_wa->next = NULL;
			new_wa->size = ALIGN_DOWN(target->working_area_size, 4); /* 4-byte align */
			new_wa->address = target->working_area;
			new_wa->backup = NULL;
			new_wa->user = NULL;
			new_wa->free = true;
		}

		target->working_areas = new_wa;
	}

	/* only allocate multiples of 4 byte */
	size = ALIGN_UP(size, 4);

	struct working_area *c = target->working_areas;

	/* Find the first large enough working area */
	while (c) {
		if (c->free && c->size >= size)
			break;
		c = c->next;
	}

	if (!c)
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;

	/* Split the working area into the requested size */
	target_split_working_area(c, size);

	LOG_DEBUG("allocated new working area of %" PRIu32 " bytes at address " TARGET_ADDR_FMT,
			  size, c->address);

	if (target->backup_working_area) {
		if (!c->backup) {
			c->backup = malloc(c->size);
			if (!c->backup)
				return ERROR_FAIL;
		}

		int retval = target_read_memory(target, c->address, 4, c->size / 4, c->backup);
		if (retval != ERROR_OK)
			return retval;
	}

	/* mark as used, and return the new (reused) area */
	c->free = false;
	*area = c;

	/* user pointer */
	c->user = area;

	print_wa_layout(target);

	return ERROR_OK;
}

int target_alloc_working_area(struct target *target, uint32_t size, struct working_area **area)
{
	int retval;

	retval = target_alloc_working_area_try(target, size, area);
	if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE)
		LOG_WARNING("not enough working area available(requested %"PRIu32")", size);
	return retval;

}

static int target_restore_working_area(struct target *target, struct working_area *area)
{
	int retval = ERROR_OK;

	if (target->backup_working_area && area->backup) {
		retval = target_write_memory(target, area->address, 4, area->size / 4, area->backup);
		if (retval != ERROR_OK)
			LOG_ERROR("failed to restore %" PRIu32 " bytes of working area at address " TARGET_ADDR_FMT,
					area->size, area->address);
	}

	return retval;
}

/* Restore the area's backup memory, if any, and return the area to the allocation pool */
static int target_free_working_area_restore(struct target *target, struct working_area *area, int restore)
{
	if (!area || area->free)
		return ERROR_OK;

	int retval = ERROR_OK;
	if (restore) {
		retval = target_restore_working_area(target, area);
		/* REVISIT: Perhaps the area should be freed even if restoring fails. */
		if (retval != ERROR_OK)
			return retval;
	}

	area->free = true;

	LOG_DEBUG("freed %" PRIu32 " bytes of working area at address " TARGET_ADDR_FMT,
			area->size, area->address);

	/* mark user pointer invalid */
	/* TODO: Is this really safe? It points to some previous caller's memory.
	 * How could we know that the area pointer is still in that place and not
	 * some other vital data? What's the purpose of this, anyway? */
	*area->user = NULL;
	area->user = NULL;

	target_merge_working_areas(target);

	print_wa_layout(target);

	return retval;
}

int target_free_working_area(struct target *target, struct working_area *area)
{
	return target_free_working_area_restore(target, area, 1);
}

/* free resources and restore memory, if restoring memory fails,
 * free up resources anyway
 */
static void target_free_all_working_areas_restore(struct target *target, int restore)
{
	struct working_area *c = target->working_areas;

	LOG_DEBUG("freeing all working areas");

	/* Loop through all areas, restoring the allocated ones and marking them as free */
	while (c) {
		if (!c->free) {
			if (restore)
				target_restore_working_area(target, c);
			c->free = true;
			*c->user = NULL; /* Same as above */
			c->user = NULL;
		}
		c = c->next;
	}

	/* Run a merge pass to combine all areas into one */
	target_merge_working_areas(target);

	print_wa_layout(target);
}

void target_free_all_working_areas(struct target *target)
{
	target_free_all_working_areas_restore(target, 1);

	/* Now we have none or only one working area marked as free */
	if (target->working_areas) {
		/* Free the last one to allow on-the-fly moving and resizing */
		free(target->working_areas->backup);
		free(target->working_areas);
		target->working_areas = NULL;
	}
}

/* Find the largest number of bytes that can be allocated */
uint32_t target_get_working_area_avail(struct target *target)
{
	struct working_area *c = target->working_areas;
	uint32_t max_size = 0;

	if (!c)
		return ALIGN_DOWN(target->working_area_size, 4);

	while (c) {
		if (c->free && max_size < c->size)
			max_size = c->size;

		c = c->next;
	}

	return max_size;
}

static void target_destroy(struct target *target)
{
	breakpoint_remove_all(target);
	watchpoint_remove_all(target);

	if (target->type->deinit_target)
		target->type->deinit_target(target);

	if (target->semihosting)
		free(target->semihosting->basedir);
	free(target->semihosting);

	jtag_unregister_event_callback(jtag_enable_callback, target);

	struct target_event_action *teap, *temp;
	list_for_each_entry_safe(teap, temp, &target->events_action, list) {
		list_del(&teap->list);
		Jim_DecrRefCount(teap->interp, teap->body);
		free(teap);
	}

	target_free_all_working_areas(target);

    //TODO-UPS -- create a patch
	rtos_destroy(target);

	/* release the targets SMP list */
	if (target->smp) {
		struct target_list *head, *tmp;

		list_for_each_entry_safe(head, tmp, target->smp_targets, lh) {
			list_del(&head->lh);
			head->target->smp = 0;
			free(head);
		}
		if (target->smp_targets != &empty_smp_targets)
			free(target->smp_targets);
		target->smp = 0;
	}

	free(target->gdb_port_override);
	free(target->type);
	free(target->trace_info);
	free(target->fileio_info);
	free(target->cmd_name);
	free(target);
}

void target_quit(void)
{
	struct target_event_callback *pe = target_event_callbacks;
	while (pe) {
		struct target_event_callback *t = pe->next;
		free(pe);
		pe = t;
	}
	target_event_callbacks = NULL;

	struct target_timer_callback *pt = target_timer_callbacks;
	while (pt) {
		struct target_timer_callback *t = pt->next;
		free(pt);
		pt = t;
	}
	target_timer_callbacks = NULL;

	for (struct target *target = all_targets; target;) {
		struct target *tmp;

		tmp = target->next;
		target_destroy(target);
		target = tmp;
	}

	all_targets = NULL;
}

int target_arch_state(struct target *target)
{
	int retval;
	if (!target) {
		LOG_WARNING("No target has been configured");
		return ERROR_OK;
	}

	if (target->state != TARGET_HALTED)
		return ERROR_OK;

	retval = target->type->arch_state(target);
	return retval;
}

static int target_get_gdb_fileio_info_default(struct target *target,
		struct gdb_fileio_info *fileio_info)
{
	/* If target does not support semi-hosting function, target
	   has no need to provide .get_gdb_fileio_info callback.
	   It just return ERROR_FAIL and gdb_server will return "Txx"
	   as target halted every time.  */
	return ERROR_FAIL;
}

static int target_gdb_fileio_end_default(struct target *target,
		int retcode, int fileio_errno, bool ctrl_c)
{
	return ERROR_OK;
}

int target_profiling_default(struct target *target, uint32_t *samples,
		uint32_t max_num_samples, uint32_t *num_samples, uint32_t seconds)
{
	struct timeval timeout, now;

	gettimeofday(&timeout, NULL);
	timeval_add_time(&timeout, seconds, 0);

	LOG_INFO("Starting profiling. Halting and resuming the"
			" target as often as we can...");

	uint32_t sample_count = 0;
	/* hopefully it is safe to cache! We want to stop/restart as quickly as possible. */
	struct reg *reg = register_get_by_name(target->reg_cache, "pc", true);

	int retval = ERROR_OK;
	for (;;) {
		target_poll(target);
		if (target->state == TARGET_HALTED) {
			uint32_t t = buf_get_u32(reg->value, 0, 32);
			samples[sample_count++] = t;
			/* current pc, addr = 0, do not handle breakpoints, not debugging */
			retval = target_resume(target, true, 0, false, false);
			target_poll(target);
			alive_sleep(10); /* sleep 10ms, i.e. <100 samples/second. */
		} else if (target->state == TARGET_RUNNING) {
			/* We want to quickly sample the PC. */
			retval = target_halt(target);
		} else {
			LOG_INFO("Target not halted or running");
			retval = ERROR_OK;
			break;
		}

		if (retval != ERROR_OK)
			break;

		gettimeofday(&now, NULL);
		if ((sample_count >= max_num_samples) || timeval_compare(&now, &timeout) >= 0) {
			LOG_INFO("Profiling completed. %" PRIu32 " samples.", sample_count);
			break;
		}
	}

	*num_samples = sample_count;
	return retval;
}

/* Single aligned words are guaranteed to use 16 or 32 bit access
 * mode respectively, otherwise data is handled as quickly as
 * possible
 */
int target_write_buffer(struct target *target, target_addr_t address, uint32_t size, const uint8_t *buffer)
{
	LOG_DEBUG("writing buffer of %" PRIu32 " byte at " TARGET_ADDR_FMT,
			  size, address);

	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	if (size == 0)
		return ERROR_OK;

	if ((address + size - 1) < address) {
		/* GDB can request this when e.g. PC is 0xfffffffc */
		LOG_ERROR("address + size wrapped (" TARGET_ADDR_FMT ", 0x%08" PRIx32 ")",
				  address,
				  size);
		return ERROR_FAIL;
	}

	return target->type->write_buffer(target, address, size, buffer);
}

static int target_write_buffer_default(struct target *target,
	target_addr_t address, uint32_t count, const uint8_t *buffer)
{
	uint32_t size;
	unsigned int data_bytes = target_data_bits(target) / 8;

	/* Align up to maximum bytes. The loop condition makes sure the next pass
	 * will have something to do with the size we leave to it. */
	for (size = 1;
			size < data_bytes && count >= size * 2 + (address & size);
			size *= 2) {
		if (address & size) {
			int retval = target_write_memory(target, address, size, 1, buffer);
			if (retval != ERROR_OK)
				return retval;
			address += size;
			count -= size;
			buffer += size;
		}
	}

	/* Write the data with as large access size as possible. */
	for (; size > 0; size /= 2) {
		uint32_t aligned = count - count % size;
		if (aligned > 0) {
			int retval = target_write_memory(target, address, size, aligned / size, buffer);
			if (retval != ERROR_OK)
				return retval;
			address += aligned;
			count -= aligned;
			buffer += aligned;
		}
	}

	return ERROR_OK;
}

/* Single aligned words are guaranteed to use 16 or 32 bit access
 * mode respectively, otherwise data is handled as quickly as
 * possible
 */
int target_read_buffer(struct target *target, target_addr_t address, uint32_t size, uint8_t *buffer)
{
	LOG_DEBUG("reading buffer of %" PRIu32 " byte at " TARGET_ADDR_FMT,
			  size, address);

	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	if (size == 0)
		return ERROR_OK;

	if ((address + size - 1) < address) {
		/* GDB can request this when e.g. PC is 0xfffffffc */
		LOG_ERROR("address + size wrapped (" TARGET_ADDR_FMT ", 0x%08" PRIx32 ")",
				  address,
				  size);
		return ERROR_FAIL;
	}

	return target->type->read_buffer(target, address, size, buffer);
}

static int target_read_buffer_default(struct target *target, target_addr_t address, uint32_t count, uint8_t *buffer)
{
	uint32_t size;
	unsigned int data_bytes = target_data_bits(target) / 8;

	/* Align up to maximum bytes. The loop condition makes sure the next pass
	 * will have something to do with the size we leave to it. */
	for (size = 1;
			size < data_bytes && count >= size * 2 + (address & size);
			size *= 2) {
		if (address & size) {
			int retval = target_read_memory(target, address, size, 1, buffer);
			if (retval != ERROR_OK)
				return retval;
			address += size;
			count -= size;
			buffer += size;
		}
	}

	/* Read the data with as large access size as possible. */
	for (; size > 0; size /= 2) {
		uint32_t aligned = count - count % size;
		if (aligned > 0) {
			int retval = target_read_memory(target, address, size, aligned / size, buffer);
			if (retval != ERROR_OK)
				return retval;
			address += aligned;
			count -= aligned;
			buffer += aligned;
		}
	}

	return ERROR_OK;
}

int target_checksum_memory(struct target *target, target_addr_t address, uint32_t size, uint32_t *crc)
{
	uint8_t *buffer;
	int retval;
	uint32_t i;
	uint32_t checksum = 0;
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}
	if (!target->type->checksum_memory) {
		LOG_ERROR("Target %s doesn't support checksum_memory", target_name(target));
		return ERROR_FAIL;
	}

	retval = target->type->checksum_memory(target, address, size, &checksum);
	if (retval != ERROR_OK) {
		buffer = malloc(size);
		if (!buffer) {
			LOG_ERROR("error allocating buffer for section (%" PRIu32 " bytes)", size);
			return ERROR_COMMAND_SYNTAX_ERROR;
		}
		retval = target_read_buffer(target, address, size, buffer);
		if (retval != ERROR_OK) {
			free(buffer);
			return retval;
		}

		/* convert to target endianness */
		for (i = 0; i < (size/sizeof(uint32_t)); i++) {
			uint32_t target_data;
			target_data = target_buffer_get_u32(target, &buffer[i*sizeof(uint32_t)]);
			target_buffer_set_u32(target, &buffer[i*sizeof(uint32_t)], target_data);
		}

		retval = image_calculate_checksum(buffer, size, &checksum);
		free(buffer);
	}

	*crc = checksum;

	return retval;
}

int target_blank_check_memory(struct target *target,
	struct target_memory_check_block *blocks, int num_blocks,
	uint8_t erased_value)
{
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	if (!target->type->blank_check_memory)
		return ERROR_NOT_IMPLEMENTED;

	return target->type->blank_check_memory(target, blocks, num_blocks, erased_value);
}

int target_read_u64(struct target *target, target_addr_t address, uint64_t *value)
{
	uint8_t value_buf[8];
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	int retval = target_read_memory(target, address, 8, 1, value_buf);

	if (retval == ERROR_OK) {
		*value = target_buffer_get_u64(target, value_buf);
		LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%16.16" PRIx64 "",
				  address,
				  *value);
	} else {
		*value = 0x0;
		LOG_DEBUG("address: " TARGET_ADDR_FMT " failed",
				  address);
	}

	return retval;
}

int target_read_u32(struct target *target, target_addr_t address, uint32_t *value)
{
	uint8_t value_buf[4];
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	int retval = target_read_memory(target, address, 4, 1, value_buf);

	if (retval == ERROR_OK) {
		*value = target_buffer_get_u32(target, value_buf);
		LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%8.8" PRIx32 "",
				  address,
				  *value);
	} else {
		*value = 0x0;
		LOG_DEBUG("address: " TARGET_ADDR_FMT " failed",
				  address);
	}

	return retval;
}

int target_read_u16(struct target *target, target_addr_t address, uint16_t *value)
{
	uint8_t value_buf[2];
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	int retval = target_read_memory(target, address, 2, 1, value_buf);

	if (retval == ERROR_OK) {
		*value = target_buffer_get_u16(target, value_buf);
		LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%4.4" PRIx16,
				  address,
				  *value);
	} else {
		*value = 0x0;
		LOG_DEBUG("address: " TARGET_ADDR_FMT " failed",
				  address);
	}

	return retval;
}

int target_read_u8(struct target *target, target_addr_t address, uint8_t *value)
{
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	int retval = target_read_memory(target, address, 1, 1, value);

	if (retval == ERROR_OK) {
		LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%2.2" PRIx8,
				  address,
				  *value);
	} else {
		*value = 0x0;
		LOG_DEBUG("address: " TARGET_ADDR_FMT " failed",
				  address);
	}

	return retval;
}

int target_write_u64(struct target *target, target_addr_t address, uint64_t value)
{
	int retval;
	uint8_t value_buf[8];
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%16.16" PRIx64 "",
			  address,
			  value);

	target_buffer_set_u64(target, value_buf, value);
	retval = target_write_memory(target, address, 8, 1, value_buf);
	if (retval != ERROR_OK)
		LOG_DEBUG("failed: %i", retval);

	return retval;
}

int target_write_u32(struct target *target, target_addr_t address, uint32_t value)
{
	int retval;
	uint8_t value_buf[4];
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%8.8" PRIx32 "",
			  address,
			  value);

	target_buffer_set_u32(target, value_buf, value);
	retval = target_write_memory(target, address, 4, 1, value_buf);
	if (retval != ERROR_OK)
		LOG_DEBUG("failed: %i", retval);

	return retval;
}

int target_write_u16(struct target *target, target_addr_t address, uint16_t value)
{
	int retval;
	uint8_t value_buf[2];
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%8.8" PRIx16,
			  address,
			  value);

	target_buffer_set_u16(target, value_buf, value);
	retval = target_write_memory(target, address, 2, 1, value_buf);
	if (retval != ERROR_OK)
		LOG_DEBUG("failed: %i", retval);

	return retval;
}

int target_write_u8(struct target *target, target_addr_t address, uint8_t value)
{
	int retval;
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%2.2" PRIx8,
			  address, value);

	retval = target_write_memory(target, address, 1, 1, &value);
	if (retval != ERROR_OK)
		LOG_DEBUG("failed: %i", retval);

	return retval;
}

int target_write_phys_u64(struct target *target, target_addr_t address, uint64_t value)
{
	int retval;
	uint8_t value_buf[8];
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%16.16" PRIx64 "",
			  address,
			  value);

	target_buffer_set_u64(target, value_buf, value);
	retval = target_write_phys_memory(target, address, 8, 1, value_buf);
	if (retval != ERROR_OK)
		LOG_DEBUG("failed: %i", retval);

	return retval;
}

int target_write_phys_u32(struct target *target, target_addr_t address, uint32_t value)
{
	int retval;
	uint8_t value_buf[4];
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%8.8" PRIx32 "",
			  address,
			  value);

	target_buffer_set_u32(target, value_buf, value);
	retval = target_write_phys_memory(target, address, 4, 1, value_buf);
	if (retval != ERROR_OK)
		LOG_DEBUG("failed: %i", retval);

	return retval;
}

int target_write_phys_u16(struct target *target, target_addr_t address, uint16_t value)
{
	int retval;
	uint8_t value_buf[2];
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%8.8" PRIx16,
			  address,
			  value);

	target_buffer_set_u16(target, value_buf, value);
	retval = target_write_phys_memory(target, address, 2, 1, value_buf);
	if (retval != ERROR_OK)
		LOG_DEBUG("failed: %i", retval);

	return retval;
}

int target_write_phys_u8(struct target *target, target_addr_t address, uint8_t value)
{
	int retval;
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_FAIL;
	}

	LOG_DEBUG("address: " TARGET_ADDR_FMT ", value: 0x%2.2" PRIx8,
			  address, value);

	retval = target_write_phys_memory(target, address, 1, 1, &value);
	if (retval != ERROR_OK)
		LOG_DEBUG("failed: %i", retval);

	return retval;
}

static int find_target(struct command_invocation *cmd, const char *name)
{
	struct target *target = get_target(name);
	if (!target) {
		command_print(cmd, "Target: %s is unknown, try one of:\n", name);
		return ERROR_FAIL;
	}
	if (!target->tap->enabled) {
		command_print(cmd, "Target: TAP %s is disabled, "
			 "can't be the current target\n",
			 target->tap->dotted_name);
		return ERROR_FAIL;
	}

	cmd->ctx->current_target = target;
	if (cmd->ctx->current_target_override)
		cmd->ctx->current_target_override = target;

	return ERROR_OK;
}


COMMAND_HANDLER(handle_targets_command)
{
	int retval = ERROR_OK;
	if (CMD_ARGC == 1) {
		retval = find_target(CMD, CMD_ARGV[0]);
		if (retval == ERROR_OK) {
			/* we're done! */
			return retval;
		}
	}

	unsigned int index = 0;
	command_print(CMD, "    TargetName         Type       Endian TapName            State       ");
	command_print(CMD, "--  ------------------ ---------- ------ ------------------ ------------");
	for (struct target *target = all_targets; target; target = target->next, ++index) {
		const char *state;
		char marker = ' ';

		if (target->tap->enabled)
			state = target_state_name(target);
		else
			state = "tap-disabled";

		if (CMD_CTX->current_target == target)
			marker = '*';

		/* keep columns lined up to match the headers above */
		command_print(CMD,
				"%2d%c %-18s %-10s %-6s %-18s %s",
				index,
				marker,
				target_name(target),
				target_type_name(target),
				nvp_value2name(nvp_target_endian, target->endianness)->name,
				target->tap->dotted_name,
				state);
	}

	return retval;
}

/* every 300ms we check for reset & powerdropout and issue a "reset halt" if so. */

static int power_dropout;
static int srst_asserted;

static int run_power_restore;
static int run_power_dropout;
static int run_srst_asserted;
static int run_srst_deasserted;

static int sense_handler(void)
{
	static int prev_srst_asserted;
	static int prev_power_dropout;

	int retval = jtag_power_dropout(&power_dropout);
	if (retval != ERROR_OK)
		return retval;

	int power_restored;
	power_restored = prev_power_dropout && !power_dropout;
	if (power_restored)
		run_power_restore = 1;

	int64_t current = timeval_ms();
	static int64_t last_power;
	bool wait_more = last_power + 2000 > current;
	if (power_dropout && !wait_more) {
		run_power_dropout = 1;
		last_power = current;
	}

	retval = jtag_srst_asserted(&srst_asserted);
	if (retval != ERROR_OK)
		return retval;

	int srst_deasserted;
	srst_deasserted = prev_srst_asserted && !srst_asserted;

	static int64_t last_srst;
	wait_more = last_srst + 2000 > current;
	if (srst_deasserted && !wait_more) {
		run_srst_deasserted = 1;
		last_srst = current;
	}

	if (!prev_srst_asserted && srst_asserted)
		run_srst_asserted = 1;

	prev_srst_asserted = srst_asserted;
	prev_power_dropout = power_dropout;

	if (srst_deasserted || power_restored) {
		/* Other than logging the event we can't do anything here.
		 * Issuing a reset is a particularly bad idea as we might
		 * be inside a reset already.
		 */
	}

	return ERROR_OK;
}

/* process target state changes */
static int handle_target(void *priv)
{
	Jim_Interp *interp = (Jim_Interp *)priv;
	int retval = ERROR_OK;

	if (!is_jtag_poll_safe()) {
		/* polling is disabled currently */
		return ERROR_OK;
	}

	/* we do not want to recurse here... */
	static int recursive;
	if (!recursive) {
		recursive = 1;
		sense_handler();
		/* danger! running these procedures can trigger srst assertions and power dropouts.
		 * We need to avoid an infinite loop/recursion here and we do that by
		 * clearing the flags after running these events.
		 */
		int did_something = 0;
		if (run_srst_asserted) {
			LOG_INFO("srst asserted detected, running srst_asserted proc.");
			Jim_Eval(interp, "srst_asserted");
			did_something = 1;
		}
		if (run_srst_deasserted) {
			Jim_Eval(interp, "srst_deasserted");
			did_something = 1;
		}
		if (run_power_dropout) {
			LOG_INFO("Power dropout detected, running power_dropout proc.");
			Jim_Eval(interp, "power_dropout");
			did_something = 1;
		}
		if (run_power_restore) {
			Jim_Eval(interp, "power_restore");
			did_something = 1;
		}

		if (did_something) {
			/* clear detect flags */
			sense_handler();
		}

		/* clear action flags */

		run_srst_asserted = 0;
		run_srst_deasserted = 0;
		run_power_restore = 0;
		run_power_dropout = 0;

		recursive = 0;
	}

	/* Poll targets for state changes unless that's globally disabled.
	 * Skip targets that are currently disabled.
	 */
	for (struct target *target = all_targets;
			is_jtag_poll_safe() && target;
			target = target->next) {

		if (!target_was_examined(target))
			continue;

		if (!target->tap->enabled)
			continue;

		if (target->backoff.times > target->backoff.count) {
			/* do not poll this time as we failed previously */
			target->backoff.count++;
			continue;
		}
		target->backoff.count = 0;

		/* only poll target if we've got power and srst isn't asserted */
		if (!power_dropout && !srst_asserted) {
			/* polling may fail silently until the target has been examined */
			retval = target_poll(target);
			if (retval != ERROR_OK) {
				/* 100ms polling interval. Increase interval between polling up to 5000ms */
				if (target->backoff.times * polling_interval < 5000) {
					target->backoff.times *= 2;
					target->backoff.times++;
				}

				/* Do *not* tell gdb the target halted. This might just
				 * be a hiccup.  We have no reason to believe the target
				 * is halted, and if it is running while gdb thinks it's
				 * halted things just get unnecessarily confused.  gdb
				 * users can hit ^C if the need to interact with the
				 * target. */
			}
			if (target->backoff.times > 0) {
				LOG_TARGET_ERROR(target, "Polling failed, trying to reexamine");
				target_reset_examined(target);
				retval = target_examine_one(target);
				/* Target examination could have failed due to unstable connection,
				 * but we set the examined flag anyway to repoll it later */
				if (retval != ERROR_OK) {
					target_set_examined(target);
					LOG_TARGET_ERROR(target, "Examination failed, GDB will be halted. Polling again in %dms",
						 target->backoff.times * polling_interval);
					return retval;
				}
			}

			/* Since we succeeded, we reset backoff count */
			target->backoff.times = 0;
		}
	}

	return retval;
}

COMMAND_HANDLER(handle_reg_command)
{
	LOG_DEBUG("-");

	struct target *target = get_current_target(CMD_CTX);
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_TARGET_NOT_EXAMINED;
	}
	struct reg *reg = NULL;

	/* list all available registers for the current target */
	if (CMD_ARGC == 0) {
		struct reg_cache *cache = target->reg_cache;

		unsigned int count = 0;
		while (cache) {
			unsigned int i;

			command_print(CMD, "===== %s", cache->name);

			for (i = 0, reg = cache->reg_list;
					i < cache->num_regs;
					i++, reg++, count++) {
				if (!reg->exist || reg->hidden)
					continue;
				/* only print cached values if they are valid */
				if (reg->valid) {
					char *value = buf_to_hex_str(reg->value,
							reg->size);
					command_print(CMD,
							"(%i) %s (/%" PRIu32 "): 0x%s%s",
							count, reg->name,
							reg->size, value,
							reg->dirty
								? " (dirty)"
								: "");
					free(value);
				} else {
					command_print(CMD, "(%i) %s (/%" PRIu32 ")",
							  count, reg->name,
							  reg->size);
				}
			}
			cache = cache->next;
		}

		return ERROR_OK;
	}

	/* access a single register by its ordinal number */
	if ((CMD_ARGV[0][0] >= '0') && (CMD_ARGV[0][0] <= '9')) {
		unsigned int num;
		COMMAND_PARSE_NUMBER(uint, CMD_ARGV[0], num);

		struct reg_cache *cache = target->reg_cache;
		unsigned int count = 0;
		while (cache) {
			unsigned int i;
			for (i = 0; i < cache->num_regs; i++) {
				if (count++ == num) {
					reg = &cache->reg_list[i];
					break;
				}
			}
			if (reg)
				break;
			cache = cache->next;
		}

		if (!reg) {
			command_print(CMD, "%i is out of bounds, the current target "
					"has only %i registers (0 - %i)", num, count, count - 1);
			return ERROR_FAIL;
		}
	} else {
		/* access a single register by its name */
		reg = register_get_by_name(target->reg_cache, CMD_ARGV[0], true);

		if (!reg)
			goto not_found;
	}

	assert(reg); /* give clang a hint that we *know* reg is != NULL here */

	if (!reg->exist)
		goto not_found;

	/* display a register */
	if ((CMD_ARGC == 1) || ((CMD_ARGC == 2) && !((CMD_ARGV[1][0] >= '0')
			&& (CMD_ARGV[1][0] <= '9')))) {
		if ((CMD_ARGC == 2) && (strcmp(CMD_ARGV[1], "force") == 0))
			reg->valid = false;

		if (!reg->valid) {
			int retval = reg->type->get(reg);
			if (retval != ERROR_OK) {
				LOG_ERROR("Could not read register '%s'", reg->name);
				return retval;
			}
		}
		char *value = buf_to_hex_str(reg->value, reg->size);
		command_print(CMD, "%s (/%i): 0x%s", reg->name, (int)(reg->size), value);
		free(value);
		return ERROR_OK;
	}

	/* set register value */
	if (CMD_ARGC == 2) {
		uint8_t *buf = malloc(DIV_ROUND_UP(reg->size, 8));
		if (!buf) {
			LOG_ERROR("Failed to allocate memory");
			return ERROR_FAIL;
		}

		int retval = CALL_COMMAND_HANDLER(command_parse_str_to_buf, CMD_ARGV[1], buf, reg->size);
		if (retval != ERROR_OK) {
			free(buf);
			return retval;
		}

		retval = reg->type->set(reg, buf);
		if (retval != ERROR_OK) {
			LOG_ERROR("Could not write to register '%s'", reg->name);
		} else {
			char *value = buf_to_hex_str(reg->value, reg->size);
			command_print(CMD, "%s (/%i): 0x%s", reg->name, (int)(reg->size), value);
			free(value);
		}

		free(buf);

		return retval;
	}

	return ERROR_COMMAND_SYNTAX_ERROR;

not_found:
	command_print(CMD, "register %s not found in current target", CMD_ARGV[0]);
	return ERROR_FAIL;
}

COMMAND_HANDLER(handle_poll_command)
{
	int retval = ERROR_OK;
	struct target *target = get_current_target(CMD_CTX);

	if (CMD_ARGC == 0) {
		command_print(CMD, "background polling: %s",
				jtag_poll_get_enabled() ? "on" : "off");
		command_print(CMD, "TAP: %s (%s)",
				target->tap->dotted_name,
				target->tap->enabled ? "enabled" : "disabled");
		if (!target->tap->enabled)
			return ERROR_OK;
		retval = target_poll(target);
		if (retval != ERROR_OK)
			return retval;
		retval = target_arch_state(target);
		if (retval != ERROR_OK)
			return retval;
	} else if (CMD_ARGC == 1) {
		bool enable;
		COMMAND_PARSE_ON_OFF(CMD_ARGV[0], enable);
		jtag_poll_set_enabled(enable);
	} else
		return ERROR_COMMAND_SYNTAX_ERROR;

	return retval;
}

COMMAND_HANDLER(handle_wait_halt_command)
{
	if (CMD_ARGC > 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	unsigned int ms = DEFAULT_HALT_TIMEOUT;
	if (1 == CMD_ARGC) {
		int retval = parse_uint(CMD_ARGV[0], &ms);
		if (retval != ERROR_OK)
			return ERROR_COMMAND_SYNTAX_ERROR;
	}

	struct target *target = get_current_target(CMD_CTX);
	return target_wait_state(target, TARGET_HALTED, ms);
}

/* wait for target state to change. The trick here is to have a low
 * latency for short waits and not to suck up all the CPU time
 * on longer waits.
 *
 * After 500ms, keep_alive() is invoked
 */
int target_wait_state(struct target *target, enum target_state state, unsigned int ms)
{
	int retval;
	int64_t then = 0, cur;
	bool once = true;

	for (;;) {
		retval = target_poll(target);
		if (retval != ERROR_OK)
			return retval;
		if (target->state == state)
			break;
		cur = timeval_ms();
		if (once) {
			once = false;
			then = timeval_ms();
			LOG_DEBUG("waiting for target %s...",
				nvp_value2name(nvp_target_state, state)->name);
		}

		if (cur - then > 500) {
			keep_alive();
			if (openocd_is_shutdown_pending())
				return ERROR_SERVER_INTERRUPTED;
		}

		if ((cur-then) > ms) {
			LOG_ERROR("timed out while waiting for target %s",
				nvp_value2name(nvp_target_state, state)->name);
			return ERROR_FAIL;
		}
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_halt_command)
{
	LOG_DEBUG("-");

	struct target *target = get_current_target(CMD_CTX);

	target->verbose_halt_msg = true;

	int retval = target_halt(target);
	if (retval != ERROR_OK)
		return retval;

	if (CMD_ARGC == 1) {
		unsigned int wait_local;
		retval = parse_uint(CMD_ARGV[0], &wait_local);
		if (retval != ERROR_OK)
			return ERROR_COMMAND_SYNTAX_ERROR;
		if (!wait_local)
			return ERROR_OK;
	}

	return CALL_COMMAND_HANDLER(handle_wait_halt_command);
}

COMMAND_HANDLER(handle_soft_reset_halt_command)
{
	struct target *target = get_current_target(CMD_CTX);

	LOG_TARGET_INFO(target, "requesting target halt and executing a soft reset");

	target_soft_reset_halt(target);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_reset_command)
{
	if (CMD_ARGC > 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	enum target_reset_mode reset_mode = RESET_RUN;
	if (CMD_ARGC == 1) {
		const struct nvp *n;
		n = nvp_name2value(nvp_reset_modes, CMD_ARGV[0]);
		if ((!n->name) || (n->value == RESET_UNKNOWN))
			return ERROR_COMMAND_SYNTAX_ERROR;
		reset_mode = n->value;
	}

	/* reset *all* targets */
	return target_process_reset(CMD, reset_mode);
}


COMMAND_HANDLER(handle_resume_command)
{
	bool current = true;
	if (CMD_ARGC > 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);

	/* with no CMD_ARGV, resume from current pc, addr = 0,
	 * with one arguments, addr = CMD_ARGV[0],
	 * handle breakpoints, not debugging */
	target_addr_t addr = 0;
	if (CMD_ARGC == 1) {
		COMMAND_PARSE_ADDRESS(CMD_ARGV[0], addr);
		current = false;
	}

	return target_resume(target, current, addr, true, false);
}

COMMAND_HANDLER(handle_step_command)
{
	if (CMD_ARGC > 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	LOG_DEBUG("-");

	/* with no CMD_ARGV, step from current pc, addr = 0,
	 * with one argument addr = CMD_ARGV[0],
	 * handle breakpoints, debugging */
	target_addr_t addr = 0;
	int current_pc = 1;
	if (CMD_ARGC == 1) {
		COMMAND_PARSE_ADDRESS(CMD_ARGV[0], addr);
		current_pc = 0;
	}

	struct target *target = get_current_target(CMD_CTX);

	return target_step(target, current_pc, addr, true);
}

void target_handle_md_output(struct command_invocation *cmd,
		struct target *target, target_addr_t address, unsigned int size,
		unsigned int count, const uint8_t *buffer)
{
	const unsigned int line_bytecnt = 32;
	unsigned int line_modulo = line_bytecnt / size;

	char output[line_bytecnt * 4 + 1];
	unsigned int output_len = 0;

	const char *value_fmt;
	switch (size) {
	case 8:
		value_fmt = "%16.16"PRIx64" ";
		break;
	case 4:
		value_fmt = "%8.8"PRIx64" ";
		break;
	case 2:
		value_fmt = "%4.4"PRIx64" ";
		break;
	case 1:
		value_fmt = "%2.2"PRIx64" ";
		break;
	default:
		/* "can't happen", caller checked */
		LOG_ERROR("invalid memory read size: %u", size);
		return;
	}

	for (unsigned int i = 0; i < count; i++) {
		if (i % line_modulo == 0) {
			output_len += snprintf(output + output_len,
					sizeof(output) - output_len,
					TARGET_ADDR_FMT ": ",
					(address + (i * size)));
		}

		uint64_t value = 0;
		const uint8_t *value_ptr = buffer + i * size;
		switch (size) {
		case 8:
			value = target_buffer_get_u64(target, value_ptr);
			break;
		case 4:
			value = target_buffer_get_u32(target, value_ptr);
			break;
		case 2:
			value = target_buffer_get_u16(target, value_ptr);
			break;
		case 1:
			value = *value_ptr;
		}
		output_len += snprintf(output + output_len,
				sizeof(output) - output_len,
				value_fmt, value);

		if ((i % line_modulo == line_modulo - 1) || (i == count - 1)) {
			command_print(cmd, "%s", output);
			output_len = 0;
		}
	}
}

COMMAND_HANDLER(handle_md_command)
{
	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	unsigned int size = 0;
	switch (CMD_NAME[2]) {
	case 'd':
		size = 8;
		break;
	case 'w':
		size = 4;
		break;
	case 'h':
		size = 2;
		break;
	case 'b':
		size = 1;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	bool physical = strcmp(CMD_ARGV[0], "phys") == 0;
	int (*fn)(struct target *target,
			target_addr_t address, uint32_t size_value, uint32_t count, uint8_t *buffer);
	if (physical) {
		CMD_ARGC--;
		CMD_ARGV++;
		fn = target_read_phys_memory;
	} else
		fn = target_read_memory;
	if ((CMD_ARGC < 1) || (CMD_ARGC > 2))
		return ERROR_COMMAND_SYNTAX_ERROR;

	target_addr_t address;
	COMMAND_PARSE_ADDRESS(CMD_ARGV[0], address);

	unsigned int count = 1;
	if (CMD_ARGC == 2)
		COMMAND_PARSE_NUMBER(uint, CMD_ARGV[1], count);

	uint8_t *buffer = calloc(count, size);
	if (!buffer) {
		LOG_ERROR("Failed to allocate md read buffer");
		return ERROR_FAIL;
	}

	struct target *target = get_current_target(CMD_CTX);
	int retval = fn(target, address, size, count, buffer);
	if (retval == ERROR_OK)
		target_handle_md_output(CMD, target, address, size, count, buffer);

	free(buffer);

	return retval;
}

typedef int (*target_write_fn)(struct target *target,
		target_addr_t address, uint32_t size, uint32_t count, const uint8_t *buffer);

static int target_fill_mem(struct target *target,
		target_addr_t address,
		target_write_fn fn,
		unsigned int data_size,
		/* value */
		uint64_t b,
		/* count */
		unsigned int c)
{
	/* We have to write in reasonably large chunks to be able
	 * to fill large memory areas with any sane speed */
	const unsigned int chunk_size = 16384;
	uint8_t *target_buf = malloc(chunk_size * data_size);
	if (!target_buf) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}

	for (unsigned int i = 0; i < chunk_size; i++) {
		switch (data_size) {
		case 8:
			target_buffer_set_u64(target, target_buf + i * data_size, b);
			break;
		case 4:
			target_buffer_set_u32(target, target_buf + i * data_size, b);
			break;
		case 2:
			target_buffer_set_u16(target, target_buf + i * data_size, b);
			break;
		case 1:
			target_buffer_set_u8(target, target_buf + i * data_size, b);
			break;
		default:
			exit(-1);
		}
	}

	int retval = ERROR_OK;

	for (unsigned int x = 0; x < c; x += chunk_size) {
		unsigned int current;
		current = c - x;
		if (current > chunk_size)
			current = chunk_size;
		retval = fn(target, address + x * data_size, data_size, current, target_buf);
		if (retval != ERROR_OK)
			break;
		/* avoid GDB timeouts */
		keep_alive();

		if (openocd_is_shutdown_pending()) {
			retval = ERROR_SERVER_INTERRUPTED;
			break;
		}
	}
	free(target_buf);

	return retval;
}


COMMAND_HANDLER(handle_mw_command)
{
	if (CMD_ARGC < 2)
		return ERROR_COMMAND_SYNTAX_ERROR;
	bool physical = strcmp(CMD_ARGV[0], "phys") == 0;
	target_write_fn fn;
	if (physical) {
		CMD_ARGC--;
		CMD_ARGV++;
		fn = target_write_phys_memory;
	} else
		fn = target_write_memory;
	if ((CMD_ARGC < 2) || (CMD_ARGC > 3))
		return ERROR_COMMAND_SYNTAX_ERROR;

	target_addr_t address;
	COMMAND_PARSE_ADDRESS(CMD_ARGV[0], address);

	uint64_t value;
	COMMAND_PARSE_NUMBER(u64, CMD_ARGV[1], value);

	unsigned int count = 1;
	if (CMD_ARGC == 3)
		COMMAND_PARSE_NUMBER(uint, CMD_ARGV[2], count);

	struct target *target = get_current_target(CMD_CTX);
	unsigned int wordsize;
	switch (CMD_NAME[2]) {
		case 'd':
			wordsize = 8;
			break;
		case 'w':
			wordsize = 4;
			break;
		case 'h':
			wordsize = 2;
			break;
		case 'b':
			wordsize = 1;
			break;
		default:
			return ERROR_COMMAND_SYNTAX_ERROR;
	}

	return target_fill_mem(target, address, fn, wordsize, value, count);
}

COMMAND_HANDLER(handle_mbatch_command)
{
	if (CMD_ARGC < 2)
		return ERROR_COMMAND_SYNTAX_ERROR;
    
	/*General syntax:
		<Unique ID> <Subcommands>
     
      Sub-command syntax:
		w:<address>:<access size>:<count>:<hex-encoded data> - write data
		r:<address>:<access size>:<count> - read data
		
	  Reply syntax for each subcommand:
		<status>:<optional data>
	*/
    
	struct target *target = get_current_target(CMD_CTX);
	command_print_sameline(cmd, "mbatch(%s):", CMD_ARGV[0]);
    
	for (int i = 1; i < CMD_ARGC; i++)
	{
		const char *pCmd = CMD_ARGV[i];
		const char *p = strchr(pCmd, ':');
        
		int status = ERROR_OK;
		uint8_t *data = NULL;
		int printData = 0;
		uint64_t args[3] = { 0, };	//address, size, count
        
		if (!p)
			status = -ERROR_BAD_ARGUMENTS;
        
		for (int j = 0; j < sizeof(args) / sizeof(args[0]); j++)
		{
			if (p)
			{
				p++;
				char *end = NULL;
				args[j] = strtoull(p, &end, 0);
				if (end == p || !end)
					status = -ERROR_BAD_ARGUMENTS;
				else if (*end != ':' && *end != 0)
					status = -ERROR_BAD_ARGUMENTS;
                
				p = end;
			}
		}
        
		int bufferSize = (int)(args[1] * args[2]);
        
		if (status == ERROR_OK)
		{
			data = (uint8_t *)malloc(bufferSize);
            
			if (pCmd[0] == 'w')
			{
				if (!p)
					status = -ERROR_BAD_ARGUMENTS;
				else
				{
					int done = unhexify(data, p + 1, args[1] * args[2]);
					if (done != (args[1] * args[2]))
						status = -ERROR_INSUFFICIENT_BUFFER;
					else
						status = target_write_memory(target, args[0], args[1], args[2], data);
				}
			}
			else if (pCmd[0] == 'r')
			{
				status = target_read_memory(target, args[0], args[1], args[2], data);
				printData = true;
			}
			else
				status = -ERROR_BAD_ARGUMENTS;
		}
        
		command_print_sameline(cmd, " %d", status);
		if (status >= 0 && printData && data)
		{
			char *formattedData = malloc(bufferSize * 2 + 2);
			hexify(formattedData, data, bufferSize, bufferSize * 2 + 2);
			command_print_sameline(cmd, ":%s", formattedData);
			free(formattedData);
		}
        
		if (data)
			free(data);        
	}
    
	command_print(cmd, "");
	return ERROR_OK;
}

static COMMAND_HELPER(parse_load_image_command, struct image *image,
		target_addr_t *min_address, target_addr_t *max_address)
{
	if (CMD_ARGC < 1 || CMD_ARGC > 5)
		return ERROR_COMMAND_SYNTAX_ERROR;

	/* a base address isn't always necessary,
	 * default to 0x0 (i.e. don't relocate) */
	if (CMD_ARGC >= 2) {
		target_addr_t addr;
		COMMAND_PARSE_ADDRESS(CMD_ARGV[1], addr);
		image->base_address = addr;
		image->base_address_set = true;
	} else
		image->base_address_set = false;

	image->start_address_set = false;

	if (CMD_ARGC >= 4)
		COMMAND_PARSE_ADDRESS(CMD_ARGV[3], *min_address);
	if (CMD_ARGC == 5) {
		COMMAND_PARSE_ADDRESS(CMD_ARGV[4], *max_address);
		/* use size (given) to find max (required) */
		*max_address += *min_address;
	}

	if (*min_address > *max_address)
		return ERROR_COMMAND_SYNTAX_ERROR;

	return ERROR_OK;
}

COMMAND_HANDLER(handle_load_image_command)
{
	uint8_t *buffer;
	size_t buf_cnt;
	uint32_t image_size;
	target_addr_t min_address = 0;
	target_addr_t max_address = -1;
	struct image image;

	int retval = CALL_COMMAND_HANDLER(parse_load_image_command,
			&image, &min_address, &max_address);
	if (retval != ERROR_OK)
		return retval;

	struct target *target = get_current_target(CMD_CTX);

	struct duration bench;
	duration_start(&bench);

	if (image_open(&image, CMD_ARGV[0], (CMD_ARGC >= 3) ? CMD_ARGV[2] : NULL) != ERROR_OK)
		return ERROR_FAIL;

	image_size = 0x0;
	retval = ERROR_OK;
	for (unsigned int i = 0; i < image.num_sections; i++) {
		buffer = malloc(image.sections[i].size);
		if (!buffer) {
			command_print(CMD,
						  "error allocating buffer for section (%d bytes)",
						  (int)(image.sections[i].size));
			retval = ERROR_FAIL;
			break;
		}

		retval = image_read_section(&image, i, 0x0, image.sections[i].size, buffer, &buf_cnt);
		if (retval != ERROR_OK) {
			free(buffer);
			break;
		}

		uint32_t offset = 0;
		uint32_t length = buf_cnt;

		/* DANGER!!! beware of unsigned comparison here!!! */

		if ((image.sections[i].base_address + buf_cnt >= min_address) &&
				(image.sections[i].base_address < max_address)) {

			if (image.sections[i].base_address < min_address) {
				/* clip addresses below */
				offset += min_address-image.sections[i].base_address;
				length -= offset;
			}

			if (image.sections[i].base_address + buf_cnt > max_address)
				length -= (image.sections[i].base_address + buf_cnt)-max_address;

			retval = target_write_buffer(target,
					image.sections[i].base_address + offset, length, buffer + offset);
			if (retval != ERROR_OK) {
				free(buffer);
				break;
			}
			image_size += length;
			command_print(CMD, "%u bytes written at address " TARGET_ADDR_FMT "",
					(unsigned int)length,
					image.sections[i].base_address + offset);
		}

		free(buffer);
	}

	if ((retval == ERROR_OK) && (duration_measure(&bench) == ERROR_OK)) {
		command_print(CMD, "downloaded %" PRIu32 " bytes "
				"in %fs (%0.3f KiB/s)", image_size,
				duration_elapsed(&bench), duration_kbps(&bench, image_size));
	}

	image_close(&image);

	return retval;

}

COMMAND_HANDLER(handle_dump_image_command)
{
	struct fileio *fileio;
	uint8_t *buffer;
	int retval, retvaltemp;
	target_addr_t address, size;
	struct duration bench;
	struct target *target = get_current_target(CMD_CTX);

	if (CMD_ARGC != 3)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_ADDRESS(CMD_ARGV[1], address);
	COMMAND_PARSE_ADDRESS(CMD_ARGV[2], size);

	uint32_t buf_size = (size > 4096) ? 4096 : size;
	buffer = malloc(buf_size);
	if (!buffer)
		return ERROR_FAIL;

	retval = fileio_open(&fileio, CMD_ARGV[0], FILEIO_WRITE, FILEIO_BINARY);
	if (retval != ERROR_OK) {
		free(buffer);
		return retval;
	}

	duration_start(&bench);

	while (size > 0) {
		size_t size_written;
		uint32_t this_run_size = (size > buf_size) ? buf_size : size;
		retval = target_read_buffer(target, address, this_run_size, buffer);
		if (retval != ERROR_OK)
			break;

		retval = fileio_write(fileio, this_run_size, buffer, &size_written);
		if (retval != ERROR_OK)
			break;

		size -= this_run_size;
		address += this_run_size;
	}

	free(buffer);

	if ((retval == ERROR_OK) && (duration_measure(&bench) == ERROR_OK)) {
		size_t filesize;
		retval = fileio_size(fileio, &filesize);
		if (retval != ERROR_OK)
			return retval;
		command_print(CMD,
				"dumped %zu bytes in %fs (%0.3f KiB/s)", filesize,
				duration_elapsed(&bench), duration_kbps(&bench, filesize));
	}

	retvaltemp = fileio_close(fileio);
	if (retvaltemp != ERROR_OK)
		return retvaltemp;

	return retval;
}

enum verify_mode {
	IMAGE_TEST = 0,
	IMAGE_VERIFY = 1,
	IMAGE_CHECKSUM_ONLY = 2
};

static COMMAND_HELPER(handle_verify_image_command_internal, enum verify_mode verify)
{
	uint8_t *buffer;
	size_t buf_cnt;
	uint32_t image_size;
	int retval;
	uint32_t checksum = 0;
	uint32_t mem_checksum = 0;

	struct image image;

	struct target *target = get_current_target(CMD_CTX);

	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (!target) {
		LOG_ERROR("no target selected");
		return ERROR_FAIL;
	}

	struct duration bench;
	duration_start(&bench);

	if (CMD_ARGC >= 2) {
		target_addr_t addr;
		COMMAND_PARSE_ADDRESS(CMD_ARGV[1], addr);
		image.base_address = addr;
		image.base_address_set = true;
	} else {
		image.base_address_set = false;
		image.base_address = 0x0;
	}

	image.start_address_set = false;

	retval = image_open(&image, CMD_ARGV[0], (CMD_ARGC == 3) ? CMD_ARGV[2] : NULL);
	if (retval != ERROR_OK)
		return retval;

	image_size = 0x0;
	int diffs = 0;
	retval = ERROR_OK;
	for (unsigned int i = 0; i < image.num_sections; i++) {
		buffer = malloc(image.sections[i].size);
		if (!buffer) {
			command_print(CMD,
					"error allocating buffer for section (%" PRIu32 " bytes)",
					image.sections[i].size);
			break;
		}
		retval = image_read_section(&image, i, 0x0, image.sections[i].size, buffer, &buf_cnt);
		if (retval != ERROR_OK) {
			free(buffer);
			break;
		}

		if (verify >= IMAGE_VERIFY) {
			/* calculate checksum of image */
			retval = image_calculate_checksum(buffer, buf_cnt, &checksum);
			if (retval != ERROR_OK) {
				free(buffer);
				break;
			}

			retval = target_checksum_memory(target, image.sections[i].base_address, buf_cnt, &mem_checksum);
			if (retval != ERROR_OK) {
				free(buffer);
				break;
			}
			if ((checksum != mem_checksum) && (verify == IMAGE_CHECKSUM_ONLY)) {
				LOG_ERROR("checksum mismatch");
				free(buffer);
				retval = ERROR_FAIL;
				goto done;
			}
			if (checksum != mem_checksum) {
				/* failed crc checksum, fall back to a binary compare */
				uint8_t *data;

				if (diffs == 0)
					LOG_ERROR("checksum mismatch - attempting binary compare");

				data = malloc(buf_cnt);

				retval = target_read_buffer(target, image.sections[i].base_address, buf_cnt, data);
				if (retval == ERROR_OK) {
					uint32_t t;
					for (t = 0; t < buf_cnt; t++) {
						if (data[t] != buffer[t]) {
							command_print(CMD,
								"diff %d address " TARGET_ADDR_FMT ". Was 0x%02" PRIx8 " instead of 0x%02" PRIx8,
								diffs,
								t + image.sections[i].base_address,
								data[t],
								buffer[t]);
							if (diffs++ >= 127) {
								command_print(CMD, "More than 128 errors, the rest are not printed.");
								free(data);
								free(buffer);
								goto done;
							}
						}
						keep_alive();
						if (openocd_is_shutdown_pending()) {
							retval = ERROR_SERVER_INTERRUPTED;
							free(data);
							free(buffer);
							goto done;
						}
					}
				}
				free(data);
			}
		} else {
			command_print(CMD, "address " TARGET_ADDR_FMT " length 0x%08zx",
						  image.sections[i].base_address,
						  buf_cnt);
		}

		free(buffer);
		image_size += buf_cnt;
	}
	if (diffs > 0)
		command_print(CMD, "No more differences found.");
done:
	if (diffs > 0)
		retval = ERROR_FAIL;
	if ((retval == ERROR_OK) && (duration_measure(&bench) == ERROR_OK)) {
		command_print(CMD, "verified %" PRIu32 " bytes "
				"in %fs (%0.3f KiB/s)", image_size,
				duration_elapsed(&bench), duration_kbps(&bench, image_size));
	}

	image_close(&image);

	return retval;
}

COMMAND_HANDLER(handle_verify_image_checksum_command)
{
	return CALL_COMMAND_HANDLER(handle_verify_image_command_internal, IMAGE_CHECKSUM_ONLY);
}

COMMAND_HANDLER(handle_verify_image_command)
{
	return CALL_COMMAND_HANDLER(handle_verify_image_command_internal, IMAGE_VERIFY);
}

COMMAND_HANDLER(handle_test_image_command)
{
	return CALL_COMMAND_HANDLER(handle_verify_image_command_internal, IMAGE_TEST);
}

static int handle_bp_command_list(struct command_invocation *cmd)
{
	struct target *target = get_current_target(cmd->ctx);
	struct breakpoint *breakpoint = target->breakpoints;
	while (breakpoint) {
		if (breakpoint->type == BKPT_SOFT) {
			char *buf = buf_to_hex_str(breakpoint->orig_instr,
					breakpoint->length * 8);
			command_print(cmd, "Software breakpoint(IVA): addr=" TARGET_ADDR_FMT ", len=0x%x, orig_instr=0x%s",
					breakpoint->address,
					breakpoint->length,
					buf);
			free(buf);
		} else {
			if ((breakpoint->address == 0) && (breakpoint->asid != 0))
				command_print(cmd, "Context breakpoint: asid=0x%8.8" PRIx32 ", len=0x%x, num=%u",
							breakpoint->asid,
							breakpoint->length, breakpoint->number);
			else if ((breakpoint->address != 0) && (breakpoint->asid != 0)) {
				command_print(cmd, "Hybrid breakpoint(IVA): addr=" TARGET_ADDR_FMT ", len=0x%x, num=%u",
							breakpoint->address,
							breakpoint->length, breakpoint->number);
				command_print(cmd, "\t|--->linked with ContextID: 0x%8.8" PRIx32,
							breakpoint->asid);
			} else
				command_print(cmd, "Hardware breakpoint(IVA): addr=" TARGET_ADDR_FMT ", len=0x%x, num=%u",
							breakpoint->address,
							breakpoint->length, breakpoint->number);
		}

		breakpoint = breakpoint->next;
	}
	return ERROR_OK;
}

static int handle_bp_command_set(struct command_invocation *cmd,
		target_addr_t addr, uint32_t asid, unsigned int length, int hw)
{
	struct target *target = get_current_target(cmd->ctx);
	int retval;

	if (asid == 0) {
		retval = breakpoint_add(target, addr, length, hw);
		/* error is always logged in breakpoint_add(), do not print it again */
		if (retval == ERROR_OK)
			command_print(cmd, "breakpoint set at " TARGET_ADDR_FMT "", addr);

	} else if (addr == 0) {
		if (!target->type->add_context_breakpoint) {
			LOG_TARGET_ERROR(target, "Context breakpoint not available");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
		retval = context_breakpoint_add(target, asid, length, hw);
		/* error is always logged in context_breakpoint_add(), do not print it again */
		if (retval == ERROR_OK)
			command_print(cmd, "Context breakpoint set at 0x%8.8" PRIx32 "", asid);

	} else {
		if (!target->type->add_hybrid_breakpoint) {
			LOG_TARGET_ERROR(target, "Hybrid breakpoint not available");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
		retval = hybrid_breakpoint_add(target, addr, asid, length, hw);
		/* error is always logged in hybrid_breakpoint_add(), do not print it again */
		if (retval == ERROR_OK)
			command_print(cmd, "Hybrid breakpoint set at 0x%8.8" PRIx32 "", asid);
	}
	return retval;
}

COMMAND_HANDLER(handle_bp_command)
{
	target_addr_t addr;
	uint32_t asid;
	uint32_t length;
	int hw = BKPT_SOFT;

	switch (CMD_ARGC) {
		case 0:
			return handle_bp_command_list(CMD);

		case 2:
			asid = 0;
			COMMAND_PARSE_ADDRESS(CMD_ARGV[0], addr);
			COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], length);
			return handle_bp_command_set(CMD, addr, asid, length, hw);

		case 3:
			if (strcmp(CMD_ARGV[2], "hw") == 0) {
				hw = BKPT_HARD;
				COMMAND_PARSE_ADDRESS(CMD_ARGV[0], addr);
				COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], length);
				asid = 0;
				return handle_bp_command_set(CMD, addr, asid, length, hw);
			} else if (strcmp(CMD_ARGV[2], "hw_ctx") == 0) {
				hw = BKPT_HARD;
				COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], asid);
				COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], length);
				addr = 0;
				return handle_bp_command_set(CMD, addr, asid, length, hw);
			}
			/* fallthrough */
		case 4:
			hw = BKPT_HARD;
			COMMAND_PARSE_ADDRESS(CMD_ARGV[0], addr);
			COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], asid);
			COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], length);
			return handle_bp_command_set(CMD, addr, asid, length, hw);

		default:
			return ERROR_COMMAND_SYNTAX_ERROR;
	}
}

COMMAND_HANDLER(handle_rbp_command)
{
	int retval;

	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);

	if (!strcmp(CMD_ARGV[0], "all")) {
		retval = breakpoint_remove_all(target);

		if (retval != ERROR_OK) {
			command_print(CMD, "Error encountered during removal of all breakpoints.");
			command_print(CMD, "Some breakpoints may have remained set.");
		}
	} else {
		target_addr_t addr;
		COMMAND_PARSE_ADDRESS(CMD_ARGV[0], addr);

		retval = breakpoint_remove(target, addr);

		if (retval != ERROR_OK)
			command_print(CMD, "Error during removal of breakpoint at address " TARGET_ADDR_FMT, addr);
	}

	return retval;
}

COMMAND_HANDLER(handle_wp_command)
{
	struct target *target = get_current_target(CMD_CTX);

	if (CMD_ARGC == 0) {
		struct watchpoint *watchpoint = target->watchpoints;

		while (watchpoint) {
			char wp_type = (watchpoint->rw == WPT_READ ? 'r' : (watchpoint->rw == WPT_WRITE ? 'w' : 'a'));
			command_print(CMD, "address: " TARGET_ADDR_FMT
					", len: 0x%8.8x"
					", r/w/a: %c, value: 0x%8.8" PRIx64
					", mask: 0x%8.8" PRIx64,
					watchpoint->address,
					watchpoint->length,
					wp_type,
					watchpoint->value,
					watchpoint->mask);
			watchpoint = watchpoint->next;
		}
		return ERROR_OK;
	}

	enum watchpoint_rw type = WPT_ACCESS;
	target_addr_t addr = 0;
	uint32_t length = 0;
	uint64_t data_value = 0x0;
	uint64_t data_mask = WATCHPOINT_IGNORE_DATA_VALUE_MASK;
	bool mask_specified = false;

	switch (CMD_ARGC) {
	case 5:
		COMMAND_PARSE_NUMBER(u64, CMD_ARGV[4], data_mask);
		mask_specified = true;
		/* fall through */
	case 4:
		COMMAND_PARSE_NUMBER(u64, CMD_ARGV[3], data_value);
		// if user specified only data value without mask - the mask should be 0
		if (!mask_specified)
			data_mask = 0;
		/* fall through */
	case 3:
		switch (CMD_ARGV[2][0]) {
		case 'r':
			type = WPT_READ;
			break;
		case 'w':
			type = WPT_WRITE;
			break;
		case 'a':
			type = WPT_ACCESS;
			break;
		default:
			LOG_TARGET_ERROR(target, "invalid watchpoint mode ('%c')", CMD_ARGV[2][0]);
			return ERROR_COMMAND_SYNTAX_ERROR;
		}
		/* fall through */
	case 2:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], length);
		COMMAND_PARSE_ADDRESS(CMD_ARGV[0], addr);
		break;

	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	int retval = watchpoint_add(target, addr, length, type,
			data_value, data_mask);
	if (retval != ERROR_OK)
		LOG_TARGET_ERROR(target, "Failure setting watchpoints");

	return retval;
}

COMMAND_HANDLER(handle_rwp_command)
{
	int retval;

	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);
	if (!strcmp(CMD_ARGV[0], "all")) {
		retval = watchpoint_remove_all(target);

		if (retval != ERROR_OK) {
			command_print(CMD, "Error encountered during removal of all watchpoints.");
			command_print(CMD, "Some watchpoints may have remained set.");
		}
	} else {
		target_addr_t addr;
		COMMAND_PARSE_ADDRESS(CMD_ARGV[0], addr);

		retval = watchpoint_remove(target, addr);

		if (retval != ERROR_OK)
			command_print(CMD, "Error during removal of watchpoint at address " TARGET_ADDR_FMT, addr);
	}

	return retval;
}

/**
 * Translate a virtual address to a physical address.
 *
 * The low-level target implementation must have logged a detailed error
 * which is forwarded to telnet/GDB session.
 */
COMMAND_HANDLER(handle_virt2phys_command)
{
	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	target_addr_t va;
	COMMAND_PARSE_ADDRESS(CMD_ARGV[0], va);
	target_addr_t pa;

	struct target *target = get_current_target(CMD_CTX);
	int retval = target->type->virt2phys(target, va, &pa);
	if (retval == ERROR_OK)
		command_print(CMD, "Physical address " TARGET_ADDR_FMT "", pa);

	return retval;
}

static void write_data(FILE *f, const void *data, size_t len)
{
	size_t written = fwrite(data, 1, len, f);
	if (written != len)
		LOG_ERROR("failed to write %zu bytes: %s", len, strerror(errno));
}

static void write_long(FILE *f, int l, struct target *target)
{
	uint8_t val[4];

	target_buffer_set_u32(target, val, l);
	write_data(f, val, 4);
}

static void write_string(FILE *f, char *s)
{
	write_data(f, s, strlen(s));
}

typedef unsigned char UNIT[2];  /* unit of profiling */

/* Dump a gmon.out histogram file. */
static void write_gmon(uint32_t *samples, uint32_t sample_num, const char *filename, bool with_range,
			uint32_t start_address, uint32_t end_address, struct target *target, uint32_t duration_ms)
{
	uint32_t i;
	FILE *f = fopen(filename, "wb");
	if (!f)
		return;
	write_string(f, "gmon");
	write_long(f, 0x00000001, target); /* Version */
	write_long(f, 0, target); /* padding */
	write_long(f, 0, target); /* padding */
	write_long(f, 0, target); /* padding */

	uint8_t zero = 0;  /* GMON_TAG_TIME_HIST */
	write_data(f, &zero, 1);

	/* figure out bucket size */
	uint32_t min;
	uint32_t max;
	if (with_range) {
		min = start_address;
		max = end_address;
	} else {
		min = samples[0];
		max = samples[0];
		for (i = 0; i < sample_num; i++) {
			if (min > samples[i])
				min = samples[i];
			if (max < samples[i])
				max = samples[i];
		}

		/* max should be (largest sample + 1)
		 * Refer to binutils/gprof/hist.c (find_histogram_for_pc) */
		if (max < UINT32_MAX)
			max++;

		/* gprof requires (max - min) >= 2 */
		while ((max - min) < 2) {
			if (max < UINT32_MAX)
				max++;
			else
				min--;
		}
	}

	uint32_t address_space = max - min;

	/* FIXME: What is the reasonable number of buckets?
	 * The profiling result will be more accurate if there are enough buckets. */
	static const uint32_t max_buckets = 128 * 1024; /* maximum buckets. */
	uint32_t num_buckets = address_space / sizeof(UNIT);
	if (num_buckets > max_buckets)
		num_buckets = max_buckets;
	int *buckets = malloc(sizeof(int) * num_buckets);
	if (!buckets) {
		fclose(f);
		return;
	}
	memset(buckets, 0, sizeof(int) * num_buckets);
	for (i = 0; i < sample_num; i++) {
		uint32_t address = samples[i];

		if ((address < min) || (max <= address))
			continue;

		long long a = address - min;
		long long b = num_buckets;
		long long c = address_space;
		int index_t = (a * b) / c; /* danger!!!! int32 overflows */
		buckets[index_t]++;
	}

	/* append binary memory gmon.out &profile_hist_hdr ((char*)&profile_hist_hdr + sizeof(struct gmon_hist_hdr)) */
	write_long(f, min, target);			/* low_pc */
	write_long(f, max, target);			/* high_pc */
	write_long(f, num_buckets, target);	/* # of buckets */
	float sample_rate = sample_num / (duration_ms / 1000.0);
	write_long(f, sample_rate, target);
	write_string(f, "seconds");
	for (i = 0; i < (15-strlen("seconds")); i++)
		write_data(f, &zero, 1);
	write_string(f, "s");

	/*append binary memory gmon.out profile_hist_data (profile_hist_data + profile_hist_hdr.hist_size) */

	char *data = malloc(2 * num_buckets);
	if (data) {
		for (i = 0; i < num_buckets; i++) {
			int val;
			val = buckets[i];
			if (val > 65535)
				val = 65535;
			data[i * 2] = val&0xff;
			data[i * 2 + 1] = (val >> 8) & 0xff;
		}
		free(buckets);
		write_data(f, data, num_buckets * 2);
		free(data);
	} else
		free(buckets);

	fclose(f);
}

/* profiling samples the CPU PC as quickly as OpenOCD is able,
 * which will be used as a random sampling of PC */
COMMAND_HANDLER(handle_profile_command)
{
	struct target *target = get_current_target(CMD_CTX);

	if ((CMD_ARGC != 2) && (CMD_ARGC != 4))
		return ERROR_COMMAND_SYNTAX_ERROR;

	const uint32_t MAX_PROFILE_SAMPLE_NUM = 1000000;
	uint32_t offset;
	uint32_t num_of_samples;
	int retval = ERROR_OK;
	bool halted_before_profiling = target->state == TARGET_HALTED;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], offset);

	uint32_t start_address = 0;
	uint32_t end_address = 0;
	bool with_range = false;
	if (CMD_ARGC == 4) {
		with_range = true;
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], start_address);
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[3], end_address);
		if (start_address > end_address || (end_address - start_address) < 2) {
			command_print(CMD, "Error: end - start < 2");
			return ERROR_COMMAND_ARGUMENT_INVALID;
		}
	}

	uint32_t *samples = malloc(sizeof(uint32_t) * MAX_PROFILE_SAMPLE_NUM);
	if (!samples) {
		LOG_ERROR("No memory to store samples.");
		return ERROR_FAIL;
	}

	uint64_t timestart_ms = timeval_ms();
	/**
	 * Some cores let us sample the PC without the
	 * annoying halt/resume step; for example, ARMv7 PCSR.
	 * Provide a way to use that more efficient mechanism.
	 */
	retval = target_profiling(target, samples, MAX_PROFILE_SAMPLE_NUM,
				&num_of_samples, offset);
	if (retval != ERROR_OK) {
		free(samples);
		return retval;
	}
	uint32_t duration_ms = timeval_ms() - timestart_ms;

	assert(num_of_samples <= MAX_PROFILE_SAMPLE_NUM);

	retval = target_poll(target);
	if (retval != ERROR_OK) {
		free(samples);
		return retval;
	}

	if (target->state == TARGET_RUNNING && halted_before_profiling) {
		/* The target was halted before we started and is running now. Halt it,
		 * for consistency. */
		retval = target_halt(target);
		if (retval != ERROR_OK) {
			free(samples);
			return retval;
		}
	} else if (target->state == TARGET_HALTED && !halted_before_profiling) {
		/* The target was running before we started and is halted now. Resume
		 * it, for consistency. */
		retval = target_resume(target, true, 0, false, false);
		if (retval != ERROR_OK) {
			free(samples);
			return retval;
		}
	}

	retval = target_poll(target);
	if (retval != ERROR_OK) {
		free(samples);
		return retval;
	}

	write_gmon(samples, num_of_samples, CMD_ARGV[1],
		   with_range, start_address, end_address, target, duration_ms);
	command_print(CMD, "Wrote %s", CMD_ARGV[1]);

	free(samples);
	return retval;
}

COMMAND_HANDLER(handle_target_read_memory)
{
	/*
	 * CMD_ARGV[0] = memory address
	 * CMD_ARGV[1] = desired element width in bits
	 * CMD_ARGV[2] = number of elements to read
	 * CMD_ARGV[3] = optional "phys"
	 */

	if (CMD_ARGC < 3 || CMD_ARGC > 4)
		return ERROR_COMMAND_SYNTAX_ERROR;

	/* Arg 1: Memory address. */
	target_addr_t addr;
	COMMAND_PARSE_NUMBER(u64, CMD_ARGV[0], addr);

	/* Arg 2: Bit width of one element. */
	unsigned int width_bits;
	COMMAND_PARSE_NUMBER(uint, CMD_ARGV[1], width_bits);

	/* Arg 3: Number of elements to read. */
	unsigned int count;
	COMMAND_PARSE_NUMBER(uint, CMD_ARGV[2], count);

	/* Arg 4: Optional 'phys'. */
	bool is_phys = false;
	if (CMD_ARGC == 4) {
		if (strcmp(CMD_ARGV[3], "phys")) {
			command_print(CMD, "invalid argument '%s', must be 'phys'", CMD_ARGV[3]);
			return ERROR_COMMAND_ARGUMENT_INVALID;
		}

		is_phys = true;
	}

	switch (width_bits) {
	case 8:
	case 16:
	case 32:
	case 64:
		break;
	default:
		command_print(CMD, "invalid width, must be 8, 16, 32 or 64");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	if (count > 65536) {
		command_print(CMD, "too large read request, exceeds 64K elements");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	const unsigned int width = width_bits / 8;
	/* -1 is needed to handle cases when (addr + count * width) results in zero
	 * due to overflow.
	 */
	if ((addr + count * width - 1) < addr) {
		command_print(CMD, "memory region wraps over address zero");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	struct target *target = get_current_target(CMD_CTX);

	const size_t buffersize = 4096;
	uint8_t *buffer = malloc(buffersize);

	if (!buffer) {
		LOG_ERROR("Failed to allocate memory");
		return ERROR_FAIL;
	}

	char *separator = "";
	while (count > 0) {
		const unsigned int max_chunk_len = buffersize / width;
		const size_t chunk_len = MIN(count, max_chunk_len);

		int retval;

		if (is_phys)
			retval = target_read_phys_memory(target, addr, width, chunk_len, buffer);
		else
			retval = target_read_memory(target, addr, width, chunk_len, buffer);

		if (retval != ERROR_OK) {
			LOG_DEBUG("read at " TARGET_ADDR_FMT " with width=%u and count=%zu failed",
				addr, width_bits, chunk_len);
			/*
			 * FIXME: we append the errmsg to the list of value already read.
			 * Add a way to flush and replace old output, but LOG_DEBUG() it
			 */
			command_print(CMD, "failed to read memory");
			free(buffer);
			return retval;
		}

		for (size_t i = 0; i < chunk_len ; i++) {
			uint64_t v = 0;

			switch (width) {
			case 8:
				v = target_buffer_get_u64(target, &buffer[i * width]);
				break;
			case 4:
				v = target_buffer_get_u32(target, &buffer[i * width]);
				break;
			case 2:
				v = target_buffer_get_u16(target, &buffer[i * width]);
				break;
			case 1:
				v = buffer[i];
				break;
			}

			command_print_sameline(CMD, "%s0x%" PRIx64, separator, v);
			separator = " ";
		}

		count -= chunk_len;
		addr += chunk_len * width;
	}

	free(buffer);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_write_memory)
{
	/*
	 * CMD_ARGV[0] = memory address
	 * CMD_ARGV[1] = desired element width in bits
	 * CMD_ARGV[2] = list of data to write
	 * CMD_ARGV[3] = optional "phys"
	 */

	if (CMD_ARGC < 3 || CMD_ARGC > 4)
		return ERROR_COMMAND_SYNTAX_ERROR;

	/* Arg 1: Memory address. */
	target_addr_t addr;
	COMMAND_PARSE_NUMBER(u64, CMD_ARGV[0], addr);

	/* Arg 2: Bit width of one element. */
	unsigned int width_bits;
	COMMAND_PARSE_NUMBER(uint, CMD_ARGV[1], width_bits);

	/* Arg 3: Elements to write. */
	size_t count = Jim_ListLength(CMD_CTX->interp, CMD_JIMTCL_ARGV[2]);

	/* Arg 4: Optional 'phys'. */
	bool is_phys = false;

	if (CMD_ARGC == 4) {
		if (strcmp(CMD_ARGV[3], "phys")) {
			command_print(CMD, "invalid argument '%s', must be 'phys'", CMD_ARGV[3]);
			return ERROR_COMMAND_ARGUMENT_INVALID;
		}

		is_phys = true;
	}

	switch (width_bits) {
	case 8:
	case 16:
	case 32:
	case 64:
		break;
	default:
		command_print(CMD, "invalid width, must be 8, 16, 32 or 64");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	if (count > 65536) {
		command_print(CMD, "too large memory write request, exceeds 64K elements");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	const unsigned int width = width_bits / 8;
	/* -1 is needed to handle cases when (addr + count * width) results in zero
	 * due to overflow.
	 */
	if ((addr + count * width - 1) < addr) {
		command_print(CMD, "memory region wraps over address zero");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	struct target *target = get_current_target(CMD_CTX);

	const size_t buffersize = 4096;
	uint8_t *buffer = malloc(buffersize);

	if (!buffer) {
		LOG_ERROR("Failed to allocate memory");
		return ERROR_FAIL;
	}

	size_t j = 0;

	while (count > 0) {
		const unsigned int max_chunk_len = buffersize / width;
		const size_t chunk_len = MIN(count, max_chunk_len);

		for (size_t i = 0; i < chunk_len; i++, j++) {
			Jim_Obj *tmp = Jim_ListGetIndex(CMD_CTX->interp, CMD_JIMTCL_ARGV[2], j);
			jim_wide element_wide;
			int jimretval = Jim_GetWide(CMD_CTX->interp, tmp, &element_wide);
			if (jimretval != JIM_OK) {
				command_print(CMD, "invalid value \"%s\"", Jim_GetString(tmp, NULL));
				free(buffer);
				return ERROR_COMMAND_ARGUMENT_INVALID;
			}

			const uint64_t v = element_wide;

			switch (width) {
			case 8:
				target_buffer_set_u64(target, &buffer[i * width], v);
				break;
			case 4:
				target_buffer_set_u32(target, &buffer[i * width], v);
				break;
			case 2:
				target_buffer_set_u16(target, &buffer[i * width], v);
				break;
			case 1:
				buffer[i] = v & 0x0ff;
				break;
			}
		}

		count -= chunk_len;

		int retval;

		if (is_phys)
			retval = target_write_phys_memory(target, addr, width, chunk_len, buffer);
		else
			retval = target_write_memory(target, addr, width, chunk_len, buffer);

		if (retval != ERROR_OK) {
			LOG_DEBUG("write at " TARGET_ADDR_FMT " with width=%u and count=%zu failed",
				addr,  width_bits, chunk_len);
			command_print(CMD, "failed to write memory");
			free(buffer);
			return retval;
		}

		addr += chunk_len * width;
	}

	free(buffer);

	return ERROR_OK;
}

/* FIX? should we propagate errors here rather than printing them
 * and continuing?
 */
void target_handle_event(struct target *target, enum target_event e)
{
	struct target_event_action *teap;
	int retval;

	list_for_each_entry(teap, &target->events_action, list) {
		if (teap->event == e) {
			LOG_DEBUG("target: %s (%s) event: %d (%s) action: %s",
					   target_name(target),
					   target_type_name(target),
					   e,
					   target_event_name(e),
					   Jim_GetString(teap->body, NULL));

			/* Override current target by the target an event
			 * is issued from (lot of scripts need it).
			 * Return back to previous override as soon
			 * as the handler processing is done */
			struct command_context *cmd_ctx = current_command_context(teap->interp);
			struct target *saved_target_override = cmd_ctx->current_target_override;
			cmd_ctx->current_target_override = target;

			retval = Jim_EvalObj(teap->interp, teap->body);

			cmd_ctx->current_target_override = saved_target_override;

			if (retval == ERROR_COMMAND_CLOSE_CONNECTION)
				return;

			if (retval == JIM_RETURN)
				retval = teap->interp->returnCode;

			if (retval != JIM_OK) {
				Jim_MakeErrorMessage(teap->interp);
				LOG_TARGET_ERROR(target, "Execution of event %s failed:\n%s",
						  target_event_name(e),
						  Jim_GetString(Jim_GetResult(teap->interp), NULL));
				/* clean both error code and stacktrace before return */
				Jim_Eval(teap->interp, "error \"\" \"\"");
			}
		}
	}
}

COMMAND_HANDLER(handle_target_get_reg)
{
	if (CMD_ARGC < 1 || CMD_ARGC > 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	bool force = false;
	Jim_Obj *next_argv = CMD_JIMTCL_ARGV[0];

	if (CMD_ARGC == 2) {
		if (strcmp(CMD_ARGV[0], "-force")) {
			command_print(CMD, "invalid argument '%s', must be '-force'", CMD_ARGV[0]);
			return ERROR_COMMAND_ARGUMENT_INVALID;
		}

		force = true;
		next_argv = CMD_JIMTCL_ARGV[1];
	}

	const int length = Jim_ListLength(CMD_CTX->interp, next_argv);

	const struct target *target = get_current_target(CMD_CTX);

	for (int i = 0; i < length; i++) {
		Jim_Obj *elem = Jim_ListGetIndex(CMD_CTX->interp, next_argv, i);

		const char *reg_name = Jim_String(elem);

		struct reg *reg = register_get_by_name(target->reg_cache, reg_name, false);

		if (!reg || !reg->exist) {
			command_print(CMD, "unknown register '%s'", reg_name);
			return ERROR_COMMAND_ARGUMENT_INVALID;
		}

		if (force || !reg->valid) {
			int retval = reg->type->get(reg);

			if (retval != ERROR_OK) {
				command_print(CMD, "failed to read register '%s'", reg_name);
				return retval;
			}
		}

		char *reg_value = buf_to_hex_str(reg->value, reg->size);

		if (!reg_value) {
			LOG_ERROR("Failed to allocate memory");
			return ERROR_FAIL;
		}

		command_print(CMD, "%s 0x%s", reg_name, reg_value);

		free(reg_value);
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_set_reg_command)
{
	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	int tmp;
#if JIM_VERSION >= 80
	Jim_Obj **dict = Jim_DictPairs(CMD_CTX->interp, CMD_JIMTCL_ARGV[0], &tmp);

	if (!dict)
		return ERROR_FAIL;
#else
	Jim_Obj **dict;
	int ret = Jim_DictPairs(CMD_CTX->interp, CMD_JIMTCL_ARGV[0], &dict, &tmp);

	if (ret != JIM_OK)
		return ERROR_FAIL;
#endif

	const unsigned int length = tmp;

	const struct target *target = get_current_target(CMD_CTX);
	assert(target);

	for (unsigned int i = 0; i < length; i += 2) {
		const char *reg_name = Jim_String(dict[i]);
		const char *reg_value = Jim_String(dict[i + 1]);
		struct reg *reg = register_get_by_name(target->reg_cache, reg_name, false);

		if (!reg || !reg->exist) {
			command_print(CMD, "unknown register '%s'", reg_name);
			return ERROR_FAIL;
		}

		uint8_t *buf = malloc(DIV_ROUND_UP(reg->size, 8));
		if (!buf) {
			LOG_ERROR("Failed to allocate memory");
			return ERROR_FAIL;
		}

		int retval = CALL_COMMAND_HANDLER(command_parse_str_to_buf, reg_value, buf, reg->size);
		if (retval != ERROR_OK) {
			free(buf);
			return retval;
		}

		retval = reg->type->set(reg, buf);
		free(buf);

		if (retval != ERROR_OK) {
			command_print(CMD, "failed to set '%s' to register '%s'",
				reg_value, reg_name);
			return retval;
		}
	}

	return ERROR_OK;
}

/**
 * Returns true only if the target has a handler for the specified event.
 */
bool target_has_event_action(const struct target *target, enum target_event event)
{
	struct target_event_action *teap;

	list_for_each_entry(teap, &target->events_action, list) {
		if (teap->event == event)
			return true;
	}
	return false;
}

enum target_cfg_param {
	TCFG_TYPE,
	TCFG_EVENT,
	TCFG_WORK_AREA_VIRT,
	TCFG_WORK_AREA_PHYS,
	TCFG_WORK_AREA_SIZE,
	TCFG_WORK_AREA_BACKUP,
	TCFG_ENDIAN,
	TCFG_COREID,
	TCFG_CHAIN_POSITION,
	TCFG_DBGBASE,
	TCFG_RTOS,
	TCFG_DEFER_EXAMINE,
	TCFG_GDB_PORT,
	TCFG_GDB_MAX_CONNECTIONS,
};

static struct nvp nvp_config_opts[] = {
	{ .name = "-type",             .value = TCFG_TYPE },
	{ .name = "-event",            .value = TCFG_EVENT },
	{ .name = "-work-area-virt",   .value = TCFG_WORK_AREA_VIRT },
	{ .name = "-work-area-phys",   .value = TCFG_WORK_AREA_PHYS },
	{ .name = "-work-area-size",   .value = TCFG_WORK_AREA_SIZE },
	{ .name = "-work-area-backup", .value = TCFG_WORK_AREA_BACKUP },
	{ .name = "-endian",           .value = TCFG_ENDIAN },
	{ .name = "-coreid",           .value = TCFG_COREID },
	{ .name = "-chain-position",   .value = TCFG_CHAIN_POSITION },
	{ .name = "-dbgbase",          .value = TCFG_DBGBASE },
	{ .name = "-rtos",             .value = TCFG_RTOS },
	{ .name = "-defer-examine",    .value = TCFG_DEFER_EXAMINE },
	{ .name = "-gdb-port",         .value = TCFG_GDB_PORT },
	{ .name = "-gdb-max-connections",   .value = TCFG_GDB_MAX_CONNECTIONS },
	{ .name = NULL, .value = -1 }
};

static COMMAND_HELPER(target_configure, struct target *target, unsigned int index, bool is_configure)
{
	const struct nvp *n;
	int retval;

	/* parse config or cget options ... */
	while (index < CMD_ARGC) {
		if (target->type->target_jim_configure) {
			/* target defines a configure function */
			/* target gets first dibs on parameters */
			struct jim_getopt_info goi;
			jim_getopt_setup(&goi, CMD_CTX->interp, CMD_ARGC - index, CMD_JIMTCL_ARGV + index);
			goi.is_configure = is_configure;
			int e = (*target->type->target_jim_configure)(target, &goi);
			index = CMD_ARGC - goi.argc;

			int reslen;
			const char *result = Jim_GetString(Jim_GetResult(CMD_CTX->interp), &reslen);
			if (reslen > 0)
				command_print(CMD, "%s", result);

			if (e == JIM_OK) {
				/* more? */
				continue;
			}
			if (e == JIM_ERR) {
				/* An error */
				return ERROR_FAIL;
			}
			/* otherwise we 'continue' below */
		}
		n = nvp_name2value(nvp_config_opts, CMD_ARGV[index]);
		if (!n->name) {
			nvp_unknown_command_print(CMD, nvp_config_opts, NULL, CMD_ARGV[index]);
			return ERROR_COMMAND_ARGUMENT_INVALID;
		}
		index++;
		switch (n->value) {
		case TCFG_TYPE:
			/* not settable */
			if (is_configure) {
				command_print(CMD, "not settable: %s", n->name);
				return ERROR_COMMAND_ARGUMENT_INVALID;
			}
			if (index != CMD_ARGC)
				return ERROR_COMMAND_SYNTAX_ERROR;
			command_print(CMD, "%s", target_type_name(target));
			/* loop for more */
			break;

		case TCFG_EVENT:
			if (index == CMD_ARGC) {
				command_print(CMD, "expecting %s event-name event-body",
						CMD_ARGV[index - 1]);
				return ERROR_COMMAND_ARGUMENT_INVALID;
			}

			n = nvp_name2value(nvp_target_event, CMD_ARGV[index]);
			if (!n->name) {
				nvp_unknown_command_print(CMD, nvp_target_event, CMD_ARGV[index - 1], CMD_ARGV[index]);
				return ERROR_COMMAND_ARGUMENT_INVALID;
			}
			index++;

			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "expecting %s %s event-body",
							CMD_ARGV[index - 2], CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
			}

			{
				struct target_event_action *teap;

				/* replace existing? */
				list_for_each_entry(teap, &target->events_action, list)
					if (teap->event == (enum target_event)n->value)
						break;

				/* not found! */
				if (&teap->list == &target->events_action)
					teap = NULL;

				if (is_configure) {
					/* START_DEPRECATED_TPIU */
					if (n->value == TARGET_EVENT_TRACE_CONFIG)
						LOG_INFO("DEPRECATED target event %s; use TPIU events {pre,post}-{enable,disable}", n->name);
					/* END_DEPRECATED_TPIU */

					if (strlen(CMD_ARGV[index]) == 0) {
						/* empty action, drop existing one */
						if (teap) {
							list_del(&teap->list);
							Jim_DecrRefCount(teap->interp, teap->body);
							free(teap);
						}
						index++;
						break;
					}

					bool replace = true;
					if (!teap) {
						/* create new */
						teap = calloc(1, sizeof(*teap));
						replace = false;
					}
					teap->event = n->value;
					teap->interp = CMD_CTX->interp;
					if (teap->body)
						Jim_DecrRefCount(teap->interp, teap->body);
					/* use jim object to keep its reference on tcl file and line */
					/* TODO: need duplicate? isn't IncrRefCount enough? */
					teap->body  = Jim_DuplicateObj(teap->interp, CMD_JIMTCL_ARGV[index++]);
					/*
					 * FIXME:
					 *     Tcl/TK - "tk events" have a nice feature.
					 *     See the "BIND" command.
					 *    We should support that here.
					 *     You can specify %X and %Y in the event code.
					 *     The idea is: %T - target name.
					 *     The idea is: %N - target number
					 *     The idea is: %E - event name.
					 */
					Jim_IncrRefCount(teap->body);

					if (!replace) {
						/* add to head of event list */
						list_add(&teap->list, &target->events_action);
					}
				} else {
					/* cget */
					if (index != CMD_ARGC)
						return ERROR_COMMAND_SYNTAX_ERROR;

					if (teap)
						command_print(CMD, "%s", Jim_GetString(teap->body, NULL));
				}
			}
			/* loop for more */
			break;

		case TCFG_WORK_AREA_VIRT:
			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				COMMAND_PARSE_NUMBER(u64, CMD_ARGV[index], target->working_area_virt);
				index++;
				target->working_area_virt_spec = true;
				target_free_all_working_areas(target);
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				command_print(CMD, TARGET_ADDR_FMT, target->working_area_virt);
			}
			/* loop for more */
			break;

		case TCFG_WORK_AREA_PHYS:
			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				COMMAND_PARSE_NUMBER(u64, CMD_ARGV[index], target->working_area_phys);
				index++;
				target->working_area_phys_spec = true;
				target_free_all_working_areas(target);
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				command_print(CMD, TARGET_ADDR_FMT, target->working_area_phys);
			}
			/* loop for more */
			break;

		case TCFG_WORK_AREA_SIZE:
			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				COMMAND_PARSE_NUMBER(u32, CMD_ARGV[index], target->working_area_size);
				index++;
				target_free_all_working_areas(target);
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				command_print(CMD, "0x%08" PRIx32, target->working_area_size);
			}
			/* loop for more */
			break;

		case TCFG_WORK_AREA_BACKUP:
			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				retval = command_parse_bool_arg(CMD_ARGV[index], &target->backup_working_area);
				if (retval != ERROR_OK)
					return retval;
				index++;
				target_free_all_working_areas(target);
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				command_print(CMD, target->backup_working_area ? "1" : "0");
			}
			/* loop for more */
			break;

		case TCFG_ENDIAN:
			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				n = nvp_name2value(nvp_target_endian, CMD_ARGV[index]);
				if (!n->name) {
					nvp_unknown_command_print(CMD, nvp_target_endian, CMD_ARGV[index - 1], CMD_ARGV[index]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				index++;
				target->endianness = n->value;
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				n = nvp_value2name(nvp_target_endian, target->endianness);
				if (!n->name) {
					target->endianness = TARGET_LITTLE_ENDIAN;
					n = nvp_value2name(nvp_target_endian, target->endianness);
				}
				command_print(CMD, "%s", n->name);
			}
			/* loop for more */
			break;

		case TCFG_COREID:
			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				COMMAND_PARSE_NUMBER(s32, CMD_ARGV[index], target->coreid);
				index++;
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				command_print(CMD, "%" PRIi32, target->coreid);
			}
			/* loop for more */
			break;

		case TCFG_CHAIN_POSITION:
			if (is_configure) {
				if (target->has_dap) {
					command_print(CMD, "target requires -dap parameter instead of -chain-position!");
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}

				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				struct jtag_tap *tap = jtag_tap_by_string(CMD_ARGV[index]);
				if (!tap) {
					command_print(CMD, "Tap '%s' could not be found", CMD_ARGV[index]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				index++;
				target->tap = tap;
				target->tap_configured = true;
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				command_print(CMD, "%s", target->tap->dotted_name);
			}
			/* loop for more */
			break;

		case TCFG_DBGBASE:
			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				COMMAND_PARSE_NUMBER(u32, CMD_ARGV[index], target->dbgbase);
				index++;
				target->dbgbase_set = true;
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				command_print(CMD, "0x%08" PRIx32, target->dbgbase);
			}
			/* loop for more */
			break;

		case TCFG_RTOS:
			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}
				retval = rtos_create(CMD, target, CMD_ARGV[index]);
				if (retval != ERROR_OK)
					return retval;
				index++;
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				if (target->rtos)
					command_print(CMD, "%s", target->rtos->type->name);
			}
			/* loop for more */
			break;

		case TCFG_DEFER_EXAMINE:
			if (is_configure)
				target->defer_examine = true;
			else
				command_print(CMD, "%s", target->defer_examine ? "true" : "false");
			/* loop for more */
			break;

		case TCFG_GDB_PORT:
			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}

				/* TODO: generalize test of COMMAND_CONFIG */
				if (CMD_CTX->mode != COMMAND_CONFIG) {
					command_print(CMD, "-gdb-port must be configured before 'init'");
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}

				char *s = strdup(CMD_ARGV[index]);
				if (!s) {
					LOG_ERROR("Out of memory");
					return ERROR_FAIL;
				}
				free(target->gdb_port_override);
				target->gdb_port_override = s;
				index++;
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				command_print(CMD, "%s", target->gdb_port_override ? target->gdb_port_override : "undefined");
			}
			/* loop for more */
			break;

		case TCFG_GDB_MAX_CONNECTIONS:
			if (is_configure) {
				if (index == CMD_ARGC) {
					command_print(CMD, "missing argument to %s", CMD_ARGV[index - 1]);
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}

				if (CMD_CTX->mode != COMMAND_CONFIG) {
					command_print(CMD, "-gdb-max-connections must be configured before 'init'");
					return ERROR_COMMAND_ARGUMENT_INVALID;
				}

				COMMAND_PARSE_NUMBER(int, CMD_ARGV[index], target->gdb_max_connections);
				index++;
				if (target->gdb_max_connections < 0)
					target->gdb_max_connections = CONNECTION_LIMIT_UNLIMITED;
			} else {
				if (index != CMD_ARGC)
					return ERROR_COMMAND_SYNTAX_ERROR;
				command_print(CMD, "%d", target->gdb_max_connections);
			}
			/* loop for more */
			break;
		}
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_configure)
{
	if (!CMD_ARGC)
		return ERROR_COMMAND_SYNTAX_ERROR;

	bool is_configure = !strcmp(CMD_NAME, "configure");

	struct target *target = get_current_target(CMD_CTX);

	return CALL_COMMAND_HANDLER(target_configure, target, 0, is_configure);
}

COMMAND_HANDLER(handle_target_examine)
{
	bool allow_defer = false;

	if (CMD_ARGC > 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (CMD_ARGC == 1) {
		if (strcmp(CMD_ARGV[0], "allow-defer"))
			return ERROR_COMMAND_ARGUMENT_INVALID;
		allow_defer = true;
	}

	struct target *target = get_current_target(CMD_CTX);
	if (!target->tap->enabled) {
		command_print(CMD, "[TAP is disabled]");
		return ERROR_FAIL;
	}

	if (allow_defer && target->defer_examine) {
		LOG_INFO("Deferring arp_examine of %s", target_name(target));
		LOG_INFO("Use arp_examine command to examine it manually!");
		return ERROR_OK;
	}

	int retval = target->type->examine(target);
	if (retval != ERROR_OK) {
		target_reset_examined(target);
		return retval;
	}

	target_set_examined(target);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_was_examined)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);

	command_print(CMD, "%d", target_was_examined(target) ? 1 : 0);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_examine_deferred)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);

	command_print(CMD, "%d", target->defer_examine ? 1 : 0);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_halt_gdb)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);

	return target_call_event_callbacks(target, TARGET_EVENT_GDB_HALT);
}

COMMAND_HANDLER(handle_target_poll)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);
	if (!target->tap->enabled) {
		command_print(CMD, "[TAP is disabled]");
		return ERROR_FAIL;
	}

	if (!(target_was_examined(target)))
		return ERROR_TARGET_NOT_EXAMINED;

	return target->type->poll(target);
}

COMMAND_HANDLER(handle_target_reset)
{
	if (CMD_ARGC != 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	const struct nvp *n = nvp_name2value(nvp_assert, CMD_ARGV[0]);
	if (!n->name) {
		nvp_unknown_command_print(CMD, nvp_assert, NULL, CMD_ARGV[0]);
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	/* the halt or not param */
	int a;
	COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], a);

	struct target *target = get_current_target(CMD_CTX);
	if (!target->tap->enabled) {
		command_print(CMD, "[TAP is disabled]");
		return ERROR_FAIL;
	}

	if (!target->type->assert_reset || !target->type->deassert_reset) {
		command_print(CMD, "No target-specific reset for %s", target_name(target));
		return ERROR_FAIL;
	}

	/* determine if we should halt or not. */
	target->reset_halt = (a != 0);
	/* When this happens - all workareas are invalid. */
	target_free_all_working_areas_restore(target, 0);

	/* do the assert */
	if (n->value == NVP_ASSERT) {
		int retval = target->type->assert_reset(target);
		if (target->defer_examine)
			target_reset_examined(target);
		return retval;
	}

	return target->type->deassert_reset(target);
}

COMMAND_HANDLER(handle_target_halt)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);
	if (!target->tap->enabled) {
		command_print(CMD, "[TAP is disabled]");
		return ERROR_FAIL;
	}

	return target->type->halt(target);
}

COMMAND_HANDLER(handle_target_wait_state)
{
	if (CMD_ARGC != 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	const struct nvp *n = nvp_name2value(nvp_target_state, CMD_ARGV[0]);
	if (!n->name) {
		nvp_unknown_command_print(CMD, nvp_target_state, NULL, CMD_ARGV[0]);
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	unsigned int a;
	COMMAND_PARSE_NUMBER(uint, CMD_ARGV[1], a);

	struct target *target = get_current_target(CMD_CTX);
	if (!target->tap->enabled) {
		command_print(CMD, "[TAP is disabled]");
		return ERROR_FAIL;
	}

	int retval = target_wait_state(target, n->value, a);
	if (retval != ERROR_OK) {
		command_print(CMD,
				"target: %s wait %s fails (%d) %s",
				target_name(target), n->name,
				retval, target_strerror_safe(retval));
		return retval;
	}
	return ERROR_OK;
}
/* List for human, Events defined for this target.
 * scripts/programs should use 'name cget -event NAME'
 */
COMMAND_HANDLER(handle_target_event_list)
{
	struct target *target = get_current_target(CMD_CTX);
	struct target_event_action *teap;

	command_print(CMD, "Event actions for target %s\n",
				   target_name(target));
	command_print(CMD, "%-25s | Body", "Event");
	command_print(CMD, "------------------------- | "
			"----------------------------------------");

	list_for_each_entry(teap, &target->events_action, list)
		command_print(CMD, "%-25s | %s",
				target_event_name(teap->event),
				Jim_GetString(teap->body, NULL));

	command_print(CMD, "***END***");
	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_current_state)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);

	command_print(CMD, "%s", target_state_name(target));

	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_debug_reason)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target(CMD_CTX);


	const char *debug_reason = nvp_value2name(nvp_target_debug_reason,
		target->debug_reason)->name;

	if (!debug_reason) {
		command_print(CMD, "bug: invalid debug reason (%d)",
			target->debug_reason);
		return ERROR_FAIL;
	}

	command_print(CMD, "%s", debug_reason);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_invoke_event)
{
	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	const struct nvp *n = nvp_name2value(nvp_target_event, CMD_ARGV[0]);
	if (!n->name) {
		nvp_unknown_command_print(CMD, nvp_target_event, NULL, CMD_ARGV[0]);
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	struct target *target = get_current_target(CMD_CTX);
	target_handle_event(target, n->value);
	return ERROR_OK;
}

static const struct command_registration target_instance_command_handlers[] = {
	{
		.name = "configure",
		.mode = COMMAND_ANY,
		.handler = handle_target_configure,
		.help  = "configure a new target for use",
		.usage = "[target_attribute ...]",
	},
	{
		.name = "cget",
		.mode = COMMAND_ANY,
		.handler = handle_target_configure,
		.help  = "returns the specified target attribute",
		.usage = "target_attribute",
	},
	{
		.name = "mwd",
		.handler = handle_mw_command,
		.mode = COMMAND_EXEC,
		.help = "Write 64-bit word(s) to target memory",
		.usage = "address data [count]",
	},
	{
		.name = "mww",
		.handler = handle_mw_command,
		.mode = COMMAND_EXEC,
		.help = "Write 32-bit word(s) to target memory",
		.usage = "address data [count]",
	},
	{
		.name = "mwh",
		.handler = handle_mw_command,
		.mode = COMMAND_EXEC,
		.help = "Write 16-bit half-word(s) to target memory",
		.usage = "address data [count]",
	},
	{
		.name = "mwb",
		.handler = handle_mw_command,
		.mode = COMMAND_EXEC,
		.help = "Write byte(s) to target memory",
		.usage = "address data [count]",
	},
	{
		.name = "mdd",
		.handler = handle_md_command,
		.mode = COMMAND_EXEC,
		.help = "Display target memory as 64-bit words",
		.usage = "address [count]",
	},
	{
		.name = "mdw",
		.handler = handle_md_command,
		.mode = COMMAND_EXEC,
		.help = "Display target memory as 32-bit words",
		.usage = "address [count]",
	},
	{
		.name = "mdh",
		.handler = handle_md_command,
		.mode = COMMAND_EXEC,
		.help = "Display target memory as 16-bit half-words",
		.usage = "address [count]",
	},
	{
		.name = "mdb",
		.handler = handle_md_command,
		.mode = COMMAND_EXEC,
		.help = "Display target memory as 8-bit bytes",
		.usage = "address [count]",
	},
	{
		.name = "get_reg",
		.mode = COMMAND_EXEC,
		.handler = handle_target_get_reg,
		.help = "Get register values from the target",
		.usage = "[-force] list",
	},
	{
		.name = "set_reg",
		.mode = COMMAND_EXEC,
		.handler = handle_set_reg_command,
		.help = "Set target register values",
		.usage = "dict",
	},
	{
		.name = "read_memory",
		.mode = COMMAND_EXEC,
		.handler = handle_target_read_memory,
		.help = "Read Tcl list of 8/16/32/64 bit numbers from target memory",
		.usage = "address width count ['phys']",
	},
	{
		.name = "write_memory",
		.mode = COMMAND_EXEC,
		.handler = handle_target_write_memory,
		.help = "Write Tcl list of 8/16/32/64 bit numbers to target memory",
		.usage = "address width data ['phys']",
	},
	{
		.name = "eventlist",
		.handler = handle_target_event_list,
		.mode = COMMAND_EXEC,
		.help = "displays a table of events defined for this target",
		.usage = "",
	},
	{
		.name = "curstate",
		.mode = COMMAND_EXEC,
		.handler = handle_target_current_state,
		.help = "displays the current state of this target",
		.usage = "",
	},
	{
		.name = "debug_reason",
		.mode = COMMAND_EXEC,
		.handler = handle_target_debug_reason,
		.help = "displays the debug reason of this target",
		.usage = "",
	},
	{
		.name = "arp_examine",
		.mode = COMMAND_EXEC,
		.handler = handle_target_examine,
		.help = "used internally for reset processing",
		.usage = "['allow-defer']",
	},
	{
		.name = "was_examined",
		.mode = COMMAND_EXEC,
		.handler = handle_target_was_examined,
		.help = "used internally for reset processing",
		.usage = "",
	},
	{
		.name = "examine_deferred",
		.mode = COMMAND_EXEC,
		.handler = handle_target_examine_deferred,
		.help = "used internally for reset processing",
		.usage = "",
	},
	{
		.name = "arp_halt_gdb",
		.mode = COMMAND_EXEC,
		.handler = handle_target_halt_gdb,
		.help = "used internally for reset processing to halt GDB",
		.usage = "",
	},
	{
		.name = "arp_poll",
		.mode = COMMAND_EXEC,
		.handler = handle_target_poll,
		.help = "used internally for reset processing",
		.usage = "",
	},
	{
		.name = "arp_reset",
		.mode = COMMAND_EXEC,
		.handler = handle_target_reset,
		.help = "used internally for reset processing",
		.usage = "'assert'|'deassert' halt",
	},
	{
		.name = "arp_halt",
		.mode = COMMAND_EXEC,
		.handler = handle_target_halt,
		.help = "used internally for reset processing",
		.usage = "",
	},
	{
		.name = "arp_waitstate",
		.mode = COMMAND_EXEC,
		.handler = handle_target_wait_state,
		.help = "used internally for reset processing",
		.usage = "statename timeoutmsecs",
	},
	{
		.name = "invoke-event",
		.mode = COMMAND_EXEC,
		.handler = handle_target_invoke_event,
		.help = "invoke handler for specified event",
		.usage = "event_name",
	},
	COMMAND_REGISTRATION_DONE
};

COMMAND_HANDLER(handle_target_create)
{
	int retval = ERROR_OK;

	if (CMD_ARGC < 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	/* check if the target name clashes with an existing command name */
	Jim_Cmd *jimcmd = Jim_GetCommand(CMD_CTX->interp, CMD_JIMTCL_ARGV[0], JIM_NONE);
	if (jimcmd) {
		command_print(CMD, "Command/target: %s Exists", CMD_ARGV[0]);
		return ERROR_FAIL;
	}

	/* TYPE */
	const char *cp = CMD_ARGV[1];
	struct transport *tr = get_current_transport();
	if (tr && tr->override_target) {
		retval = tr->override_target(&cp);
		if (retval != ERROR_OK) {
			command_print(CMD, "The selected transport doesn't support this target");
			return retval;
		}
		LOG_INFO("The selected transport took over low-level target control. The results might differ compared to plain JTAG/SWD");
	}
	/* now does target type exist */
	size_t x;
	for (x = 0 ; x < ARRAY_SIZE(target_types) ; x++) {
		if (strcmp(cp, target_types[x]->name) == 0) {
			/* found */
			break;
		}
	}
	if (x == ARRAY_SIZE(target_types)) {
		char *all = NULL;
		for (x = 0 ; x < ARRAY_SIZE(target_types) ; x++) {
			char *prev = all;
			if (all)
				all = alloc_printf("%s, %s", all, target_types[x]->name);
			else
				all = alloc_printf("%s", target_types[x]->name);
			free(prev);
			if (!all) {
				LOG_ERROR("Out of memory");
				return ERROR_FAIL;
			}
		}
		command_print(CMD, "Unknown target type %s, try one of %s", cp, all);
		free(all);
		return ERROR_FAIL;
	}

	/* Create it */
	struct target *target = calloc(1, sizeof(struct target));
	if (!target) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}

	/* set empty smp cluster */
	target->smp_targets = &empty_smp_targets;

	/* allocate memory for each unique target type */
	target->type = malloc(sizeof(struct target_type));
	if (!target->type) {
		LOG_ERROR("Out of memory");
		free(target);
		return ERROR_FAIL;
	}

	memcpy(target->type, target_types[x], sizeof(struct target_type));

	/* default to first core, override with -coreid */
	target->coreid = 0;

	target->working_area        = 0x0;
	target->working_area_size   = 0x0;
	target->working_areas       = NULL;
	target->backup_working_area = false;

	target->state               = TARGET_UNKNOWN;
	target->debug_reason        = DBG_REASON_UNDEFINED;
	target->reg_cache           = NULL;
	target->breakpoints         = NULL;
	target->watchpoints         = NULL;
	target->next                = NULL;
	target->arch_info           = NULL;

	target->verbose_halt_msg	= true;

	target->halt_issued			= false;

	INIT_LIST_HEAD(&target->events_action);

	/* initialize trace information */
	target->trace_info = calloc(1, sizeof(struct trace));
	if (!target->trace_info) {
		LOG_ERROR("Out of memory");
		free(target->type);
		free(target);
		return ERROR_FAIL;
	}

	target->dbgmsg          = NULL;
	target->dbg_msg_enabled = false;

	target->endianness = TARGET_ENDIAN_UNKNOWN;

	target->rtos = NULL;
	target->rtos_auto_detect = false;

	target->gdb_port_override = NULL;
	target->gdb_max_connections = 1;

	target->cmd_name = strdup(CMD_ARGV[0]);
	if (!target->cmd_name) {
		LOG_ERROR("Out of memory");
		free(target->trace_info);
		free(target->type);
		free(target);
		return ERROR_FAIL;
	}

	/* Do the rest as "configure" options */
	bool is_configure = true;
	retval = CALL_COMMAND_HANDLER(target_configure, target, 2, is_configure);
	if (retval == ERROR_OK) {
		if (target->has_dap) {
			if (!target->dap_configured) {
				command_print(CMD, "-dap ?name? required when creating target");
				retval = ERROR_COMMAND_ARGUMENT_INVALID;
			}
		} else {
			if (!target->tap_configured) {
				command_print(CMD, "-chain-position ?name? required when creating target");
				retval = ERROR_COMMAND_ARGUMENT_INVALID;
			}
		}
		/* tap must be set after target was configured */
		if (!target->tap)
			retval = ERROR_COMMAND_ARGUMENT_INVALID;
	}

	if (retval != ERROR_OK) {
		rtos_destroy(target);
		free(target->gdb_port_override);
		free(target->trace_info);
		free(target->type);
		free(target->private_config);
		free(target);
		return retval;
	}

	if (target->endianness == TARGET_ENDIAN_UNKNOWN) {
		/* default endian to little if not specified */
		target->endianness = TARGET_LITTLE_ENDIAN;
	}

	if (target->type->target_create) {
		retval = (*target->type->target_create)(target);
		if (retval != ERROR_OK) {
			LOG_DEBUG("target_create failed");
			free(target->cmd_name);
			rtos_destroy(target);
			free(target->gdb_port_override);
			free(target->trace_info);
			free(target->type);
			free(target->private_config);
			free(target);
			return retval;
		}
	}

	/* create the target specific commands */
	if (target->type->commands) {
		retval = register_commands(CMD_CTX, NULL, target->type->commands);
		if (retval != ERROR_OK)
			LOG_ERROR("unable to register '%s' commands", CMD_ARGV[0]);
	}

	/* now - create the new target name command */
	const struct command_registration target_subcommands[] = {
		{
			.chain = target_instance_command_handlers,
		},
		{
			.chain = target->type->commands,
		},
		COMMAND_REGISTRATION_DONE
	};
	const struct command_registration target_commands[] = {
		{
			.name = CMD_ARGV[0],
			.mode = COMMAND_ANY,
			.help = "target command group",
			.usage = "",
			.chain = target_subcommands,
		},
		COMMAND_REGISTRATION_DONE
	};
	retval = register_commands_override_target(CMD_CTX, NULL, target_commands, target);
	if (retval != ERROR_OK) {
		if (target->type->deinit_target)
			target->type->deinit_target(target);
		free(target->cmd_name);
		rtos_destroy(target);
		free(target->gdb_port_override);
		free(target->trace_info);
		free(target->type);
		free(target);
		return retval;
	}

	/* append to end of list */
	append_to_list_all_targets(target);

	CMD_CTX->current_target = target;
	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_current)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = get_current_target_or_null(CMD_CTX);
	if (target)
		command_print(CMD, "%s", target_name(target));

	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_types)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	for (size_t x = 0; x < ARRAY_SIZE(target_types); x++)
		command_print(CMD, "%s", target_types[x]->name);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_target_names)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct target *target = all_targets;
	while (target) {
		command_print(CMD, "%s", target_name(target));
		target = target->next;
	}

	return ERROR_OK;
}

static struct target_list *
__attribute__((warn_unused_result))
create_target_list_node(const char *targetname)
{
	struct target *target = get_target(targetname);
	LOG_DEBUG("%s ", targetname);
	if (!target)
		return NULL;

	struct target_list *new = malloc(sizeof(struct target_list));
	if (!new) {
		LOG_ERROR("Out of memory");
		return new;
	}

	new->target = target;
	return new;
}

static int get_target_with_common_rtos_type(struct command_invocation *cmd,
	struct list_head *lh, struct target **result)
{
	struct target *target = NULL;
	struct target *first_target = NULL;
	struct target_list *curr;
	foreach_smp_target(curr, lh) {
		struct rtos *curr_rtos = curr->target->rtos;
		if (curr_rtos) {
			if (target && target->rtos && target->rtos->type != curr_rtos->type) {
				command_print(cmd, "Different rtos types in members of one smp target!");
				return ERROR_FAIL;
			}
			target = curr->target;
			if (!first_target)
				first_target = target;
		}
	}
	*result = first_target;
	return JIM_OK;
}

COMMAND_HANDLER(handle_target_smp)
{
	static unsigned int smp_group = 1;

	if (CMD_ARGC == 0) {
		LOG_DEBUG("Empty SMP target");
		return ERROR_OK;
	}
	LOG_DEBUG("%d", CMD_ARGC);
	/* CMD_ARGC[0] = target to associate in smp
	 * CMD_ARGC[1] = target to associate in smp
	 * CMD_ARGC[2] ...
	 */

	struct list_head *lh = malloc(sizeof(*lh));
	if (!lh) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}
	INIT_LIST_HEAD(lh);

	for (unsigned int i = 0; i < CMD_ARGC; i++) {
		struct target_list *new = create_target_list_node(CMD_ARGV[i]);
		if (new)
			list_add_tail(&new->lh, lh);
	}
	/*  now parse the list of cpu and put the target in smp mode*/
	struct target_list *curr;
	foreach_smp_target(curr, lh) {
		struct target *target = curr->target;
		target->smp = smp_group;
		target->smp_targets = lh;
	}
	smp_group++;

	struct target *rtos_target;
	int retval = get_target_with_common_rtos_type(CMD, lh, &rtos_target);
	if (retval == ERROR_OK && rtos_target)
		retval = rtos_smp_init(rtos_target);

	return retval;
}

static const struct command_registration target_subcommand_handlers[] = {
	{
		.name = "init",
		.mode = COMMAND_CONFIG,
		.handler = handle_target_init_command,
		.help = "initialize targets",
		.usage = "",
	},
	{
		.name = "create",
		.mode = COMMAND_CONFIG,
		.handler = handle_target_create,
		.usage = "name type [options ...]",
		.help = "Creates and selects a new target",
	},
	{
		.name = "current",
		.mode = COMMAND_ANY,
		.handler = handle_target_current,
		.help = "Returns the currently selected target",
		.usage = "",
	},
	{
		.name = "types",
		.mode = COMMAND_ANY,
		.handler = handle_target_types,
		.help = "Returns the available target types as "
				"a list of strings",
		.usage = "",
	},
	{
		.name = "names",
		.mode = COMMAND_ANY,
		.handler = handle_target_names,
		.help = "Returns the names of all targets as a list of strings",
		.usage = "",
	},
	{
		.name = "smp",
		.mode = COMMAND_ANY,
		.handler = handle_target_smp,
		.usage = "targetname1 targetname2 ...",
		.help = "gather several target in a smp list"
	},

	COMMAND_REGISTRATION_DONE
};

struct fast_load {
	target_addr_t address;
	uint8_t *data;
	int length;

};

static int fastload_num;
static struct fast_load *fastload;

static void free_fastload(void)
{
	if (fastload) {
		for (int i = 0; i < fastload_num; i++)
			free(fastload[i].data);
		free(fastload);
		fastload = NULL;
	}
}

COMMAND_HANDLER(handle_fast_load_image_command)
{
	uint8_t *buffer;
	size_t buf_cnt;
	uint32_t image_size;
	target_addr_t min_address = 0;
	target_addr_t max_address = -1;

	struct image image;

	int retval = CALL_COMMAND_HANDLER(parse_load_image_command,
			&image, &min_address, &max_address);
	if (retval != ERROR_OK)
		return retval;

	struct duration bench;
	duration_start(&bench);

	retval = image_open(&image, CMD_ARGV[0], (CMD_ARGC >= 3) ? CMD_ARGV[2] : NULL);
	if (retval != ERROR_OK)
		return retval;

	image_size = 0x0;
	retval = ERROR_OK;
	fastload_num = image.num_sections;
	fastload = malloc(sizeof(struct fast_load)*image.num_sections);
	if (!fastload) {
		command_print(CMD, "out of memory");
		image_close(&image);
		return ERROR_FAIL;
	}
	memset(fastload, 0, sizeof(struct fast_load)*image.num_sections);
	for (unsigned int i = 0; i < image.num_sections; i++) {
		buffer = malloc(image.sections[i].size);
		if (!buffer) {
			command_print(CMD, "error allocating buffer for section (%d bytes)",
						  (int)(image.sections[i].size));
			retval = ERROR_FAIL;
			break;
		}

		retval = image_read_section(&image, i, 0x0, image.sections[i].size, buffer, &buf_cnt);
		if (retval != ERROR_OK) {
			free(buffer);
			break;
		}

		uint32_t offset = 0;
		uint32_t length = buf_cnt;

		/* DANGER!!! beware of unsigned comparison here!!! */

		if ((image.sections[i].base_address + buf_cnt >= min_address) &&
				(image.sections[i].base_address < max_address)) {
			if (image.sections[i].base_address < min_address) {
				/* clip addresses below */
				offset += min_address-image.sections[i].base_address;
				length -= offset;
			}

			if (image.sections[i].base_address + buf_cnt > max_address)
				length -= (image.sections[i].base_address + buf_cnt)-max_address;

			fastload[i].address = image.sections[i].base_address + offset;
			fastload[i].data = malloc(length);
			if (!fastload[i].data) {
				free(buffer);
				command_print(CMD, "error allocating buffer for section (%" PRIu32 " bytes)",
							  length);
				retval = ERROR_FAIL;
				break;
			}
			memcpy(fastload[i].data, buffer + offset, length);
			fastload[i].length = length;

			image_size += length;
			command_print(CMD, "%u bytes written at address 0x%8.8x",
						  (unsigned int)length,
						  ((unsigned int)(image.sections[i].base_address + offset)));
		}

		free(buffer);
	}

	if ((retval == ERROR_OK) && (duration_measure(&bench) == ERROR_OK)) {
		command_print(CMD, "Loaded %" PRIu32 " bytes "
				"in %fs (%0.3f KiB/s)", image_size,
				duration_elapsed(&bench), duration_kbps(&bench, image_size));

		command_print(CMD,
				"WARNING: image has not been loaded to target!"
				"You can issue a 'fast_load' to finish loading.");
	}

	image_close(&image);

	if (retval != ERROR_OK)
		free_fastload();

	return retval;
}

COMMAND_HANDLER(handle_fast_load_command)
{
	if (CMD_ARGC > 0)
		return ERROR_COMMAND_SYNTAX_ERROR;
	if (!fastload) {
		LOG_ERROR("No image in memory");
		return ERROR_FAIL;
	}
	int i;
	int64_t ms = timeval_ms();
	int size = 0;
	int retval = ERROR_OK;
	for (i = 0; i < fastload_num; i++) {
		struct target *target = get_current_target(CMD_CTX);
		command_print(CMD, "Write to 0x%08x, length 0x%08x",
					  (unsigned int)(fastload[i].address),
					  (unsigned int)(fastload[i].length));
		retval = target_write_buffer(target, fastload[i].address, fastload[i].length, fastload[i].data);
		if (retval != ERROR_OK)
			break;
		size += fastload[i].length;
	}
	if (retval == ERROR_OK) {
		int64_t after = timeval_ms();
		command_print(CMD, "Loaded image %f kBytes/s", (float)(size/1024.0)/((float)(after-ms)/1000.0));
	}
	return retval;
}

static const struct command_registration target_command_handlers[] = {
	{
		.name = "targets",
		.handler = handle_targets_command,
		.mode = COMMAND_ANY,
		.help = "change current default target (one parameter) "
			"or prints table of all targets (no parameters)",
		.usage = "[target]",
	},
	{
		.name = "target",
		.mode = COMMAND_CONFIG,
		.help = "configure target",
		.chain = target_subcommand_handlers,
		.usage = "",
	},
	COMMAND_REGISTRATION_DONE
};

int target_register_commands(struct command_context *cmd_ctx)
{
	return register_commands(cmd_ctx, NULL, target_command_handlers);
}

static bool target_reset_nag = true;

bool get_target_reset_nag(void)
{
	return target_reset_nag;
}

COMMAND_HANDLER(handle_target_reset_nag)
{
	return CALL_COMMAND_HANDLER(handle_command_parse_bool,
			&target_reset_nag, "Nag after each reset about options to improve "
			"performance");
}

COMMAND_HANDLER(handle_ps_command)
{
	struct target *target = get_current_target(CMD_CTX);
	char *display;
	if (target->state != TARGET_HALTED) {
		command_print(CMD, "Error: [%s] not halted", target_name(target));
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((target->rtos) && (target->rtos->type)
			&& (target->rtos->type->ps_command)) {
		display = target->rtos->type->ps_command(target);
		command_print(CMD, "%s", display);
		free(display);
		return ERROR_OK;
	} else {
		LOG_INFO("failed");
		return ERROR_TARGET_FAILURE;
	}
}

static void binprint(struct command_invocation *cmd, const char *text, const uint8_t *buf, int size)
{
	if (text)
		command_print_sameline(cmd, "%s", text);
	for (int i = 0; i < size; i++)
		command_print_sameline(cmd, " %02x", buf[i]);
	command_print(cmd, " ");
}

COMMAND_HANDLER(handle_test_mem_access_command)
{
	struct target *target = get_current_target(CMD_CTX);
	uint32_t test_size;
	int retval = ERROR_OK;

	if (target->state != TARGET_HALTED) {
		command_print(CMD, "Error: [%s] not halted", target_name(target));
		return ERROR_TARGET_NOT_HALTED;
	}

	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], test_size);

	/* Test reads */
	size_t num_bytes = test_size + 4;

	struct working_area *wa = NULL;
	retval = target_alloc_working_area(target, num_bytes, &wa);
	if (retval != ERROR_OK) {
		LOG_ERROR("Not enough working area");
		return ERROR_FAIL;
	}

	uint8_t *test_pattern = malloc(num_bytes);

	for (size_t i = 0; i < num_bytes; i++)
		test_pattern[i] = rand();

	retval = target_write_memory(target, wa->address, 1, num_bytes, test_pattern);
	if (retval != ERROR_OK) {
		LOG_ERROR("Test pattern write failed");
		goto out;
	}

	for (int host_offset = 0; host_offset <= 1; host_offset++) {
		for (int size = 1; size <= 4; size *= 2) {
			for (int offset = 0; offset < 4; offset++) {
				uint32_t count = test_size / size;
				size_t host_bufsiz = (count + 2) * size + host_offset;
				uint8_t *read_ref = malloc(host_bufsiz);
				uint8_t *read_buf = malloc(host_bufsiz);

				for (size_t i = 0; i < host_bufsiz; i++) {
					read_ref[i] = rand();
					read_buf[i] = read_ref[i];
				}
				command_print_sameline(CMD,
						"Test read %" PRIu32 " x %d @ %d to %saligned buffer: ", count,
						size, offset, host_offset ? "un" : "");

				struct duration bench;
				duration_start(&bench);

				retval = target_read_memory(target, wa->address + offset, size, count,
						read_buf + size + host_offset);

				duration_measure(&bench);

				if (retval == ERROR_TARGET_UNALIGNED_ACCESS) {
					command_print(CMD, "Unsupported alignment");
					goto next;
				} else if (retval != ERROR_OK) {
					command_print(CMD, "Memory read failed");
					goto next;
				}

				/* replay on host */
				memcpy(read_ref + size + host_offset, test_pattern + offset, count * size);

				/* check result */
				int result = memcmp(read_ref, read_buf, host_bufsiz);
				if (result == 0) {
					command_print(CMD, "Pass in %fs (%0.3f KiB/s)",
							duration_elapsed(&bench),
							duration_kbps(&bench, count * size));
				} else {
					command_print(CMD, "Compare failed");
					binprint(CMD, "ref:", read_ref, host_bufsiz);
					binprint(CMD, "buf:", read_buf, host_bufsiz);
				}
next:
				free(read_ref);
				free(read_buf);
			}
		}
	}

out:
	free(test_pattern);

	target_free_working_area(target, wa);

	/* Test writes */
	num_bytes = test_size + 4 + 4 + 4;

	retval = target_alloc_working_area(target, num_bytes, &wa);
	if (retval != ERROR_OK) {
		LOG_ERROR("Not enough working area");
		return ERROR_FAIL;
	}

	test_pattern = malloc(num_bytes);

	for (size_t i = 0; i < num_bytes; i++)
		test_pattern[i] = rand();

	for (int host_offset = 0; host_offset <= 1; host_offset++) {
		for (int size = 1; size <= 4; size *= 2) {
			for (int offset = 0; offset < 4; offset++) {
				uint32_t count = test_size / size;
				size_t host_bufsiz = count * size + host_offset;
				uint8_t *read_ref = malloc(num_bytes);
				uint8_t *read_buf = malloc(num_bytes);
				uint8_t *write_buf = malloc(host_bufsiz);

				for (size_t i = 0; i < host_bufsiz; i++)
					write_buf[i] = rand();
				command_print_sameline(CMD,
						"Test write %" PRIu32 " x %d @ %d from %saligned buffer: ", count,
						size, offset, host_offset ? "un" : "");

				retval = target_write_memory(target, wa->address, 1, num_bytes, test_pattern);
				if (retval != ERROR_OK) {
					command_print(CMD, "Test pattern write failed");
					goto nextw;
				}

				/* replay on host */
				memcpy(read_ref, test_pattern, num_bytes);
				memcpy(read_ref + size + offset, write_buf + host_offset, count * size);

				struct duration bench;
				duration_start(&bench);

				retval = target_write_memory(target, wa->address + size + offset, size, count,
						write_buf + host_offset);

				duration_measure(&bench);

				if (retval == ERROR_TARGET_UNALIGNED_ACCESS) {
					command_print(CMD, "Unsupported alignment");
					goto nextw;
				} else if (retval != ERROR_OK) {
					command_print(CMD, "Memory write failed");
					goto nextw;
				}

				/* read back */
				retval = target_read_memory(target, wa->address, 1, num_bytes, read_buf);
				if (retval != ERROR_OK) {
					command_print(CMD, "Test pattern write failed");
					goto nextw;
				}

				/* check result */
				int result = memcmp(read_ref, read_buf, num_bytes);
				if (result == 0) {
					command_print(CMD, "Pass in %fs (%0.3f KiB/s)",
							duration_elapsed(&bench),
							duration_kbps(&bench, count * size));
				} else {
					command_print(CMD, "Compare failed");
					binprint(CMD, "ref:", read_ref, num_bytes);
					binprint(CMD, "buf:", read_buf, num_bytes);
				}
nextw:
				free(read_ref);
				free(read_buf);
			}
		}
	}

	free(test_pattern);

	target_free_working_area(target, wa);
	return retval;
}

static const struct command_registration target_exec_command_handlers[] = {
	{
		.name = "fast_load_image",
		.handler = handle_fast_load_image_command,
		.mode = COMMAND_ANY,
		.help = "Load image into server memory for later use by "
			"fast_load; primarily for profiling",
		.usage = "filename [address ['bin'|'ihex'|'elf'|'s19' "
			"[min_address [max_length]]]]",
	},
	{
		.name = "fast_load",
		.handler = handle_fast_load_command,
		.mode = COMMAND_EXEC,
		.help = "loads active fast load image to current target "
			"- mainly for profiling purposes",
		.usage = "",
	},
	{
		.name = "profile",
		.handler = handle_profile_command,
		.mode = COMMAND_EXEC,
		.usage = "seconds filename [start end]",
		.help = "profiling samples the CPU PC",
	},
	/** @todo don't register virt2phys() unless target supports it */
	{
		.name = "virt2phys",
		.handler = handle_virt2phys_command,
		.mode = COMMAND_ANY,
		.help = "translate a virtual address into a physical address",
		.usage = "virtual_address",
	},
	{
		.name = "reg",
		.handler = handle_reg_command,
		.mode = COMMAND_EXEC,
		.help = "display (reread from target with \"force\") or set a register; "
			"with no arguments, displays all registers and their values",
		.usage = "[(register_number|register_name) [(value|'force')]]",
	},
	{
		.name = "poll",
		.handler = handle_poll_command,
		.mode = COMMAND_EXEC,
		.help = "poll target state; or reconfigure background polling",
		.usage = "['on'|'off']",
	},
	{
		.name = "wait_halt",
		.handler = handle_wait_halt_command,
		.mode = COMMAND_EXEC,
		.help = "wait up to the specified number of milliseconds "
			"(default 5000) for a previously requested halt",
		.usage = "[milliseconds]",
	},
	{
		.name = "halt",
		.handler = handle_halt_command,
		.mode = COMMAND_EXEC,
		.help = "request target to halt, then wait up to the specified "
			"number of milliseconds (default 5000) for it to complete",
		.usage = "[milliseconds]",
	},
	{
		.name = "resume",
		.handler = handle_resume_command,
		.mode = COMMAND_EXEC,
		.help =	"resume target execution from current PC or address",
		.usage = "[address]",
	},
	{
		.name = "reset",
		.handler = handle_reset_command,
		.mode = COMMAND_EXEC,
		.usage = "[run|halt|init]",
		.help = "Reset all targets into the specified mode. "
			"Default reset mode is run, if not given.",
	},
	{
		.name = "soft_reset_halt",
		.handler = handle_soft_reset_halt_command,
		.mode = COMMAND_EXEC,
		.usage = "",
		.help = "halt the target and do a soft reset",
	},
	{
		.name = "step",
		.handler = handle_step_command,
		.mode = COMMAND_EXEC,
		.help =	"step one instruction from current PC or address",
		.usage = "[address]",
	},
	{
		.name = "mdd",
		.handler = handle_md_command,
		.mode = COMMAND_EXEC,
		.help = "display memory double-words",
		.usage = "['phys'] address [count]",
	},
	{
		.name = "mdw",
		.handler = handle_md_command,
		.mode = COMMAND_EXEC,
		.help = "display memory words",
		.usage = "['phys'] address [count]",
	},
	{
		.name = "mdh",
		.handler = handle_md_command,
		.mode = COMMAND_EXEC,
		.help = "display memory half-words",
		.usage = "['phys'] address [count]",
	},
	{
		.name = "mdb",
		.handler = handle_md_command,
		.mode = COMMAND_EXEC,
		.help = "display memory bytes",
		.usage = "['phys'] address [count]",
	},
	{
		.name = "mwd",
		.handler = handle_mw_command,
		.mode = COMMAND_EXEC,
		.help = "write memory double-word",
		.usage = "['phys'] address value [count]",
	},
	{
		.name = "mww",
		.handler = handle_mw_command,
		.mode = COMMAND_EXEC,
		.help = "write memory word",
		.usage = "['phys'] address value [count]",
	},
	{
		.name = "mwh",
		.handler = handle_mw_command,
		.mode = COMMAND_EXEC,
		.help = "write memory half-word",
		.usage = "['phys'] address value [count]",
	},
	{
		.name = "mwb",
		.handler = handle_mw_command,
		.mode = COMMAND_EXEC,
		.help = "write memory byte",
		.usage = "['phys'] address value [count]",
	},
	{
		.name = "mbatch",
		.handler = handle_mbatch_command,
		.mode = COMMAND_EXEC,
		.help = "execute a batch of memory commands",
		.usage = "[unique ID] [list of memory read/write requests]",
	},
	{
		.name = "bp",
		.handler = handle_bp_command,
		.mode = COMMAND_EXEC,
		.help = "list or set hardware or software breakpoint",
		.usage = "[<address> [<asid>] <length> ['hw'|'hw_ctx']]",
	},
	{
		.name = "rbp",
		.handler = handle_rbp_command,
		.mode = COMMAND_EXEC,
		.help = "remove breakpoint",
		.usage = "'all' | address",
	},
	{
		.name = "wp",
		.handler = handle_wp_command,
		.mode = COMMAND_EXEC,
		.help = "list (no params) or create watchpoints",
		.usage = "[address length [('r'|'w'|'a') [value [mask]]]]",
	},
	{
		.name = "rwp",
		.handler = handle_rwp_command,
		.mode = COMMAND_EXEC,
		.help = "remove watchpoint",
		.usage = "'all' | address",
	},
	{
		.name = "load_image",
		.handler = handle_load_image_command,
		.mode = COMMAND_EXEC,
		.usage = "filename [address ['bin'|'ihex'|'elf'|'s19' "
			"[min_address [max_length]]]]",
	},
	{
		.name = "dump_image",
		.handler = handle_dump_image_command,
		.mode = COMMAND_EXEC,
		.usage = "filename address size",
	},
	{
		.name = "verify_image_checksum",
		.handler = handle_verify_image_checksum_command,
		.mode = COMMAND_EXEC,
		.usage = "filename [offset [type]]",
	},
	{
		.name = "verify_image",
		.handler = handle_verify_image_command,
		.mode = COMMAND_EXEC,
		.usage = "filename [offset [type]]",
	},
	{
		.name = "test_image",
		.handler = handle_test_image_command,
		.mode = COMMAND_EXEC,
		.usage = "filename [offset [type]]",
	},
	{
		.name = "get_reg",
		.mode = COMMAND_EXEC,
		.handler = handle_target_get_reg,
		.help = "Get register values from the target",
		.usage = "[-force] list",
	},
	{
		.name = "set_reg",
		.mode = COMMAND_EXEC,
		.handler = handle_set_reg_command,
		.help = "Set target register values",
		.usage = "dict",
	},
	{
		.name = "read_memory",
		.mode = COMMAND_EXEC,
		.handler = handle_target_read_memory,
		.help = "Read Tcl list of 8/16/32/64 bit numbers from target memory",
		.usage = "address width count ['phys']",
	},
	{
		.name = "write_memory",
		.mode = COMMAND_EXEC,
		.handler = handle_target_write_memory,
		.help = "Write Tcl list of 8/16/32/64 bit numbers to target memory",
		.usage = "address width data ['phys']",
	},
	{
		.name = "debug_reason",
		.mode = COMMAND_EXEC,
		.handler = handle_target_debug_reason,
		.help = "displays the debug reason of this target",
		.usage = "",
	},
	{
		.name = "reset_nag",
		.handler = handle_target_reset_nag,
		.mode = COMMAND_ANY,
		.help = "Nag after each reset about options that could have been "
				"enabled to improve performance.",
		.usage = "['enable'|'disable']",
	},
	{
		.name = "ps",
		.handler = handle_ps_command,
		.mode = COMMAND_EXEC,
		.help = "list all tasks",
		.usage = "",
	},
	{
		.name = "test_mem_access",
		.handler = handle_test_mem_access_command,
		.mode = COMMAND_EXEC,
		.help = "Test the target's memory access functions",
		.usage = "size",
	},

	COMMAND_REGISTRATION_DONE
};
static int target_register_user_commands(struct command_context *cmd_ctx)
{
	int retval = ERROR_OK;
	retval = target_request_register_commands(cmd_ctx);
	if (retval != ERROR_OK)
		return retval;

	retval = trace_register_commands(cmd_ctx);
	if (retval != ERROR_OK)
		return retval;


	return register_commands(cmd_ctx, NULL, target_exec_command_handlers);
}

const char *target_debug_reason_str(enum target_debug_reason reason)
{
	switch (reason) {
		case DBG_REASON_DBGRQ:
			return "DBGRQ";
		case DBG_REASON_BREAKPOINT:
			return "BREAKPOINT";
		case DBG_REASON_WATCHPOINT:
			return "WATCHPOINT";
		case DBG_REASON_WPTANDBKPT:
			return "WPTANDBKPT";
		case DBG_REASON_SINGLESTEP:
			return "SINGLESTEP";
		case DBG_REASON_NOTHALTED:
			return "NOTHALTED";
		case DBG_REASON_EXIT:
			return "EXIT";
		case DBG_REASON_EXC_CATCH:
			return "EXC_CATCH";
		case DBG_REASON_UNDEFINED:
			return "UNDEFINED";
		default:
			return "UNKNOWN!";
	}
}

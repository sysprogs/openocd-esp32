// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007-2010 Øyvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2008 Richard Missenden                                  *
 *   richard.missenden@googlemail.com                                      *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "openocd.h"
#include <jtag/adapter.h>
#include <jtag/jtag.h>
#include <transport/transport.h>
#include <helper/util.h>
#include <helper/configuration.h>
#include <flash/nor/core.h>
#include <flash/nand/core.h>
#include <pld/pld.h>
#include <target/arm_cti.h>
#include <target/arm_adi_v5.h>
#include <target/arm_tpiu_swo.h>
#include <rtt/rtt.h>

#include <server/server.h>
#include <server/gdb_server.h>
#include <server/rtt_server.h>

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#ifdef PKGBLDDATE
#define OPENOCD_VERSION	\
	"Open On-Chip Debugger " VERSION RELSTR " (" PKGBLDDATE ")"
#else
#define OPENOCD_VERSION	\
	"Open On-Chip Debugger " VERSION RELSTR
#endif

static const char openocd_startup_tcl[] = {
#include "startup_tcl.inc"
0 /* Terminate with zero */
};

/* Give scripts and TELNET a way to find out what version this is */
COMMAND_HANDLER(handler_version_command)
{
	char *version_str = OPENOCD_VERSION;

	if (CMD_ARGC > 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (CMD_ARGC == 1) {
		if (strcmp("git", CMD_ARGV[0]))
			return ERROR_COMMAND_ARGUMENT_INVALID;

		version_str = GITVERSION;
	}

	command_print(CMD, "%s", version_str);

	return ERROR_OK;
}

static int log_target_callback_event_handler(struct target *target,
	enum target_event event,
	void *priv)
{
	switch (event) {
		case TARGET_EVENT_GDB_START:
			target->verbose_halt_msg = false;
			break;
		case TARGET_EVENT_GDB_END:
			target->verbose_halt_msg = true;
			break;
		case TARGET_EVENT_HALTED:
			if (target->verbose_halt_msg) {
				/* do not display information when debugger caused the halt */
				target_arch_state(target);
			}
			break;
		default:
			break;
	}

	return ERROR_OK;
}

static bool init_at_startup = true;

COMMAND_HANDLER(handle_noinit_command)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;
	init_at_startup = false;
	return ERROR_OK;
}

/* OpenOCD can't really handle failure of this command. Patches welcome! :-) */
COMMAND_HANDLER(handle_init_command)
{

	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	int retval;
	static int initialized;
	if (initialized)
		return ERROR_OK;

	initialized = 1;

	bool save_poll_mask = jtag_poll_mask();

	retval = command_run_line(CMD_CTX, "target init");
	if (retval != ERROR_OK)
		return ERROR_FAIL;

	retval = adapter_init(CMD_CTX);
	if (retval != ERROR_OK) {
		/* we must be able to set up the debug adapter */
		return retval;
	}

	LOG_DEBUG("Debug Adapter init complete");

	/* "transport init" verifies the expected devices are present;
	 * for JTAG, it checks the list of configured TAPs against
	 * what's discoverable, possibly with help from the platform's
	 * JTAG event handlers.  (which require COMMAND_EXEC)
	 */
	command_context_mode(CMD_CTX, COMMAND_EXEC);

	retval = command_run_line(CMD_CTX, "transport init");
	if (retval != ERROR_OK)
		return ERROR_FAIL;

	retval = command_run_line(CMD_CTX, "dap init");
	if (retval != ERROR_OK)
		return ERROR_FAIL;

	LOG_DEBUG("Examining targets...");
	if (target_examine() != ERROR_OK)
		LOG_DEBUG("target examination failed");

	command_context_mode(CMD_CTX, COMMAND_CONFIG);

	if (command_run_line(CMD_CTX, "flash init") != ERROR_OK)
		return ERROR_FAIL;

	if (command_run_line(CMD_CTX, "nand init") != ERROR_OK)
		return ERROR_FAIL;

	if (command_run_line(CMD_CTX, "pld init") != ERROR_OK)
		return ERROR_FAIL;
	command_context_mode(CMD_CTX, COMMAND_EXEC);

	/* in COMMAND_EXEC, after target_examine(), only tpiu or only swo */
	if (command_run_line(CMD_CTX, "tpiu init") != ERROR_OK)
		return ERROR_FAIL;

	jtag_poll_unmask(save_poll_mask);

	/* initialize telnet subsystem */
	if (gdb_target_add_all(all_targets) != ERROR_OK)
		return ERROR_FAIL;

	target_register_event_callback(log_target_callback_event_handler, CMD_CTX);

	if (command_run_line(CMD_CTX, "_run_post_init_commands") != ERROR_OK)
		return ERROR_FAIL;

	return ERROR_OK;
}

COMMAND_HANDLER(handle_add_script_search_dir_command)
{
	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	add_script_search_dir(CMD_ARGV[0]);

	return ERROR_OK;
}

static const struct command_registration openocd_command_handlers[] = {
	{
		.name = "version",
		.handler = handler_version_command,
		.mode = COMMAND_ANY,
		.help = "show program version",
		.usage = "[git]",
	},
	{
		.name = "noinit",
		.handler = &handle_noinit_command,
		.mode = COMMAND_CONFIG,
		.help = "Prevent 'init' from being called at startup.",
		.usage = ""
	},
	{
		.name = "init",
		.handler = &handle_init_command,
		.mode = COMMAND_ANY,
		.help = "Initializes configured targets and servers.  "
			"Changes command mode from CONFIG to EXEC.  "
			"Unless 'noinit' is called, this command is "
			"called automatically at the end of startup.",
		.usage = ""
	},
	{
		.name = "add_script_search_dir",
		.handler = &handle_add_script_search_dir_command,
		.mode = COMMAND_ANY,
		.help = "dir to search for config files and scripts",
		.usage = "<directory>"
	},
	COMMAND_REGISTRATION_DONE
};

static int openocd_register_commands(struct command_context *cmd_ctx)
{
	return register_commands(cmd_ctx, NULL, openocd_command_handlers);
}

struct command_context *global_cmd_ctx;

static int (* const command_registrants[])(struct command_context *cmd_ctx_value) = {
	openocd_register_commands,
	server_register_commands,
	gdb_register_commands,
	log_register_commands,
	rtt_server_register_commands,
	transport_register_commands,
	adapter_register_commands,
	target_register_commands,
	flash_register_commands,
	nand_register_commands,
	pld_register_commands,
	cti_register_commands,
	dap_register_commands,
	arm_tpiu_swo_register_commands,
};

static struct command_context *setup_command_handler(Jim_Interp *interp)
{
	log_init();
	LOG_DEBUG("log_init: complete");

	struct command_context *cmd_ctx = command_init(openocd_startup_tcl, interp);

	/* register subsystem commands */
	for (unsigned int i = 0; i < ARRAY_SIZE(command_registrants); i++) {
		int retval = (*command_registrants[i])(cmd_ctx);
		if (retval != ERROR_OK) {
			command_done(cmd_ctx);
			return NULL;
		}
	}
	LOG_DEBUG("command registration: complete");

	LOG_OUTPUT(OPENOCD_VERSION "\n"
		"Licensed under GNU GPL v2\n");
	LOG_OUTPUT("libusb1 " LIBUSB1_COMMIT "\n");

	global_cmd_ctx = cmd_ctx;

	return cmd_ctx;
}

/** OpenOCD runtime meat that can become single-thread in future. It parse
 * commandline, reads configuration, sets up the target and starts server loop.
 * Commandline arguments are passed into this function from openocd_main().
 */
static int openocd_thread(int argc, char *argv[], struct command_context *cmd_ctx)
{
	int ret;

	if (parse_cmdline_args(cmd_ctx, argc, argv) != ERROR_OK)
		return ERROR_FAIL;

	if (server_preinit() != ERROR_OK)
		return ERROR_FAIL;

	ret = parse_config_file(cmd_ctx);
	if (ret == ERROR_COMMAND_CLOSE_CONNECTION) {
		server_quit(); /* gdb server may be initialized by -c init */
		return ERROR_OK;
	} else if (ret != ERROR_OK) {
		server_quit(); /* gdb server may be initialized by -c init */
		return ERROR_FAIL;
	}

	ret = server_init(cmd_ctx);
	if (ret != ERROR_OK)
		return ERROR_FAIL;

	if (init_at_startup) {
		ret = command_run_line(cmd_ctx, "init");
		if (ret != ERROR_OK) {
			server_quit();
			return ERROR_FAIL;
		}
	}

	ret = server_loop(cmd_ctx);

	int last_signal = server_quit();
	if (last_signal != ERROR_OK)
		return last_signal;

	if (ret != ERROR_OK)
		return ERROR_FAIL;
	return ERROR_OK;
}

/* normally this is the main() function entry, but if OpenOCD is linked
 * into application, then this fn will not be invoked, but rather that
 * application will have it's own implementation of main(). */
int openocd_main(int argc, char *argv[])
{
	int ret;

	/* initialize commandline interface */
	struct command_context *cmd_ctx;

	cmd_ctx = setup_command_handler(NULL);

	if (util_init(cmd_ctx) != ERROR_OK)
		return EXIT_FAILURE;

	if (rtt_init() != ERROR_OK)
		return EXIT_FAILURE;

	LOG_OUTPUT("For bug reports, read\n\t"
		"http://openocd.org/doc/doxygen/bugs.html"
		"\n");

	command_context_mode(cmd_ctx, COMMAND_CONFIG);
	command_set_output_handler(cmd_ctx, configuration_output_handler, NULL);

	server_host_os_entry();

	/* Start the executable meat that can evolve into thread in future. */
	ret = openocd_thread(argc, argv, cmd_ctx);

	flash_free_all_banks();
	gdb_service_free();
	arm_tpiu_swo_cleanup_all();
	server_free();

	unregister_all_commands(cmd_ctx, NULL);
	help_del_all_commands(cmd_ctx);

	/* free all DAP and CTI objects */
	arm_cti_cleanup_all();
	dap_cleanup_all();

	adapter_quit();

	server_host_os_close();

	/* Shutdown commandline interface */
	command_exit(cmd_ctx);

	rtt_exit();
	free_config();

	log_exit();

#if USE_GCOV
	/* Always explicitly dump coverage data before terminating.
	 * Otherwise coverage would not be dumped when exit_on_signal occurs. */
	void __gcov_dump(void);
	__gcov_dump();
#endif

	if (ret == ERROR_FAIL)
		return EXIT_FAILURE;
	else if (ret != ERROR_OK)
		exit_on_signal(ret);

	return ret;
}

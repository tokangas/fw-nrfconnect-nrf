/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdlib.h>
#include <shell/shell.h>
#include "modem_fota_internal.h"

static int fota_cmd_timer(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t seconds;

	seconds = strtoul(argv[1], NULL, 10);

	if (seconds == 0 || seconds > MAX_TIMER_DURATION_S) {
		shell_error(shell, "timer: invalid timer value");
		return -EINVAL;
	}

	set_time_to_next_update_check(seconds);

	return 0;
}

static int fota_cmd_server(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t port;

	port = strtoul(argv[2], NULL, 10);

	if (port == 0 || port > USHRT_MAX) {
		shell_error(shell, "set: invalid port number");
		return -EINVAL;
	}

	set_api_override(argv[1]);
	set_api_port_override(port);

	return 0;
}

static int fota_cmd_fw_server(const struct shell *shell, size_t argc, char **argv)
{
	set_fw_api_override(argv[1]);

	return 0;
}

static int fota_cmd_disable(const struct shell *shell, size_t argc, char **argv)
{
	if (!is_fota_enabled()) {
		shell_error(shell, "disable: FOTA already disabled");
		return -ENOEXEC;
	}

	disable_fota();

	return 0;
}

static int fota_cmd_enable(const struct shell *shell, size_t argc, char **argv)
{
	if (is_fota_enabled()) {
		shell_error(shell, "enable: FOTA already enabled");
		return -ENOEXEC;
	}

	enable_fota();

	return 0;
}

static int fota_cmd_status(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t time_to_check;
	uint32_t time_to_check_without_days;
	const char *fw_api_address;

	shell_print(shell, "FOTA %s",
		    is_fota_enabled() ? "enabled" : "disabled");
	time_to_check = get_time_to_next_update_check();
	if (time_to_check > 0) {
		time_to_check_without_days = time_to_check % SECONDS_IN_DAY;
		shell_print(shell, "Time until next update check: %d days, %02d:%02d:%02d",
			time_to_check / SECONDS_IN_DAY,
			time_to_check_without_days / 3600,
			(time_to_check_without_days % 3600) / 60,
			(time_to_check_without_days % 3600) % 60);
	} else {
		shell_print(shell, "Next update check not scheduled or no " \
				   "network time");
	}
	shell_print(shell, "DM server address: %s", get_api_address());
	shell_print(shell, "DM server port: %d", get_api_port());
	fw_api_address = get_fw_api_address();
	if (fw_api_address == NULL) {
		fw_api_address = "got from FOTA job";
	}
	shell_print(shell, "FW download server address: %s", fw_api_address);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(fota_cmds,
	SHELL_CMD_ARG(timer, NULL,
		  "'fota timer <seconds>' sets the FOTA timer to expire in "
		  "given number of seconds.", fota_cmd_timer, 2, 0),
	SHELL_CMD_ARG(server, NULL,
		  "'fota server <hostname> <port>' sets the Device Management "
		  "server hostname and port number.", fota_cmd_server, 3, 0),
	SHELL_CMD_ARG(fw_server, NULL,
		  "'fota fw_server <hostname>' sets the firmware download "
		  "server hostname.", fota_cmd_fw_server, 2, 0),
	SHELL_CMD(disable, NULL, "Disable FOTA.", fota_cmd_disable),
	SHELL_CMD(enable, NULL, "Enable FOTA.", fota_cmd_enable),
	SHELL_CMD(status, NULL, "Show FOTA status.", fota_cmd_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(fota, &fota_cmds,
		   "Commands for controlling the FOTA Client", NULL);

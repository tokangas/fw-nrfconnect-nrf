/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>
#include <bsd.h>
#include <string.h>
#include <zephyr.h>
#include <power/reboot.h>
#include <modem/bsdlib.h>
#include <modem/lte_lc.h>
#include <modem/at_cmd.h>
#include <modem/at_notif.h>
#include <modem/modem_fota.h>

void ping_init(void);

void bsd_recoverable_error_handler(uint32_t err)
{
	printk("bsdlib recoverable error: %u\n", err);
}

void modem_fota_callback(enum modem_fota_evt_id event_id)
{
	switch (event_id) {
	case MODEM_FOTA_EVT_CHECKING_FOR_UPDATE:
		printk("Modem FOTA library: Checking for update\n");
		break;

	case MODEM_FOTA_EVT_NO_UPDATE_AVAILABLE:
		printk("Modem FOTA library: No update available\n");
		break;

	case MODEM_FOTA_EVT_UPDATE_DOWNLOADED:
		printk("Modem FOTA library: Update downloaded, device will "
		       "reboot to apply update\n");
		break;

	case MODEM_FOTA_EVT_ERROR:
	default:
		printk("Modem FOTA library: Error during update check or "
		       "download\n");
		break;
	}
}

void main(void)
{
	int err;

	printk("Modem FOTA sample started\n");

	printk("Initializing bsdlib...\n");
	err = bsdlib_init();
	switch (err) {
	case MODEM_DFU_RESULT_OK:
		printk("Modem firmware update successful!\n");
		printk("Modem will run the new firmware after reboot\n");
		sys_reboot(SYS_REBOOT_WARM);
		break;
	case MODEM_DFU_RESULT_UUID_ERROR:
	case MODEM_DFU_RESULT_AUTH_ERROR:
		printk("Modem firmware update failed!\n");
		printk("Modem will run non-updated firmware on reboot.\n");
		sys_reboot(SYS_REBOOT_WARM);
		break;
	case MODEM_DFU_RESULT_HARDWARE_ERROR:
	case MODEM_DFU_RESULT_INTERNAL_ERROR:
		printk("Modem firmware update failed!\n");
		printk("Fatal error.\n");
		sys_reboot(SYS_REBOOT_WARM);
		break;
	case -1:
		printk("Could not initialize bsdlib.\n");
		printk("Fatal error.\n");
		return;
	default:
		break;
	}
	printk("Initialized bsdlib\n");

	ping_init();

	/* Initialize AT command and notification libraries because
	 * CONFIG_BSD_LIBRARY_SYS_INIT is disabled and these libraries aren't
	 * initialized automatically.
	 */
	at_cmd_init();
	at_notif_init();

	if (modem_fota_init(&modem_fota_callback) != 0) {
		printk("Failed to initialize modem FOTA\n");
		return;
	}

	printk("LTE link connecting...\n");
	err = lte_lc_init_and_connect();
	__ASSERT(err == 0, "LTE link could not be established.");
	printk("LTE link connected!\n");

	modem_fota_configure();

	k_sleep(K_FOREVER);
}

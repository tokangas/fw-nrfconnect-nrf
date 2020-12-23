/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef MODEM_FOTA_INTERNAL_H_
#define MODEM_FOTA_INTERNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#define SECONDS_IN_DAY (24 * 60 * 60)

/* Currently the maximum timer duration is ~18h, so we'll use that */
#define MAX_TIMER_DURATION_S (18 * 60 * 60)

bool is_fota_enabled(void);

void enable_fota(void);

void disable_fota(void);

uint32_t get_time_to_next_update_check(void);

void set_time_to_next_update_check(uint32_t seconds);

const char *get_api_address(void);

void set_api_override(const char *hostname);

uint16_t get_api_port(void);

void set_api_port_override(uint16_t port);

const char *get_fw_api_address(void);

void set_fw_api_override(const char *hostname);

#ifdef __cplusplus
}
#endif

#endif /* MODEM_FOTA_INTERNAL_H_ */

/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

/**
 * @file modem_fota.h
 *
 * @brief Public APIs for the modem FOTA client.
 * @defgroup modem_fota Modem FOTA client
 * @{
 */

#ifndef MODEM_FOTA_H_
#define MODEM_FOTA_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum length for the device ID string (not including the
 *        terminating null).
 */
#define MODEM_FOTA_DEVICE_ID_MAX_LEN 64

/**
 * @brief Modem FOTA event IDs.
 */
enum modem_fota_evt_id {
	/** Checking for update started */
	MODEM_FOTA_EVT_CHECKING_FOR_UPDATE,
	/** No update available */
	MODEM_FOTA_EVT_NO_UPDATE_AVAILABLE,
	/** Update downloaded, system will restart to apply the update */
	MODEM_FOTA_EVT_UPDATE_DOWNLOADED,
	/** Error during update check or download */
	MODEM_FOTA_EVT_ERROR,
};

/**
 * @brief Modem FOTA asynchronous callback function.
 *
 * @param event_id Event ID.
 */
typedef void (*modem_fota_callback_t)(enum modem_fota_evt_id event_id);

/**
 * @brief Initializes the modem FOTA client.
 *
 * This API must be called before LTE attach.
 *
 * @param callback Callback for the generated events.
 * @param device_id nRFCloud device ID. The device ID must be a globally unique
 *                  string, for example a GUID. The device ID is used to
 *                  associate the device to an account and to create FOTA jobs
 *                  for the device. See MODEM_FOTA_DEVICE_ID_MAX_LEN for
 *                  maximum string length.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int modem_fota_init(modem_fota_callback_t callback, const char *device_id);

/**
 * @brief Configure the modem FOTA client.
 *
 * This API must be called right after LTE attach.
 */
void modem_fota_configure(void);

/**
 * @brief Sets current time for the modem FOTA client and modem clock.
 *
 * Sets the current time used by the modem FOTA client. The given time is also
 * set to the modem. If LTE network time is not available, current time has
 * to be provided to the modem FOTA client using this function.
 *
 * LTE network time overrides the time set using this function. If time is
 * available from the LTE network, time doesn't have to be set using this
 * function.
 *
 * @param time_str Time as a null terminated string in format
 *                 "yy/MM/dd,hh:mm:ss±zz", where the characters, from left to
 *                 right, indicate year, month, day, hour, minutes, seconds and
 *                 time zone. Time zone indicates the difference, expressed in
 *                 quarters of an hour, between the local time and GMT (value
 *                 range -48...+48). For example "20/10/15,09:12:47+12".
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int modem_fota_set_clock(const char *time_str);

#ifdef __cplusplus
}
#endif

#endif /* MODEM_FOTA_H_ */

/** @} */
